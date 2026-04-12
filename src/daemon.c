#include "daemon.h"
#include "player.h"
#include "playlist.h"
#include "monitor.h"
#include "ipc.h"
#include "config.h"
#include "hooks.h"
#include "source_state.h"
#include "strutil.h"
#include "varnish_client.h"

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
static source_state_t source_state;
static daemon_state_t state = STATE_IDLE;
static int          single_track_mode = 0;

/* Preview state */
static daemon_state_t pre_preview_state = STATE_IDLE;
static char           preview_track_name[256] = {0};
static char           preview_track_path[CONFIG_MAX_PATH] = {0};
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
static void start_current_track(void);
static void restore_pre_preview_state(void);
static void save_source_state(void);
static int activate_all_songs_source(void);
static int activate_named_source(const char *name);
static int activate_single_track_source(const char *path);
static int activate_saved_source_with_fallback(void);
static void set_menu_music_enabled_state(int enabled, int menu_active);

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

static void save_source_state(void) {
    if (source_state_save(&source_state) < 0)
        fprintf(stderr, "menulody: failed to save source state\n");
}

static int activate_all_songs_source(void) {
    playlist_free(&playlist);
    single_track_mode = 0;

    if (playlist_scan(&playlist, &config) < 0)
        return -1;

    source_state_set_all_songs(&source_state);
    return 0;
}

static int activate_named_source(const char *name) {
    if (!name || !name[0]) return activate_all_songs_source();

    playlist_free(&playlist);
    single_track_mode = 0;

    if (playlist_load_named(&playlist, name) < 0)
        return -1;

    apply_playlist_config_from_settings(&playlist);
    source_state_set_named_playlist(&source_state, name);
    return 0;
}

static int activate_single_track_source(const char *path) {
    if (!path || !path[0]) return -1;

    playlist_free(&playlist);
    single_track_mode = 0;

    if (playlist_load_single_track(&playlist, path) < 0)
        return -1;

    single_track_mode = 1;
    source_state_set_single_track(&source_state, path);
    return 0;
}

static int activate_saved_source_with_fallback(void) {
    int rc = -1;

    switch (source_state.mode) {
        case SOURCE_MODE_SINGLE_TRACK:
            rc = activate_single_track_source(source_state.track_path);
            if (rc == 0) return 0;
            break;
        case SOURCE_MODE_NAMED_PLAYLIST:
            rc = activate_named_source(source_state.playlist_name);
            if (rc == 0) return 0;
            break;
        case SOURCE_MODE_ALL_SONGS:
        case SOURCE_MODE_NONE:
        default:
            break;
    }

    if (activate_all_songs_source() == 0)
        return 0;

    source_state_set_none(&source_state);
    return -1;
}

static void restore_pre_preview_state(void) {
    preview_track_name[0] = '\0';
    preview_track_path[0] = '\0';

    if (pre_preview_state == STATE_PLAYING) {
        start_current_track();
    } else {
        state = pre_preview_state;
        update_status();
    }
}

static void start_current_track(void) {
    const char *path = playlist_current_path(&playlist);
    if (!path) {
        state = STATE_IDLE;
        update_status();
        return;
    }

    if (player_open(&player, path) < 0) {
        fprintf(stderr, "menulody: failed to open %s, skipping\n", path);
        int next = playlist_next(&playlist);
        if (next < 0) {
            state = STATE_IDLE;
            update_status();
            return;
        }
        path = playlist_current_path(&playlist);
        if (!path || player_open(&player, path) < 0) {
            state = STATE_IDLE;
            update_status();
            return;
        }
    }

    state = STATE_PLAYING;

    /* Show overlay notification once on first playback, then only when track changes. */
    const char *name = playlist_current_name(&playlist);
    if (name && path && config.overlay_duration > 0
        && (!overlay_has_shown || strcmp(last_overlay_track_path, path) != 0)) {
        (void)varnish_client_show_pill(name, config.overlay_duration);
        str_copy_trunc(last_overlay_track_path, sizeof(last_overlay_track_path), path);
        overlay_has_shown = 1;
    }

    update_status();
}

static void update_status(void) {
    const char *name = NULL;
    ipc_status_t st = {
        .playing      = (state == STATE_PLAYING || state == STATE_PREVIEWING) ? 1 : 0,
        .shuffle      = playlist.shuffle,
        .repeat       = playlist.repeat,
        .track_index  = playlist_current_index(&playlist),
        .track_count  = playlist.count,
        .volume       = config.volume,
        .previewing   = (state == STATE_PREVIEWING) ? 1 : 0,
        .menu_music_enabled = source_state.menu_music_enabled ? 1 : 0,
        .single_track = single_track_mode ? 1 : 0,
    };
    if (state == STATE_PREVIEWING && preview_track_name[0])
        name = preview_track_name;
    else
        name = playlist_current_name(&playlist);
    if (name) str_copy_trunc(st.track_name, sizeof(st.track_name), name);
    str_copy_trunc(st.playlist_name, sizeof(st.playlist_name), playlist.name);
    if (state == STATE_PREVIEWING && preview_track_path[0])
        str_copy_trunc(st.preview_path, sizeof(st.preview_path), preview_track_path);
    ipc_daemon_write_status(&st);
}

static void set_menu_music_enabled_state(int enabled, int menu_active) {
    if (enabled) {
        if (!source_state.menu_music_enabled) {
            source_state.menu_music_enabled = 1;
            save_source_state();
        }

        if (playlist.count <= 0) {
            state = STATE_IDLE;
            update_status();
            return;
        }

        if (menu_active) {
            if (player_resume(&player) == 0)
                state = STATE_PLAYING;
            else
                start_current_track();
            update_status();
        } else {
            state = STATE_PAUSED_AUTO;
            update_status();
        }
        return;
    }

    if (source_state.menu_music_enabled) {
        source_state.menu_music_enabled = 0;
        save_source_state();
    }

    if (state == STATE_PLAYING)
        player_pause(&player);

    state = STATE_PAUSED_MANUAL;
    update_status();
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

    /* Install hooks if not already present */
    hooks_install_playback();
    hooks_apply_config(config.auto_start);

    source_state_load(&source_state);

    /* Load the remembered playback source. */
    if (activate_saved_source_with_fallback() < 0) {
        fprintf(stderr, "menulody: no music found, waiting...\n");
        state = STATE_IDLE;
        save_source_state();
    } else {
        fprintf(stderr, "menulody: %d tracks loaded\n", playlist.count);
        state = source_state.menu_music_enabled ? STATE_IDLE : STATE_PAUSED_MANUAL;
    }
    update_status();

    /* ── Main loop ───────────────────────────────────────────── */
    while (!quit_flag) {
        int menu_active = monitor_is_menu_active();

        /* Process all pending IPC commands */
        for (;;) {
        int int_arg = 0;
        char str_arg[IPC_STRING_ARG_MAX] = {0};
        ipc_cmd_t cmd = ipc_daemon_read(&int_arg, str_arg, sizeof(str_arg));
        if (cmd == IPC_CMD_NONE) break;

        switch (cmd) {
            case IPC_CMD_QUIT:
                quit_flag = 1;
                break;

            case IPC_CMD_PLAY:
                set_menu_music_enabled_state(1, menu_active);
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
                set_menu_music_enabled_state(source_state.menu_music_enabled ? 0 : 1,
                                             menu_active);
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
                    else
                        update_status();
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
                else
                    update_status();
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
                else
                    update_status();
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

                if (playlist_current_path(&playlist))
                    str_copy_trunc(current_path, sizeof(current_path),
                                   playlist_current_path(&playlist));

                player_close(&player);
                config = config_load();
                player_set_volume(&player, config.volume);

                preview_track_name[0] = '\0';
                preview_track_path[0] = '\0';

                if (activate_saved_source_with_fallback() == 0) {
                    if (current_path[0]) select_current_track_by_path(&playlist, current_path);
                    save_source_state();

                    if (!source_state.menu_music_enabled) {
                        state = STATE_PAUSED_MANUAL;
                        update_status();
                    } else if (menu_active && old_state == STATE_PLAYING) {
                        start_current_track();
                    } else {
                        state = menu_active ? STATE_IDLE : STATE_PAUSED_AUTO;
                        update_status();
                    }
                } else {
                    state = STATE_IDLE;
                    save_source_state();
                    update_status();
                }
                break;
            }

            case IPC_CMD_PLAYLIST:
                player_close(&player);
                preview_track_name[0] = '\0';
                preview_track_path[0] = '\0';
                if (strcmp(str_arg, "all") == 0 || str_arg[0] == '\0') {
                    if (activate_all_songs_source() == 0) {
                        source_state.menu_music_enabled = 1;
                        save_source_state();
                        if (menu_active) {
                            start_current_track();
                        } else {
                            state = STATE_PAUSED_AUTO;
                            update_status();
                        }
                    } else {
                        source_state_set_none(&source_state);
                        save_source_state();
                        state = STATE_IDLE;
                        update_status();
                    }
                } else {
                    if (activate_named_source(str_arg) == 0) {
                        source_state.menu_music_enabled = 1;
                        save_source_state();
                        if (menu_active) {
                            start_current_track();
                        } else {
                            state = STATE_PAUSED_AUTO;
                            update_status();
                        }
                    } else {
                        source_state_set_none(&source_state);
                        save_source_state();
                        state = STATE_IDLE;
                        update_status();
                    }
                }
                break;

            case IPC_CMD_PLAY_TRACK:
                player_close(&player);
                preview_track_name[0] = '\0';
                preview_track_path[0] = '\0';
                if (activate_single_track_source(str_arg) == 0) {
                    source_state.menu_music_enabled = 1;
                    save_source_state();
                    if (menu_active) {
                        start_current_track();
                    } else {
                        state = STATE_PAUSED_AUTO;
                        update_status();
                    }
                } else {
                    source_state_set_none(&source_state);
                    save_source_state();
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
                } else if (state != STATE_PREVIEWING) {
                    pre_preview_state = state;
                }
                player_close(&player);
                if (player_open(&player, str_arg) == 0) {
                    load_track_name_from_path(str_arg, preview_track_name, sizeof(preview_track_name));
                    str_copy_trunc(preview_track_path, sizeof(preview_track_path), str_arg);
                    state = STATE_PREVIEWING;
                    update_status();
                } else {
                    restore_pre_preview_state();
                }
                break;

            case IPC_CMD_STOP_PREVIEW:
                if (state == STATE_PREVIEWING) {
                    player_close(&player);
                    restore_pre_preview_state();
                }
                break;

            case IPC_CMD_RESUME:
                if (state == STATE_PAUSED_AUTO && source_state.menu_music_enabled) {
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
        if (quit_flag) break;
        } /* end command drain loop */

        /* State machine: auto-pause/resume based on menu */
        switch (state) {
            case STATE_IDLE:
                if (source_state.menu_music_enabled && playlist.count > 0 && menu_active)
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
                if (source_state.menu_music_enabled && menu_active) {
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
                break;

            case STATE_PREVIEWING:
                if (player_track_done(&player)) {
                    /* Preview ended — go back to previous state */
                    player_close(&player);
                    restore_pre_preview_state();
                }
                break;
        }

        for (int i = 0; i < 10 && !quit_flag; i++)
            usleep(10000);
    }

cleanup:
    fprintf(stderr, "menulody: daemon shutting down\n");
    player_destroy(&player);
    config_save(&config);
    playlist_free(&playlist);
    (void)varnish_client_hide();
    ipc_daemon_cleanup();
    return 0;
}
