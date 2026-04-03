#include "daemon.h"
#include "player.h"
#include "playlist.h"
#include "monitor.h"
#include "ipc.h"
#include "overlay.h"
#include "config.h"
#include "hooks.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Daemon state ───────────────────────────────────────────────── */

typedef enum {
    STATE_IDLE,           /* no music loaded / no tracks */
    STATE_PLAYING,        /* menu active, music playing */
    STATE_PAUSED_AUTO,    /* game/tool running, audio released */
    STATE_PAUSED_MANUAL,  /* user manually paused via TOGGLE */
    STATE_PREVIEWING,     /* playing a preview track from UI */
} daemon_state_t;

static volatile int quit_flag = 0;
static player_t     player;
static playlist_t   playlist;
static config_t     config;
static daemon_state_t state = STATE_IDLE;

/* Preview state */
static daemon_state_t pre_preview_state = STATE_IDLE;

/* ── Signal handler ─────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    quit_flag = 1;
}

/* ── Forward declarations ──────────────────────────────────────── */

static void update_status(void);

/* ── Helpers ────────────────────────────────────────────────────── */

static void start_current_track(void) {
    const char *path = playlist_current_path(&playlist);
    if (!path) { state = STATE_IDLE; return; }

    if (player_open(&player, path) < 0) {
        fprintf(stderr, "menulody: failed to open %s, skipping\n", path);
        int next = playlist_next(&playlist);
        if (next < 0) { state = STATE_IDLE; return; }
        path = playlist_current_path(&playlist);
        if (!path || player_open(&player, path) < 0) {
            state = STATE_IDLE;
            return;
        }
    }

    state = STATE_PLAYING;

    /* Show overlay notification */
    const char *name = playlist_current_name(&playlist);
    if (name && config.overlay_duration > 0)
        overlay_set_text(name, config.overlay_duration);

    update_status();
}

static void update_status(void) {
    const char *name = playlist_current_name(&playlist);
    ipc_status_t st = {
        .playing     = (state == STATE_PLAYING || state == STATE_PREVIEWING) ? 1 : 0,
        .shuffle     = playlist.shuffle,
        .repeat      = playlist.repeat,
        .track_index = playlist_current_index(&playlist),
        .track_count = playlist.count,
        .volume      = config.volume,
    };
    if (name) str_copy_trunc(st.track_name, sizeof(st.track_name), name);
    str_copy_trunc(st.playlist_name, sizeof(st.playlist_name), playlist.name);
    ipc_daemon_write_status(&st);
}

/* ── Daemonize ──────────────────────────────────────────────────── */

static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("menulody: fork"); return -1; }
    if (pid > 0) _exit(0);

    setsid();

    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);

    char data_dir[CONFIG_MAX_PATH];
    config_get_data_dir(data_dir, sizeof(data_dir));
    mkdir(data_dir, 0755);
    char log_path[CONFIG_MAX_PATH];
    FILE *log_f = NULL;
    if (path_join(log_path, sizeof(log_path), data_dir, "daemon.log") == 0)
        log_f = freopen(log_path, "a", stderr);
    if (!log_f) freopen("/dev/null", "w", stderr);

    return 0;
}

/* ── Main daemon loop ───────────────────────────────────────────── */

int daemon_run(void) {
    if (daemonize() < 0) return 1;

    fprintf(stderr, "menulody: daemon starting (pid %d)\n", (int)getpid());

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);
    signal(SIGHUP,  SIG_IGN);

    ipc_write_pid();

    /* Load configuration */
    config = config_load();

    if (player_init(&player) < 0) {
        fprintf(stderr, "menulody: player init failed\n");
        goto cleanup;
    }
    player_set_volume(&player, config.volume);

    if (ipc_daemon_init() < 0) {
        fprintf(stderr, "menulody: IPC init failed\n");
        goto cleanup;
    }

    overlay_init(); /* non-fatal */

    /* Install hooks if not already present */
    hooks_install_playback();
    hooks_apply_config(config.auto_start);

    /* Scan music library */
    if (playlist_scan(&playlist, &config) < 0) {
        fprintf(stderr, "menulody: no music found, waiting...\n");
        state = STATE_IDLE;
    } else {
        fprintf(stderr, "menulody: %d tracks loaded\n", playlist.count);
    }

    /* ── Main loop ───────────────────────────────────────────── */
    while (!quit_flag) {
        int menu_active = monitor_is_menu_active();

        /* Process IPC commands */
        int int_arg = 0;
        char str_arg[512] = {0};
        ipc_cmd_t cmd = ipc_daemon_read(&int_arg, str_arg, sizeof(str_arg));

        switch (cmd) {
            case IPC_CMD_QUIT:
                quit_flag = 1;
                continue;

            case IPC_CMD_PLAY:
                if (state == STATE_IDLE && playlist.count > 0) {
                    start_current_track();
                } else if (state != STATE_PLAYING) {
                    if (player_resume(&player) == 0)
                        state = STATE_PLAYING;
                    else
                        start_current_track();
                    update_status();
                }
                break;

            case IPC_CMD_PAUSE:
                if (state == STATE_PLAYING || state == STATE_PREVIEWING) {
                    player_pause(&player);
                    state = STATE_PAUSED_AUTO;
                    update_status();
                    fprintf(stderr, "menulody: paused by hook\n");
                }
                break;

            case IPC_CMD_TOGGLE:
                if (state == STATE_PLAYING) {
                    player_pause(&player);
                    state = STATE_PAUSED_MANUAL;
                    update_status();
                } else if (state == STATE_PAUSED_MANUAL || state == STATE_PAUSED_AUTO) {
                    if (player_resume(&player) == 0)
                        state = STATE_PLAYING;
                    else
                        start_current_track();
                    update_status();
                } else if (state == STATE_IDLE && playlist.count > 0) {
                    start_current_track();
                }
                break;

            case IPC_CMD_NEXT:
                player_close(&player);
                if (playlist_next(&playlist) >= 0) {
                    if (state == STATE_PLAYING || menu_active)
                        start_current_track();
                } else {
                    state = STATE_IDLE;
                    update_status();
                }
                break;

            case IPC_CMD_PREV:
                player_close(&player);
                playlist_prev(&playlist);
                if (state == STATE_PLAYING || menu_active)
                    start_current_track();
                break;

            case IPC_CMD_SELECT:
                player_close(&player);
                playlist_select(&playlist, int_arg);
                if (state == STATE_PLAYING || menu_active)
                    start_current_track();
                break;

            case IPC_CMD_SHUFFLE:
                playlist_shuffle_toggle(&playlist);
                config.shuffle = playlist.shuffle;
                config_save(&config);
                update_status();
                break;

            case IPC_CMD_REPEAT:
                playlist_repeat_cycle(&playlist);
                config.repeat = (repeat_mode_t)playlist.repeat;
                config_save(&config);
                update_status();
                break;

            case IPC_CMD_VOLUME:
                config.volume = int_arg;
                player_set_volume(&player, config.volume);
                config_save(&config);
                update_status();
                break;

            case IPC_CMD_RESCAN:
                player_close(&player);
                playlist_free(&playlist);
                config = config_load();
                player_set_volume(&player, config.volume);
                if (playlist_scan(&playlist, &config) == 0) {
                    if (menu_active) start_current_track();
                } else {
                    state = STATE_IDLE;
                }
                update_status();
                break;

            case IPC_CMD_PLAYLIST:
                player_close(&player);
                playlist_free(&playlist);
                if (strcmp(str_arg, "all") == 0 || str_arg[0] == '\0') {
                    if (playlist_scan(&playlist, &config) == 0) {
                        if (menu_active) start_current_track();
                    } else {
                        state = STATE_IDLE;
                    }
                } else {
                    if (playlist_load_named(&playlist, str_arg) == 0) {
                        playlist.shuffle = config.shuffle;
                        playlist.repeat = (int)config.repeat;
                        if (playlist.shuffle) playlist_reshuffle(&playlist);
                        if (menu_active) start_current_track();
                    } else {
                        state = STATE_IDLE;
                    }
                }
                update_status();
                break;

            case IPC_CMD_PREVIEW:
                if (state == STATE_PLAYING) {
                    pre_preview_state = STATE_PLAYING;
                    player_pause(&player);
                } else {
                    pre_preview_state = state;
                }
                player_close(&player);
                if (player_open(&player, str_arg) == 0) {
                    state = STATE_PREVIEWING;
                }
                break;

            case IPC_CMD_STOP_PREVIEW:
                if (state == STATE_PREVIEWING) {
                    player_close(&player);
                    if (pre_preview_state == STATE_PLAYING) {
                        start_current_track();
                    } else {
                        state = pre_preview_state;
                    }
                }
                break;

            case IPC_CMD_RESUME:
                if (state == STATE_PAUSED_AUTO) {
                    if (player_resume(&player) == 0) {
                        state = STATE_PLAYING;
                    } else {
                        player_close(&player);
                        start_current_track();
                    }
                    update_status();
                    fprintf(stderr, "menulody: resumed by hook\n");
                }
                break;

            case IPC_CMD_STATUS:
                update_status();
                break;

            default:
                break;
        }

        /* State machine: auto-pause/resume based on menu */
        switch (state) {
            case STATE_IDLE:
                if (playlist.count > 0 && menu_active)
                    start_current_track();
                break;

            case STATE_PLAYING:
                if (player_track_done(&player)) {
                    player_close(&player);
                    int next = playlist_next(&playlist);
                    if (next >= 0) {
                        start_current_track();
                    } else {
                        state = STATE_IDLE;
                        update_status();
                    }
                }
                /* Fallback: auto-pause if menu disappeared (hooks should handle this,
                   but monitor provides a safety net) */
                if (!menu_active) {
                    player_pause(&player);
                    state = STATE_PAUSED_AUTO;
                    update_status();
                    fprintf(stderr, "menulody: menu gone, auto-pausing\n");
                }
                break;

            case STATE_PAUSED_AUTO:
                /* Hooks will send RESUME, but monitor provides fallback */
                if (menu_active) {
                    if (player_resume(&player) == 0) {
                        state = STATE_PLAYING;
                    } else {
                        player_close(&player);
                        start_current_track();
                    }
                    update_status();
                    fprintf(stderr, "menulody: menu back, auto-resuming\n");
                }
                break;

            case STATE_PAUSED_MANUAL:
                /* Don't auto-resume, but auto-pause if game launches */
                if (!menu_active) {
                    player_pause(&player);
                    state = STATE_PAUSED_AUTO;
                }
                break;

            case STATE_PREVIEWING:
                if (player_track_done(&player)) {
                    /* Preview ended — go back to previous state */
                    player_close(&player);
                    if (pre_preview_state == STATE_PLAYING) {
                        start_current_track();
                    } else {
                        state = pre_preview_state;
                    }
                }
                break;
        }

        /* Overlay tick */
        overlay_tick(menu_active);

        /* Sleep ~100ms in 10ms increments (check quit_flag) */
        for (int i = 0; i < 10 && !quit_flag; i++)
            usleep(10000);
    }

cleanup:
    fprintf(stderr, "menulody: daemon shutting down\n");
    player_destroy(&player);
    config_save(&config);
    playlist_free(&playlist);
    overlay_cleanup();
    ipc_daemon_cleanup();
    return 0;
}
