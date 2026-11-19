#ifndef __NETDEBUG_CONTROL_H__
#define __NETDEBUG_CONTROL_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Settings-side control for the read-only netdebug server. The server binary
 * lives in $SYSTEM_PATH/bin/netdebug.elf and is only spawned while the
 * "Network debugging" toggle is on. */
#define NETDEBUG_DEFAULT_PORT 8686
#define NETDEBUG_DEFAULT_TIMEOUT_SECS 900
#define NETDEBUG_TOKEN_MAX 65

/* Enable the debug session: generate a token, persist the enabled flag,
 * expiry and token, then spawn the detached server. Returns 0 on success. */
int NETDEBUG_enable(void);

/* Stop the server immediately (SIGTERM, then SIGKILL after a short grace) and
 * clear the persisted enabled flag. Safe when nothing is running. */
int NETDEBUG_disable(void);

/* True when the enabled flag, the state file and the live process all agree. */
int NETDEBUG_isRunning(void);

/* Copy the URL the server reported (including the token). Returns 0 when the
 * server is running and a URL is available. */
int NETDEBUG_getUrl(char* out, size_t out_size);

/* Seconds left before the server's own timeout, or -1 when it is not running. */
int NETDEBUG_remainingSeconds(void);

/* Disable and reset to the defaults (the Settings "Reset to defaults" action). */
void NETDEBUG_reset(void);

#ifdef __cplusplus
}
#endif

#endif
