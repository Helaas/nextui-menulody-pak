#include "ipc.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

/* ── Daemon side ────────────────────────────────────────────────── */

static int fifo_fd = -1;
static int fifo_wr_fd = -1;

int ipc_daemon_init(void) {
    unlink(IPC_FIFO_PATH);
    if (mkfifo(IPC_FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        perror("menulody: mkfifo");
        return -1;
    }

    /* Open read-end non-blocking. Also open a write-end so the read
       never returns EOF when the last writer closes. */
    fifo_fd = open(IPC_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("menulody: open fifo (rd)");
        return -1;
    }
    int wr = open(IPC_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (wr < 0) {
        perror("menulody: open fifo (wr-keepalive)");
    }
    fifo_wr_fd = wr;

    return 0;
}

#define IPC_READ_BUF_SIZE 2048

ipc_cmd_t ipc_daemon_read(int *out_int_arg, char *out_str_arg, int str_arg_size) {
    static char buf[IPC_READ_BUF_SIZE];
    static int  buf_len = 0;

    if (fifo_fd < 0) return IPC_CMD_NONE;

    if (out_int_arg) *out_int_arg = 0;
    if (out_str_arg && str_arg_size > 0) out_str_arg[0] = '\0';

    /* Append any new data from the FIFO into the persistent buffer */
    if (buf_len < (int)sizeof(buf) - 1) {
        ssize_t n = read(fifo_fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
        if (n > 0) buf_len += (int)n;
    }

    if (buf_len <= 0) return IPC_CMD_NONE;
    buf[buf_len] = '\0';

    /* Extract the first complete newline-terminated command */
    char *nl = strchr(buf, '\n');
    if (!nl) return IPC_CMD_NONE;

    *nl = '\0';
    char line[IPC_STRING_CMD_BUF_SIZE];
    size_t line_len = (size_t)(nl - buf);
    if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    /* Shift remaining data to the front of the buffer */
    int consumed = (int)(nl - buf) + 1;
    buf_len -= consumed;
    if (buf_len > 0)
        memmove(buf, nl + 1, buf_len);

    if (strcmp(line, "PLAY")         == 0) return IPC_CMD_PLAY;
    if (strcmp(line, "PAUSE")        == 0) return IPC_CMD_PAUSE;
    if (strcmp(line, "UI_PLAY")      == 0) return IPC_CMD_UI_PLAY;
    if (strcmp(line, "UI_PAUSE")     == 0) return IPC_CMD_UI_PAUSE;
    if (strcmp(line, "TOGGLE")       == 0) return IPC_CMD_TOGGLE;
    if (strcmp(line, "NEXT")         == 0) return IPC_CMD_NEXT;
    if (strcmp(line, "PREV")         == 0) return IPC_CMD_PREV;
    if (strcmp(line, "SHUFFLE")      == 0) return IPC_CMD_SHUFFLE;
    if (strcmp(line, "REPEAT")       == 0) return IPC_CMD_REPEAT;
    if (strcmp(line, "RESCAN")       == 0) return IPC_CMD_RESCAN;
    if (strcmp(line, "RELOAD_CONFIG") == 0) return IPC_CMD_RELOAD_CONFIG;
    if (strcmp(line, "STOP_PREVIEW") == 0) return IPC_CMD_STOP_PREVIEW;
    if (strcmp(line, "RESUME")       == 0) return IPC_CMD_RESUME;
    if (strcmp(line, "STATUS")       == 0) return IPC_CMD_STATUS;
    if (strcmp(line, "QUIT")         == 0) return IPC_CMD_QUIT;

    if (strncmp(line, "SELECT ", 7) == 0) {
        if (out_int_arg) *out_int_arg = atoi(line + 7);
        return IPC_CMD_SELECT;
    }
    if (strncmp(line, "VOLUME ", 7) == 0) {
        if (out_int_arg) *out_int_arg = atoi(line + 7);
        return IPC_CMD_VOLUME;
    }
    if (strncmp(line, "PLAYLIST ", 9) == 0) {
        if (out_str_arg && str_arg_size > 0)
            str_copy_trunc(out_str_arg, (size_t)str_arg_size, line + 9);
        return IPC_CMD_PLAYLIST;
    }
    if (strncmp(line, "PLAY_TRACK ", 11) == 0) {
        if (out_str_arg && str_arg_size > 0)
            str_copy_trunc(out_str_arg, (size_t)str_arg_size, line + 11);
        return IPC_CMD_PLAY_TRACK;
    }
    if (strncmp(line, "PREVIEW ", 8) == 0) {
        if (out_str_arg && str_arg_size > 0)
            str_copy_trunc(out_str_arg, (size_t)str_arg_size, line + 8);
        return IPC_CMD_PREVIEW;
    }

    return IPC_CMD_NONE;
}

void ipc_daemon_write_status(const ipc_status_t *st) {
    char tmp_path[sizeof(IPC_STATUS_PATH) + 4];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", IPC_STATUS_PATH);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fprintf(f, "playing=%d\n", st->playing);
    fprintf(f, "shuffle=%d\n", st->shuffle);
    fprintf(f, "repeat=%d\n", st->repeat);
    fprintf(f, "track_index=%d\n", st->track_index);
    fprintf(f, "track_count=%d\n", st->track_count);
    fprintf(f, "volume=%d\n", st->volume);
    fprintf(f, "previewing=%d\n", st->previewing);
    fprintf(f, "menu_music_enabled=%d\n", st->menu_music_enabled);
    fprintf(f, "single_track=%d\n", st->single_track);
    fprintf(f, "track_name=%s\n", st->track_name);
    fprintf(f, "playlist=%s\n", st->playlist_name);
    fprintf(f, "preview_path=%s\n", st->preview_path);
    fclose(f);
    rename(tmp_path, IPC_STATUS_PATH);
}

void ipc_daemon_cleanup(void) {
    if (fifo_wr_fd >= 0) { close(fifo_wr_fd); fifo_wr_fd = -1; }
    if (fifo_fd >= 0) { close(fifo_fd); fifo_fd = -1; }
    unlink(IPC_FIFO_PATH);
    unlink(IPC_PID_PATH);
    unlink(IPC_STATUS_PATH);
    unlink(IPC_LOCK_PATH);
}

void ipc_write_pid(void) {
    FILE *f = fopen(IPC_PID_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

/* ── Client side ────────────────────────────────────────────────── */

static int send_raw(const char *msg) {
    int fd = open(IPC_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(msg);
    ssize_t written = write(fd, msg, len);
    close(fd);
    return (written == len) ? 0 : -1;
}

int ipc_client_send(ipc_cmd_t cmd, int arg) {
    char buf[256];
    const char *str = NULL;
    switch (cmd) {
        case IPC_CMD_PLAY:         str = "PLAY\n";         break;
        case IPC_CMD_PAUSE:        str = "PAUSE\n";        break;
        case IPC_CMD_UI_PLAY:      str = "UI_PLAY\n";      break;
        case IPC_CMD_UI_PAUSE:     str = "UI_PAUSE\n";     break;
        case IPC_CMD_TOGGLE:       str = "TOGGLE\n";       break;
        case IPC_CMD_NEXT:         str = "NEXT\n";         break;
        case IPC_CMD_PREV:         str = "PREV\n";         break;
        case IPC_CMD_SHUFFLE:      str = "SHUFFLE\n";      break;
        case IPC_CMD_REPEAT:       str = "REPEAT\n";       break;
        case IPC_CMD_RESCAN:       str = "RESCAN\n";       break;
        case IPC_CMD_RELOAD_CONFIG:str = "RELOAD_CONFIG\n"; break;
        case IPC_CMD_STOP_PREVIEW: str = "STOP_PREVIEW\n"; break;
        case IPC_CMD_RESUME:       str = "RESUME\n";       break;
        case IPC_CMD_STATUS:       str = "STATUS\n";       break;
        case IPC_CMD_QUIT:         str = "QUIT\n";         break;
        case IPC_CMD_SELECT:
            snprintf(buf, sizeof(buf), "SELECT %d\n", arg);
            str = buf;
            break;
        case IPC_CMD_VOLUME:
            snprintf(buf, sizeof(buf), "VOLUME %d\n", arg);
            str = buf;
            break;
        default:
            return -1;
    }
    return send_raw(str);
}

int ipc_client_send_str(ipc_cmd_t cmd, const char *str_arg) {
    char buf[IPC_STRING_CMD_BUF_SIZE];
    int written;

    if (!str_arg) return -1;
    if (strlen(str_arg) >= IPC_STRING_ARG_MAX) return -1;

    switch (cmd) {
        case IPC_CMD_PLAYLIST:
            written = snprintf(buf, sizeof(buf), "PLAYLIST %s\n", str_arg);
            break;
        case IPC_CMD_PLAY_TRACK:
            written = snprintf(buf, sizeof(buf), "PLAY_TRACK %s\n", str_arg);
            break;
        case IPC_CMD_PREVIEW:
            written = snprintf(buf, sizeof(buf), "PREVIEW %s\n", str_arg);
            break;
        default:
            return -1;
    }

    if (written < 0 || (size_t)written >= sizeof(buf))
        return -1;

    return send_raw(buf);
}

int ipc_client_read_status(ipc_status_t *st) {
    memset(st, 0, sizeof(*st));
    FILE *f = fopen(IPC_STATUS_PATH, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "playing=%d", &st->playing) == 1) continue;
        if (sscanf(line, "shuffle=%d", &st->shuffle) == 1) continue;
        if (sscanf(line, "repeat=%d", &st->repeat) == 1) continue;
        if (sscanf(line, "track_index=%d", &st->track_index) == 1) continue;
        if (sscanf(line, "track_count=%d", &st->track_count) == 1) continue;
        if (sscanf(line, "volume=%d", &st->volume) == 1) continue;
        if (sscanf(line, "previewing=%d", &st->previewing) == 1) continue;
        if (sscanf(line, "menu_music_enabled=%d", &st->menu_music_enabled) == 1) continue;
        if (sscanf(line, "single_track=%d", &st->single_track) == 1) continue;
        if (strncmp(line, "track_name=", 11) == 0) {
            str_copy_trunc(st->track_name, sizeof(st->track_name), line + 11);
            continue;
        }
        if (strncmp(line, "playlist=", 9) == 0) {
            str_copy_trunc(st->playlist_name, sizeof(st->playlist_name), line + 9);
            continue;
        }
        if (strncmp(line, "preview_path=", 13) == 0) {
            str_copy_trunc(st->preview_path, sizeof(st->preview_path), line + 13);
            continue;
        }
    }

    fclose(f);
    return 0;
}

int ipc_daemon_running(void) {
    FILE *f = fopen(IPC_PID_PATH, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return 0; }
    fclose(f);
    if (pid <= 0) return 0;
    return (kill((pid_t)pid, 0) == 0) ? 1 : 0;
}
