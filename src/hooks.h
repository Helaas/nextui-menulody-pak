#ifndef MENULODY_HOOKS_H
#define MENULODY_HOOKS_H

#include <stdbool.h>

/* Install pre-launch and post-launch hooks (always needed when daemon is active) */
int hooks_install_playback(void);

/* Ensure playback hooks exist and update auto-start hook state. */
int hooks_apply_config(bool auto_start);

#endif /* MENULODY_HOOKS_H */
