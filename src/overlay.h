#ifndef MENULODY_OVERLAY_H
#define MENULODY_OVERLAY_H

/*
 * Shared-memory overlay publisher — renders a bottom-centered now-playing pill
 * for the LD_PRELOAD SDL compositor to consume.
 */

/* Initialize overlay publishing. Returns 0 on success, -1 on failure (non-fatal). */
int  overlay_init(void);

/* Set the now-playing text. Duration in seconds. */
void overlay_set_text(const char *text, int duration_secs);

/* Tick function — call every ~100ms while the daemon runs. */
void overlay_tick(int menu_active);

/* Returns non-zero while the overlay is logically active. */
int  overlay_is_active(void);

/* Cleanup */
void overlay_cleanup(void);

#endif /* MENULODY_OVERLAY_H */
