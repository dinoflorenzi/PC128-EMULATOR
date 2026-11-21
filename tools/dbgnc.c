/* Minimal terminal client for the frontend's TCP debugger (--debug-port): a tiny netcat.
 *   cc dbgnc.c -o dbgnc -lpthread        usage: ./dbgnc [HOST] [PORT]   (default 127.0.0.1 5555)
 * Type debugger commands, Ctrl-D (or `exit` in your shell) to leave; `quit` closes the emulator. */
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *reader(void *arg)
{
  int fd = *(int *) arg;
  char b[1024];
  ssize_t n;
  while ((n = recv(fd, b, sizeof(b), 0)) > 0) { fwrite(b, 1, (size_t) n, stdout); fflush(stdout); }
  fprintf(stderr, "\n[connection closed]\n");
  exit(0);
  return NULL;
}

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "127.0.0.1", *port = argc > 2 ? argv[2] : "5555";
  struct addrinfo hints, *res;
  int fd;
  char line[1024];
  pthread_t t;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &res) != 0) { fprintf(stderr, "cannot resolve %s\n", host); return 1; }
  fd = socket(res->ai_family, res->ai_socktype, 0);
  if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) { perror("connect"); return 1; }
  freeaddrinfo(res);
  pthread_create(&t, NULL, reader, &fd);
  while (fgets(line, sizeof(line), stdin) != NULL) if (send(fd, line, strlen(line), 0) < 0) break;
  return 0;
}
