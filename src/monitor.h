#ifndef MENULODY_MONITOR_H
#define MENULODY_MONITOR_H

/*
 * Process monitor — detects whether the NextUI menu (nextui.elf) is active
 * or whether a game/emulator is running instead.
 */

/* Returns 1 if NextUI menu is currently the foreground (nextui.elf running) */
int monitor_is_menu_active(void);

#endif /* MENULODY_MONITOR_H */
