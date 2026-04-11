#ifndef MENULODY_UI_H
#define MENULODY_UI_H

/*
 * Interactive UI — runs when the user opens the Menulody Tool pak.
 * Uses Apostrophe widgets for native NextUI look.
 * Communicates with the daemon via FIFO IPC.
 */

/* Start the daemon when it is not already running. */
int ui_start_daemon_if_needed(void);

/* Run the interactive UI. Returns 0 on clean exit. */
void run_app(void);

#endif /* MENULODY_UI_H */
