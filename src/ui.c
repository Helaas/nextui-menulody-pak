#include "apostrophe.h"
#include "apostrophe_widgets.h"

#include "ui.h"
#include "ipc.h"
#include "config.h"
#include "playlist.h"
#include "hooks.h"
#include "strutil.h"
#include "varnish_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static ipc_status_t poll_status(void) {
    ipc_status_t st = {0};
    ipc_client_send(IPC_CMD_STATUS, 0);
    usleep(50000); /* 50ms for daemon to write */
    ipc_client_read_status(&st);
    return st;
}

static const char *repeat_label(int mode) {
    switch (mode) {
        case 0: return "Off";
        case 1: return "One";
        case 2: return "All";
        default: return "Off";
    }
}

static void sync_settings_items_to_config(config_t *cfg, ap_options_item *items) {
    int overlay_values[] = {0, 3, 5, 10, 9999};

    cfg->shuffle = items[1].selected_option == 1;
    cfg->repeat = (repeat_mode_t)items[2].selected_option;
    cfg->volume = items[3].selected_option * 10;
    cfg->pause_on_pak = items[4].selected_option == 1;
    cfg->auto_start = items[5].selected_option == 1;
    cfg->overlay_duration = overlay_values[items[6].selected_option];
}

static int music_folders_changed(const config_t *before, const config_t *after) {
    if (before->music_dir_count != after->music_dir_count) return 1;
    for (int i = 0; i < before->music_dir_count; i++) {
        if (strcmp(before->music_dirs[i], after->music_dirs[i]) != 0)
            return 1;
    }
    return 0;
}

static void toggle_preview_for_path(const char *path) {
    ipc_status_t st = poll_status();
    if (st.previewing) {
        ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
    } else if (path && path[0]) {
        ipc_client_send_str(IPC_CMD_PREVIEW, path);
    }
}

static void show_info_message(const char *message) {
    ap_message_opts msg = {.message = message};
    ap_confirm_result dummy;
    ap_confirmation(&msg, &dummy);
}

/* ── Now Playing Screen ────────────────────────────────────────── */

static void show_now_playing(void) {
    for (;;) {
        ipc_status_t st = poll_status();

        char title[300];
        if (st.track_name[0])
            str_copy_trunc(title, sizeof(title), st.track_name);
        else
            snprintf(title, sizeof(title), "No track");

        char subtitle[128];
        if (st.previewing) {
            str_copy_trunc(subtitle, sizeof(subtitle), "Previewing | Source: ");
            str_append(subtitle, sizeof(subtitle),
                       st.playlist_name[0] ? st.playlist_name : "Unknown");
        } else if (st.single_track) {
            snprintf(subtitle, sizeof(subtitle), "%s | Single Song Loop",
                     st.playing ? "Playing" : "Paused");
        } else {
            snprintf(subtitle, sizeof(subtitle), "%s | Shuffle: %s | Repeat: %s",
                     st.playing ? "Playing" : "Paused",
                     st.shuffle ? "On" : "Off",
                     repeat_label(st.repeat));
        }

        char track_info[96];
        if (st.single_track) {
            snprintf(track_info, sizeof(track_info), "Looping selected song | Volume: %d%%",
                     st.volume);
        } else if (st.track_count > 0) {
            snprintf(track_info, sizeof(track_info), "Track %d / %d | Volume: %d%%",
                     st.track_index + 1, st.track_count, st.volume);
        } else {
            snprintf(track_info, sizeof(track_info), "No tracks loaded");
        }

        ap_list_item items[] = {
            {.label = title},
            {.label = subtitle},
            {.label = track_info},
        };

        ap_list_opts opts = ap_list_default_opts("Now Playing", items, 3);
        if (st.single_track) {
            ap_footer_item footer[] = {
                {AP_BTN_B, "Back", false, NULL},
                {AP_BTN_A, "Play/Pause", true, NULL},
            };
            opts.footer = footer;
            opts.footer_count = 2;
            opts.action_button = AP_BTN_NONE;
            opts.secondary_action_button = AP_BTN_NONE;
            opts.tertiary_action_button = AP_BTN_NONE;
            opts.confirm_button = AP_BTN_NONE;

            ap_list_result result;
            int rc = ap_list(&opts, &result);

            if (rc == AP_CANCELLED) return;
            if (rc == AP_OK && result.action == AP_ACTION_SELECTED)
                ipc_client_send(IPC_CMD_TOGGLE, 0);
            continue;
        } else {
            ap_footer_item footer[] = {
                {AP_BTN_B, "Back", false, NULL},
                {AP_BTN_L2, "Prev", false, NULL},
                {AP_BTN_R2, "Next", false, NULL},
                {AP_BTN_X, "Shuffle", false, NULL},
                {AP_BTN_Y, "Repeat", false, NULL},
                {AP_BTN_A, "Play/Pause", true, NULL},
            };
            opts.footer = footer;
            opts.footer_count = 6;
            opts.action_button = AP_BTN_L2;
            opts.secondary_action_button = AP_BTN_R2;
            opts.tertiary_action_button = AP_BTN_X;
            opts.confirm_button = AP_BTN_Y;
            ap_list_result result;
            int rc = ap_list(&opts, &result);

            if (rc == AP_CANCELLED) return;

            if (rc == AP_OK) {
                switch (result.action) {
                    case AP_ACTION_SELECTED:
                        /* A — toggle play/pause */
                        ipc_client_send(IPC_CMD_TOGGLE, 0);
                        break;
                    case AP_ACTION_TRIGGERED:
                        /* L2 — previous track */
                        ipc_client_send(IPC_CMD_PREV, 0);
                        break;
                    case AP_ACTION_SECONDARY_TRIGGERED:
                        /* R2 — next track */
                        ipc_client_send(IPC_CMD_NEXT, 0);
                        break;
                    case AP_ACTION_TERTIARY_TRIGGERED:
                        /* X — toggle shuffle */
                        ipc_client_send(IPC_CMD_SHUFFLE, 0);
                        break;
                    case AP_ACTION_CONFIRMED:
                        /* Y — cycle repeat */
                        ipc_client_send(IPC_CMD_REPEAT, 0);
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

/* ── Library Screen ────────────────────────────────────────────── */

static void show_library(void) {
    config_t cfg = config_load();

    /* Scan library locally for display */
    playlist_t lib = {0};
    if (playlist_scan(&lib, &cfg) < 0) {
        ap_message_opts msg = {
            .message = "No MP3 or WAV files found in your music folders.\n"
                       "Add music to /mnt/SDCARD/Music/ and try again.",
        };
        ap_confirm_result dummy;
        ap_confirmation(&msg, &dummy);
        return;
    }

    /* Build list items */
    ap_list_item *items = calloc(lib.count, sizeof(ap_list_item));
    if (!items) { playlist_free(&lib); return; }

    for (int i = 0; i < lib.count; i++) {
        items[i] = (ap_list_item){.label = lib.names[i]};
    }

    char header[64];
    snprintf(header, sizeof(header), "Library (%d tracks)", lib.count);

    for (;;) {
        ipc_status_t st = poll_status();
        const char *preview_label = st.previewing ? "Stop Preview" : "Preview";
        ap_footer_item footer[] = {
            {AP_BTN_B, "Back", false, NULL},
            {AP_BTN_Y, preview_label, false, NULL},
            {AP_BTN_A, "Play", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts(header, items, lib.count);
        opts.footer = footer;
        opts.footer_count = 3;
        opts.secondary_action_button = AP_BTN_Y;

        ap_list_result result;
        int rc = ap_list(&opts, &result);

        if (rc == AP_CANCELLED) break;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_SELECTED) {
                /* Start single-track loop mode */
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    ipc_client_send_str(IPC_CMD_PLAY_TRACK, lib.paths[result.selected_index]);
                break;
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
                /* Preview toggle */
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    toggle_preview_for_path(lib.paths[result.selected_index]);
            }
        }
    }

    ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
    free(items);
    playlist_free(&lib);
}

/* ── Playlists Screen ──────────────────────────────────────────── */

static void show_create_playlist(void) {
    /* Get playlist name via keyboard */
    ap_keyboard_result kb_result;
    int rc = ap_keyboard("", "Playlist Name", AP_KB_GENERAL, &kb_result);
    if (rc != AP_OK || kb_result.text[0] == '\0') return;

    /* Scan library for track selection */
    config_t cfg = config_load();
    playlist_t lib = {0};
    if (playlist_scan(&lib, &cfg) < 0) {
        ap_message_opts msg = {.message = "No tracks found to add."};
        ap_confirm_result dummy;
        ap_confirmation(&msg, &dummy);
        return;
    }

    /* Build multi-select list items */
    ap_list_item *items = calloc(lib.count, sizeof(ap_list_item));
    if (!items) { playlist_free(&lib); return; }
    for (int i = 0; i < lib.count; i++) {
        items[i] = (ap_list_item){.label = lib.names[i]};
    }

    char header[128];
    snprintf(header, sizeof(header), "Select tracks for \"%.*s\"",
             (int)(sizeof(header) - sizeof("Select tracks for \"\"")),
             kb_result.text);

    for (;;) {
        ipc_status_t st = poll_status();
        const char *preview_label = st.previewing ? "Stop Preview" : "Preview";
        ap_footer_item footer[] = {
            {AP_BTN_B, "Cancel", false, NULL},
            {AP_BTN_A, "Toggle", false, NULL},
            {AP_BTN_Y, preview_label, false, NULL},
            {AP_BTN_START, "Save", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts(header, items, lib.count);
        opts.footer = footer;
        opts.footer_count = 4;
        opts.multi_select = true;
        opts.confirm_button = AP_BTN_START;
        opts.secondary_action_button = AP_BTN_Y;

        ap_list_result result;
        rc = ap_list(&opts, &result);

        if (rc == AP_CANCELLED) break;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_CONFIRMED) {
                /* Save playlist — collect selected tracks from items */
                int sel_count = 0;
                for (int i = 0; i < lib.count; i++) {
                    if (items[i].selected) sel_count++;
                }

                if (sel_count > 0) {
                    named_playlist_t named = {0};
                    str_copy_trunc(named.name, sizeof(named.name), kb_result.text);
                    named.tracks = calloc(sel_count, sizeof(char *));
                    if (named.tracks) {
                        for (int i = 0; i < lib.count; i++) {
                            if (items[i].selected)
                                named.tracks[named.count++] = strdup(lib.paths[i]);
                        }
                        playlist_named_save(&named);
                        playlist_named_free(&named);
                    }
                }
                break;
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
                /* Preview toggle */
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    toggle_preview_for_path(lib.paths[result.selected_index]);
            }
        }
    }

    /* Stop preview if running */
    ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);

    free(items);
    playlist_free(&lib);
}

static void show_playlists(void) {
    for (;;) {
        char **names = NULL;
        int count = 0;
        playlist_list_saved(&names, &count);

        /* +2 for "All Songs" and "Create New" */
        int total = count + 2;
        ap_list_item *items = calloc(total, sizeof(ap_list_item));
        if (!items) {
            for (int i = 0; i < count; i++) free(names[i]);
            free(names);
            return;
        }

        items[0] = (ap_list_item){.label = "All Songs"};
        items[1] = (ap_list_item){.label = "+ Create New Playlist"};
        for (int i = 0; i < count; i++) {
            items[i + 2] = (ap_list_item){.label = names[i]};
        }

        ap_footer_item footer[] = {
            {AP_BTN_B, "Back", false, NULL},
            {AP_BTN_X, "Delete", false, NULL},
            {AP_BTN_A, "Select", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Playlists", items, total);
        opts.footer = footer;
        opts.footer_count = 3;
        opts.secondary_action_button = AP_BTN_X;

        ap_list_result result;
        int rc = ap_list(&opts, &result);

        if (rc == AP_CANCELLED) {
            free(items);
            for (int i = 0; i < count; i++) free(names[i]);
            free(names);
            return;
        }

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_SELECTED) {
                if (result.selected_index == 0) {
                    /* All Songs source */
                    ipc_client_send_str(IPC_CMD_PLAYLIST, "all");
                    free(items);
                    for (int i = 0; i < count; i++) free(names[i]);
                    free(names);
                    return;
                } else if (result.selected_index == 1) {
                    show_create_playlist();
                } else {
                    ipc_client_send_str(IPC_CMD_PLAYLIST, names[result.selected_index - 2]);
                    free(items);
                    for (int i = 0; i < count; i++) free(names[i]);
                    free(names);
                    return;
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED
                       && result.selected_index >= 2) {
                /* Delete playlist */
                char msg_text[256];
                snprintf(msg_text, sizeof(msg_text), "Delete playlist \"%.*s\"?",
                         (int)(sizeof(msg_text) - sizeof("Delete playlist \"\"?")),
                         names[result.selected_index - 2]);
                ap_message_opts msg = {.message = msg_text};
                ap_confirm_result cresult;
                if (ap_confirmation(&msg, &cresult) == AP_OK) {
                    playlist_named_delete(names[result.selected_index - 2]);
                }
            }
        }

        free(items);
        for (int i = 0; i < count; i++) free(names[i]);
        free(names);
    }
}

/* ── Settings Screen ───────────────────────────────────────────── */

static void show_music_folders(config_t *cfg) {
    for (;;) {
        int total = cfg->music_dir_count + 1; /* +1 for "Add Folder" */
        ap_list_item *items = calloc(total, sizeof(ap_list_item));
        if (!items) return;

        for (int i = 0; i < cfg->music_dir_count; i++) {
            items[i] = (ap_list_item){.label = cfg->music_dirs[i]};
        }
        items[cfg->music_dir_count] = (ap_list_item){.label = "+ Add Folder"};

        ap_footer_item footer[] = {
            {AP_BTN_B, "Back", false, NULL},
            {AP_BTN_X, "Remove", false, NULL},
            {AP_BTN_A, "Select", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Music Folders", items, total);
        opts.footer = footer;
        opts.footer_count = 3;
        opts.secondary_action_button = AP_BTN_X;

        ap_list_result result;
        int rc = ap_list(&opts, &result);
        free(items);

        if (rc == AP_CANCELLED) return;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_SELECTED
                && result.selected_index == cfg->music_dir_count) {
                /* Add folder via file picker */
                if (cfg->music_dir_count < CONFIG_MAX_MUSIC_DIRS) {
                    ap_file_picker_opts fp_opts = ap_file_picker_default_opts("Select Music Folder");
                    fp_opts.mode = AP_FILE_PICKER_DIRS;
                    ap_file_picker_result fp_result;
                    if (ap_file_picker(&fp_opts, &fp_result) == AP_OK) {
                        str_copy_trunc(cfg->music_dirs[cfg->music_dir_count], CONFIG_MAX_PATH,
                                       fp_result.path);
                        cfg->music_dir_count++;
                    }
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED
                       && result.selected_index >= 0
                       && result.selected_index < cfg->music_dir_count) {
                /* Remove folder */
                if (cfg->music_dir_count > 1) {
                    for (int i = result.selected_index; i < cfg->music_dir_count - 1; i++) {
                        str_copy_trunc(cfg->music_dirs[i], CONFIG_MAX_PATH,
                                       cfg->music_dirs[i + 1]);
                    }
                    cfg->music_dir_count--;
                }
            }
        }
    }
}

static void show_settings(void) {
    config_t original = config_load();
    config_t cfg = original;

    /* Build ap_option arrays for each setting */
    ap_option shuffle_opts[] = {{.label = "Off"}, {.label = "On"}};
    ap_option repeat_opts[]  = {{.label = "Off"}, {.label = "One"}, {.label = "All"}};
    ap_option volume_opts[]  = {
        {.label = "0"},  {.label = "10"}, {.label = "20"}, {.label = "30"},
        {.label = "40"}, {.label = "50"}, {.label = "60"}, {.label = "70"},
        {.label = "80"}, {.label = "90"}, {.label = "100"},
    };
    ap_option pause_pak_opts[]  = {{.label = "No"}, {.label = "Yes"}};
    ap_option auto_start_opts[] = {{.label = "No"}, {.label = "Yes"}};
    ap_option overlay_opts[]    = {
        {.label = "Off"}, {.label = "3s"}, {.label = "5s"},
        {.label = "10s"}, {.label = "Always"},
    };

    ap_options_item items[] = {
        {.label = "Music Folders",       .type = AP_OPT_CLICKABLE},
        {.label = "Shuffle",             .type = AP_OPT_STANDARD,
         .options = shuffle_opts,        .option_count = 2,
         .selected_option = cfg.shuffle ? 1 : 0},
        {.label = "Repeat",              .type = AP_OPT_STANDARD,
         .options = repeat_opts,         .option_count = 3,
         .selected_option = (int)cfg.repeat},
        {.label = "Volume",              .type = AP_OPT_STANDARD,
         .options = volume_opts,         .option_count = 11,
         .selected_option = cfg.volume / 10},
        {.label = "Pause on Pak Launch", .type = AP_OPT_STANDARD,
         .options = pause_pak_opts,      .option_count = 2,
         .selected_option = cfg.pause_on_pak ? 1 : 0},
        {.label = "Auto-Start Daemon",   .type = AP_OPT_STANDARD,
         .options = auto_start_opts,     .option_count = 2,
         .selected_option = cfg.auto_start ? 1 : 0},
        {.label = "Now Playing Overlay (Varnish)", .type = AP_OPT_STANDARD,
         .options = overlay_opts,        .option_count = 5,
         .selected_option = cfg.overlay_duration == 0 ? 0 :
                           cfg.overlay_duration <= 3 ? 1 :
                           cfg.overlay_duration <= 5 ? 2 :
                           cfg.overlay_duration <= 10 ? 3 : 4},
        {.label = "Stop Daemon (Session)", .type = AP_OPT_CLICKABLE},
    };

    int last_cursor = 0;
    int last_visible = 0;
    for (;;) {
        int daemon_running = ipc_daemon_running();
        items[7].label = daemon_running
            ? "Stop Daemon (Session)"
            : "Daemon Stopped (Session)";

        ap_footer_item footer[] = {
            {AP_BTN_B, "Cancel", false, NULL},
            {AP_BTN_START, "Save", true, NULL},
        };
        ap_options_list_opts opts = {
            .title = "Settings",
            .items = items,
            .item_count = 8,
            .footer = footer,
            .footer_count = 2,
            .confirm_button = AP_BTN_START,
            .initial_selected_index = last_cursor,
            .visible_start_index = last_visible,
        };
        ap_options_list_result result;
        int rc = ap_options_list(&opts, &result);
        last_cursor = result.focused_index;
        last_visible = result.visible_start_index;
        sync_settings_items_to_config(&cfg, items);

        if (rc == AP_CANCELLED) return;

        if (rc == AP_OK && result.action == AP_ACTION_SELECTED
            && result.focused_index == 0) {
            show_music_folders(&cfg);
            continue;
        }

        if (rc == AP_OK && result.action == AP_ACTION_SELECTED
            && result.focused_index == 7) {
            if (daemon_running) {
                ipc_client_send(IPC_CMD_QUIT, 0);
                usleep(150000);
                show_info_message("Daemon stopped for this session.\n"
                                  "Launching Menulody again will start it back up.");
            } else {
                show_info_message("Daemon is already stopped for this session.");
            }
            continue;
        }

        if (rc == AP_OK && result.action == AP_ACTION_CONFIRMED) {
            int folders_changed = music_folders_changed(&original, &cfg);
            config_save(&cfg);
            if (ipc_daemon_running()) {
                if (folders_changed)
                    ipc_client_send(IPC_CMD_RESCAN, 0);
                else
                    ipc_client_send(IPC_CMD_RELOAD_CONFIG, 0);
            }
            hooks_apply_config(cfg.auto_start);
            if (cfg.overlay_duration > 0 &&
                (!varnish_client_is_installed() || !varnish_client_is_running())) {
                show_info_message("Settings saved.\n\n"
                                  "Now Playing overlay requires Varnish.pak to be installed and enabled.\n"
                                  "Music playback will still work without it.");
            }
            return;
        }
    }
}

/* ── Main Menu ─────────────────────────────────────────────────── */

void run_app(void) {
    /* Ensure hooks are installed on first run */
    hooks_install_playback();

    for (;;) {
        ipc_status_t st = {0};
        ipc_client_read_status(&st);

        char now_playing_hint[128];
        if (st.track_name[0]) {
            str_copy_trunc(now_playing_hint, sizeof(now_playing_hint), st.track_name);
        } else {
            snprintf(now_playing_hint, sizeof(now_playing_hint), "Not playing");
        }

        ap_list_item items[] = {
            {.label = "Now Playing", .trailing_text = now_playing_hint},
            {.label = "Library"},
            {.label = "Playlists"},
            {.label = "Settings"},
        };

        ap_footer_item footer[] = {
            {AP_BTN_B, "Quit", false, NULL},
            {AP_BTN_A, "Select", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Menulody", items, 4);
        opts.footer = footer;
        opts.footer_count = 2;

        ap_list_result result;
        int rc = ap_list(&opts, &result);

        if (rc == AP_CANCELLED) return;

        if (rc == AP_OK && result.action == AP_ACTION_SELECTED) {
            switch (result.selected_index) {
                case 0: show_now_playing(); break;
                case 1: show_library(); break;
                case 2: show_playlists(); break;
                case 3: show_settings(); break;
            }
        }
    }
}
