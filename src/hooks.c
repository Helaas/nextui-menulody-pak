#include "hooks.h"
#include "config.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ── Path helpers ──────────────────────────────────────────────── */

static void get_userdata_path(char *out, int size) {
    const char *p = getenv("USERDATA_PATH");
    if (p) {
        str_copy_trunc(out, (size_t)size, p);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform) platform = "tg5040";
    if (size > 0) {
        if (path_join(out, (size_t)size, sd, ".userdata") != 0 ||
            path_join(out, (size_t)size, out, platform) != 0) {
            out[0] = '\0';
        }
    }
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    if (size > 0) {
        if (path_join(out, (size_t)size, home, ".userdata") != 0 ||
            path_join(out, (size_t)size, out, "desktop") != 0) {
            out[0] = '\0';
        }
    }
#endif
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

static void get_hook_dir(const char *category, char *out, int size) {
    char ud[CONFIG_MAX_PATH];
    get_userdata_path(ud, sizeof(ud));
    if (size > 0) {
        if (!ud[0] ||
            path_join(out, (size_t)size, ud, ".hooks") != 0 ||
            path_join(out, (size_t)size, out, category) != 0) {
            out[0] = '\0';
        }
    }
}

/* ── Script content ────────────────────────────────────────────── */

static const char *pre_launch_script =
    "#!/bin/sh\n"
    "# Menulody: handle music before ROM/Pak launch\n"
    "MENULODY_FIFO=\"/tmp/menulody.fifo\"\n"
    "[ -p \"$MENULODY_FIFO\" ] || exit 0\n"
    "\n"
    "send_pause() {\n"
    "    echo \"PAUSE\" > \"$MENULODY_FIFO\"\n"
    "    exit 0\n"
    "}\n"
    "\n"
    "case \"$HOOK_TYPE\" in\n"
    "    rom)\n"
    "        send_pause\n"
    "        ;;\n"
    "    pak)\n"
    "        SETTINGS_DIR=\"${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}\"\n"
    "        SETTINGS=\"$SETTINGS_DIR/Menulody/settings.json\"\n"
    "        if [ ! -f \"$SETTINGS\" ]; then\n"
    "            send_pause\n"
    "        fi\n"
    "        if grep -q '\"pause_on_pak\".*true' \"$SETTINGS\" 2>/dev/null; then\n"
    "            send_pause\n"
    "        fi\n"
    "        if grep -q '\"pause_on_pak\".*false' \"$SETTINGS\" 2>/dev/null; then\n"
    "            echo \"PAK_LAUNCH\" > \"$MENULODY_FIFO\"\n"
    "            exit 0\n"
    "        fi\n"
    "        send_pause\n"
    "        ;;\n"
    "    *)\n"
    "        send_pause\n"
    "        ;;\n"
    "esac\n";

static const char *post_launch_script =
    "#!/bin/sh\n"
    "# Menulody: resume music after ROM/Pak exit\n"
    "MENULODY_FIFO=\"/tmp/menulody.fifo\"\n"
    "[ -p \"$MENULODY_FIFO\" ] || exit 0\n"
    "echo \"RESUME\" > \"$MENULODY_FIFO\"\n";

static const char *boot_script_template =
    "#!/bin/sh\n"
    "# Menulody: start background music daemon at boot\n"
    "PAK_DIR=\"/mnt/SDCARD/Tools/${PLATFORM}/Menulody.pak\"\n"
    "[ -x \"$PAK_DIR/menulody\" ] || exit 0\n"
    "cd \"$PAK_DIR\"\n"
    "export LD_LIBRARY_PATH=\"/usr/trimui/lib:$PAK_DIR/lib:$LD_LIBRARY_PATH\"\n"
    "./menulody --daemon &\n";

/* ── Write script helper ──────────────────────────────────────── */

static int write_script(const char *dir, const char *filename, const char *content) {
    if (!dir[0]) return -1;
    mkdirp(dir);

    char path[CONFIG_MAX_PATH];
    if (path_join(path, sizeof(path), dir, filename) != 0) return -1;

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "menulody: cannot write hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs(content, f);
    fclose(f);
    chmod(path, 0755);

    fprintf(stderr, "menulody: installed hook %s\n", path);
    return 0;
}

static int remove_script(const char *dir, const char *filename) {
    char path[CONFIG_MAX_PATH];
    if (!dir[0] || path_join(path, sizeof(path), dir, filename) != 0)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "menulody: cannot remove hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Public API ────────────────────────────────────────────────── */

int hooks_install_playback(void) {
    char pre_dir[CONFIG_MAX_PATH], post_dir[CONFIG_MAX_PATH];
    get_hook_dir("pre-launch.d", pre_dir, sizeof(pre_dir));
    get_hook_dir("post-launch.d", post_dir, sizeof(post_dir));

    int err = 0;
    /* .sync.sh so music is fully paused before emulator starts */
    if (write_script(pre_dir, "menulody.sync.sh", pre_launch_script) != 0) err++;
    if (write_script(post_dir, "menulody.sh", post_launch_script) != 0) err++;
    return err ? -1 : 0;
}

static int hooks_install_autostart(void) {
    char dir[CONFIG_MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return write_script(dir, "menulody.sh", boot_script_template);
}

static int hooks_uninstall_autostart(void) {
    char dir[CONFIG_MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return remove_script(dir, "menulody.sh");
}

int hooks_apply_config(bool auto_start) {
    /* Playback hooks are always installed when the pak is used */
    hooks_install_playback();

    if (auto_start)
        return hooks_install_autostart();
    else
        return hooks_uninstall_autostart();
}
