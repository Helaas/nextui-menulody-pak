#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, msg) do { if (!(cond)) fail(msg, __LINE__); } while (0)

static void fail(const char *msg, int line) {
    fprintf(stderr, "ipc_tests:%d: %s\n", line, msg);
    exit(1);
}

static ipc_cmd_t read_command(int *out_int_arg, char *out_str_arg, int str_arg_size) {
    for (int i = 0; i < 50; i++) {
        ipc_cmd_t cmd = ipc_daemon_read(out_int_arg, out_str_arg, str_arg_size);
        if (cmd != IPC_CMD_NONE)
            return cmd;
        usleep(10000);
    }
    return IPC_CMD_NONE;
}

static void expect_no_command(void) {
    int int_arg = 0;
    char str_arg[IPC_STRING_ARG_MAX] = {0};

    for (int i = 0; i < 5; i++) {
        CHECK(ipc_daemon_read(&int_arg, str_arg, sizeof(str_arg)) == IPC_CMD_NONE,
              "unexpected command received");
        usleep(10000);
    }
}

int main(void) {
    char max_path[IPC_STRING_ARG_MAX];
    char too_long[IPC_STRING_ARG_MAX + 1];
    char expected[IPC_STRING_ARG_MAX];
    int int_arg = 0;
    ipc_cmd_t cmd;

    unlink(IPC_FIFO_PATH);
    unlink(IPC_STATUS_PATH);
    unlink(IPC_PID_PATH);
    unlink(IPC_LOCK_PATH);

    CHECK(ipc_daemon_init() == 0, "ipc_daemon_init failed");

    memset(max_path, 'a', sizeof(max_path) - 1);
    max_path[sizeof(max_path) - 1] = '\0';

    memset(too_long, 'b', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    CHECK(ipc_client_send(IPC_CMD_PAK_LAUNCH, 0) == 0,
          "PAK_LAUNCH send failed");
    cmd = read_command(&int_arg, expected, sizeof(expected));
    CHECK(cmd == IPC_CMD_PAK_LAUNCH, "PAK_LAUNCH command not received");

    memset(expected, 0, sizeof(expected));
    CHECK(ipc_client_send_str(IPC_CMD_PLAY_TRACK, max_path) == 0,
          "PLAY_TRACK send failed");
    cmd = read_command(&int_arg, expected, sizeof(expected));
    CHECK(cmd == IPC_CMD_PLAY_TRACK, "PLAY_TRACK command not received");
    CHECK(strcmp(expected, max_path) == 0, "PLAY_TRACK path mismatch");

    memset(expected, 0, sizeof(expected));
    CHECK(ipc_client_send_str(IPC_CMD_PREVIEW, max_path) == 0,
          "PREVIEW send failed");
    cmd = read_command(&int_arg, expected, sizeof(expected));
    CHECK(cmd == IPC_CMD_PREVIEW, "PREVIEW command not received");
    CHECK(strcmp(expected, max_path) == 0, "PREVIEW path mismatch");

    CHECK(ipc_client_send_str(IPC_CMD_PLAY_TRACK, too_long) == -1,
          "overflow PLAY_TRACK should fail");
    expect_no_command();

    CHECK(ipc_client_send_str(IPC_CMD_PREVIEW, too_long) == -1,
          "overflow PREVIEW should fail");
    expect_no_command();

    ipc_daemon_cleanup();

    puts("ipc_tests: ok");
    return 0;
}
