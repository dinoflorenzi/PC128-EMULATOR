#include "sdlcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "core.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define CLOSE_SOCK closesocket
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define CLOSE_SOCK close
#endif

#define MAX_BRIDGES 8
#define IO_TIMEOUT_MS 500

typedef enum { BR_EXP, BR_CART, BR_JOY } BridgeKind;

typedef struct
{
  int used;
  BridgeKind kind;
  char host[96];
  int port;
  sock_t fd;
  int down;
  int irq_source;                 /* 1..31, or 0 = none */
  unsigned long long poll_cycles;
  unsigned long long next_poll;
  /* statistics */
  unsigned long long n_xfer;
  double ms_total, ms_max;
  /* PIA bridges: control-line levels received, applied from tick() */
  int lines[4];                   /* CA1 CA2 CB1 CB2 last seen from the device */
  int lines_pending;
} Bridge;

static Bridge bridges[MAX_BRIDGES];
static int sockets_ready = 0;

/* ---------------------------------------------------------------- sockets */
static int sockets_init(void)
{
#ifdef _WIN32
  WSADATA wsa;
  if (!sockets_ready && WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
  sockets_ready = 1;
  return 0;
}

static void set_timeouts(sock_t fd, int ms)
{
#ifdef _WIN32
  DWORD t = (DWORD) ms;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &t, sizeof(t));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &t, sizeof(t));
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static sock_t connect_to(const char *host, int port, int timeout_ms, char *err, size_t errsz)
{
  struct addrinfo hints, *res, *ai;
  char portstr[16];
  sock_t fd = BAD_SOCK;
  if (sockets_init() != 0) { snprintf(err, errsz, "network layer not available"); return BAD_SOCK; }
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", port);
  if (getaddrinfo(host, portstr, &hints, &res) != 0)
  {
    snprintf(err, errsz, "cannot resolve '%s'", host);
    return BAD_SOCK;
  }
  for (ai = res; ai != NULL; ai = ai->ai_next)
  {
    fd_set wf;
    struct timeval tv;
    int so_err = 0;
    socklen_t len = sizeof(so_err);
    int rc;
#ifdef _WIN32
    u_long nb = 1;
#endif
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == BAD_SOCK) continue;
#ifdef _WIN32
    ioctlsocket(fd, FIONBIO, &nb);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    rc = connect(fd, ai->ai_addr, (int) ai->ai_addrlen);
    if (rc != 0)
    {
      FD_ZERO(&wf);
      FD_SET(fd, &wf);
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      if (select((int) fd + 1, NULL, &wf, NULL, &tv) <= 0) { CLOSE_SOCK(fd); fd = BAD_SOCK; continue; }
      getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &so_err, &len);
      if (so_err != 0) { CLOSE_SOCK(fd); fd = BAD_SOCK; continue; }
    }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
#endif
    break;
  }
  freeaddrinfo(res);
  if (fd == BAD_SOCK)
  {
    snprintf(err, errsz, "cannot connect to %s:%d", host, port);
    return BAD_SOCK;
  }
  {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
  }
  set_timeouts(fd, IO_TIMEOUT_MS);
  return fd;
}

static int send_all(sock_t fd, const uint8_t *buf, size_t n)
{
  while (n > 0)
  {
    int r = (int) send(fd, (const char *) buf, (int) n, 0);
    if (r <= 0) return -1;
    buf += r;
    n -= (size_t) r;
  }
  return 0;
}

static int recv_all(sock_t fd, uint8_t *buf, size_t n)
{
  while (n > 0)
  {
    int r = (int) recv(fd, (char *) buf, (int) n, 0);
    if (r <= 0) return -1;
    buf += r;
    n -= (size_t) r;
  }
  return 0;
}

/* ---------------------------------------------------------------- transfer */
static void release_irq(Bridge *b)
{
  if (b->irq_source > 0) core.theodore_periph_set_irq_source(b->irq_source, 0);
}

static int pia_id(void) { return core.theodore_slot_pia(); }

/* One request/response exchange. Returns 0, or -1 when the link is down. */
static int xfer(Bridge *b, char op, uint8_t value, uint16_t addr, uint64_t cycle, uint8_t *data, uint8_t *flags)
{
  uint8_t out[12], in[2];
  Uint64 t0, t1;
  int i;
  if (b->down) return -1;
  out[0] = (uint8_t) op;
  out[1] = value;
  out[2] = (uint8_t) (addr & 0xFF);
  out[3] = (uint8_t) (addr >> 8);
  for (i = 0; i < 8; i++) out[4 + i] = (uint8_t) (cycle >> (8 * i));
  t0 = SDL_GetPerformanceCounter();
  if (send_all(b->fd, out, sizeof(out)) != 0 || recv_all(b->fd, in, sizeof(in)) != 0)
  {
    fprintf(stderr, "bridge %s:%d: link lost (no answer within %d ms); the device is now ignored\n", b->host, b->port, IO_TIMEOUT_MS);
    b->down = 1;
    release_irq(b);
    return -1;
  }
  t1 = SDL_GetPerformanceCounter();
  {
    double ms = (double) (t1 - t0) * 1000.0 / (double) SDL_GetPerformanceFrequency();
    b->n_xfer++;
    b->ms_total += ms;
    if (ms > b->ms_max) b->ms_max = ms;
  }
  if (data != NULL) *data = in[0];
  if (flags != NULL) *flags = in[1];
  /* IRQ level travels in every answer */
  if (b->irq_source > 0) core.theodore_periph_set_irq_source(b->irq_source, in[1] & 1);
  /* PIA control inputs (flag bit 7 = levels present, bits 2..5 = CA1 CA2 CB1 CB2) */
  if (b->kind == BR_JOY && (in[1] & 0x80))
  {
    for (i = 0; i < 4; i++)
    {
      int lvl = (in[1] >> (2 + i)) & 1;
      if (lvl != b->lines[i]) { b->lines[i] = lvl; b->lines_pending |= 1 << i; }
    }
  }
  return 0;
}

/* -------------------------------------------------- bus callbacks (exp/cart) */
static int br_read(void *ctx, unsigned short addr, uint64_t cycle)
{
  uint8_t d = 0xFF, f = 0;
  if (xfer((Bridge *) ctx, 'R', 0, addr, cycle, &d, &f) != 0 || (f & 2)) return THEODORE_PERIPH_PASS;
  return d;
}

static int br_write(void *ctx, unsigned short addr, unsigned char value, uint64_t cycle)
{
  uint8_t f = 0;
  if (xfer((Bridge *) ctx, 'W', value, addr, cycle, NULL, &f) != 0) return 0;
  return (f & 2) ? 0 : 1;
}

static void poll_if_due(Bridge *b, uint64_t cycle)
{
  if (b->poll_cycles > 0 && cycle >= b->next_poll && !b->down)
  {
    b->next_poll = cycle + b->poll_cycles;
    xfer(b, 'P', 0, 0, cycle, NULL, NULL);
  }
}

static void br_tick(void *ctx, uint64_t cycle) { poll_if_due((Bridge *) ctx, cycle); }

static void br_reset(void *ctx)
{
  Bridge *b = (Bridge *) ctx;
  b->next_poll = 0;
  xfer(b, 'X', 0, 0, 0, NULL, NULL);
}

/* ------------------------------------------------------- PIA callbacks (joy) */
static int br_read_pins(void *ctx, ThPiaPort port, uint64_t cycle)
{
  uint8_t d = 0xFF, f = 0;
  if (xfer((Bridge *) ctx, 'I', 0, (uint16_t) (port == THPIA_PORT_B ? 1 : 0), cycle, &d, &f) != 0 || (f & 2)) return -1;
  return d;
}

static void br_port_output(void *ctx, ThPiaPort port, uint8_t out, uint8_t ddr, uint64_t cycle)
{
  xfer((Bridge *) ctx, 'O', out, (uint16_t) ((port == THPIA_PORT_B ? 1 : 0) | (ddr << 8)), cycle, NULL, NULL);
}

static void br_control_output(void *ctx, ThPiaLine line, int level, uint64_t cycle)
{
  xfer((Bridge *) ctx, 'C', (uint8_t) (level != 0), (uint16_t) line, cycle, NULL, NULL);
}

static void br_pia_tick(void *ctx, uint64_t cycle)
{
  Bridge *b = (Bridge *) ctx;
  poll_if_due(b, cycle);
  if (b->lines_pending)
  {
    int i, id = pia_id();
    int pending = b->lines_pending;
    b->lines_pending = 0;
    for (i = 0; i < 4 && id >= 0; i++)
    {
      if (pending & (1 << i)) core.theodore_pia_set_line(id, (ThPiaLine) i, b->lines[i]);
    }
  }
}

/* ------------------------------------------------------------ spec parsing */
static int parse_options(Bridge *b, char *opts, int *override_native, int *ctl, int *builtin, int *std, char *err, size_t errsz)
{
  char *p = opts;
  while (p != NULL && *p != '\0')
  {
    char *comma = strchr(p, ',');
    if (comma != NULL) *comma = '\0';
    if (!strncmp(p, "irq=", 4))
    {
      b->irq_source = atoi(p + 4);
      if (b->irq_source < 1 || b->irq_source > 31) { snprintf(err, errsz, "irq must be 1..31"); return -1; }
    }
    else if (!strncmp(p, "poll=", 5)) b->poll_cycles = strtoull(p + 5, NULL, 10);
    else if (!strcmp(p, "override")) *override_native = 1;
    else if (!strcmp(p, "ctl")) *ctl = 1;
    else if (!strcmp(p, "builtin")) *builtin = 1;
    else if (!strcmp(p, "std")) *std = 1;
    else if (*p != '\0') { snprintf(err, errsz, "unknown option '%s'", p); return -1; }
    p = comma != NULL ? comma + 1 : NULL;
  }
  return 0;
}

int bridge_attach_spec(const char *spec, char *err, size_t errsz)
{
  char buf[512], *kind, *rest, *at, *colon, *opts;
  Bridge *b = NULL;
  int i, override_native = 0, ctl = 0, builtin = 0, std = 0;
  unsigned base = 0, size = 0;

  snprintf(buf, sizeof(buf), "%s", spec);
  kind = buf;
  rest = strchr(buf, '|');
  if (rest == NULL) { snprintf(err, errsz, "malformed spec"); return -1; }
  *rest++ = '\0';

  for (i = 0; i < MAX_BRIDGES; i++) if (!bridges[i].used) { b = &bridges[i]; break; }
  if (b == NULL) { snprintf(err, errsz, "too many bridges"); return -1; }
  memset(b, 0, sizeof(*b));
  b->fd = BAD_SOCK;
  for (i = 0; i < 4; i++) b->lines[i] = 1;

  if (!strcmp(kind, "exp")) b->kind = BR_EXP;
  else if (!strcmp(kind, "cart")) b->kind = BR_CART;
  else if (!strcmp(kind, "joy")) b->kind = BR_JOY;
  else { snprintf(err, errsz, "unknown bridge kind '%s' (exp, cart or joy)", kind); return -1; }

  /* HOST:PORT[@BASE+SIZE][,options] */
  at = strchr(rest, '@');
  opts = strchr(rest, ',');
  if (b->kind == BR_JOY)
  {
    if (at != NULL) { snprintf(err, errsz, "the joystick bridge has no @BASE+SIZE"); return -1; }
    if (opts != NULL) *opts++ = '\0';
  }
  else
  {
    char *plus;
    if (at == NULL) { snprintf(err, errsz, "expected HOST:PORT@BASE+SIZE"); return -1; }
    *at++ = '\0';
    if (opts != NULL && opts < at) { snprintf(err, errsz, "options must follow @BASE+SIZE"); return -1; }
    opts = strchr(at, ',');
    if (opts != NULL) *opts++ = '\0';
    plus = strchr(at, '+');
    if (plus == NULL) { snprintf(err, errsz, "expected BASE+SIZE in hexadecimal"); return -1; }
    *plus++ = '\0';
    base = (unsigned) strtoul(at, NULL, 16);
    size = (unsigned) strtoul(plus, NULL, 16);
    if (size == 0) { snprintf(err, errsz, "SIZE must be > 0"); return -1; }
  }
  colon = strrchr(rest, ':');
  if (colon == NULL) { snprintf(err, errsz, "expected HOST:PORT"); return -1; }
  *colon++ = '\0';
  snprintf(b->host, sizeof(b->host), "%s", rest);
  b->port = atoi(colon);
  if (b->port <= 0 || b->port > 65535) { snprintf(err, errsz, "bad port"); return -1; }
  if (parse_options(b, opts, &override_native, &ctl, &builtin, &std, err, errsz) != 0) return -1;

  b->fd = connect_to(b->host, b->port, 2000, err, errsz);
  if (b->fd == BAD_SOCK) return -1;
  b->used = 1;

  {
    ThSlotConfig sc;
    memset(&sc, 0, sizeof(sc));
    sc.irq_source = b->irq_source;
    if (b->kind == BR_JOY)
    {
      sc.builtin_joystick = builtin;
      sc.pia.layout = std ? THPIA_LAYOUT_STANDARD : THPIA_LAYOUT_THOMSON;
      sc.pia.ctx = b;
      sc.pia.read_pins = br_read_pins;
      sc.pia.on_port_output = br_port_output;
      sc.pia.on_control_output = br_control_output;
      sc.pia.tick = br_pia_tick;
      if (core.theodore_slot_attach(THSLOT_JOYSTICK, &sc) != 0) goto attach_failed;
    }
    else
    {
      sc.base = (unsigned short) base;
      sc.size = (unsigned short) size;
      sc.override_native = override_native;
      sc.cartridge_control = ctl;
      sc.dev.name = b->kind == BR_EXP ? "bridge-exp" : "bridge-cart";
      sc.dev.read = br_read;
      sc.dev.write = br_write;
      sc.dev.tick = br_tick;
      sc.dev.reset = br_reset;
      sc.dev.ctx = b;
      if (core.theodore_slot_attach(b->kind == BR_EXP ? THSLOT_EXPANSION : THSLOT_CARTRIDGE, &sc) != 0) goto attach_failed;
    }
  }
  printf("bridge %s: %s:%d attached%s\n", kind, b->host, b->port, b->irq_source ? " (IRQ wired)" : "");
  return 0;

attach_failed:
  snprintf(err, errsz, "%s", core.theodore_slot_error());
  CLOSE_SOCK(b->fd);
  b->used = 0;
  return -1;
}

void bridge_print_status(void)
{
  static const char *kinds[] = { "expansion", "cartridge", "joystick" };
  int i, any = 0;
  for (i = 0; i < MAX_BRIDGES; i++)
  {
    const Bridge *b = &bridges[i];
    if (!b->used) continue;
    any = 1;
    printf("%-9s %s:%d %s, %llu transactions, avg %.3f ms, max %.3f ms%s\n", kinds[b->kind], b->host, b->port,
           b->down ? "DOWN" : "up", b->n_xfer, b->n_xfer ? b->ms_total / (double) b->n_xfer : 0.0, b->ms_max,
           b->poll_cycles ? " (polling)" : "");
  }
  if (!any) puts("no bridges attached");
}

void bridge_shutdown(void)
{
  int i;
  for (i = 0; i < MAX_BRIDGES; i++)
  {
    if (bridges[i].used)
    {
      release_irq(&bridges[i]);
      if (bridges[i].fd != BAD_SOCK) CLOSE_SOCK(bridges[i].fd);
      bridges[i].used = 0;
    }
  }
  core.theodore_slot_detach(THSLOT_EXPANSION);
  core.theodore_slot_detach(THSLOT_CARTRIDGE);
  core.theodore_slot_detach(THSLOT_JOYSTICK);
}
