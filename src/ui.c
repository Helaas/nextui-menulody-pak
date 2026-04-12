#include "apostrophe.h"
#include "apostrophe_widgets.h"

#include "ui.h"
#include "ipc.h"
#include "config.h"
#include "playlist.h"
#include "hooks.h"
#include "source_state.h"
#include "strutil.h"
#include "varnish_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static int daemon_ready(void) {
    return ipc_daemon_running() && access(IPC_FIFO_PATH, W_OK) == 0;
}

static ipc_status_t poll_status(void) {
    ipc_status_t st = {0};
    if (!daemon_ready()) return st;

    /* Try reading the existing status file first — avoids sending a
       STATUS command that could crowd out real commands in the FIFO. */
    if (ipc_client_read_status(&st) == 0)
        return st;

    /* Status file missing; ask the daemon to write one. */
    for (int i = 0; i < 4; i++) {
        ipc_client_send(IPC_CMD_STATUS, 0);
        usleep(50000);
        if (ipc_client_read_status(&st) == 0)
            return st;
    }
    return st;
}

static ipc_status_t poll_status_fresh(void) {
    ipc_status_t st = {0};

    if (!daemon_ready()) return st;

    for (int i = 0; i < 6; i++) {
        ipc_client_send(IPC_CMD_STATUS, 0);
        usleep(20000);
        if (ipc_client_read_status(&st) == 0)
            return st;
    }

    return poll_status();
}

static void show_info_message(const char *message) {
    ap_message_opts msg = {.message = message};
    ap_confirm_result dummy;
    ap_confirmation(&msg, &dummy);
}

static int ensure_daemon_running(void) {
    if (daemon_ready()) return 0;

    if (!ipc_daemon_running() && ui_start_daemon_if_needed() < 0) {
        show_info_message("Could not start the Menulody daemon.");
        return -1;
    }

    for (int i = 0; i < 20; i++) {
        if (daemon_ready()) return 0;
        usleep(50000);
    }

    show_info_message("Could not start the Menulody daemon.");
    return -1;
}

static const char *playback_state_label(int daemon_running, const ipc_status_t *st) {
    if (!daemon_running) return "Off";
    if (st->previewing) return "Previewing";
    return st->playing ? "Playing" : "Paused";
}

static void load_track_name_from_path(const char *path, char *out, size_t size) {
    const char *slash;
    const char *filename;
    const char *dot;
    size_t len;

    if (!out || size == 0) return;

    out[0] = '\0';
    if (!path || !path[0]) return;

    slash = strrchr(path, '/');
    filename = slash ? slash + 1 : path;
    dot = strrchr(filename, '.');
    len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (len >= size) len = size - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
}

static void format_source_name(int daemon_running, const ipc_status_t *st,
                               char *out, size_t size) {
    if (!out || size == 0) return;

    if (st->single_track) {
        str_copy_trunc(out, size, "Single Song");
    } else if (st->playlist_name[0]) {
        str_copy_trunc(out, size, st->playlist_name);
    } else if (daemon_running && st->track_count > 0) {
        str_copy_trunc(out, size, "All Songs");
    } else {
        str_copy_trunc(out, size, "Not set");
    }
}

static void format_saved_source_name(const source_state_t *saved,
                                     char *out, size_t size) {
    if (!out || size == 0) return;

    out[0] = '\0';
    if (!saved) return;

    switch (saved->mode) {
        case SOURCE_MODE_ALL_SONGS:
            str_copy_trunc(out, size, "All Songs");
            break;
        case SOURCE_MODE_NAMED_PLAYLIST:
            str_copy_trunc(out, size, saved->playlist_name);
            break;
        default:
            break;
    }
}

static const char *preview_action_label(const ipc_status_t *st,
                                        const char *selected_path) {
    if (!st || !selected_path || !selected_path[0]) return "Preview";

    if (st->previewing && st->preview_path[0]) {
        if (strcmp(st->preview_path, selected_path) == 0)
            return "Stop Preview";
        return "Switch Preview";
    }

    return "Preview";
}

typedef struct {
    const char    **paths;
    int             path_count;
    ap_footer_item *footer;
    int             footer_index;
    uint32_t        last_status_poll_ms;
    ipc_status_t    status;
    int             have_status;
} preview_footer_context_t;

static void update_preview_footer(ap_list_opts *opts, int cursor, void *userdata) {
    preview_footer_context_t *ctx = userdata;
    const char *selected_path = NULL;
    uint32_t now = SDL_GetTicks();

    (void)opts;

    if (!ctx || !ctx->footer || ctx->footer_index < 0) return;

    if (cursor >= 0 && cursor < ctx->path_count)
        selected_path = ctx->paths[cursor];

    if (!ctx->have_status || now - ctx->last_status_poll_ms >= 50) {
        if (daemon_ready() && ipc_client_read_status(&ctx->status) == 0) {
            ctx->have_status = 1;
        } else {
            memset(&ctx->status, 0, sizeof(ctx->status));
            ctx->have_status = 1;
        }
        ctx->last_status_poll_ms = now;
    }

    ctx->footer[ctx->footer_index].label =
        preview_action_label(&ctx->status, selected_path);
}

static void wait_for_menu_music_enabled(int enabled) {
    if (!daemon_ready()) return;

    for (int i = 0; i < 10; i++) {
        ipc_status_t st = poll_status();
        if (st.menu_music_enabled == enabled)
            return;
        usleep(50000);
    }
}

static void wait_for_playlist_source(const char *expected_name) {
    if (!daemon_ready()) return;

    for (int i = 0; i < 10; i++) {
        ipc_status_t st = poll_status();
        char source_name[PLAYLIST_NAME_MAX];

        if (st.single_track) {
            usleep(50000);
            continue;
        }

        format_source_name(1, &st, source_name, sizeof(source_name));
        if (expected_name && strcmp(source_name, expected_name) == 0)
            return;

        usleep(50000);
    }
}

static void wait_for_preview_status(const char *path, int previewing) {
    if (!daemon_ready()) return;

    for (int i = 0; i < 20; i++) {
        ipc_status_t st = poll_status_fresh();

        if (!previewing) {
            if (!st.previewing)
                return;
        } else if (st.previewing && strcmp(st.preview_path, path) == 0) {
            return;
        }

        usleep(10000);
    }
}

static void wait_for_single_track_source(void) {
    if (!daemon_ready()) return;

    for (int i = 0; i < 10; i++) {
        ipc_status_t st = poll_status();
        if (st.single_track)
            return;
        usleep(50000);
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

static int named_playlist_contains_track(const named_playlist_t *playlist, const char *path) {
    if (!playlist || !path || !path[0]) return 0;

    for (int i = 0; i < playlist->count; i++) {
        if (playlist->tracks[i] && strcmp(playlist->tracks[i], path) == 0)
            return 1;
    }

    return 0;
}

static int selected_track_count(ap_list_item *items, int count) {
    int selected = 0;

    if (!items) return 0;

    for (int i = 0; i < count; i++) {
        if (items[i].selected) selected++;
    }

    return selected;
}

static int save_named_playlist_selection(const char *name, const playlist_t *lib,
                                         ap_list_item *items) {
    int selected = selected_track_count(items, lib ? lib->count : 0);
    named_playlist_t named = {0};
    char validation_error[128];

    if (!name || !name[0] || !lib || !items) return -1;
    if (!playlist_name_is_valid(name, validation_error, sizeof(validation_error))) {
        show_info_message(validation_error);
        return -1;
    }

    if (selected == 0) {
        show_info_message("Select at least one track before saving.");
        return -1;
    }

    str_copy_trunc(named.name, sizeof(named.name), name);
    named.tracks = calloc(selected, sizeof(char *));
    if (!named.tracks) {
        show_info_message("Could not allocate playlist tracks.");
        return -1;
    }

    for (int i = 0; i < lib->count; i++) {
        if (items[i].selected)
            named.tracks[named.count++] = strdup(lib->paths[i]);
    }

    if (named.count != selected) {
        playlist_named_free(&named);
        show_info_message("Could not build the playlist selection.");
        return -1;
    }

    if (playlist_named_save(&named) < 0) {
        playlist_named_free(&named);
        show_info_message("Could not save the playlist.");
        return -1;
    }

    playlist_named_free(&named);
    return 0;
}

static void show_varnish_overlay_warning_if_needed(const config_t *cfg,
                                                   const char *prefix) {
    varnish_client_status_t status;
    char status_text[256];
    char message[640];

    if (!cfg || cfg->overlay_duration <= 0) return;

    varnish_client_get_status(&status);
    if (varnish_client_is_enabled(&status)) return;

    varnish_client_format_status(&status, status_text, sizeof(status_text));
    snprintf(message, sizeof(message),
             "%s\n\n%s\n\nMusic playback will still work without it.",
             prefix ? prefix
                    : "Now Playing overlay is enabled, but Varnish is not fully enabled.",
             status_text);
    show_info_message(message);
}

static int prompt_for_playlist_name(char *out, size_t size) {
    char validation_error[128];

    if (!out || size == 0) return -1;

    for (;;) {
        ap_keyboard_result kb_result = {0};
        int rc = ap_keyboard("", "Playlist Name", AP_KB_GENERAL, &kb_result);
        if (rc != AP_OK || kb_result.text[0] == '\0') return -1;

        if (playlist_name_is_valid(kb_result.text,
                                   validation_error, sizeof(validation_error))) {
            str_copy_trunc(out, size, kb_result.text);
            return 0;
        }

        show_info_message(validation_error);
    }
}

static void toggle_preview_for_path(const char *path) {
    ipc_status_t st;

    if (!path || !path[0]) return;
    if (ensure_daemon_running() < 0) return;
    st = poll_status_fresh();

    if (st.previewing && st.preview_path[0]
        && strcmp(st.preview_path, path) == 0) {
        if (ipc_client_send(IPC_CMD_STOP_PREVIEW, 0) < 0) {
            show_info_message("Could not stop the current preview.");
            return;
        }
        wait_for_preview_status(path, 0);
    } else {
        if (ipc_client_send_str(IPC_CMD_PREVIEW, path) < 0) {
            show_info_message("Could not send that preview track to the Menulody daemon.");
            return;
        }
        wait_for_preview_status(path, 1);
    }
}

static void set_menu_music_enabled(int enabled) {
    ipc_status_t st;

    if (enabled) {
        if (ensure_daemon_running() < 0) return;
        st = poll_status();
        if (!st.menu_music_enabled) {
            ipc_client_send(IPC_CMD_TOGGLE, 0);
            wait_for_menu_music_enabled(1);
        }
        return;
    }

    if (!daemon_ready()) return;

    st = poll_status();
    if (st.previewing) {
        ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
        usleep(50000);
        st = poll_status();
    }
    if (st.menu_music_enabled) {
        ipc_client_send(IPC_CMD_TOGGLE, 0);
        wait_for_menu_music_enabled(0);
    }
}

static void toggle_menu_music(void) {
    ipc_status_t st;

    if (ensure_daemon_running() < 0) return;

    st = poll_status();
    if (st.previewing) {
        ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
        usleep(50000);
        st = poll_status();
    }

    set_menu_music_enabled(st.menu_music_enabled ? 0 : 1);
}

/* ── Playback Details Screen ───────────────────────────────────── */

static void show_playback_details(void) {
    for (;;) {
        int daemon_running = daemon_ready();
        ipc_status_t st = poll_status();
        config_t cfg = config_load();
        char source_name[PLAYLIST_NAME_MAX];
        format_source_name(daemon_running, &st, source_name, sizeof(source_name));

        char state_line[64];
        snprintf(state_line, sizeof(state_line), "State: %s",
                 playback_state_label(daemon_running, &st));

        char source_line[160];
        snprintf(source_line, sizeof(source_line), "Source: %s", source_name);

        char track_line[320];
        snprintf(track_line, sizeof(track_line), "Track: %s | Volume: %d%%",
                 st.track_name[0] ? st.track_name : "No track loaded",
                 daemon_running ? st.volume : cfg.volume);

        ap_list_item items[] = {
            {.label = state_line},
            {.label = source_line},
            {.label = track_line},
        };

        ap_list_opts opts = ap_list_default_opts("Details", items, 3);
        if (!daemon_running || st.previewing || st.single_track || st.track_count <= 0) {
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
            if (rc == AP_OK && result.action == AP_ACTION_SELECTED) {
                if (st.previewing) {
                    ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
                } else {
                    toggle_menu_music();
                }
            }
            continue;
        } else {
            ap_footer_item footer[] = {
                {AP_BTN_B, "Back", false, NULL},
                {AP_BTN_L2, "Prev", false, NULL},
                {AP_BTN_R2, "Next", false, NULL},
                {AP_BTN_A, "Play/Pause", true, NULL},
            };
            opts.footer = footer;
            opts.footer_count = 4;
            opts.action_button = AP_BTN_L2;
            opts.secondary_action_button = AP_BTN_R2;
            opts.tertiary_action_button = AP_BTN_NONE;
            opts.confirm_button = AP_BTN_NONE;
            ap_list_result result;
            int rc = ap_list(&opts, &result);

            if (rc == AP_CANCELLED) return;

            if (rc == AP_OK) {
                switch (result.action) {
                    case AP_ACTION_SELECTED:
                        toggle_menu_music();
                        break;
                    case AP_ACTION_TRIGGERED:
                        ipc_client_send(IPC_CMD_PREV, 0);
                        break;
                    case AP_ACTION_SECONDARY_TRIGGERED:
                        ipc_client_send(IPC_CMD_NEXT, 0);
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

/* ── Song Selection Screen ─────────────────────────────────────── */

static void show_song_selector(void) {
    config_t cfg = config_load();

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
    snprintf(header, sizeof(header), "Choose Song (%d)", lib.count);
    int last_index = 0;
    int last_visible = 0;
    preview_footer_context_t preview_ctx = {
        .paths = (const char **)lib.paths,
        .path_count = lib.count,
        .footer_index = 1,
    };

    for (;;) {
        ap_footer_item footer[] = {
            {AP_BTN_B, "Back", false, NULL},
            {AP_BTN_Y, "Preview", false, NULL},
            {AP_BTN_A, "Loop Song", true, NULL},
        };
        preview_ctx.footer = footer;

        ap_list_opts opts = ap_list_default_opts(header, items, lib.count);
        opts.footer = footer;
        opts.footer_count = 3;
        opts.secondary_action_button = AP_BTN_Y;
        opts.initial_index = last_index;
        opts.visible_start_index = last_visible;
        opts.footer_update = update_preview_footer;
        opts.footer_update_userdata = &preview_ctx;

        ap_list_result result;
        int rc = ap_list(&opts, &result);
        if (result.selected_index >= 0)
            last_index = result.selected_index;
        last_visible = result.visible_start_index;

        if (rc == AP_CANCELLED) break;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_SELECTED) {
                if (result.selected_index >= 0 && result.selected_index < lib.count
                    && ensure_daemon_running() == 0) {
                    if (ipc_client_send_str(IPC_CMD_PLAY_TRACK,
                                            lib.paths[result.selected_index]) < 0) {
                        show_info_message("Could not send that track to the Menulody daemon.");
                        continue;
                    }
                    wait_for_single_track_source();
                    wait_for_menu_music_enabled(1);
                    break;
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
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

static void show_playlist_editor(const char *playlist_name) {
    named_playlist_t existing = {0};
    char active_name[PLAYLIST_NAME_MAX] = {0};
    config_t cfg = config_load();
    playlist_t lib = {0};
    int editing = playlist_name && playlist_name[0];
    int rc;

    if (editing) {
        if (playlist_named_load(playlist_name, &existing) < 0) {
            show_info_message("Could not load that playlist.");
            return;
        }
        str_copy_trunc(active_name, sizeof(active_name),
                       existing.name[0] ? existing.name : playlist_name);
    } else {
        if (prompt_for_playlist_name(active_name, sizeof(active_name)) != 0)
            return;
    }

    if (playlist_scan(&lib, &cfg) < 0) {
        ap_message_opts msg = {.message = "No tracks found to add or edit."};
        ap_confirm_result dummy;
        ap_confirmation(&msg, &dummy);
        playlist_named_free(&existing);
        return;
    }

    ap_list_item *items = calloc(lib.count, sizeof(ap_list_item));
    if (!items) {
        playlist_named_free(&existing);
        playlist_free(&lib);
        return;
    }

    for (int i = 0; i < lib.count; i++) {
        items[i] = (ap_list_item){
            .label = lib.names[i],
            .selected = named_playlist_contains_track(&existing, lib.paths[i]),
        };
    }

    char header[128];
    if (editing) {
        snprintf(header, sizeof(header), "Edit Playlist \"%.*s\"",
                 (int)(sizeof(header) - sizeof("Edit Playlist \"\"")),
                 active_name);
    } else {
        snprintf(header, sizeof(header), "Select tracks for \"%.*s\"",
                 (int)(sizeof(header) - sizeof("Select tracks for \"\"")),
                 active_name);
    }
    int last_index = 0;
    int last_visible = 0;
    preview_footer_context_t preview_ctx = {
        .paths = (const char **)lib.paths,
        .path_count = lib.count,
        .footer_index = 2,
    };

    for (;;) {
        ap_footer_item footer[] = {
            {AP_BTN_B, "Cancel", false, NULL},
            {AP_BTN_A, "Toggle", false, NULL},
            {AP_BTN_Y, "Preview", false, NULL},
            {AP_BTN_START, "Save", true, NULL},
        };
        preview_ctx.footer = footer;

        ap_list_opts opts = ap_list_default_opts(header, items, lib.count);
        opts.footer = footer;
        opts.footer_count = 4;
        opts.multi_select = true;
        opts.confirm_button = AP_BTN_START;
        opts.secondary_action_button = AP_BTN_Y;
        opts.initial_index = last_index;
        opts.visible_start_index = last_visible;
        opts.footer_update = update_preview_footer;
        opts.footer_update_userdata = &preview_ctx;

        ap_list_result result;
        rc = ap_list(&opts, &result);
        if (result.selected_index >= 0)
            last_index = result.selected_index;
        last_visible = result.visible_start_index;

        if (rc == AP_CANCELLED) break;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_CONFIRMED) {
                if (save_named_playlist_selection(active_name, &lib, items) == 0)
                    break;
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
                if (result.selected_index >= 0 && result.selected_index < lib.count)
                    toggle_preview_for_path(lib.paths[result.selected_index]);
            }
        }
    }

    ipc_client_send(IPC_CMD_STOP_PREVIEW, 0);
    playlist_named_free(&existing);
    free(items);
    playlist_free(&lib);
}

static void show_create_playlist(void) {
    show_playlist_editor(NULL);
}

static void show_playlist_selector(void) {
    for (;;) {
        char **names = NULL;
        int count = 0;
        playlist_list_saved(&names, &count);

        int total = count + 2;
        ap_list_item *items = calloc(total, sizeof(ap_list_item));
        if (!items) {
            for (int i = 0; i < count; i++) free(names[i]);
            free(names);
            return;
        }

        items[0] = (ap_list_item){.label = "All Songs"};
        for (int i = 0; i < count; i++) {
            items[i + 1] = (ap_list_item){.label = names[i]};
        }
        items[total - 1] = (ap_list_item){.label = "+ Create New Playlist"};

        ap_footer_item footer[] = {
            {AP_BTN_B, "Back", false, NULL},
            {AP_BTN_X, "Delete", false, NULL},
            {AP_BTN_Y, "Edit", false, NULL},
            {AP_BTN_A, "Choose", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Choose Playlist", items, total);
        opts.footer = footer;
        opts.footer_count = 4;
        opts.secondary_action_button = AP_BTN_X;
        opts.tertiary_action_button = AP_BTN_Y;

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
                    if (ensure_daemon_running() == 0) {
                        ipc_client_send_str(IPC_CMD_PLAYLIST, "all");
                        wait_for_playlist_source("All Songs");
                        wait_for_menu_music_enabled(1);
                        free(items);
                        for (int i = 0; i < count; i++) free(names[i]);
                        free(names);
                        return;
                    }
                } else if (result.selected_index == total - 1) {
                    show_create_playlist();
                } else {
                    if (ensure_daemon_running() == 0) {
                        ipc_client_send_str(IPC_CMD_PLAYLIST, names[result.selected_index - 1]);
                        wait_for_playlist_source(names[result.selected_index - 1]);
                        wait_for_menu_music_enabled(1);
                        free(items);
                        for (int i = 0; i < count; i++) free(names[i]);
                        free(names);
                        return;
                    }
                }
            } else if (result.action == AP_ACTION_SECONDARY_TRIGGERED
                       && result.selected_index >= 1
                       && result.selected_index < total - 1) {
                char msg_text[256];
                snprintf(msg_text, sizeof(msg_text), "Delete playlist \"%.*s\"?",
                         (int)(sizeof(msg_text) - sizeof("Delete playlist \"\"?")),
                         names[result.selected_index - 1]);
                ap_message_opts msg = {.message = msg_text};
                ap_confirm_result cresult;
                if (ap_confirmation(&msg, &cresult) == AP_OK) {
                    playlist_named_delete(names[result.selected_index - 1]);
                }
            } else if (result.action == AP_ACTION_TERTIARY_TRIGGERED) {
                if (result.selected_index >= 1 && result.selected_index < total - 1) {
                    show_playlist_editor(names[result.selected_index - 1]);
                } else {
                    show_info_message("Select a saved playlist to edit.");
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

    show_varnish_overlay_warning_if_needed(
        &cfg,
        "Now Playing overlay is enabled, but Varnish is not fully enabled."
    );

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
            show_varnish_overlay_warning_if_needed(
                &cfg,
                "Settings saved, but Now Playing overlay will not appear until Varnish is fully enabled."
            );
            return;
        }
    }
}

/* ── Main Menu ─────────────────────────────────────────────────── */

void run_app(void) {
    /* Ensure hooks are installed on first run */
    hooks_install_playback();

    for (;;) {
        int daemon_running = daemon_ready();
        ipc_status_t st = poll_status();
        source_state_t saved = {0};
        char song_hint[256] = {0};
        char playlist_hint[PLAYLIST_NAME_MAX] = {0};
        int menu_music_enabled = daemon_running ? st.menu_music_enabled : 0;

        source_state_load(&saved);

        if (!st.previewing && st.single_track && st.track_name[0])
            str_copy_trunc(song_hint, sizeof(song_hint), st.track_name);
        else if (!daemon_running && saved.mode == SOURCE_MODE_SINGLE_TRACK && saved.track_path[0])
            load_track_name_from_path(saved.track_path, song_hint, sizeof(song_hint));

        if (!st.previewing && !st.single_track && daemon_running) {
            format_source_name(daemon_running, &st, playlist_hint, sizeof(playlist_hint));
            if (strcmp(playlist_hint, "Not set") == 0)
                playlist_hint[0] = '\0';
        } else if (!daemon_running) {
            format_saved_source_name(&saved, playlist_hint, sizeof(playlist_hint));
        }

        ap_list_item items[] = {
            {.label = "Menu Music", .trailing_text = menu_music_enabled ? "On" : "Off"},
            {.label = "Choose Song", .trailing_text = song_hint[0] ? song_hint : NULL},
            {.label = "Choose Playlist", .trailing_text = playlist_hint[0] ? playlist_hint : NULL},
            {.label = "Settings"},
        };

        ap_footer_item footer[] = {
            {AP_BTN_B, "Quit", false, NULL},
            {AP_BTN_Y, "Details", false, NULL},
            {AP_BTN_A, "Use", true, NULL},
        };

        ap_list_opts opts = ap_list_default_opts("Menulody", items, 4);
        opts.footer = footer;
        opts.footer_count = 3;
        opts.secondary_action_button = AP_BTN_Y;

        ap_list_result result;
        int rc = ap_list(&opts, &result);

        if (rc == AP_CANCELLED) return;

        if (rc == AP_OK) {
            if (result.action == AP_ACTION_SECONDARY_TRIGGERED) {
                show_playback_details();
                continue;
            }

            if (result.action == AP_ACTION_SELECTED) {
                switch (result.selected_index) {
                    case 0:
                        toggle_menu_music();
                        break;
                    case 1:
                        show_song_selector();
                        break;
                    case 2:
                        show_playlist_selector();
                        break;
                    case 3:
                        show_settings();
                        break;
                    default:
                        break;
                }
            }
        }
    }
}
