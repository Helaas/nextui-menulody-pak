#ifndef MENULODY_OVERLAY_H
#define MENULODY_OVERLAY_H

/*
 * Framebuffer overlay — shows a top-centered now-playing pill directly on
 * /dev/fb0 and only repaints when another writer overwrites its rect.
 */

/* Initialize framebuffer overlay. Returns 0 on success, -1 on failure (non-fatal). */
int  overlay_init(void);

/* Set the now-playing text. Duration in seconds. */
void overlay_set_text(const char *text, int duration_secs);

/* Tick function — call every ~100ms while the daemon runs. */
void overlay_tick(int menu_active);

/* Cleanup */
void overlay_cleanup(void);

#endif /* MENULODY_OVERLAY_H */
