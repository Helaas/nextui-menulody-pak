#include "varnish_client.h"
#include "strutil.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VARNISH_FIFO_DEFAULT "/tmp/varnish.fifo"
#define VARNISH_CLIENT_ID    "menulody"
#define VARNISH_POSITION     "bottom-center"
#define MAX_PATH             512
#define STARTUP_MARKER_START "# >>> VARNISH STARTUP >>>"
#define STARTUP_MARKER_END   "# <<< VARNISH STARTUP <<<"

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

static void get_sdcard_path(char *out, size_t size) {
    const char *sdcard = getenv("SDCARD_PATH");

    if (!out || size == 0) return;
    if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
    str_copy_trunc(out, size, sdcard);
}

static void get_userdata_path(char *out, size_t size) {
    const char *userdata = getenv("USERDATA_PATH");

    if (!out || size == 0) return;

    if (userdata && userdata[0]) {
        str_copy_trunc(out, size, userdata);
        return;
    }

#ifndef PLATFORM_MAC
    {
        char sdcard[MAX_PATH];
        char platform[32];

        get_sdcard_path(sdcard, sizeof(sdcard));
        str_copy_trunc(platform, sizeof(platform), default_platform());
        if (path_join(out, size, sdcard, ".userdata") != 0 ||
            path_join(out, size, out, platform) != 0) {
            out[0] = '\0';
        }
    }
#else
    {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        if (path_join(out, size, home, ".userdata") != 0 ||
            path_join(out, size, out, "desktop") != 0) {
            out[0] = '\0';
        }
    }
#endif
}

static int build_varnish_bin_path(char *out, size_t size) {
    const char *pak_dir = getenv("VARNISH_PAK_DIR");
    char sdcard[MAX_PATH];
    const char *platform;

    if (!out || size == 0) return -1;
    out[0] = '\0';

    if (pak_dir && pak_dir[0]) {
        if (snprintf(out, size, "%s/varnish", pak_dir) >= (int)size)
            out[0] = '\0';
        return out[0] ? 0 : -1;
    }

    get_sdcard_path(sdcard, sizeof(sdcard));
    platform = default_platform();

    if (snprintf(out, size, "%s/Tools/%s/Varnish.pak/varnish",
                 sdcard, platform) >= (int)size) {
        out[0] = '\0';
        return -1;
    }

    return 0;
}

static void get_state_dir(char *out, size_t size) {
    char userdata[MAX_PATH];

    get_userdata_path(userdata, sizeof(userdata));
    if (!out || size == 0) return;
    if (!userdata[0] || path_join(out, size, userdata, "Varnish") != 0)
        out[0] = '\0';
}

static void get_enabled_marker_path(char *out, size_t size) {
    char state_dir[MAX_PATH];

    get_state_dir(state_dir, sizeof(state_dir));
    if (!out || size == 0) return;
    if (!state_dir[0] || path_join(out, size, state_dir, "enabled") != 0)
        out[0] = '\0';
}

static void get_boot_hook_path(char *out, size_t size) {
    char userdata[MAX_PATH];

    get_userdata_path(userdata, sizeof(userdata));
    if (!out || size == 0) return;
    if (!userdata[0] ||
        path_join(out, size, userdata, ".hooks") != 0 ||
        path_join(out, size, out, "boot.d") != 0 ||
        path_join(out, size, out, "varnish.sync.sh") != 0) {
        out[0] = '\0';
    }
}

static void get_startup_script_path(char *out, size_t size) {
    char sdcard[MAX_PATH];
    char tmp_update[MAX_PATH];
    char script_name[64];
    const char *platform = default_platform();

    if (!out || size == 0) return;

    get_sdcard_path(sdcard, sizeof(sdcard));
    if (!sdcard[0] ||
        path_join(tmp_update, sizeof(tmp_update), sdcard, ".tmp_update") != 0 ||
        snprintf(script_name, sizeof(script_name), "%s.sh", platform) >= (int)sizeof(script_name) ||
        path_join(out, size, tmp_update, script_name) != 0) {
        out[0] = '\0';
    }
}

static int file_contains_markers(const char *path, const char *start_marker,
                                 const char *end_marker) {
    FILE *f;
    long len;
    char *buf;
    int found;

    if (!path || !path[0] || !start_marker || !end_marker) return 0;

    f = fopen(path, "r");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    len = ftell(f);
    if (len <= 0 || len > 65536) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return 0;
    }
    buf[len] = '\0';
    fclose(f);

    found = strstr(buf, start_marker) != NULL && strstr(buf, end_marker) != NULL;
    free(buf);
    return found;
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

void varnish_client_get_status(varnish_client_status_t *out) {
    char path[MAX_PATH];

    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->pak_installed = varnish_client_is_installed();

    get_enabled_marker_path(path, sizeof(path));
    out->enabled = path[0] && access(path, F_OK) == 0;

    get_startup_script_path(path, sizeof(path));
    out->startup_installed = file_contains_markers(path, STARTUP_MARKER_START,
                                                   STARTUP_MARKER_END) != 0;

    get_boot_hook_path(path, sizeof(path));
    out->boot_installed = path[0] && access(path, F_OK) == 0;

    out->daemon_running = varnish_client_is_running();
}

bool varnish_client_is_enabled(const varnish_client_status_t *status) {
    return status
        && status->pak_installed
        && status->enabled
        && status->startup_installed
        && status->boot_installed
        && status->daemon_running;
}

void varnish_client_format_status(const varnish_client_status_t *status,
                                  char *out, size_t size) {
    if (!out || size == 0) return;
    out[0] = '\0';
    if (!status) return;

    snprintf(out, size,
             "Pak: %s\n"
             "Enabled: %s\n"
             "Startup patch: %s\n"
             "Boot hook: %s\n"
             "Daemon: %s",
             status->pak_installed ? "Installed" : "Missing",
             status->enabled ? "On" : "Off",
             status->startup_installed ? "Installed" : "Missing",
             status->boot_installed ? "Installed" : "Missing",
             status->daemon_running ? "Running" : "Stopped");
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
