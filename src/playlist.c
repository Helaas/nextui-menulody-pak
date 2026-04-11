#include "playlist.h"
#include "cJSON.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static void playlist_reshuffle(playlist_t *pl);

static int str_ends_with_ci(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return 0;
    for (size_t i = 0; i < xlen; i++) {
        if (tolower((unsigned char)s[slen - xlen + i]) !=
            tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

static int is_music_file(const char *name) {
    return str_ends_with_ci(name, ".mp3") || str_ends_with_ci(name, ".wav");
}

static int cmp_str(const void *a, const void *b) {
    return strcasecmp(*(const char **)a, *(const char **)b);
}

static char *strip_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
    char *name = malloc(len + 1);
    if (name) { memcpy(name, filename, len); name[len] = '\0'; }
    return name;
}

static void shuffle_order(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

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

/* ── Recursive directory scan ──────────────────────────────────── */

static int scan_dir_recursive(const char *dir, char ***paths_out, int *count, int *capacity) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[CONFIG_MAX_PATH];
        if (path_join(path, sizeof(path), dir, ent->d_name) != 0)
            continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(path, paths_out, count, capacity);
            continue;
        }

        if (!S_ISREG(st.st_mode)) continue;
        if (!is_music_file(ent->d_name)) continue;

        if (*count >= *capacity) {
            *capacity = (*capacity == 0) ? 64 : *capacity * 2;
            char **new_paths = realloc(*paths_out, *capacity * sizeof(char *));
            if (!new_paths) { closedir(d); return -1; }
            *paths_out = new_paths;
        }

        (*paths_out)[*count] = strdup(path);
        if ((*paths_out)[*count]) (*count)++;
    }

    closedir(d);
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────── */

int playlist_scan(playlist_t *pl, const config_t *cfg) {
    memset(pl, 0, sizeof(*pl));
    srand((unsigned)time(NULL));
    snprintf(pl->name, sizeof(pl->name), "All Songs");

    char **paths = NULL;
    int count = 0, capacity = 0;

    for (int i = 0; i < cfg->music_dir_count; i++) {
        scan_dir_recursive(cfg->music_dirs[i], &paths, &count, &capacity);
    }

    if (count == 0) {
        free(paths);
        fprintf(stderr, "menulody: no music files found\n");
        return -1;
    }

    /* Sort by full path (case-insensitive) */
    qsort(paths, count, sizeof(char *), cmp_str);

    /* Build names array */
    char **names = calloc(count, sizeof(char *));
    int *order = calloc(count, sizeof(int));
    if (!names || !order) {
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths); free(names); free(order);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const char *slash = strrchr(paths[i], '/');
        names[i] = strip_extension(slash ? slash + 1 : paths[i]);
        order[i] = i;
    }

    pl->paths = paths;
    pl->names = names;
    pl->order = order;
    pl->count = count;
    pl->position = 0;
    pl->shuffle = cfg->shuffle ? 1 : 0;
    pl->repeat = (int)cfg->repeat;

    if (pl->shuffle) playlist_reshuffle(pl);

    return 0;
}

void playlist_free(playlist_t *pl) {
    for (int i = 0; i < pl->count; i++) {
        free(pl->paths[i]);
        free(pl->names[i]);
    }
    free(pl->paths);
    free(pl->names);
    free(pl->order);
    memset(pl, 0, sizeof(*pl));
}

int playlist_next(playlist_t *pl) {
    if (pl->count == 0) return -1;

    if (pl->repeat == REPEAT_ONE) {
        /* Stay on same track */
        return pl->order[pl->position];
    }

    int next_pos = pl->position + 1;
    if (next_pos >= pl->count) {
        if (pl->repeat == REPEAT_ALL) {
            next_pos = 0;
            if (pl->shuffle) playlist_reshuffle(pl);
        } else {
            return -1; /* end of playlist */
        }
    }

    pl->position = next_pos;
    return pl->order[pl->position];
}

int playlist_prev(playlist_t *pl) {
    if (pl->count == 0) return -1;
    pl->position = (pl->position - 1 + pl->count) % pl->count;
    return pl->order[pl->position];
}

int playlist_current_index(const playlist_t *pl) {
    if (pl->count == 0) return -1;
    return pl->order[pl->position];
}

const char *playlist_current_path(const playlist_t *pl) {
    int idx = playlist_current_index(pl);
    if (idx < 0) return NULL;
    return pl->paths[idx];
}

const char *playlist_current_name(const playlist_t *pl) {
    int idx = playlist_current_index(pl);
    if (idx < 0) return NULL;
    return pl->names[idx];
}

void playlist_select(playlist_t *pl, int index) {
    if (index < 0 || index >= pl->count) return;
    for (int i = 0; i < pl->count; i++) {
        if (pl->order[i] == index) {
            pl->position = i;
            return;
        }
    }
}

void playlist_shuffle_toggle(playlist_t *pl) {
    pl->shuffle = !pl->shuffle;
    if (pl->shuffle) {
        playlist_reshuffle(pl);
    } else {
        int cur = playlist_current_index(pl);
        for (int i = 0; i < pl->count; i++)
            pl->order[i] = i;
        pl->position = (cur >= 0) ? cur : 0;
    }
}

static void playlist_reshuffle(playlist_t *pl) {
    if (pl->count <= 1) return;
    int cur = playlist_current_index(pl);
    shuffle_order(pl->order, pl->count);

    /* Move current track to position 0 so it doesn't restart */
    for (int i = 0; i < pl->count; i++) {
        if (pl->order[i] == cur) {
            int tmp = pl->order[0];
            pl->order[0] = pl->order[i];
            pl->order[i] = tmp;
            break;
        }
    }
    pl->position = 0;
}

void playlist_repeat_cycle(playlist_t *pl) {
    pl->repeat = (pl->repeat + 1) % 3;
}

/* ── Named playlist CRUD ───────────────────────────────────────── */

static void get_playlist_path(const char *name, char *out, int size) {
    char dir[CONFIG_MAX_PATH];
    config_get_playlists_dir(dir, sizeof(dir));
    if (size > 0) {
        if (path_join(out, (size_t)size, dir, name) != 0 ||
            str_append(out, (size_t)size, ".json") != 0) {
            out[0] = '\0';
        }
    }
}

int playlist_list_saved(char ***out_names, int *out_count) {
    char dir[CONFIG_MAX_PATH];
    config_get_playlists_dir(dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) {
        *out_names = NULL;
        *out_count = 0;
        return 0;
    }

    int count = 0, capacity = 16;
    char **names = calloc(capacity, sizeof(char *));
    if (!names) { closedir(d); return -1; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!str_ends_with_ci(ent->d_name, ".json")) continue;

        if (count >= capacity) {
            capacity *= 2;
            char **new_names = realloc(names, capacity * sizeof(char *));
            if (!new_names) break;
            names = new_names;
        }

        names[count] = strip_extension(ent->d_name);
        if (names[count]) count++;
    }

    closedir(d);
    qsort(names, count, sizeof(char *), cmp_str);

    *out_names = names;
    *out_count = count;
    return 0;
}

int playlist_named_load(const char *name, named_playlist_t *pl) {
    memset(pl, 0, sizeof(*pl));
    str_copy_trunc(pl->name, sizeof(pl->name), name);

    char path[CONFIG_MAX_PATH];
    get_playlist_path(name, path, sizeof(path));
    if (!path[0]) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 65536) { fclose(f); return -1; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, len, f);
    fclose(f);
    if (nread < (size_t)len) { free(buf); return -1; }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;

    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name_item))
        str_copy_trunc(pl->name, sizeof(pl->name), cJSON_GetStringValue(name_item));

    cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
    if (cJSON_IsArray(tracks)) {
        int count = cJSON_GetArraySize(tracks);
        pl->tracks = calloc(count, sizeof(char *));
        if (pl->tracks) {
            cJSON *item;
            cJSON_ArrayForEach(item, tracks) {
                if (cJSON_IsString(item)) {
                    pl->tracks[pl->count] = strdup(cJSON_GetStringValue(item));
                    if (pl->tracks[pl->count]) pl->count++;
                }
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

int playlist_named_save(const named_playlist_t *pl) {
    char dir[CONFIG_MAX_PATH];
    config_get_playlists_dir(dir, sizeof(dir));
    mkdirp(dir);

    char path[CONFIG_MAX_PATH];
    get_playlist_path(pl->name, path, sizeof(path));
    if (!path[0]) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddStringToObject(root, "name", pl->name);
    cJSON *tracks = cJSON_CreateArray();
    for (int i = 0; i < pl->count; i++)
        cJSON_AddItemToArray(tracks, cJSON_CreateString(pl->tracks[i]));
    cJSON_AddItemToObject(root, "tracks", tracks);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    /* Atomic write */
    char tmp_path[CONFIG_MAX_PATH];
    str_copy_trunc(tmp_path, sizeof(tmp_path), path);
    if (str_append(tmp_path, sizeof(tmp_path), ".tmp") != 0) {
        free(json);
        return -1;
    }
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(json); return -1; }
    fputs(json, f);
    fclose(f);
    free(json);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int playlist_named_delete(const char *name) {
    char path[CONFIG_MAX_PATH];
    get_playlist_path(name, path, sizeof(path));
    return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
}

void playlist_named_free(named_playlist_t *pl) {
    for (int i = 0; i < pl->count; i++)
        free(pl->tracks[i]);
    free(pl->tracks);
    memset(pl, 0, sizeof(*pl));
}

int playlist_load_named(playlist_t *pl, const char *name) {
    named_playlist_t named;
    if (playlist_named_load(name, &named) < 0) return -1;

    playlist_free(pl);
    memset(pl, 0, sizeof(*pl));
    str_copy_trunc(pl->name, sizeof(pl->name), named.name);

    if (named.count == 0) {
        playlist_named_free(&named);
        return -1;
    }

    pl->paths = calloc(named.count, sizeof(char *));
    pl->names = calloc(named.count, sizeof(char *));
    pl->order = calloc(named.count, sizeof(int));
    if (!pl->paths || !pl->names || !pl->order) {
        playlist_named_free(&named);
        return -1;
    }

    /* Only include tracks that still exist on disk */
    for (int i = 0; i < named.count; i++) {
        struct stat st;
        if (stat(named.tracks[i], &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        pl->paths[pl->count] = strdup(named.tracks[i]);
        const char *slash = strrchr(named.tracks[i], '/');
        pl->names[pl->count] = strip_extension(slash ? slash + 1 : named.tracks[i]);
        pl->order[pl->count] = pl->count;
        pl->count++;
    }

    playlist_named_free(&named);

    if (pl->count == 0) {
        playlist_free(pl);
        return -1;
    }

    return 0;
}

int playlist_load_single_track(playlist_t *pl, const char *path) {
    struct stat st;
    const char *slash;
    const char *filename;

    if (!path || !path[0]) return -1;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    slash = strrchr(path, '/');
    filename = slash ? slash + 1 : path;
    if (!is_music_file(filename)) return -1;

    playlist_free(pl);
    memset(pl, 0, sizeof(*pl));

    pl->paths = calloc(1, sizeof(char *));
    pl->names = calloc(1, sizeof(char *));
    pl->order = calloc(1, sizeof(int));
    if (!pl->paths || !pl->names || !pl->order) {
        playlist_free(pl);
        return -1;
    }

    pl->paths[0] = strdup(path);
    pl->names[0] = strip_extension(filename);
    if (!pl->paths[0] || !pl->names[0]) {
        playlist_free(pl);
        return -1;
    }

    pl->order[0] = 0;
    pl->count = 1;
    pl->position = 0;
    pl->shuffle = 0;
    pl->repeat = REPEAT_ONE;
    str_copy_trunc(pl->name, sizeof(pl->name), "Single Track");
    return 0;
}
