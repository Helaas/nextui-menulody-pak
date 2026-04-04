#include "hooks.h"
#include "config.h"
#include "strutil.h"

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
    char tmp[MAX_PATH];
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
    char ud[MAX_PATH];
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
    if (!dir[0]) return -1;
    mkdirp(dir);

    char path[MAX_PATH];
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
    char path[MAX_PATH];
    if (!dir[0] || path_join(path, sizeof(path), dir, filename) != 0)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "menulody: cannot remove hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int script_exists(const char *dir, const char *filename) {
    char path[MAX_PATH];
    if (!dir[0] || path_join(path, sizeof(path), dir, filename) != 0)
        return 0;
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

/* ── Preload wrapper ───────────────────────────────────────────── */

static const char *preload_wrapper_script =
    "#!/bin/sh\n"
    "# Menulody: LD_PRELOAD wrapper for flicker-free overlay\n"
    "PLATFORM=\"${PLATFORM:-tg5040}\"\n"
    "SO=\"/mnt/SDCARD/Tools/${PLATFORM}/Menulody.pak/menulody_overlay.so\"\n"
    "[ -f \"$SO\" ] && export LD_PRELOAD=\"$SO\"\n"
    "exec \"$(dirname \"$0\")/nextui.elf.real\" \"$@\"\n";

static void get_nextui_bin_path(char *out, size_t size) {
    const char *sys = getenv("SYSTEM_PATH");
    if (sys && sys[0]) {
        snprintf(out, size, "%s/bin", sys);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd || !sd[0]) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) {
#if defined(PLATFORM_TG5040)
        platform = "tg5040";
#elif defined(PLATFORM_TG5050)
        platform = "tg5050";
#elif defined(PLATFORM_MY355)
        platform = "my355";
#else
        platform = "tg5040";
#endif
    }
    snprintf(out, size, "%s/.system/%s/bin", sd, platform);
#else
    out[0] = '\0';
#endif
}

int hooks_install_preload(void) {
#ifdef PLATFORM_MAC
    return 0;
#else
    char bin_dir[MAX_PATH];
    char elf_path[MAX_PATH + 32];
    char real_path[MAX_PATH + 32];

    get_nextui_bin_path(bin_dir, sizeof(bin_dir));
    if (!bin_dir[0]) return -1;

    snprintf(elf_path, sizeof(elf_path), "%s/nextui.elf", bin_dir);
    snprintf(real_path, sizeof(real_path), "%s/nextui.elf.real", bin_dir);

    /* Already wrapped? */
    if (access(real_path, X_OK) == 0) {
        fprintf(stderr, "menulody: preload wrapper already installed\n");
        return 0;
    }

    /* Verify the original binary exists */
    if (access(elf_path, X_OK) != 0) {
        fprintf(stderr, "menulody: nextui.elf not found at %s\n", elf_path);
        return -1;
    }

    /* Check it's actually a binary (starts with ELF magic), not already a script */
    {
        FILE *f = fopen(elf_path, "rb");
        if (!f) return -1;
        unsigned char hdr[4] = {0};
        size_t n = fread(hdr, 1, 4, f);
        fclose(f);
        if (n < 4 || hdr[0] != 0x7F || hdr[1] != 'E' ||
            hdr[2] != 'L' || hdr[3] != 'F') {
            fprintf(stderr, "menulody: %s is not an ELF binary, skipping wrapper\n",
                    elf_path);
            return -1;
        }
    }

    /* Rename original binary */
    if (rename(elf_path, real_path) != 0) {
        fprintf(stderr, "menulody: cannot rename %s -> %s: %s\n",
                elf_path, real_path, strerror(errno));
        return -1;
    }

    /* Write wrapper script */
    FILE *f = fopen(elf_path, "w");
    if (!f) {
        /* Roll back */
        rename(real_path, elf_path);
        fprintf(stderr, "menulody: cannot write wrapper: %s\n", strerror(errno));
        return -1;
    }
    fputs(preload_wrapper_script, f);
    fclose(f);
    chmod(elf_path, 0755);

    fprintf(stderr, "menulody: preload wrapper installed at %s\n", elf_path);
    return 0;
#endif
}

int hooks_uninstall_preload(void) {
#ifdef PLATFORM_MAC
    return 0;
#else
    char bin_dir[MAX_PATH];
    char elf_path[MAX_PATH + 32];
    char real_path[MAX_PATH + 32];

    get_nextui_bin_path(bin_dir, sizeof(bin_dir));
    if (!bin_dir[0]) return -1;

    snprintf(elf_path, sizeof(elf_path), "%s/nextui.elf", bin_dir);
    snprintf(real_path, sizeof(real_path), "%s/nextui.elf.real", bin_dir);

    if (access(real_path, X_OK) != 0) return 0; /* Not wrapped */

    /* Remove wrapper script, restore original */
    unlink(elf_path);
    if (rename(real_path, elf_path) != 0) {
        fprintf(stderr, "menulody: cannot restore %s: %s\n", elf_path, strerror(errno));
        return -1;
    }

    fprintf(stderr, "menulody: preload wrapper removed\n");
    return 0;
#endif
}

int hooks_apply_config(bool auto_start) {
    /* Playback hooks are always installed when the pak is used */
    hooks_install_playback();

    if (auto_start)
        return hooks_install_autostart();
    else
        return hooks_uninstall_autostart();
}
