#include "varnish_client.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VARNISH_FIFO_DEFAULT "/tmp/varnish.fifo"
#define VARNISH_CLIENT_ID    "menulody"
#define VARNISH_POSITION     "bottom-center"
#define MAX_PATH             512

static const char *varnish_fifo_path(void) {
    const char *path = getenv("VARNISH_FIFO_PATH");
    return (path && path[0]) ? path : VARNISH_FIFO_DEFAULT;
}

static const char *default_platform(void) {
    const char *platform = getenv("PLATFORM");

    if (platform && platform[0]) return platform;

#if defined(PLATFORM_TG5040)
    return "tg5040";
#elif defined(PLATFORM_TG5050)
    return "tg5050";
#elif defined(PLATFORM_MY355)
    return "my355";
#else
    return "tg5040";
#endif
}

static int build_varnish_bin_path(char *out, size_t size) {
    const char *pak_dir = getenv("VARNISH_PAK_DIR");
    const char *sdcard;
    const char *platform;

    if (!out || size == 0) return -1;
    out[0] = '\0';

    if (pak_dir && pak_dir[0]) {
        if (snprintf(out, size, "%s/varnish", pak_dir) >= (int)size)
            out[0] = '\0';
        return out[0] ? 0 : -1;
    }

    sdcard = getenv("SDCARD_PATH");
    if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
    platform = default_platform();

    if (snprintf(out, size, "%s/Tools/%s/Varnish.pak/varnish",
                 sdcard, platform) >= (int)size) {
        out[0] = '\0';
        return -1;
    }

    return 0;
}

static int varnish_send(const char *fmt, ...) {
    char message[512];
    int fd;
    int len;
    va_list args;

    fd = open(varnish_fifo_path(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    va_start(args, fmt);
    len = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (len < 0 || len >= (int)sizeof(message)) {
        close(fd);
        return -1;
    }

    if (write(fd, message, (size_t)len) != len) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void sanitize_text(const char *in, char *out, size_t size) {
    size_t i = 0;

    if (!out || size == 0) return;
    out[0] = '\0';
    if (!in) return;

    while (in[i] && i + 1 < size) {
        char c = in[i];
        out[i] = (c == '\n' || c == '\r') ? ' ' : c;
        i++;
    }
    out[i] = '\0';
}

bool varnish_client_is_running(void) {
    return access(varnish_fifo_path(), F_OK) == 0;
}

bool varnish_client_is_installed(void) {
    char path[MAX_PATH];

    if (build_varnish_bin_path(path, sizeof(path)) != 0)
        return false;

    return access(path, X_OK) == 0;
}

int varnish_client_show_pill(const char *text, int duration_secs) {
    char sanitized[320];

    if (!text || !text[0] || duration_secs <= 0) return -1;

    sanitize_text(text, sanitized, sizeof(sanitized));
    if (!sanitized[0]) return -1;

    return varnish_send("PILL %s %s %d %s\n",
                        VARNISH_CLIENT_ID, VARNISH_POSITION,
                        duration_secs, sanitized);
}

int varnish_client_hide(void) {
    return varnish_send("HIDE %s\n", VARNISH_CLIENT_ID);
}
