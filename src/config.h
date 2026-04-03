#ifndef MENULODY_CONFIG_H
#define MENULODY_CONFIG_H

#include <stdbool.h>

#define CONFIG_MAX_MUSIC_DIRS 8
#define CONFIG_MAX_PATH 512

typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ONE,
    REPEAT_ALL,
} repeat_mode_t;

typedef struct {
    char   music_dirs[CONFIG_MAX_MUSIC_DIRS][CONFIG_MAX_PATH];
    int    music_dir_count;
    bool   shuffle;
    repeat_mode_t repeat;
    int    volume;            /* 0-100 */
    bool   pause_on_pak;
    bool   auto_start;
    int    overlay_duration;  /* seconds, 0=off */
} config_t;

/* Load settings from shared userdata. Returns defaults if file missing. */
config_t config_load(void);

/* Save settings to shared userdata. Returns 0 on success. */
int config_save(const config_t *cfg);

/* Get the shared data directory for Menulody */
void config_get_data_dir(char *out, int size);

/* Get the playlists directory */
void config_get_playlists_dir(char *out, int size);

/* Get default music directory */
const char *config_default_music_dir(void);

#endif /* MENULODY_CONFIG_H */
