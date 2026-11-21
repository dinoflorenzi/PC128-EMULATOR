/* Text debugger for the frontend: console thread and script runner. */
#ifndef FE_DBGCLI_H
#define FE_DBGCLI_H

void dbg_init(void);
/* Starts a thread that reads commands from stdin. */
void dbg_console_start(void);
/* Remote console over TCP: spec "PORT" (localhost only) or "ADDR:PORT". Redirects stdout to the client
   (and still to the original stdout). Not available on Windows. Returns 0 or -1. */
int  dbg_remote_start(const char *spec);
/* Loads a command file executed one command per step (see `wait`, `waitstop`). Returns 0 or -1. */
int  dbg_script_load(const char *path);
/* Called once per frame, before the core runs: executes queued commands. */
void dbg_poll(void);
/* Called once per frame after the core ran: if the emulation stopped since the
   last report, prints why and where. */
void dbg_check_stop(void);
/* Executes one command line. */
void dbg_exec(const char *line);
/* Nonzero while a script or a `wait` still has something to do. */
int  dbg_busy(void);

#endif /* FE_DBGCLI_H */
