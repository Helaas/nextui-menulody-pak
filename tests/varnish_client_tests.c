#include "varnish_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, msg) do { if (!(cond)) fail(msg, __LINE__); } while (0)

static void fail(const char *msg, int line) {
    fprintf(stderr, "varnish_client_tests:%d: %s\n", line, msg);
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

static int open_reader(const char *fifo_path) {
    int fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fail("open reader failed", __LINE__);
    return fd;
}

static void read_fifo_line(int fd, char *out, size_t size) {
    int retries = 50;

    while (retries-- > 0) {
        ssize_t n = read(fd, out, size - 1);
        if (n > 0) {
            out[n] = '\0';
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            fail("read failed", __LINE__);
        usleep(10000);
    }

    fail("timed out waiting for FIFO data", __LINE__);
}

static void write_stub_binary(const char *dir) {
    char path[PATH_MAX];
    FILE *f;

    join_path(path, sizeof(path), dir, "varnish");
    f = fopen(path, "w");
    if (!f)
        fail("fopen stub binary failed", __LINE__);
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    if (chmod(path, 0755) != 0)
        fail("chmod stub binary failed", __LINE__);
}

int main(void) {
    char tmp_template[] = "/tmp/menulody-varnish-test-XXXXXX";
    char *tmp_root = mkdtemp(tmp_template);
    char fifo_path[PATH_MAX];
    char pak_dir[PATH_MAX];
    char userdata_dir[PATH_MAX];
    char sdcard_dir[PATH_MAX];
    char state_dir[PATH_MAX];
    char enabled_path[PATH_MAX];
    char boot_dir[PATH_MAX];
    char boot_hook_path[PATH_MAX];
    char startup_dir[PATH_MAX];
    char startup_path[PATH_MAX];
    char buffer[512];
    char status_text[256];
    varnish_client_status_t status;
    int reader_fd;

    if (!tmp_root)
        fail("mkdtemp failed", __LINE__);

    join_path(fifo_path, sizeof(fifo_path), tmp_root, "varnish.fifo");
    if (mkfifo(fifo_path, 0600) != 0)
        fail("mkfifo failed", __LINE__);
    CHECK(setenv("VARNISH_FIFO_PATH", fifo_path, 1) == 0, "setenv VARNISH_FIFO_PATH failed");

    reader_fd = open_reader(fifo_path);
    CHECK(varnish_client_show_pill("Song Title", 5) == 0, "show_pill failed");
    read_fifo_line(reader_fd, buffer, sizeof(buffer));
    CHECK(strcmp(buffer, "PILL menulody bottom-center 5 Song Title\n") == 0,
          "unexpected PILL command");
    close(reader_fd);

    reader_fd = open_reader(fifo_path);
    CHECK(varnish_client_hide() == 0, "hide failed");
    read_fifo_line(reader_fd, buffer, sizeof(buffer));
    CHECK(strcmp(buffer, "HIDE menulody\n") == 0, "unexpected HIDE command");
    close(reader_fd);

    unlink(fifo_path);
    CHECK(!varnish_client_is_running(), "running detection should fail without FIFO");
    CHECK(varnish_client_show_pill("Song Title", 5) == -1,
          "show_pill should fail gracefully when FIFO is missing");

    join_path(pak_dir, sizeof(pak_dir), tmp_root, "Varnish.pak");
    CHECK(mkdir(pak_dir, 0755) == 0, "mkdir pak dir failed");
    write_stub_binary(pak_dir);
    CHECK(setenv("VARNISH_PAK_DIR", pak_dir, 1) == 0, "setenv VARNISH_PAK_DIR failed");
    CHECK(varnish_client_is_installed(), "installed detection failed");

    join_path(userdata_dir, sizeof(userdata_dir), tmp_root, "userdata");
    join_path(sdcard_dir, sizeof(sdcard_dir), tmp_root, "sdcard");
    CHECK(setenv("USERDATA_PATH", userdata_dir, 1) == 0, "setenv USERDATA_PATH failed");
    CHECK(setenv("SDCARD_PATH", sdcard_dir, 1) == 0, "setenv SDCARD_PATH failed");
    CHECK(setenv("PLATFORM", "tg5040", 1) == 0, "setenv PLATFORM failed");

    join_path(state_dir, sizeof(state_dir), userdata_dir, "Varnish");
    mkdir_p(state_dir);
    join_path(enabled_path, sizeof(enabled_path), state_dir, "enabled");
    write_text_file(enabled_path, "enabled\n");

    join_path(boot_dir, sizeof(boot_dir), userdata_dir, ".hooks/boot.d");
    mkdir_p(boot_dir);
    join_path(boot_hook_path, sizeof(boot_hook_path), boot_dir, "varnish.sync.sh");
    write_text_file(boot_hook_path, "#!/bin/sh\n");

    join_path(startup_dir, sizeof(startup_dir), sdcard_dir, ".tmp_update");
    mkdir_p(startup_dir);
    join_path(startup_path, sizeof(startup_path), startup_dir, "tg5040.sh");
    write_text_file(startup_path,
                    "# >>> VARNISH STARTUP >>>\n"
                    "test\n"
                    "# <<< VARNISH STARTUP <<<\n");

    CHECK(mkfifo(fifo_path, 0600) == 0 || errno == EEXIST, "recreate fifo failed");
    varnish_client_get_status(&status);
    CHECK(status.pak_installed, "status pak_installed failed");
    CHECK(status.enabled, "status enabled failed");
    CHECK(status.startup_installed, "status startup_installed failed");
    CHECK(status.boot_installed, "status boot_installed failed");
    CHECK(status.daemon_running, "status daemon_running failed");
    CHECK(varnish_client_is_enabled(&status), "enabled aggregate failed");
    varnish_client_format_status(&status, status_text, sizeof(status_text));
    CHECK(strstr(status_text, "Pak: Installed") != NULL, "formatted status missing pak");
    CHECK(strstr(status_text, "Startup patch: Installed") != NULL,
          "formatted status missing startup");
    unlink(enabled_path);
    varnish_client_get_status(&status);
    CHECK(!status.enabled, "status enabled should clear after unlink");
    CHECK(!varnish_client_is_enabled(&status), "aggregate enabled should fail when marker missing");

    unsetenv("VARNISH_PAK_DIR");
    unsetenv("VARNISH_FIFO_PATH");
    unsetenv("USERDATA_PATH");
    unsetenv("SDCARD_PATH");
    unsetenv("PLATFORM");
    unlink(fifo_path);
    {
        char binary_path[PATH_MAX];
        join_path(binary_path, sizeof(binary_path), pak_dir, "varnish");
        unlink(binary_path);
    }
    unlink(boot_hook_path);
    unlink(startup_path);
    rmdir(pak_dir);
    rmdir(boot_dir);
    {
        char hooks_dir[PATH_MAX];
        join_path(hooks_dir, sizeof(hooks_dir), userdata_dir, ".hooks");
        rmdir(hooks_dir);
    }
    rmdir(startup_dir);
    rmdir(state_dir);
    rmdir(userdata_dir);
    rmdir(sdcard_dir);
    rmdir(tmp_root);

    puts("varnish_client_tests: ok");
    return 0;
}
