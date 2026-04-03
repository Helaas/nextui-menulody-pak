#include "apostrophe.h"
#include "apostrophe_widgets.h"

#include "ui.h"
#include "ipc.h"
#include "config.h"
#include "playlist.h"
#include "hooks.h"

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

/* ── Now Playing Screen ────────────────────────────────────────── */

static void show_now_playing(void) {
    for (;;) {
        ipc_status_t st = poll_status();

        char title[300];
        if (st.track_name[0])
            snprintf(title, sizeof(title), "%s", st.track_name);
        else
            snprintf(title, sizeof(title), "No track");

        char subtitle[128];
        snprintf(subtitle, sizeof(subtitle), "%s | Shuffle: %s | Repeat: %s",
                 st.playing ? "Playing" : "Paused",
                 st.shuffle ? "On" : "Off",
                 repeat_label(st.repeat));

        char track_info[64];
        if (st.track_count > 0)
            snprintf(track_info, sizeof(track_info), "Track %d / %d | Volume: %d%%",
                     st.track_index + 1, st.track_count, st.volume);
        else
            snprintf(track_info, sizeof(track_info), "No tracks loaded");

        ap_list_item items[] = {
            {.label = title},
            {.label = subtitle},
            {.label = track_info},
        };

        ap_footer_item footer[] = {
            {AP_BTN_A, "Play/Pause", false, NULL},
            {AP_BTN_L1, "Prev", false, NULL},
            {AP_BTN_R1, "Next", false, NULL},
            {AP_BTN_X, "Shuffle", false, NULL},
            {AP_BTN_Y, "Repeat", false, NULL},
            {AP_BTN_B, "Back", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Now Playing", items, 3);
        opts.footer = footer;
        opts.footer_count = 6;
        opts.action_button = AP_BTN_L1;
        opts.secondary_action_button = AP_BTN_R1;
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
                    /* L1 — previous track */
                    ipc_client_send(IPC_CMD_PREV, 0);
                    break;
                case AP_ACTION_SECONDARY_TRIGGERED:
                    /* R1 — next track */
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
        ap_footer_item footer[] = {
            {AP_BTN_A, "Play", false, NULL},
            {AP_BTN_Y, "Preview", false, NULL},
            {AP_BTN_B, "Back", true, NULL},
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
                /* Play selected track */
                ipc_client_send(IPC_CMD_SELECT, result.selected_index);
                break;
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
                /* Preview selected track */
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    ipc_client_send_str(IPC_CMD_PREVIEW, lib.paths[result.selected_index]);
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
    snprintf(header, sizeof(header), "Select tracks for \"%s\"", kb_result.text);

    for (;;) {
        ap_footer_item footer[] = {
            {AP_BTN_A, "Toggle", false, NULL},
            {AP_BTN_Y, "Preview", false, NULL},
            {AP_BTN_START, "Save", true, NULL},
            {AP_BTN_B, "Cancel", true, NULL},
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
                    snprintf(named.name, sizeof(named.name), "%s", kb_result.text);
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
                /* Preview focused track */
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    ipc_client_send_str(IPC_CMD_PREVIEW, lib.paths[result.selected_index]);
            }
        }
    }

    /* Stop preview if running */
    ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);

    free(items);
    playlist_free(&lib);
}

static void show_playlist_detail(const char *name) {
    named_playlist_t named;
    if (playlist_named_load(name, &named) < 0) return;

    if (named.count == 0) {
        ap_message_opts msg = {.message = "This playlist is empty."};
        ap_confirm_result dummy;
        ap_confirmation(&msg, &dummy);
        playlist_named_free(&named);
        return;
    }

    /* Build list items from track paths */
    ap_list_item *items = calloc(named.count, sizeof(ap_list_item));
    char **display_names = calloc(named.count, sizeof(char *));
    if (!items || !display_names) {
        free(items); free(display_names);
        playlist_named_free(&named);
        return;
    }

    for (int i = 0; i < named.count; i++) {
        const char *slash = strrchr(named.tracks[i], '/');
        const char *fname = slash ? slash + 1 : named.tracks[i];
        /* Strip extension */
        const char *dot = strrchr(fname, '.');
        size_t len = dot ? (size_t)(dot - fname) : strlen(fname);
        display_names[i] = malloc(len + 1);
        if (display_names[i]) {
            memcpy(display_names[i], fname, len);
            display_names[i][len] = '\0';
        } else {
            display_names[i] = strdup(fname);
        }
        items[i] = (ap_list_item){.label = display_names[i] ? display_names[i] : fname};
    }

    ap_footer_item footer[] = {
        {AP_BTN_A, "Play", false, NULL},
        {AP_BTN_B, "Back", true, NULL},
    };

    ap_list_opts opts = ap_list_default_opts(name, items, named.count);
    opts.footer = footer;
    opts.footer_count = 2;

    ap_list_result result;
    int rc = ap_list(&opts, &result);

    if (rc == AP_OK && result.action == AP_ACTION_SELECTED) {
        /* Switch to this playlist and play selected track */
        ipc_client_send_str(IPC_CMD_PLAYLIST, name);
        usleep(200000);
        ipc_client_send(IPC_CMD_SELECT, result.selected_index);
    }

    for (int i = 0; i < named.count; i++) free(display_names[i]);
    free(display_names);
    free(items);
    playlist_named_free(&named);
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
            {AP_BTN_A, "Select", false, NULL},
            {AP_BTN_X, "Delete", false, NULL},
            {AP_BTN_B, "Back", true, NULL},
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
                    /* All Songs */
                    ipc_client_send_str(IPC_CMD_PLAYLIST, "all");
                } else if (result.selected_index == 1) {
                    show_create_playlist();
                } else {
                    show_playlist_detail(names[result.selected_index - 2]);
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED
                       && result.selected_index >= 2) {
                /* Delete playlist */
                char msg_text[256];
                snprintf(msg_text, sizeof(msg_text),
                         "Delete playlist \"%s\"?", names[result.selected_index - 2]);
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
            {AP_BTN_A, "Select", false, NULL},
            {AP_BTN_X, "Remove", false, NULL},
            {AP_BTN_B, "Back", true, NULL},
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
                        snprintf(cfg->music_dirs[cfg->music_dir_count], CONFIG_MAX_PATH,
                                 "%s", fp_result.path);
                        cfg->music_dir_count++;
                        config_save(cfg);
                        /* Tell daemon to rescan */
                        ipc_client_send(IPC_CMD_RESCAN, 0);
                    }
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED
                       && result.selected_index >= 0
                       && result.selected_index < cfg->music_dir_count) {
                /* Remove folder */
                if (cfg->music_dir_count > 1) {
                    for (int i = result.selected_index; i < cfg->music_dir_count - 1; i++) {
                        snprintf(cfg->music_dirs[i], CONFIG_MAX_PATH, "%s", cfg->music_dirs[i + 1]);
                    }
                    cfg->music_dir_count--;
                    config_save(cfg);
                    ipc_client_send(IPC_CMD_RESCAN, 0);
                }
            }
        }
    }
}

static void show_settings(void) {
    config_t cfg = config_load();

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
        {.label = "Now Playing Overlay", .type = AP_OPT_STANDARD,
         .options = overlay_opts,        .option_count = 5,
         .selected_option = cfg.overlay_duration == 0 ? 0 :
                           cfg.overlay_duration <= 3 ? 1 :
                           cfg.overlay_duration <= 5 ? 2 :
                           cfg.overlay_duration <= 10 ? 3 : 4},
    };

    for (;;) {
        ap_options_list_opts opts = {
            .title = "Settings",
            .items = items,
            .item_count = 7,
        };
        ap_options_list_result result;
        int rc = ap_options_list(&opts, &result);

        /* Read back current values on every exit */
        cfg.shuffle = items[1].selected_option == 1;
        cfg.repeat = (repeat_mode_t)items[2].selected_option;
        cfg.volume = items[3].selected_option * 10;
        cfg.pause_on_pak = items[4].selected_option == 1;
        cfg.auto_start = items[5].selected_option == 1;

        int overlay_values[] = {0, 3, 5, 10, 9999};
        cfg.overlay_duration = overlay_values[items[6].selected_option];

        config_save(&cfg);

        /* Apply changes to daemon */
        ipc_client_send(IPC_CMD_VOLUME, cfg.volume);
        hooks_apply_config(cfg.auto_start);

        if (rc == AP_CANCELLED) break;

        if (rc == AP_OK && result.focused_index == 0) {
            /* Music Folders clicked */
            show_music_folders(&cfg);
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
            snprintf(now_playing_hint, sizeof(now_playing_hint), "%s", st.track_name);
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
            {AP_BTN_A, "Select", false, NULL},
            {AP_BTN_B, "Quit", true, NULL},
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
