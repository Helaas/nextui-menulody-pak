#ifndef MENULODY_IPC_H
#define MENULODY_IPC_H

#include "config.h"

#ifndef IPC_FIFO_PATH
#define IPC_FIFO_PATH    "/tmp/menulody.fifo"
#endif
#ifndef IPC_STATUS_PATH
#define IPC_STATUS_PATH  "/tmp/menulody_status"
#endif
#ifndef IPC_PID_PATH
#define IPC_PID_PATH     "/tmp/menulody.pid"
#endif
#ifndef IPC_LOCK_PATH
#define IPC_LOCK_PATH    "/tmp/menulody.lock"
#endif

#define IPC_STRING_ARG_MAX      CONFIG_MAX_PATH
#define IPC_STRING_CMD_BUF_SIZE (sizeof("PLAY_TRACK ") + IPC_STRING_ARG_MAX + 1)

/* Commands sent from UI/hooks to daemon via FIFO */
typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_PLAY,
    IPC_CMD_PAUSE,
    IPC_CMD_PAK_LAUNCH,  /* allow playback to continue during a pak launch */
    IPC_CMD_UI_PLAY,      /* play current source from inside Menulody UI */
    IPC_CMD_UI_PAUSE,     /* pause current source from inside Menulody UI */
    IPC_CMD_TOGGLE,       /* play/pause */
    IPC_CMD_NEXT,
    IPC_CMD_PREV,
    IPC_CMD_SELECT,       /* select track by index */
    IPC_CMD_SHUFFLE,
    IPC_CMD_REPEAT,       /* cycle repeat mode */
    IPC_CMD_VOLUME,       /* set volume 0-100 */
    IPC_CMD_PLAYLIST,     /* switch playlist by name */
    IPC_CMD_PLAY_TRACK,   /* switch to a single-song looping source by path */
    IPC_CMD_RESCAN,
    IPC_CMD_RELOAD_CONFIG, /* apply config changes without rebuilding source */
    IPC_CMD_PREVIEW,      /* preview a track by path */
    IPC_CMD_STOP_PREVIEW,
    IPC_CMD_RESUME,       /* resume from hook pause */
    IPC_CMD_STATUS,       /* request status write */
    IPC_CMD_QUIT,
} ipc_cmd_t;

/* Status written by daemon to IPC_STATUS_PATH */
typedef struct {
    int    playing;          /* 1 = playing, 0 = paused */
    int    shuffle;          /* 1 = on */
    int    repeat;           /* 0=off, 1=one, 2=all */
    int    track_index;      /* current track index */
    int    track_count;      /* total tracks */
    int    volume;           /* 0-100 */
    int    previewing;       /* 1 = preview currently playing */
    int    menu_music_enabled; /* 1 = remembered menu music source is enabled */
    int    single_track;     /* 1 = active source is a single looping track */
    char   track_name[256];  /* display name of current track */
    char   playlist_name[128]; /* name of active playlist */
    char   preview_path[IPC_STRING_ARG_MAX]; /* full path of active preview track */
} ipc_status_t;

/* Daemon side: create FIFO, open for non-blocking read */
int  ipc_daemon_init(void);
/* Daemon side: read next command (non-blocking).
   If cmd has an arg, it is stored in out_str_arg (for string args) or out_int_arg. */
ipc_cmd_t ipc_daemon_read(int *out_int_arg, char *out_str_arg, int str_arg_size);
/* Daemon side: write status file */
void ipc_daemon_write_status(const ipc_status_t *status);
/* Daemon side: cleanup FIFO and PID file */
void ipc_daemon_cleanup(void);
/* Daemon side: write PID file */
void ipc_write_pid(void);

/* Client side: send a command to the daemon (int arg) */
int  ipc_client_send(ipc_cmd_t cmd, int arg);
/* Client side: send a command with a string argument */
int  ipc_client_send_str(ipc_cmd_t cmd, const char *str_arg);
/* Client side: read the latest status. Returns 0 on success. */
int  ipc_client_read_status(ipc_status_t *status);
/* Client side: check if daemon is running */
int  ipc_daemon_running(void);

#endif /* MENULODY_IPC_H */
