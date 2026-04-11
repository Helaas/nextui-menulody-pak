#include "source_state.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, msg) do { if (!(cond)) fail(msg, __LINE__); } while (0)

static void fail(const char *msg, int line) {
    fprintf(stderr, "source_state_tests:%d: %s\n", line, msg);
    exit(1);
}

static void join_path(char *out, size_t size, const char *a, const char *b) {
    int n = snprintf(out, size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= size)
        fail("path overflow", __LINE__);
}

int main(void) {
    char tmp_template[] = "/tmp/menulody-source-state-test-XXXXXX";
    char *tmp_root = mkdtemp(tmp_template);
    char shared_dir[PATH_MAX];
    char data_dir[PATH_MAX];
    char state_path[PATH_MAX];
    source_state_t state;
    source_state_t loaded;

    if (!tmp_root)
        fail("mkdtemp failed", __LINE__);

    join_path(shared_dir, sizeof(shared_dir), tmp_root, "shared");
    CHECK(setenv("SHARED_USERDATA_PATH", shared_dir, 1) == 0,
          "setenv SHARED_USERDATA_PATH failed");

    CHECK(source_state_load(&loaded) == 0, "default load failed");
    CHECK(loaded.menu_music_enabled == 1, "default menu music should be enabled");
    CHECK(loaded.mode == SOURCE_MODE_ALL_SONGS, "default mode should be all songs");

    source_state_default(&state);
    state.menu_music_enabled = 0;
    source_state_set_named_playlist(&state, "Road Trip");
    CHECK(source_state_save(&state) == 0, "save named playlist state failed");
    CHECK(source_state_load(&loaded) == 0, "reload named playlist state failed");
    CHECK(loaded.menu_music_enabled == 0, "named playlist enabled flag mismatch");
    CHECK(loaded.mode == SOURCE_MODE_NAMED_PLAYLIST, "named playlist mode mismatch");
    CHECK(strcmp(loaded.playlist_name, "Road Trip") == 0, "playlist name mismatch");
    CHECK(loaded.track_path[0] == '\0', "named playlist should not store track path");

    source_state_default(&state);
    source_state_set_single_track(&state, "/mnt/SDCARD/Music/Favorites/song.mp3");
    CHECK(source_state_save(&state) == 0, "save single-track state failed");
    CHECK(source_state_load(&loaded) == 0, "reload single-track state failed");
    CHECK(loaded.menu_music_enabled == 1, "single-track enabled flag mismatch");
    CHECK(loaded.mode == SOURCE_MODE_SINGLE_TRACK, "single-track mode mismatch");
    CHECK(strcmp(loaded.track_path, "/mnt/SDCARD/Music/Favorites/song.mp3") == 0,
          "single-track path mismatch");
    CHECK(loaded.playlist_name[0] == '\0', "single-track should not store playlist name");

    join_path(data_dir, sizeof(data_dir), shared_dir, "Menulody");
    join_path(state_path, sizeof(state_path), data_dir, "source_state.json");
    CHECK(access(state_path, F_OK) == 0, "source_state.json was not written");

    unlink(state_path);
    rmdir(data_dir);
    rmdir(shared_dir);
    rmdir(tmp_root);
    unsetenv("SHARED_USERDATA_PATH");

    puts("source_state_tests: ok");
    return 0;
}
