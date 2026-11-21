/* TCP lock-step bridge between the emulated peripheral bus and real (or simulated) hardware.
 * Protocol: see BRIDGE_PROTOCOL.md. */
#ifndef FE_BRIDGE_H
#define FE_BRIDGE_H

#include <stddef.h>

/* spec = "KIND|HOST:PORT@BASE+SIZE[,opt...]" with KIND exp, cart or joy (joy has no @BASE+SIZE).
   Returns 0, or -1 with a message in err. */
int  bridge_attach_spec(const char *spec, char *err, size_t err_size);
void bridge_print_status(void);
void bridge_shutdown(void);

#endif /* FE_BRIDGE_H */
