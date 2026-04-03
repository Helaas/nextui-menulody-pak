#include "monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>

static pid_t cached_pid = 0;

static pid_t find_process(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;

    struct dirent *ent;
    char path[300], comm[64];

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;

        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        comm[0] = '\0';
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, name) == 0) {
                pid_t pid = (pid_t)atoi(ent->d_name);
                fclose(f);
                closedir(d);
                return pid;
            }
        }
        fclose(f);
    }
    closedir(d);
    return 0;
}

int monitor_is_menu_active(void) {
#ifdef PLATFORM_MAC
    /* On macOS, always return true (no /proc) */
    return 1;
#else
    /* Fast path: check cached PID first */
    if (cached_pid > 0) {
        if (kill(cached_pid, 0) == 0) {
            char path[300], comm[64];
            snprintf(path, sizeof(path), "/proc/%d/comm", (int)cached_pid);
            FILE *f = fopen(path, "r");
            if (f) {
                comm[0] = '\0';
                if (fgets(comm, sizeof(comm), f)) {
                    comm[strcspn(comm, "\n")] = '\0';
                    fclose(f);
                    if (strcmp(comm, "nextui.elf") == 0)
                        return 1;
                } else {
                    fclose(f);
                }
            }
        }
        cached_pid = 0;
    }

    /* Full scan */
    cached_pid = find_process("nextui.elf");
    return (cached_pid > 0) ? 1 : 0;
#endif
}
