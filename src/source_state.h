#ifndef MENULODY_SOURCE_STATE_H
#define MENULODY_SOURCE_STATE_H

#include "config.h"
#include "playlist.h"

typedef enum {
    SOURCE_MODE_NONE = 0,
    SOURCE_MODE_ALL_SONGS,
    SOURCE_MODE_NAMED_PLAYLIST,
    SOURCE_MODE_SINGLE_TRACK,
} source_mode_t;

typedef struct {
    int menu_music_enabled;
    source_mode_t mode;
    char playlist_name[PLAYLIST_NAME_MAX];
    char track_path[CONFIG_MAX_PATH];
} source_state_t;

void source_state_default(source_state_t *state);
void source_state_set_none(source_state_t *state);
void source_state_set_all_songs(source_state_t *state);
void source_state_set_named_playlist(source_state_t *state, const char *name);
void source_state_set_single_track(source_state_t *state, const char *path);
int source_state_load(source_state_t *state);
int source_state_save(const source_state_t *state);

#endif /* MENULODY_SOURCE_STATE_H */
