#ifndef MENULODY_DAEMON_H
#define MENULODY_DAEMON_H

/*
 * Background music daemon.
 *
 * Lifecycle:
 *   1. Daemonize (fork + setsid)
 *   2. Initialize player, playlist, IPC, overlay
 *   3. Main loop (~100ms tick):
 *      - Poll IPC commands from UI/hooks
 *      - State machine: IDLE/PLAYING/PAUSED_AUTO/PAUSED_MANUAL/PREVIEWING
 *      - Advance tracks on completion
 *      - Redraw overlay when active
 *   4. Cleanup on SIGTERM or QUIT command
 */

/* Entry point for daemon mode. Does not return on success. */
int daemon_run(void);

#endif /* MENULODY_DAEMON_H */
