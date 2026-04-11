#include "source_state.h"
#include "cJSON.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mkdirp(const char *path) {
    char tmp[CONFIG_MAX_PATH];

    str_copy_trunc(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int get_source_state_path(char *out, size_t size) {
    char data_dir[CONFIG_MAX_PATH];

    config_get_data_dir(data_dir, sizeof(data_dir));
    if (!data_dir[0]) {
        if (size > 0) out[0] = '\0';
        return -1;
    }

    return path_join(out, size, data_dir, "source_state.json");
}

static const char *source_mode_to_string(source_mode_t mode) {
    switch (mode) {
        case SOURCE_MODE_ALL_SONGS:
            return "all";
        case SOURCE_MODE_NAMED_PLAYLIST:
            return "playlist";
        case SOURCE_MODE_SINGLE_TRACK:
            return "single";
        case SOURCE_MODE_NONE:
        default:
            return "none";
    }
}

static source_mode_t source_mode_from_string(const char *mode) {
    if (!mode || !mode[0]) return SOURCE_MODE_NONE;
    if (strcmp(mode, "all") == 0) return SOURCE_MODE_ALL_SONGS;
    if (strcmp(mode, "playlist") == 0) return SOURCE_MODE_NAMED_PLAYLIST;
    if (strcmp(mode, "single") == 0) return SOURCE_MODE_SINGLE_TRACK;
    return SOURCE_MODE_NONE;
}

void source_state_default(source_state_t *state) {
    if (!state) return;

    memset(state, 0, sizeof(*state));
    state->menu_music_enabled = 1;
    state->mode = SOURCE_MODE_ALL_SONGS;
}

void source_state_set_none(source_state_t *state) {
    if (!state) return;

    state->mode = SOURCE_MODE_NONE;
    state->menu_music_enabled = 0;
    state->playlist_name[0] = '\0';
    state->track_path[0] = '\0';
}

void source_state_set_all_songs(source_state_t *state) {
    if (!state) return;

    state->mode = SOURCE_MODE_ALL_SONGS;
    state->playlist_name[0] = '\0';
    state->track_path[0] = '\0';
}

void source_state_set_named_playlist(source_state_t *state, const char *name) {
    if (!state) return;

    state->mode = SOURCE_MODE_NAMED_PLAYLIST;
    str_copy_trunc(state->playlist_name, sizeof(state->playlist_name), name);
    state->track_path[0] = '\0';
}

void source_state_set_single_track(source_state_t *state, const char *path) {
    if (!state) return;

    state->mode = SOURCE_MODE_SINGLE_TRACK;
    state->playlist_name[0] = '\0';
    str_copy_trunc(state->track_path, sizeof(state->track_path), path);
}

int source_state_load(source_state_t *state) {
    char path[CONFIG_MAX_PATH];
    FILE *f;
    char *buf;
    long len;
    cJSON *root;
    cJSON *item;

    source_state_default(state);
    if (get_source_state_path(path, sizeof(path)) != 0) return -1;

    f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) {
        fclose(f);
        return -1;
    }

    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (nread < (size_t)len) { free(buf); return -1; }
    buf[len] = '\0';

    root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    item = cJSON_GetObjectItem(root, "menu_music_enabled");
    if (cJSON_IsBool(item))
        state->menu_music_enabled = cJSON_IsTrue(item) ? 1 : 0;
    else if (cJSON_IsNumber(item))
        state->menu_music_enabled = item->valueint ? 1 : 0;

    item = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(item))
        state->mode = source_mode_from_string(cJSON_GetStringValue(item));

    item = cJSON_GetObjectItem(root, "playlist_name");
    if (cJSON_IsString(item))
        str_copy_trunc(state->playlist_name, sizeof(state->playlist_name),
                       cJSON_GetStringValue(item));

    item = cJSON_GetObjectItem(root, "track_path");
    if (cJSON_IsString(item))
        str_copy_trunc(state->track_path, sizeof(state->track_path),
                       cJSON_GetStringValue(item));

    cJSON_Delete(root);
    return 0;
}

int source_state_save(const source_state_t *state) {
    char data_dir[CONFIG_MAX_PATH];
    char path[CONFIG_MAX_PATH];
    char tmp_path[CONFIG_MAX_PATH];
    cJSON *root;
    char *json;
    FILE *f;

    if (!state) return -1;

    config_get_data_dir(data_dir, sizeof(data_dir));
    if (!data_dir[0]) return -1;
    mkdirp(data_dir);

    if (get_source_state_path(path, sizeof(path)) != 0) return -1;

    root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddBoolToObject(root, "menu_music_enabled",
                          state->menu_music_enabled ? 1 : 0);
    cJSON_AddStringToObject(root, "mode", source_mode_to_string(state->mode));
    cJSON_AddStringToObject(root, "playlist_name", state->playlist_name);
    cJSON_AddStringToObject(root, "track_path", state->track_path);

    json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    str_copy_trunc(tmp_path, sizeof(tmp_path), path);
    if (str_append(tmp_path, sizeof(tmp_path), ".tmp") != 0) {
        free(json);
        return -1;
    }

    f = fopen(tmp_path, "w");
    if (!f) {
        free(json);
        return -1;
    }

    fputs(json, f);
    fclose(f);
    free(json);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}
