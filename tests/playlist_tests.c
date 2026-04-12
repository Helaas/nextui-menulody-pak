#include "playlist.h"
#include "strutil.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, msg) do { if (!(cond)) fail(msg, __LINE__); } while (0)

static void fail(const char *msg, int line) {
    fprintf(stderr, "playlist_tests:%d: %s\n", line, msg);
    exit(1);
}

static void join_path(char *out, size_t size, const char *a, const char *b) {
    int n = snprintf(out, size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= size)
        fail("path overflow", __LINE__);
}

static void mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len;

    join_path(buf, sizeof(buf), path, "");
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '/')
        buf[len - 1] = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                fail("mkdir_p failed", __LINE__);
            *p = '/';
        }
    }

    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        fail("mkdir_p final failed", __LINE__);
}

static void write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f)
        fail("fopen write_text_file failed", __LINE__);
    fputs(content, f);
    fclose(f);
}

static void read_text_file(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    size_t nread;

    if (!f)
        fail("fopen read_text_file failed", __LINE__);
    nread = fread(out, 1, size - 1, f);
    fclose(f);
    out[nread] = '\0';
}

int main(void) {
    char tmp_template[] = "/tmp/menulody-playlist-test-XXXXXX";
    char *tmp_root = mkdtemp(tmp_template);
    char shared_dir[PATH_MAX];
    char data_dir[PATH_MAX];
    char playlists_dir[PATH_MAX];
    char valid_path[PATH_MAX];
    char settings_path[PATH_MAX];
    char invalid_path[PATH_MAX];
    char hidden_invalid_path[PATH_MAX];
    char settings_buf[64];
    char err[128];
    char **names = NULL;
    int count = 0;
    named_playlist_t saved = {0};
    named_playlist_t loaded = {0};
    named_playlist_t invalid = {0};
    char *tracks[] = {
        "/mnt/SDCARD/Music/alpha.mp3",
        "/mnt/SDCARD/Music/beta.wav",
    };

    if (!tmp_root)
        fail("mkdtemp failed", __LINE__);

    join_path(shared_dir, sizeof(shared_dir), tmp_root, "shared");
    CHECK(setenv("SHARED_USERDATA_PATH", shared_dir, 1) == 0,
          "setenv SHARED_USERDATA_PATH failed");

    join_path(data_dir, sizeof(data_dir), shared_dir, "Menulody");
    join_path(playlists_dir, sizeof(playlists_dir), data_dir, "playlists");
    mkdir_p(playlists_dir);

    CHECK(playlist_name_is_valid("Road Trip", err, sizeof(err)),
          "valid playlist name rejected");
    CHECK(!playlist_name_is_valid("../settings", err, sizeof(err)),
          "unsafe playlist name should be rejected");
    CHECK(strstr(err, "cannot") != NULL, "validation error should explain failure");

    str_copy_trunc(saved.name, sizeof(saved.name), "Road Trip");
    saved.tracks = tracks;
    saved.count = 2;
    CHECK(playlist_named_save(&saved) == 0, "save valid playlist failed");

    join_path(valid_path, sizeof(valid_path), playlists_dir, "Road Trip.json");
    CHECK(access(valid_path, F_OK) == 0, "valid playlist file missing");

    CHECK(playlist_named_load("Road Trip", &loaded) == 0, "load valid playlist failed");
    CHECK(strcmp(loaded.name, "Road Trip") == 0, "loaded playlist name mismatch");
    CHECK(loaded.count == 2, "loaded playlist track count mismatch");
    CHECK(strcmp(loaded.tracks[0], tracks[0]) == 0, "loaded track 0 mismatch");
    CHECK(strcmp(loaded.tracks[1], tracks[1]) == 0, "loaded track 1 mismatch");
    playlist_named_free(&loaded);

    join_path(settings_path, sizeof(settings_path), data_dir, "settings.json");
    write_text_file(settings_path, "sentinel\n");

    str_copy_trunc(invalid.name, sizeof(invalid.name), "../settings");
    invalid.tracks = tracks;
    invalid.count = 2;
    CHECK(playlist_named_save(&invalid) == -1,
          "invalid playlist save should fail");
    CHECK(playlist_named_delete("../settings") == -1,
          "invalid playlist delete should fail");
    CHECK(playlist_named_load("../settings", &loaded) == -1,
          "invalid playlist load should fail");
    read_text_file(settings_path, settings_buf, sizeof(settings_buf));
    CHECK(strcmp(settings_buf, "sentinel\n") == 0,
          "invalid playlist name should not overwrite sibling files");

    join_path(invalid_path, sizeof(invalid_path), playlists_dir, "bad\\name.json");
    write_text_file(invalid_path, "{\"name\":\"bad\\\\name\",\"tracks\":[]}\n");
    CHECK(playlist_named_load("bad\\name", &loaded) == -1,
          "backslash playlist name should be rejected");

    join_path(hidden_invalid_path, sizeof(hidden_invalid_path), playlists_dir, "..json");
    write_text_file(hidden_invalid_path, "{\"name\":\".\",\"tracks\":[]}\n");

    CHECK(playlist_list_saved(&names, &count) == 0, "playlist_list_saved failed");
    CHECK(count == 1, "invalid playlist filenames should be skipped");
    CHECK(strcmp(names[0], "Road Trip") == 0, "unexpected playlist listed");
    free(names[0]);
    free(names);

    CHECK(playlist_named_delete("Road Trip") == 0, "delete valid playlist failed");
    CHECK(access(valid_path, F_OK) != 0, "playlist file still exists after delete");

    unlink(settings_path);
    unlink(invalid_path);
    unlink(hidden_invalid_path);
    rmdir(playlists_dir);
    rmdir(data_dir);
    rmdir(shared_dir);
    rmdir(tmp_root);
    unsetenv("SHARED_USERDATA_PATH");

    puts("playlist_tests: ok");
    return 0;
}
