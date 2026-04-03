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
static int          single_track_mode = 0;

/* Preview state */
static daemon_state_t pre_preview_state = STATE_IDLE;
static char           preview_track_name[256] = {0};
static char           last_overlay_track_path[CONFIG_MAX_PATH] = {0};
static int            overlay_has_shown = 0;

/* ── Signal handler ─────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    quit_flag = 1;
}

/* ── Forward declarations ──────────────────────────────────────── */

static void update_status(void);
static void load_track_name_from_path(const char *path, char *out, int out_size);
static void select_current_track_by_path(playlist_t *pl, const char *path);
static void apply_playlist_config_from_settings(playlist_t *pl);

/* ── Helpers ────────────────────────────────────────────────────── */

static void load_track_name_from_path(const char *path, char *out, int out_size) {
    const char *slash;
    const char *filename;
    const char *dot;
    size_t len;

    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    slash = strrchr(path, '/');
    filename = slash ? slash + 1 : path;
    dot = strrchr(filename, '.');
    len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (len >= (size_t)out_size) len = (size_t)out_size - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
}

static void select_current_track_by_path(playlist_t *pl, const char *path) {
    if (!pl || !path || !path[0]) return;
    for (int i = 0; i < pl->count; i++) {
        if (pl->paths[i] && strcmp(pl->paths[i], path) == 0) {
            playlist_select(pl, i);
            return;
        }
    }
}

static void apply_playlist_config_from_settings(playlist_t *pl) {
    if (!pl || single_track_mode) return;

    if (pl->shuffle != (config.shuffle ? 1 : 0))
        playlist_shuffle_toggle(pl);
    pl->repeat = (int)config.repeat;
}

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

    /* Show overlay notification once on first playback, then only when track changes. */
    const char *name = playlist_current_name(&playlist);
    if (name && path && config.overlay_duration > 0
        && (!overlay_has_shown || strcmp(last_overlay_track_path, path) != 0)) {
        overlay_set_text(name, config.overlay_duration);
        str_copy_trunc(last_overlay_track_path, sizeof(last_overlay_track_path), path);
        overlay_has_shown = 1;
    }

    update_status();
}

static void update_status(void) {
    const char *name = NULL;
    ipc_status_t st = {
        .playing     = (state == STATE_PLAYING || state == STATE_PREVIEWING) ? 1 : 0,
        .shuffle     = playlist.shuffle,
        .repeat      = playlist.repeat,
        .track_index = playlist_current_index(&playlist),
        .track_count = playlist.count,
        .volume      = config.volume,
        .previewing  = (state == STATE_PREVIEWING) ? 1 : 0,
        .single_track = single_track_mode ? 1 : 0,
    };
    if (state == STATE_PREVIEWING && preview_track_name[0])
        name = preview_track_name;
    else
        name = playlist_current_name(&playlist);
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
                if (single_track_mode) {
                    update_status();
                    break;
                }
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
                if (single_track_mode) {
                    update_status();
                    break;
                }
                player_close(&player);
                playlist_prev(&playlist);
                if (state == STATE_PLAYING || menu_active)
                    start_current_track();
                break;

            case IPC_CMD_SELECT:
                if (single_track_mode) {
                    update_status();
                    break;
                }
                player_close(&player);
                playlist_select(&playlist, int_arg);
                if (state == STATE_PLAYING || menu_active)
                    start_current_track();
                break;

            case IPC_CMD_SHUFFLE:
                if (single_track_mode) {
                    update_status();
                    break;
                }
                playlist_shuffle_toggle(&playlist);
                config.shuffle = playlist.shuffle;
                config_save(&config);
                update_status();
                break;

            case IPC_CMD_REPEAT:
                if (single_track_mode) {
                    update_status();
                    break;
                }
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
            {
                daemon_state_t old_state = state;
                char current_path[CONFIG_MAX_PATH] = {0};
                char current_source[PLAYLIST_NAME_MAX] = {0};

                if (playlist_current_path(&playlist))
                    str_copy_trunc(current_path, sizeof(current_path), playlist_current_path(&playlist));
                str_copy_trunc(current_source, sizeof(current_source), playlist.name);

                player_close(&player);
                config = config_load();
                player_set_volume(&player, config.volume);

                if (single_track_mode) {
                    if (current_path[0] && playlist_load_single_track(&playlist, current_path) == 0) {
                        if (old_state == STATE_PLAYING || old_state == STATE_PREVIEWING) {
                            start_current_track();
                        } else {
                            state = old_state;
                            update_status();
                        }
                    } else {
                        playlist_free(&playlist);
                        state = STATE_IDLE;
                        update_status();
                    }
                } else if (strcmp(current_source, "All Songs") == 0 || current_source[0] == '\0') {
                    playlist_free(&playlist);
                    if (playlist_scan(&playlist, &config) == 0) {
                        if (current_path[0]) select_current_track_by_path(&playlist, current_path);
                        if (old_state == STATE_PLAYING || old_state == STATE_PREVIEWING) {
                            start_current_track();
                        } else {
                            state = old_state;
                            update_status();
                        }
                    } else {
                        state = STATE_IDLE;
                        update_status();
                    }
                } else {
                    playlist_free(&playlist);
                    if (playlist_load_named(&playlist, current_source) == 0) {
                        apply_playlist_config_from_settings(&playlist);
                        if (current_path[0]) select_current_track_by_path(&playlist, current_path);
                        if (old_state == STATE_PLAYING || old_state == STATE_PREVIEWING) {
                            start_current_track();
                        } else {
                            state = old_state;
                            update_status();
                        }
                    } else {
                        state = STATE_IDLE;
                        update_status();
                    }
                }
                break;
            }

            case IPC_CMD_PLAYLIST:
                player_close(&player);
                playlist_free(&playlist);
                single_track_mode = 0;
                preview_track_name[0] = '\0';
                if (strcmp(str_arg, "all") == 0 || str_arg[0] == '\0') {
                    if (playlist_scan(&playlist, &config) == 0) {
                        start_current_track();
                    } else {
                        state = STATE_IDLE;
                    }
                } else {
                    if (playlist_load_named(&playlist, str_arg) == 0) {
                        apply_playlist_config_from_settings(&playlist);
                        start_current_track();
                    } else {
                        state = STATE_IDLE;
                    }
                }
                update_status();
                break;

            case IPC_CMD_PLAY_TRACK:
                player_close(&player);
                playlist_free(&playlist);
                preview_track_name[0] = '\0';
                if (playlist_load_single_track(&playlist, str_arg) == 0) {
                    single_track_mode = 1;
                    start_current_track();
                } else {
                    single_track_mode = 0;
                    state = STATE_IDLE;
                    update_status();
                }
                break;

            case IPC_CMD_RELOAD_CONFIG:
                config = config_load();
                player_set_volume(&player, config.volume);
                apply_playlist_config_from_settings(&playlist);
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
                    load_track_name_from_path(str_arg, preview_track_name, sizeof(preview_track_name));
                    state = STATE_PREVIEWING;
                    update_status();
                }
                break;

            case IPC_CMD_STOP_PREVIEW:
                if (state == STATE_PREVIEWING) {
                    player_close(&player);
                    preview_track_name[0] = '\0';
                    if (pre_preview_state == STATE_PLAYING) {
                        start_current_track();
                    } else {
                        state = pre_preview_state;
                        update_status();
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
