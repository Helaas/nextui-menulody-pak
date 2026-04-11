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
    char buffer[512];
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

    unsetenv("VARNISH_PAK_DIR");
    unsetenv("VARNISH_FIFO_PATH");
    unlink(fifo_path);
    {
        char binary_path[PATH_MAX];
        join_path(binary_path, sizeof(binary_path), pak_dir, "varnish");
        unlink(binary_path);
    }
    rmdir(pak_dir);
    rmdir(tmp_root);

    puts("varnish_client_tests: ok");
    return 0;
}
