#ifndef MENULODY_HOOKS_H
#define MENULODY_HOOKS_H

#include <stdbool.h>

/* Install/uninstall Menulody hook scripts into NextUI's .hooks/ directories */

/* Install pre-launch and post-launch hooks (always needed when daemon is active) */
int hooks_install_playback(void);

/* Uninstall pre-launch and post-launch hooks */
int hooks_uninstall_playback(void);

/* Install boot.d hook for auto-start */
int hooks_install_autostart(void);

/* Uninstall boot.d hook */
int hooks_uninstall_autostart(void);

/* Check if hooks are installed */
bool hooks_playback_installed(void);
bool hooks_autostart_installed(void);

/* Apply hook state based on config */
int hooks_apply_config(bool auto_start);

#endif /* MENULODY_HOOKS_H */
