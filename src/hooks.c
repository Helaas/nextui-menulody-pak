#include "hooks.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define MAX_PATH 512

/* ── Path helpers ──────────────────────────────────────────────── */

static void get_userdata_path(char *out, int size) {
    const char *p = getenv("USERDATA_PATH");
    if (p) {
        snprintf(out, size, "%s", p);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform) platform = "tg5040";
    snprintf(out, size, "%s/.userdata/%s", sd, platform);
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, size, "%s/.userdata/desktop", home);
#endif
}

static void mkdirp(const char *path) {
    char tmp[MAX_PATH];
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

static void get_hook_dir(const char *category, char *out, int size) {
    char ud[MAX_PATH];
    get_userdata_path(ud, sizeof(ud));
    snprintf(out, size, "%s/.hooks/%s", ud, category);
}

/* ── Script content ────────────────────────────────────────────── */

static const char *pre_launch_script =
    "#!/bin/sh\n"
    "# Menulody: pause music before ROM/Pak launch\n"
    "MENULODY_FIFO=\"/tmp/menulody.fifo\"\n"
    "[ -p \"$MENULODY_FIFO\" ] || exit 0\n"
    "\n"
    "# Always pause for ROM launches\n"
    "if [ \"$HOOK_TYPE\" = \"rom\" ]; then\n"
    "    echo \"PAUSE\" > \"$MENULODY_FIFO\"\n"
    "    exit 0\n"
    "fi\n"
    "\n"
    "# For Pak launches: check config\n"
    "SETTINGS_DIR=\"${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}\"\n"
    "SETTINGS=\"$SETTINGS_DIR/Menulody/settings.json\"\n"
    "if [ -f \"$SETTINGS\" ]; then\n"
    "    if grep -q '\"pause_on_pak\".*true' \"$SETTINGS\" 2>/dev/null; then\n"
    "        echo \"PAUSE\" > \"$MENULODY_FIFO\"\n"
    "    fi\n"
    "else\n"
    "    # Default: pause on pak\n"
    "    echo \"PAUSE\" > \"$MENULODY_FIFO\"\n"
    "fi\n";

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
    mkdirp(dir);

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);

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
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "menulody: cannot remove hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int script_exists(const char *dir, const char *filename) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    return access(path, F_OK) == 0;
}

/* ── Public API ────────────────────────────────────────────────── */

int hooks_install_playback(void) {
    char pre_dir[MAX_PATH], post_dir[MAX_PATH];
    get_hook_dir("pre-launch.d", pre_dir, sizeof(pre_dir));
    get_hook_dir("post-launch.d", post_dir, sizeof(post_dir));

    int err = 0;
    /* .sync.sh so music is fully paused before emulator starts */
    if (write_script(pre_dir, "menulody.sync.sh", pre_launch_script) != 0) err++;
    if (write_script(post_dir, "menulody.sh", post_launch_script) != 0) err++;
    return err ? -1 : 0;
}

int hooks_uninstall_playback(void) {
    char pre_dir[MAX_PATH], post_dir[MAX_PATH];
    get_hook_dir("pre-launch.d", pre_dir, sizeof(pre_dir));
    get_hook_dir("post-launch.d", post_dir, sizeof(post_dir));

    int err = 0;
    if (remove_script(pre_dir, "menulody.sync.sh") != 0) err++;
    if (remove_script(post_dir, "menulody.sh") != 0) err++;
    return err ? -1 : 0;
}

int hooks_install_autostart(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return write_script(dir, "menulody.sh", boot_script_template);
}

int hooks_uninstall_autostart(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return remove_script(dir, "menulody.sh");
}

bool hooks_playback_installed(void) {
    char dir[MAX_PATH];
    get_hook_dir("pre-launch.d", dir, sizeof(dir));
    return script_exists(dir, "menulody.sync.sh");
}

bool hooks_autostart_installed(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return script_exists(dir, "menulody.sh");
}

int hooks_apply_config(bool auto_start) {
    /* Playback hooks are always installed when the pak is used */
    hooks_install_playback();

    if (auto_start)
        return hooks_install_autostart();
    else
        return hooks_uninstall_autostart();
}
