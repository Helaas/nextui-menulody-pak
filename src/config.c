#include "config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

/* ── Path helpers ──────────────────────────────────────────────── */

static void get_shared_userdata(char *out, int size) {
    const char *p = getenv("SHARED_USERDATA_PATH");
    if (p) {
        snprintf(out, size, "%s", p);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd) sd = "/mnt/SDCARD";
    snprintf(out, size, "%s/.userdata/shared", sd);
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, size, "%s/.userdata/shared", home);
#endif
}

void config_get_data_dir(char *out, int size) {
    char shared[CONFIG_MAX_PATH];
    get_shared_userdata(shared, sizeof(shared));
    snprintf(out, size, "%s/Menulody", shared);
}

void config_get_playlists_dir(char *out, int size) {
    char data[CONFIG_MAX_PATH];
    config_get_data_dir(data, sizeof(data));
    snprintf(out, size, "%s/playlists", data);
}

const char *config_default_music_dir(void) {
#ifdef PLATFORM_MAC
    static char buf[CONFIG_MAX_PATH];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sizeof(buf), "%s/Music", home);
    return buf;
#else
    return "/mnt/SDCARD/Music";
#endif
}

static void mkdirp(const char *path) {
    char tmp[CONFIG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ── Load / Save ───────────────────────────────────────────────── */

static config_t default_config(void) {
    config_t cfg = {0};
    snprintf(cfg.music_dirs[0], CONFIG_MAX_PATH, "%s", config_default_music_dir());
    cfg.music_dir_count = 1;
    cfg.shuffle = false;
    cfg.repeat = REPEAT_OFF;
    cfg.volume = 80;
    cfg.pause_on_pak = true;
    cfg.auto_start = true;
    cfg.overlay_duration = 3;
    return cfg;
}

config_t config_load(void) {
    config_t cfg = default_config();

    char data_dir[CONFIG_MAX_PATH];
    config_get_data_dir(data_dir, sizeof(data_dir));
    char path[CONFIG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/settings.json", data_dir);

    FILE *f = fopen(path, "r");
    if (!f) return cfg;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) { fclose(f); return cfg; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return cfg; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return cfg;

    cJSON *dirs = cJSON_GetObjectItem(root, "music_dirs");
    if (cJSON_IsArray(dirs)) {
        cfg.music_dir_count = 0;
        cJSON *item;
        cJSON_ArrayForEach(item, dirs) {
            if (cJSON_IsString(item) && cfg.music_dir_count < CONFIG_MAX_MUSIC_DIRS) {
                snprintf(cfg.music_dirs[cfg.music_dir_count], CONFIG_MAX_PATH,
                         "%s", cJSON_GetStringValue(item));
                cfg.music_dir_count++;
            }
        }
    }

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "shuffle")) && cJSON_IsBool(v))
        cfg.shuffle = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "repeat"))) {
        if (cJSON_IsString(v)) {
            const char *s = cJSON_GetStringValue(v);
            if (strcmp(s, "one") == 0) cfg.repeat = REPEAT_ONE;
            else if (strcmp(s, "all") == 0) cfg.repeat = REPEAT_ALL;
            else cfg.repeat = REPEAT_OFF;
        } else if (cJSON_IsNumber(v)) {
            cfg.repeat = (repeat_mode_t)v->valueint;
        }
    }
    if ((v = cJSON_GetObjectItem(root, "volume")) && cJSON_IsNumber(v))
        cfg.volume = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "pause_on_pak")) && cJSON_IsBool(v))
        cfg.pause_on_pak = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "auto_start")) && cJSON_IsBool(v))
        cfg.auto_start = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "overlay_duration")) && cJSON_IsNumber(v))
        cfg.overlay_duration = v->valueint;

    cJSON_Delete(root);
    return cfg;
}

int config_save(const config_t *cfg) {
    char data_dir[CONFIG_MAX_PATH];
    config_get_data_dir(data_dir, sizeof(data_dir));
    mkdirp(data_dir);

    char path[CONFIG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/settings.json", data_dir);

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *dirs = cJSON_CreateArray();
    for (int i = 0; i < cfg->music_dir_count; i++)
        cJSON_AddItemToArray(dirs, cJSON_CreateString(cfg->music_dirs[i]));
    cJSON_AddItemToObject(root, "music_dirs", dirs);

    cJSON_AddBoolToObject(root, "shuffle", cfg->shuffle);

    const char *repeat_str = "off";
    if (cfg->repeat == REPEAT_ONE) repeat_str = "one";
    else if (cfg->repeat == REPEAT_ALL) repeat_str = "all";
    cJSON_AddStringToObject(root, "repeat", repeat_str);

    cJSON_AddNumberToObject(root, "volume", cfg->volume);
    cJSON_AddBoolToObject(root, "pause_on_pak", cfg->pause_on_pak);
    cJSON_AddBoolToObject(root, "auto_start", cfg->auto_start);
    cJSON_AddNumberToObject(root, "overlay_duration", cfg->overlay_duration);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    /* Atomic write: write to tmp then rename */
    char tmp_path[CONFIG_MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(json); return -1; }
    fputs(json, f);
    fclose(f);
    free(json);

    if (rename(tmp_path, path) != 0) {
        perror("menulody: config rename");
        unlink(tmp_path);
        return -1;
    }

    return 0;
}
