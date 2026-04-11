#ifndef MENULODY_PLAYLIST_H
#define MENULODY_PLAYLIST_H

#include "config.h"
#include <stdbool.h>

#define PLAYLIST_NAME_MAX 128

typedef struct {
    char **paths;       /* full paths to music files */
    char **names;       /* display names (filename without extension) */
    int    count;       /* total number of tracks */
    int   *order;       /* playback order (indices into paths/names) */
    int    position;    /* current position in the order */
    int    shuffle;     /* shuffle mode active */
    int    repeat;      /* repeat mode: 0=off, 1=one, 2=all */
    char   name[PLAYLIST_NAME_MAX]; /* playlist name ("All Songs" for library) */
} playlist_t;

/* Scan all configured music directories for *.mp3 and *.wav files.
   Returns 0 on success, -1 if no tracks found. */
int  playlist_scan(playlist_t *pl, const config_t *cfg);

/* Free all allocated memory */
void playlist_free(playlist_t *pl);

/* Advance to next track. Returns index into paths/names, or -1 if end.
   Respects repeat mode. */
int  playlist_next(playlist_t *pl);

/* Go to previous track. Returns index into paths/names. */
int  playlist_prev(playlist_t *pl);

/* Current track index (into paths/names). */
int  playlist_current_index(const playlist_t *pl);

/* Get current track path */
const char *playlist_current_path(const playlist_t *pl);

/* Get current track display name */
const char *playlist_current_name(const playlist_t *pl);

/* Select a specific track by its index in the sorted list */
void playlist_select(playlist_t *pl, int index);

/* Toggle shuffle and reshuffle the order array */
void playlist_shuffle_toggle(playlist_t *pl);

/* Cycle repeat mode: off -> all -> one -> off */
void playlist_repeat_cycle(playlist_t *pl);

/* ── Named playlist CRUD (JSON files) ─────────────────────────── */

typedef struct {
    char   name[PLAYLIST_NAME_MAX];
    char **tracks;    /* array of full paths */
    int    count;
} named_playlist_t;

/* List saved playlists. Caller must free returned array and names. */
int  playlist_list_saved(char ***out_names, int *out_count);

/* Load a named playlist from JSON. Caller must call playlist_named_free(). */
int  playlist_named_load(const char *name, named_playlist_t *pl);

/* Save a named playlist to JSON. */
int  playlist_named_save(const named_playlist_t *pl);

/* Delete a named playlist. */
int  playlist_named_delete(const char *name);

/* Free a named_playlist_t */
void playlist_named_free(named_playlist_t *pl);

/* Load a named playlist into the active playlist_t for playback */
int  playlist_load_named(playlist_t *pl, const char *name);

/* Load a single track as a one-song looping playback source. */
int  playlist_load_single_track(playlist_t *pl, const char *path);

#endif /* MENULODY_PLAYLIST_H */
