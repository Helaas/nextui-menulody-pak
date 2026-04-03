#!/bin/sh
set -eu

APP_BIN="menulody"
PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PAK_NAME=$(basename "$PAK_DIR")
PAK_NAME=${PAK_NAME%.pak}

cd "$PAK_DIR"

if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    export XDG_RUNTIME_DIR=/tmp/runtime-root
    mkdir -p "$XDG_RUNTIME_DIR"
fi

# Logging
if [ -n "${SHARED_USERDATA_PATH:-}" ]; then
    SHARED_USERDATA_ROOT="$SHARED_USERDATA_PATH"
elif [ -d "/mnt/SDCARD/.userdata/shared" ] || [ -d "/mnt/SDCARD" ]; then
    SHARED_USERDATA_ROOT="/mnt/SDCARD/.userdata/shared"
else
    SHARED_USERDATA_ROOT="${HOME:-/tmp}/.userdata/shared"
fi
LOG_ROOT=${LOGS_PATH:-"$SHARED_USERDATA_ROOT/logs"}
mkdir -p "$LOG_ROOT"
LOG_FILE="$LOG_ROOT/$APP_BIN.txt"
: >"$LOG_FILE"

exec >>"$LOG_FILE"
exec 2>&1

echo "=== Launching $PAK_NAME ($APP_BIN) at $(date) ==="
echo "platform=${PLATFORM:-unknown} device=${DEVICE:-unknown}"
echo "args: $*"

# Start daemon if not already running
if [ ! -f /tmp/menulody.pid ] || ! kill -0 "$(cat /tmp/menulody.pid 2>/dev/null)" 2>/dev/null; then
    ./$APP_BIN --daemon &
    sleep 0.3
fi

# Launch interactive UI
exec "./$APP_BIN" "$@"
