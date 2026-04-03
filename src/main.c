/*
 * Menulody — Background music player for NextUI
 *
 * Usage:
 *   menulody              Launch the interactive UI
 *   menulody --daemon     Start the background music daemon
 *   menulody --kill       Stop the running daemon
 *   menulody --status     Print current daemon status
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include "daemon.h"
#include "ui.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef PLATFORM_MAC
#include <limits.h>

static void configure_desktop_nextui_preview(void) {
    const char *cache_root = ".cache/nextui-preview";
    const char *cache_assets_dir = ".cache/nextui-preview/assets";
    const char *env_assets = getenv("AP_STATUS_ASSETS_DIR");
    const char *env_nextval = getenv("AP_NEXTVAL_PATH");
    const char *env_settings = getenv("AP_MINUI_SETTINGS_PATH");
    char path[PATH_MAX];
    bool have_cache_assets;
    bool have_cache_nextval;
    bool have_cache_settings;

    have_cache_assets = access(cache_assets_dir, R_OK) == 0;

    snprintf(path, sizeof(path), "%s/nextval.json", cache_root);
    have_cache_nextval = access(path, R_OK) == 0;
    if ((!env_nextval || !env_nextval[0]) && have_cache_nextval)
        setenv("AP_NEXTVAL_PATH", path, 0);

    snprintf(path, sizeof(path), "%s/minuisettings.txt", cache_root);
    have_cache_settings = access(path, R_OK) == 0;
    if ((!env_settings || !env_settings[0]) && have_cache_settings)
        setenv("AP_MINUI_SETTINGS_PATH", path, 0);

    if ((!env_assets || !env_assets[0]) && have_cache_assets)
        setenv("AP_STATUS_ASSETS_DIR", cache_assets_dir, 0);

    if ((!env_assets || !env_assets[0] || !env_nextval || !env_nextval[0]
         || !env_settings || !env_settings[0])
        && (!have_cache_assets || !have_cache_nextval || !have_cache_settings)) {
        fprintf(stderr,
                "menulody: desktop NextUI preview cache missing; run `make setup-nextui-preview-cache`\n");
    }

    setenv("AP_PREVIEW_WIFI_STRENGTH", "3", 0);
    setenv("AP_PREVIEW_BATTERY_PERCENT", "100", 0);
    setenv("AP_PREVIEW_CHARGING", "0", 0);
}
#endif

static void print_usage(void) {
    fprintf(stderr,
        "Menulody — Background music player for NextUI\n"
        "\n"
        "Usage:\n"
        "  menulody           Launch interactive UI\n"
        "  menulody --daemon  Start background daemon\n"
        "  menulody --kill    Stop the daemon\n"
        "  menulody --status  Print daemon status\n"
        "  menulody --help    Show this help\n"
    );
}

static void print_status(void) {
    if (!ipc_daemon_running()) {
        printf("Daemon: not running\n");
        return;
    }

    ipc_client_send(IPC_CMD_STATUS, 0);
    usleep(100000);

    ipc_status_t st;
    if (ipc_client_read_status(&st) < 0) {
        printf("Daemon: running (no status available)\n");
        return;
    }

    printf("Daemon: running\n");
    printf("State:  %s\n", st.playing ? "Playing" : "Paused");
    printf("Track:  %s (%d/%d)\n", st.track_name, st.track_index + 1, st.track_count);
    printf("Shuffle: %s\n", st.shuffle ? "ON" : "OFF");
    printf("Repeat: %s\n", st.repeat == 0 ? "OFF" : st.repeat == 1 ? "ONE" : "ALL");
    printf("Volume: %d%%\n", st.volume);
    if (st.playlist_name[0])
        printf("Playlist: %s\n", st.playlist_name);
}

static void kill_daemon(void) {
    if (!ipc_daemon_running()) {
        fprintf(stderr, "Daemon is not running\n");
        return;
    }
    ipc_client_send(IPC_CMD_QUIT, 0);
    fprintf(stderr, "Sent QUIT to daemon\n");
}

int main(int argc, char *argv[]) {
    /* CLI mode dispatch */
    if (argc > 1) {
        if (strcmp(argv[1], "--daemon") == 0) {
            if (ipc_daemon_running()) {
                fprintf(stderr, "menulody: daemon already running\n");
                return 0;
            }
            return daemon_run();
        }
        if (strcmp(argv[1], "--kill") == 0) {
            kill_daemon();
            return 0;
        }
        if (strcmp(argv[1], "--status") == 0) {
            print_status();
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage();
            return 0;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        print_usage();
        return 1;
    }

    /* Interactive UI mode */
    fprintf(stderr, "menulody: starting UI (platform=%s)\n", AP_PLATFORM_NAME);

    ap_config cfg = {0};
    cfg.window_title = "Menulody";
    cfg.log_path = ap_resolve_log_path("menulody");
    cfg.is_nextui = true;
    cfg.cpu_speed = AP_CPU_SPEED_MENU;

#ifndef PLATFORM_MAC
    /* On device: use system font */
#else
    configure_desktop_nextui_preview();
    cfg.font_path = "third_party/apostrophe/res/font.ttf";
#endif

    if (ap_init(&cfg) != AP_OK) {
        fprintf(stderr, "menulody: failed to initialize UI\n");
        return 1;
    }

    run_app();

    ap_quit();
    return 0;
}
