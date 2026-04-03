#ifndef MENULODY_OVERLAY_H
#define MENULODY_OVERLAY_H

/*
 * Framebuffer overlay — shows a centered pill with now-playing info
 * directly on /dev/fb0. Periodically redraws to fight the GL compositor.
 */

/* Initialize framebuffer overlay. Returns 0 on success, -1 on failure (non-fatal). */
int  overlay_init(void);

/* Set the now-playing text. Duration in seconds. */
void overlay_set_text(const char *text, int duration_secs);

/* Tick function — call every ~100ms. Redraws pill if active and menu is showing. */
void overlay_tick(int menu_active);

/* Cleanup */
void overlay_cleanup(void);

#endif /* MENULODY_OVERLAY_H */
