#include "sdlcompat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "bridge.h"
#include "core.h"
#include "dbgcli.h"

#define LINE_MAX_LEN 512
#define QUEUE_LEN 256

static SDL_mutex *qlock;
static char queue[QUEUE_LEN][LINE_MAX_LEN];
static int q_head = 0, q_tail = 0;

/* script: lines loaded from a file, consumed from the main loop */
static char (*script)[LINE_MAX_LEN] = NULL;
static int script_len = 0, script_pos = 0;
static int wait_frames = 0;       /* `wait N` */
static int wait_stop = 0;         /* `waitstop N`: frames left before giving up; 0 = not waiting */
static int wait_stop_active = 0;

void dbg_init(void)
{
  qlock = SDL_CreateMutex();
}

/* ------------------------------------------------------------ small helpers */
static int parse_hex(const char *s, unsigned *out)
{
  char *end;
  unsigned long v;
  if (s == NULL || *s == '\0') return -1;
  if (s[0] == '$') s++;
  else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  v = strtoul(s, &end, 16);
  if (*end != '\0' || end == s) return -1;
  *out = (unsigned) v;
  return 0;
}

static void cc_string(unsigned cc, char *out)
{
  static const char names[] = "EFHINZVC";
  int i;
  for (i = 0; i < 8; i++) out[i] = (cc & (0x80u >> i)) ? names[i] : '.';
  out[8] = '\0';
}

static void print_regs(void)
{
  TheodoreRegs r;
  char cc[9];
  core.theodore_dbg_get_regs(&r);
  cc_string(r.cc, cc);
  printf("A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X[%s] PC=%04X cycle=%llu\n",
         r.a, r.b, r.x, r.y, r.u, r.s, r.dp, r.cc, cc, r.pc, (unsigned long long) core.theodore_periph_cycle());
}

static void print_next_instruction(void)
{
  TheodoreRegs r;
  char line[160];
  core.theodore_dbg_get_regs(&r);
  core.theodore_dbg_disasm(r.pc, line, sizeof(line));
  printf("  next: %s\n", line);
}

/* stop counter value already shown to the user */
static unsigned reported_stops = 0;

static void report_stop(void)
{
  TheodoreStopInfo si = core.theodore_dbg_stop_info();
  switch (si.reason)
  {
    case TDBG_STOP_PC: printf("STOP: breakpoint at PC=%04X\n", si.addr); break;
    case TDBG_STOP_READ: printf("STOP: read of %04X returned %02X\n", si.addr, si.value); break;
    case TDBG_STOP_WRITE: printf("STOP: write of %02X to %04X\n", si.value, si.addr); break;
    case TDBG_STOP_DATA:
      printf("STOP: data breakpoint #%d, %s of %02X at %04X\n", si.data_id, si.is_read ? "read" : "write", si.value, si.addr);
      break;
    case TDBG_STOP_USER: printf("STOP: halted by user\n"); break;
    case TDBG_STOP_STEP: printf("STOP: step\n"); break;
    default: printf("STOP\n"); break;
  }
  print_regs();
  print_next_instruction();
  fflush(stdout);
}

void dbg_check_stop(void)
{
  unsigned n = core.theodore_dbg_stop_count();
  if (n != reported_stops && core.theodore_dbg_is_halted()) report_stop();
  reported_stops = n;
}

/* ------------------------------------------------------------- tokenization */
#define MAXTOK 64
/* Splits the line in place. Returns the token count, or -1 when there are more
   than MAXTOK tokens (nothing is silently dropped). */
static int tokenize(char *line, char *tok[])
{
  int n = 0;
  char *p = line;
  for (;;)
  {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    if (n >= MAXTOK) return -1;
    tok[n++] = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') *p++ = '\0';
  }
  return n;
}

static int parse_bytes(const char *s, uint8_t *out, unsigned max)
{
  unsigned n = 0;
  for (;;)
  {
    char tok[16];
    size_t len = strcspn(s, ",");
    unsigned v;
    if (len == 0 || len >= sizeof(tok) || n >= max) return -1;
    memcpy(tok, s, len);
    tok[len] = '\0';
    if (parse_hex(tok, &v) != 0 || v > 255) return -1;
    out[n++] = (uint8_t) v;
    s += len;
    if (*s == '\0') break;
    s++; /* skip the comma */
  }
  return (int) n;
}

static int parse_dec(const char *s, unsigned *out)
{
  char *end;
  unsigned long v = strtoul(s, &end, 10);
  if (end == s || *end != '\0') return -1;
  *out = (unsigned) v;
  return 0;
}

/* ------------------------------------------------------------------ commands */
static void cmd_help(void)
{
  puts(
    "Execution:  go | c            resume          halt | b       stop now\n"
    "            step | s [N]      execute N instructions (default 1)\n"
    "            reset             machine reset   quit | q\n"
    "State:      regs              show registers  set REG VALUE  (A B DP CC X Y U S PC)\n"
    "            mem ADDR [LEN]    hex dump (no side effects)\n"
    "            poke ADDR V [V..] write RAM (0000-9FFF)\n"
    "            find [-i] FIRST[-LAST] \"text\" | B1,B2,??,B4   search memory (?? = any byte, -i ignores case)\n"
    "            dis [ADDR] [N]    disassemble N instructions (default PC, 8)\n"
    "Breakpoints (hex): bp pc|r|w ADDR     bpdel pc|r|w ADDR     bpl   bpclear\n"
    "            bpd r|w|rw FIRST[-LAST] V[,V..] [MASK[,MASK..]]   data pattern, up to 4 bytes\n"
    "                  a pattern of N bytes matches the last N accessed bytes inside the range;\n"
    "                  bytes compare as (byte & mask) == (value & mask)\n"
    "            bpddel ID\n"
    "Trace:      trace on|off|clear|dump [N]\n"
    "Hardware:   slots             attached slots and bridge statistics\n"
    "            irq               peripheral IRQ source mask\n"
    "            pia               6821 state (joystick slot)\n"
    "            pia pins A|B VAL  drive input pins     pia line ca1|ca2|cb1|cb2 0|1\n"
    "            pad PORT BTN 0|1  press/release a joystick button (up down left right fire)\n"
    "Frontend:   type TEXT         type on the Thomson keyboard (\\n = Return)\n"
    "            paste             type the clipboard (SDL2; SDL 1.2: the file paste.txt)\n"
    "            typefile FILE     type the contents of a text file    typestop   drop what is still queued\n"
    "            press CODE [sbac] raw key press by libretro key code (decimal) with Shift / BASIC / RAZ / CNT\n"
    "            scan HEX [sbac]   same, by Thomson key matrix position 00-4F (keys with no libretro code)\n"
    "            typespeed H G [R] frames a key is held / released / extra pause after Return (default 3 1 30)\n"
    "            shot [FILE.bmp]   screenshot            load FILE   load media\n"
    "            ss FILE | ls FILE save / load state     model NAME   var KEY VALUE\n"
    "Script:     wait N            wait N frames         waitstop [N]  wait until halted (max N frames, default 3000)\n"
    "            echo TEXT         print TEXT            # comment");
}

static void cmd_regs_set(char **tok, int n)
{
  TheodoreRegs r;
  unsigned v;
  char *name = tok[1];
  if (n < 3 || parse_hex(tok[2], &v) != 0) { puts("usage: set REG VALUE"); return; }
  core.theodore_dbg_get_regs(&r);
  for (char *c = name; *c; c++) *c = (char) toupper((unsigned char) *c);
  if (!strcmp(name, "A")) r.a = (uint8_t) v;
  else if (!strcmp(name, "B")) r.b = (uint8_t) v;
  else if (!strcmp(name, "DP")) r.dp = (uint8_t) v;
  else if (!strcmp(name, "CC")) r.cc = (uint8_t) v;
  else if (!strcmp(name, "X")) r.x = (uint16_t) v;
  else if (!strcmp(name, "Y")) r.y = (uint16_t) v;
  else if (!strcmp(name, "U")) r.u = (uint16_t) v;
  else if (!strcmp(name, "S")) r.s = (uint16_t) v;
  else if (!strcmp(name, "PC")) r.pc = (uint16_t) v;
  else { printf("unknown register %s\n", name); return; }
  core.theodore_dbg_set_regs(&r);
  print_regs();
}

static void cmd_mem(char **tok, int n)
{
  unsigned addr, len = 64, i;
  if (n < 2 || parse_hex(tok[1], &addr) != 0) { puts("usage: mem ADDR [LEN]"); return; }
  if (n >= 3) parse_hex(tok[2], &len);
  if (len > 4096) len = 4096;
  for (i = 0; i < len; i += 16)
  {
    unsigned j;
    char ascii[17];
    printf("%04X: ", (addr + i) & 0xFFFF);
    for (j = 0; j < 16; j++)
    {
      if (i + j < len)
      {
        uint8_t b = core.theodore_dbg_peek((uint16_t) (addr + i + j));
        printf("%02X ", b);
        ascii[j] = (b >= 32 && b < 127) ? (char) b : '.';
      }
      else { printf("   "); ascii[j] = ' '; }
    }
    ascii[16] = '\0';
    printf(" %s\n", ascii);
  }
}

/* find [-i] FIRST[-LAST] "text" | find [-i] FIRST[-LAST] B1,B2,??,B4 ...
   Searches the CPU-visible memory (side-effect-free peek, current banking). */
#define FIND_MAX_PAT 64
#define FIND_MAX_HITS 100
static void cmd_find(const char *args)
{
  uint8_t mem[0x10000], pat[FIND_MAX_PAT];
  unsigned char wild[FIND_MAX_PAT];
  char range[64], *dash;
  unsigned first = 0, last = 0xFFFF, plen = 0, a, i, hits = 0;
  int icase = 0;
  size_t rl;
  const char *p = args;

  while (*p == ' ' || *p == '\t') p++;
  if (p[0] == '-' && p[1] == 'i' && (p[2] == ' ' || p[2] == '\t')) { icase = 1; p += 2; while (*p == ' ' || *p == '\t') p++; }
  rl = strcspn(p, " \t");
  if (rl == 0 || rl >= sizeof(range)) { puts("usage: find [-i] FIRST[-LAST] \"text\" | BYTE[,BYTE..] (?? = any byte)"); return; }
  memcpy(range, p, rl); range[rl] = '\0';
  p += rl;
  while (*p == ' ' || *p == '\t') p++;
  dash = strchr(range, '-');
  if (dash != NULL) *dash = '\0';
  if (parse_hex(range, &first) != 0 || first > 0xFFFF || (dash != NULL && (parse_hex(dash + 1, &last) != 0 || last > 0xFFFF)) || last < first)
  { puts("bad range (hex, FIRST[-LAST] within 0000-FFFF)"); return; }
  if (*p == '"')
  {
    const char *q = strrchr(p + 1, '"');
    size_t n;
    if (q == NULL) { puts("missing closing quote"); return; }
    n = (size_t) (q - (p + 1));
    if (n == 0 || n > FIND_MAX_PAT) { printf("string must be 1-%d characters\n", FIND_MAX_PAT); return; }
    for (i = 0; i < n; i++) { pat[i] = (uint8_t) p[1 + i]; wild[i] = 0; }
    plen = (unsigned) n;
  }
  else
  {
    char buf[LINE_MAX_LEN], *t;
    snprintf(buf, sizeof(buf), "%s", p);
    for (t = buf; *t; t++) if (*t == ',') *t = ' ';
    for (t = strtok(buf, " \t"); t != NULL; t = strtok(NULL, " \t"))
    {
      unsigned v;
      if (plen >= FIND_MAX_PAT) { printf("pattern too long (max %d bytes)\n", FIND_MAX_PAT); return; }
      if (!strcmp(t, "??")) { pat[plen] = 0; wild[plen++] = 1; }
      else if (parse_hex(t, &v) == 0 && v <= 255) { pat[plen] = (uint8_t) v; wild[plen++] = 0; }
      else { printf("bad byte '%s' (hex 00-FF or ?\?)\n", t); return; }
    }
    if (plen == 0) { puts("usage: find [-i] FIRST[-LAST] \"text\" | BYTE[,BYTE..] (?? = any byte)"); return; }
  }
  if (icase) for (i = 0; i < plen; i++) if (!wild[i] && pat[i] >= 'A' && pat[i] <= 'Z') pat[i] = (uint8_t) (pat[i] + 32);
  for (a = 0; a < 0x10000; a++) mem[a] = core.theodore_dbg_peek((uint16_t) a);
  if (plen > last - first + 1) { puts("no match"); return; }
  for (a = first; a + plen - 1 <= last; a++)
  {
    int ok = 1;
    for (i = 0; i < plen && ok; i++)
    {
      uint8_t b = mem[a + i];
      if (wild[i]) continue;
      if (icase && b >= 'A' && b <= 'Z') b = (uint8_t) (b + 32);
      if (b != pat[i]) ok = 0;
    }
    if (!ok) continue;
    if (hits == FIND_MAX_HITS)
    {
      printf("... more matches: continue with  find %s%04X-%04X ...\n", icase ? "-i " : "", a, last);
      return;
    }
    hits++;
    printf("%04X: ", a);
    for (i = 0; i < 16; i++) { if (a + i < 0x10000) printf("%02X ", mem[a + i]); else printf("   "); }
    putchar(' ');
    for (i = 0; i < 16 && a + i < 0x10000; i++) putchar(mem[a + i] >= 32 && mem[a + i] < 127 ? mem[a + i] : '.');
    putchar('\n');
  }
  printf("%u match(es)\n", hits);
}

static void cmd_dis(char **tok, int n)
{
  TheodoreRegs r;
  unsigned addr, count = 8, i;
  core.theodore_dbg_get_regs(&r);
  addr = r.pc;
  if (n >= 2 && parse_hex(tok[1], &addr) != 0) { puts("usage: dis [ADDR] [N]"); return; }
  if (n >= 3) count = (unsigned) atoi(tok[2]);
  if (count > 256) count = 256;
  for (i = 0; i < count; i++)
  {
    char line[160];
    unsigned next = core.theodore_dbg_disasm((uint16_t) addr, line, sizeof(line));
    printf("%s%s\n", addr == r.pc ? "=> " : "   ", line);
    addr = next;
  }
}

static int bp_type(const char *s, TheodoreBpType *t)
{
  if (!strcmp(s, "pc")) *t = TDBG_BP_PC;
  else if (!strcmp(s, "r") || !strcmp(s, "read")) *t = TDBG_BP_READ;
  else if (!strcmp(s, "w") || !strcmp(s, "write")) *t = TDBG_BP_WRITE;
  else return -1;
  return 0;
}

static void cmd_bp(char **tok, int n, int del)
{
  TheodoreBpType t;
  unsigned addr;
  if (n < 3 || bp_type(tok[1], &t) != 0 || parse_hex(tok[2], &addr) != 0 || addr > 0xFFFF)
  {
    printf("usage: %s pc|r|w ADDR\n", del ? "bpdel" : "bp");
    return;
  }
  if (del) printf("%s\n", core.theodore_dbg_del_breakpoint(t, (uint16_t) addr) == 0 ? "removed" : "no such breakpoint");
  else printf("%s\n", core.theodore_dbg_add_breakpoint(t, (uint16_t) addr) == 0 ? "ok" : "table full");
}

static void cmd_bpd(char **tok, int n)
{
  unsigned access = 0, first, last;
  uint8_t value[TDBG_DATA_LEN_MAX], mask[TDBG_DATA_LEN_MAX];
  int nv, nm = 0, id;
  char range[64], *dash;
  if (n < 4) { puts("usage: bpd r|w|rw FIRST[-LAST] V[,V..] [MASK[,MASK..]]"); return; }
  if (!strcmp(tok[1], "r")) access = TDBG_ACCESS_READ;
  else if (!strcmp(tok[1], "w")) access = TDBG_ACCESS_WRITE;
  else if (!strcmp(tok[1], "rw")) access = TDBG_ACCESS_READ | TDBG_ACCESS_WRITE;
  else { puts("access must be r, w or rw"); return; }
  snprintf(range, sizeof(range), "%s", tok[2]);
  dash = strchr(range, '-');
  if (dash != NULL) *dash = '\0';
  if (parse_hex(range, &first) != 0 || (dash != NULL ? parse_hex(dash + 1, &last) != 0 : (last = first, 0)))
  {
    puts("bad address range");
    return;
  }
  nv = parse_bytes(tok[3], value, TDBG_DATA_LEN_MAX);
  if (nv < 1) { puts("bad value list (1 to 4 hex bytes separated by commas)"); return; }
  if (n >= 5)
  {
    nm = parse_bytes(tok[4], mask, TDBG_DATA_LEN_MAX);
    if (nm != nv) { puts("mask must have as many bytes as the value"); return; }
  }
  id = core.theodore_dbg_add_data_breakpoint(access, (uint16_t) first, (uint16_t) last, value, nm ? mask : NULL, (unsigned) nv);
  if (id < 0) puts("cannot add the data breakpoint (bad arguments or table full)");
  else printf("data breakpoint #%d\n", id);
}

static void cmd_bpl(void)
{
  TheodoreBreakpoint bps[64];
  TheodoreDataBreakpoint dbs[TDBG_DATA_BP_MAX];
  unsigned n = core.theodore_dbg_list_breakpoints(bps, 64), i, k;
  static const char *names[] = { "pc", "read", "write" };
  if (n > 64) n = 64;
  for (i = 0; i < n; i++) printf("  %-5s %04X\n", names[bps[i].type], bps[i].addr);
  n = core.theodore_dbg_list_data_breakpoints(dbs, TDBG_DATA_BP_MAX);
  for (i = 0; i < n && i < TDBG_DATA_BP_MAX; i++)
  {
    printf("  data #%d %s%s %04X-%04X value", dbs[i].id, (dbs[i].access & TDBG_ACCESS_READ) ? "r" : "", (dbs[i].access & TDBG_ACCESS_WRITE) ? "w" : "", dbs[i].first, dbs[i].last);
    for (k = 0; k < dbs[i].len; k++) printf(" %02X/%02X", dbs[i].value[k], dbs[i].mask[k]);
    printf("\n");
  }
}

static void cmd_trace(char **tok, int n)
{
  if (n < 2) { puts("usage: trace on|off|clear|dump [N]"); return; }
  if (!strcmp(tok[1], "on")) core.theodore_dbg_trace_enable(1);
  else if (!strcmp(tok[1], "off")) core.theodore_dbg_trace_enable(0);
  else if (!strcmp(tok[1], "clear")) core.theodore_dbg_trace_clear();
  else if (!strcmp(tok[1], "dump"))
  {
    unsigned want = n >= 3 ? (unsigned) atoi(tok[2]) : 20, got, i;
    TheodoreTraceEntry *buf;
    if (want == 0 || want > 8192) want = 20;
    buf = (TheodoreTraceEntry *) malloc(sizeof(*buf) * want);
    if (buf == NULL) return;
    got = core.theodore_dbg_trace_read(buf, want);
    for (i = 0; i < got; i++)
    {
      char line[160];
      core.theodore_dbg_disasm(buf[i].regs.pc, line, sizeof(line));
      printf("%10llu  A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X CC=%02X  %s\n", (unsigned long long) buf[i].cycle,
             buf[i].regs.a, buf[i].regs.b, buf[i].regs.x, buf[i].regs.y, buf[i].regs.u, buf[i].regs.s, buf[i].regs.cc, line);
    }
    free(buf);
  }
  else puts("usage: trace on|off|clear|dump [N]");
}

static void cmd_pia(char **tok, int n)
{
  ThPiaState st;
  int id = core.theodore_slot_pia();
  if (id < 0) { puts("no PIA: attach the joystick slot (--bridge-joy)"); return; }
  if (n >= 4 && !strcmp(tok[1], "pins"))
  {
    unsigned v;
    if (parse_hex(tok[3], &v) != 0 || v > 255) { puts("bad value"); return; }
    core.theodore_pia_set_pins(id, (tok[2][0] == 'B' || tok[2][0] == 'b') ? THPIA_PORT_B : THPIA_PORT_A, (uint8_t) v);
    return;
  }
  if (n >= 4 && !strcmp(tok[1], "line"))
  {
    ThPiaLine line;
    if (!strcmp(tok[2], "ca1")) line = THPIA_CA1;
    else if (!strcmp(tok[2], "ca2")) line = THPIA_CA2;
    else if (!strcmp(tok[2], "cb1")) line = THPIA_CB1;
    else if (!strcmp(tok[2], "cb2")) line = THPIA_CB2;
    else { puts("line must be ca1, ca2, cb1 or cb2"); return; }
    core.theodore_pia_set_line(id, line, atoi(tok[3]));
    return;
  }
  core.theodore_pia_get_state(id, &st);
  printf("PIA A: ORA=%02X DDRA=%02X CRA=%02X pins=%02X CA1=%d CA2=%d IRQA=%d\n", st.ora, st.ddra, st.cra, st.pins_a, st.ca1, st.ca2, st.irqa);
  printf("PIA B: ORB=%02X DDRB=%02X CRB=%02X pins=%02X CB1=%d CB2=%d IRQB=%d\n", st.orb, st.ddrb, st.crb, st.pins_b, st.cb1, st.cb2, st.irqb);
}

static void cmd_step(char **tok, int n)
{
  int count = n >= 2 ? atoi(tok[1]) : 1, i;
  if (count < 1) count = 1;
  if (!core.theodore_dbg_is_halted()) core.theodore_dbg_halt();
  for (i = 0; i < count; i++)
  {
    TheodoreRegs r;
    char line[160];
    core.theodore_dbg_get_regs(&r);
    core.theodore_dbg_disasm(r.pc, line, sizeof(line));
    if (core.theodore_dbg_step() != 0) { puts("cannot step"); return; }
    if (count <= 16) printf("  %s\n", line);
  }
  print_regs();
  print_next_instruction();
  reported_stops = core.theodore_dbg_stop_count(); /* `step` shows its own state */
}

void dbg_exec(const char *input)
{
  char line[LINE_MAX_LEN], *tok[MAXTOK];
  int n;
  snprintf(line, sizeof(line), "%s", input);
  {
    size_t l = strlen(line);
    while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
  }
  /* commands whose argument is the rest of the line */
  if (!strncmp(line, "type ", 5)) { int u = app_type(line + 5); if (u) printf("%d character(s) have no key on this model\n", u); return; }
  if (!strncmp(line, "typefile ", 9)) { app_type_file(line + 9); return; }
  if (!strncmp(line, "find ", 5)) { cmd_find(line + 5); return; }
  if (!strcmp(line, "find")) { cmd_find(""); return; }
  if (!strncmp(line, "echo ", 5) || !strcmp(line, "echo")) { printf("%s\n", line[4] ? line + 5 : ""); return; }
  n = tokenize(line, tok);
  if (n < 0) { printf("too many arguments (max %d per command)\n", MAXTOK - 1); return; }
  if (n == 0 || tok[0][0] == '#') return;
  if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) cmd_help();
  else if (!strcmp(tok[0], "go") || !strcmp(tok[0], "c")) core.theodore_dbg_resume();
  else if (!strcmp(tok[0], "halt") || !strcmp(tok[0], "b")) core.theodore_dbg_halt();
  else if (!strcmp(tok[0], "step") || !strcmp(tok[0], "s")) cmd_step(tok, n);
  else if (!strcmp(tok[0], "regs") || !strcmp(tok[0], "r")) { print_regs(); print_next_instruction(); }
  else if (!strcmp(tok[0], "set")) cmd_regs_set(tok, n);
  else if (!strcmp(tok[0], "mem") || !strcmp(tok[0], "m")) cmd_mem(tok, n);
  else if (!strcmp(tok[0], "poke"))
  {
    unsigned addr, v;
    int i, bad = 0;
    if (n < 3 || parse_hex(tok[1], &addr) != 0) { puts("usage: poke ADDR V [V..]"); return; }
    for (i = 2; i < n; i++)
    {
      if (parse_hex(tok[i], &v) != 0 || v > 255 || core.theodore_dbg_poke((uint16_t) (addr + i - 2), (uint8_t) v) != 0) bad++;
    }
    if (bad) printf("%d byte(s) not written (only RAM 0000-9FFF is writable)\n", bad);
  }
  else if (!strcmp(tok[0], "dis") || !strcmp(tok[0], "d")) cmd_dis(tok, n);
  else if (!strcmp(tok[0], "bp")) cmd_bp(tok, n, 0);
  else if (!strcmp(tok[0], "bpdel")) cmd_bp(tok, n, 1);
  else if (!strcmp(tok[0], "bpd")) cmd_bpd(tok, n);
  else if (!strcmp(tok[0], "bpddel"))
  {
    if (n < 2) puts("usage: bpddel ID");
    else printf("%s\n", core.theodore_dbg_del_data_breakpoint(atoi(tok[1])) == 0 ? "removed" : "no such data breakpoint");
  }
  else if (!strcmp(tok[0], "bpl")) cmd_bpl();
  else if (!strcmp(tok[0], "bpclear")) { core.theodore_dbg_clear_breakpoints(); puts("cleared"); }
  else if (!strcmp(tok[0], "trace")) cmd_trace(tok, n);
  else if (!strcmp(tok[0], "slots")) bridge_print_status();
  else if (!strcmp(tok[0], "paste")) app_paste();
  else if (!strcmp(tok[0], "press"))
  {
    unsigned code;
    int mods = 0;
    const char *f = n >= 3 ? tok[2] : "";
    for (; *f; f++) mods |= *f == 's' ? 1 : *f == 'b' ? 2 : *f == 'a' ? 4 : *f == 'c' ? 8 : 0;
    if (n < 2 || parse_dec(tok[1], &code) != 0) puts("usage: press KEYCODE [FLAGS]   (libretro key code in decimal; flags: s Shift, b BASIC key, a RAZ, c CNT)");
    else if (app_press(code, mods) != 0) printf("press %u: no Thomson key\n", code);
  }
  else if (!strcmp(tok[0], "scan"))
  {
    unsigned code;
    int mods = 0;
    const char *f = n >= 3 ? tok[2] : "";
    for (; *f; f++) mods |= *f == 's' ? 1 : *f == 'b' ? 2 : *f == 'a' ? 4 : *f == 'c' ? 8 : 0;
    if (n < 2 || parse_hex(tok[1], &code) != 0 || app_press_scancode(code, mods) != 0) puts("usage: scan SCANCODE [sbac]   (Thomson key matrix position, hex 00-4F)");
  }
  else if (!strcmp(tok[0], "typestop")) { app_type_stop(); puts("typing stopped"); }
  else if (!strcmp(tok[0], "typespeed"))
  {
    if (n < 3 || app_type_speed(atoi(tok[1]), atoi(tok[2]), n >= 4 ? atoi(tok[3]) : 0) != 0) puts("usage: typespeed HOLD GAP [AFTER_RETURN]  (frames; default 3 1 30)");
  }
  else if (!strcmp(tok[0], "irq")) printf("IRQ source mask: %08X\n", core.theodore_periph_irq_mask());
  else if (!strcmp(tok[0], "pia")) cmd_pia(tok, n);
  else if (!strcmp(tok[0], "shot"))
  {
    const char *f = n >= 2 ? tok[1] : "shot.bmp";
    printf("screenshot %s: %s\n", f, app_screenshot(f) == 0 ? "ok" : "failed");
  }
  else if (!strcmp(tok[0], "load")) { if (n < 2) puts("usage: load FILE"); else if (app_load_content(tok[1]) == 0) puts("loaded"); }
  else if (!strcmp(tok[0], "ss")) printf("%s\n", n >= 2 && app_save_state(tok[1]) == 0 ? "state saved" : "failed");
  else if (!strcmp(tok[0], "ls")) printf("%s\n", n >= 2 && app_load_state(tok[1]) == 0 ? "state loaded" : "failed");
  else if (!strcmp(tok[0], "pad"))
  {
    if (n < 4 || app_set_pad(atoi(tok[1]), tok[2], atoi(tok[3])) != 0) puts("usage: pad 0|1 up|down|left|right|fire|start|select 0|1");
  }
  else if (!strcmp(tok[0], "reset")) { app_reset(); puts("reset"); }
  else if (!strcmp(tok[0], "model")) { if (n < 2) printf("model %s\n", app_model()); else app_set_var("theodore_rom", tok[1]); /* the core resets the machine when it applies it */ }
  else if (!strcmp(tok[0], "var")) { if (n < 3) puts("usage: var KEY VALUE"); else app_set_var(tok[1], tok[2]); }
  else if (!strcmp(tok[0], "wait")) wait_frames = n >= 2 ? atoi(tok[1]) : 1;
  else if (!strcmp(tok[0], "waitstop")) { wait_stop = n >= 2 ? atoi(tok[1]) : 3000; wait_stop_active = 1; }
  else if (!strcmp(tok[0], "quit") || !strcmp(tok[0], "q") || !strcmp(tok[0], "exit")) app_quit();
  else printf("unknown command '%s' (help)\n", tok[0]);
  fflush(stdout);
}

/* ----------------------------------------------------------------- console */
static void queue_line(const char *buf)
{
  SDL_LockMutex(qlock);
  if ((q_tail + 1) % QUEUE_LEN != q_head)
  {
    snprintf(queue[q_tail], LINE_MAX_LEN, "%s", buf);
    q_tail = (q_tail + 1) % QUEUE_LEN;
  }
  SDL_UnlockMutex(qlock);
}

static int console_thread(void *unused)
{
  char buf[LINE_MAX_LEN];
  (void) unused;
  while (fgets(buf, sizeof(buf), stdin) != NULL)
  {
    if (strchr(buf, '\n') == NULL && strlen(buf) == sizeof(buf) - 1)
    {
      int ch;
      while ((ch = fgetc(stdin)) != EOF && ch != '\n') {}
      printf("line too long (max %d characters): ignored\n", LINE_MAX_LEN - 1);
      fflush(stdout);
      continue;
    }
    queue_line(buf);
  }
  return 0;
}

/* ------------------------------------------------------------ remote console */
/* For hosts without a usable console (Android): the same commands over TCP.
   stdout is redirected into a pipe; a thread copies it to the original stdout (log/console)
   and to the connected client, so every printf of the debugger reaches the client unchanged. */
#ifdef _WIN32
int dbg_remote_start(const char *spec)
{
  (void) spec;
  printf("--debug-port is not available on Windows (the console works there)\n");
  return -1;
}
#else
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int remote_listen = -1, pipe_rd = -1, real_out = -1;
static volatile int remote_client = -1;

static int out_thread(void *unused)
{
  char b[1024];
  ssize_t n;
  (void) unused;
  while ((n = read(pipe_rd, b, sizeof(b))) > 0)
  {
    int c = remote_client;
    ssize_t off = 0;
    while (off < n) { ssize_t w = write(real_out, b + off, (size_t) (n - off)); if (w <= 0) break; off += w; }
    if (c >= 0) { if (send(c, b, (size_t) n, 0) < 0) { /* client gone: its reader notices */ } }
  }
  return 0;
}

static int remote_thread(void *unused)
{
  (void) unused;
  for (;;)
  {
    static const char banner[] = "theodore debugger (type 'help')\n";
    char line[LINE_MAX_LEN], ch;
    int len = 0, c = accept(remote_listen, NULL, NULL);
    if (c < 0) { if (errno == EINTR) continue; break; }
    remote_client = c;
    if (send(c, banner, sizeof(banner) - 1, 0) < 0) { /* ignore */ }
    while (recv(c, &ch, 1, 0) == 1)
    {
      if (ch == '\r') continue;
      if (ch == '\n' || len >= LINE_MAX_LEN - 2) { line[len++] = '\n'; line[len] = '\0'; queue_line(line); len = 0; }
      else line[len++] = ch;
    }
    remote_client = -1;
    close(c);
  }
  return 0;
}

/* spec: "PORT" (127.0.0.1 only) or "ADDR:PORT" (e.g. 0.0.0.0:5555 to reach it from a PC; no authentication) */
int dbg_remote_start(const char *spec)
{
  struct sockaddr_in a;
  char host[64] = "127.0.0.1";
  int fds[2], one = 1, port;
  const char *colon = strrchr(spec, ':');
  SDL_Thread *t;
  if (colon != NULL)
  {
    size_t n = (size_t) (colon - spec);
    if (n >= sizeof(host)) n = sizeof(host) - 1;
    memcpy(host, spec, n); host[n] = '\0';
    port = atoi(colon + 1);
  }
  else port = atoi(spec);
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short) port);
  if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &a.sin_addr) != 1)
  {
    fprintf(stderr, "--debug-port: bad address '%s' (use PORT or ADDR:PORT)\n", spec);
    return -1;
  }
  signal(SIGPIPE, SIG_IGN);
  remote_listen = socket(AF_INET, SOCK_STREAM, 0);
  if (remote_listen < 0) { perror("socket"); return -1; }
  setsockopt(remote_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(remote_listen, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(remote_listen, 1) != 0)
  {
    fprintf(stderr, "--debug-port %s: %s\n", spec, strerror(errno));
    close(remote_listen);
    return -1;
  }
  fflush(stdout);
  real_out = dup(1);
  if (real_out < 0 || pipe(fds) != 0) { perror("pipe"); return -1; }
  dup2(fds[1], 1);
  close(fds[1]);
  pipe_rd = fds[0];
  setvbuf(stdout, NULL, _IOLBF, 0);
  t = SDL_CreateThread(out_thread, "dbgout", NULL);
  if (t != NULL) SDL_DetachThread(t);
  t = SDL_CreateThread(remote_thread, "dbgnet", NULL);
  if (t != NULL) SDL_DetachThread(t);
  printf("debugger listening on %s:%d (one client at a time)\n", host, port);
  fflush(stdout);
  return 0;
}
#endif

void dbg_console_start(void)
{
  SDL_Thread *t = SDL_CreateThread(console_thread, "console", NULL);
  if (t != NULL) SDL_DetachThread(t);
  printf("debugger console ready (type 'help')\n");
  fflush(stdout);
}

int dbg_script_load(const char *path)
{
  FILE *f = fopen(path, "r");
  char buf[LINE_MAX_LEN];
  int cap = 64;
  if (f == NULL) { fprintf(stderr, "cannot open script %s\n", path); return -1; }
  script = (char (*)[LINE_MAX_LEN]) malloc((size_t) cap * LINE_MAX_LEN);
  while (fgets(buf, sizeof(buf), f) != NULL)
  {
    if (strchr(buf, '\n') == NULL && strlen(buf) == sizeof(buf) - 1)
    {
      int ch;
      while ((ch = fgetc(f)) != EOF && ch != '\n') {}
      fprintf(stderr, "script %s: line %d too long (max %d characters), ignored\n", path, script_len + 1, LINE_MAX_LEN - 1);
      snprintf(buf, sizeof(buf), "# (line too long)\n");
    }
    if (script_len == cap)
    {
      cap *= 2;
      script = (char (*)[LINE_MAX_LEN]) realloc(script, (size_t) cap * LINE_MAX_LEN);
    }
    snprintf(script[script_len++], LINE_MAX_LEN, "%s", buf);
  }
  fclose(f);
  return 0;
}

int dbg_busy(void)
{
  return script_pos < script_len || wait_frames > 0 || wait_stop_active || app_typing();
}

void dbg_poll(void)
{
  char line[LINE_MAX_LEN];
  int have;

  /* commands typed in the console are always executed */
  for (;;)
  {
    have = 0;
    SDL_LockMutex(qlock);
    if (q_head != q_tail)
    {
      snprintf(line, sizeof(line), "%s", queue[q_head]);
      q_head = (q_head + 1) % QUEUE_LEN;
      have = 1;
    }
    SDL_UnlockMutex(qlock);
    if (!have) break;
    dbg_exec(line);
  }

  /* script flow control */
  if (wait_frames > 0) { wait_frames--; return; }
  if (wait_stop_active)
  {
    if (core.theodore_dbg_is_halted() || --wait_stop <= 0)
    {
      if (!core.theodore_dbg_is_halted()) printf("waitstop: timeout, still running\n");
      wait_stop_active = 0;
    }
    else return;
  }
  if (app_typing()) return; /* let typed text finish before the next script command */
  while (script_pos < script_len && wait_frames == 0 && !wait_stop_active)
  {
    dbg_exec(script[script_pos++]);
    if (app_typing()) break;
  }
}
