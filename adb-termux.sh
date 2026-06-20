#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CERT_DIR="$HOME/.termux-adb-bridge/certs"
PORT="${BRIDGE_PORT:-10099}"
ADDR="127.0.0.1"

CURL_CA=""
CURL_CERT=""
CURL_KEY=""

case "${1:-}" in
    --help|-h|help)
        cat <<EOF
Usage: adb-termux <command> [args...]

Commands:
  shell <cmd>        Execute command via bridge (POST /api/exec)
  exec <cmd>         Same as shell
  stream <cmd>       Execute with streaming output (POST /api/exec/stream)
  push <src> <dst>   Upload file to device (POST /api/upload)
  pull <src>         Download file from device (GET /api/download)
  health             Check bridge health (GET /api/health)
  help               Show this help

Environment:
  BRIDGE_PORT        Override port (default: 10099)
  BRIDGE_CERT_DIR    Override cert directory (default: ~/.termux-adb-bridge/certs)

Examples:
  adb-termux shell echo hello
  adb-termux shell uptime
  adb-termux push ~/file.txt /sdcard/file.txt
  adb-termux pull /sdcard/file.txt > ~/file.txt
  adb-termux health
EOF
        exit 0
        ;;
esac

find_certs() {
    local fp_dir
    fp_dir=$(ls -d "$CERT_DIR"/*/ 2>/dev/null | head -1)
    if [ -n "$fp_dir" ]; then
        CURL_CA="$fp_dir/ca.crt"
        CURL_CERT="$fp_dir/client.crt"
        CURL_KEY="$fp_dir/client.key"
        if [ -f "$CURL_CA" ] && [ -f "$CURL_CERT" ] && [ -f "$CURL_KEY" ]; then
            return 0
        fi
    fi

    # Fallback to root of cert dir
    CURL_CA="$CERT_DIR/ca.crt"
    CURL_CERT="$CERT_DIR/client.crt"
    CURL_KEY="$CERT_DIR/client.key"
    if [ -f "$CURL_CA" ] && [ -f "$CURL_CERT" ] && [ -f "$CURL_KEY" ]; then
        return 0
    fi

    echo "Error: certificates not found in $CERT_DIR" >&2
    echo "Run inject.sh first to deploy the bridge." >&2
    exit 1
}

find_certs

curl_bridge() {
    local method="$1" path="$2" data="$3" extra="$4"
    local curl_args=(-s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY")
    curl_args+=(-X "$method")
    if [ -n "$data" ]; then
        curl_args+=(-d "$data")
    fi
    if [ -n "$extra" ]; then
        # shellcheck disable=SC2086
        curl_args+=($extra)
    fi
    curl_args+=("https://${ADDR}:${PORT}${path}")
    curl "${curl_args[@]}"
}

case "$1" in
    shell|exec)
        shift
        if [ $# -eq 0 ]; then
            echo "Usage: adb-termux shell <command>" >&2
            exit 1
        fi
        cmd="$*"
        # Escape for JSON
        escaped=$(printf '%s' "$cmd" | sed 's/"/\\"/g')
        curl_bridge POST "/api/exec" "{\"command\":\"$escaped\"}"
        echo
        ;;

    stream)
        shift
        if [ $# -eq 0 ]; then
            echo "Usage: adb-termux stream <command>" >&2
            exit 1
        fi
        cmd="$*"
        escaped=$(printf '%s' "$cmd" | sed 's/"/\\"/g')
        curl_bridge POST "/api/exec/stream" "{\"command\":\"$escaped\"" "-N"
        echo
        ;;

    push)
        if [ $# -lt 3 ]; then
            echo "Usage: adb-termux push <local-file> <remote-path>" >&2
            exit 1
        fi
        src="$2"
        dst="$3"
        if [ ! -f "$src" ]; then
            echo "Error: file not found: $src" >&2
            exit 1
        fi
        curl_bridge POST "/api/upload?path=$dst" "" "--data-binary @$src"
        echo
        ;;

    pull)
        if [ $# -lt 2 ]; then
            echo "Usage: adb-termux pull <remote-path>" >&2
            exit 1
        fi
        src="$2"
        curl_bridge GET "/api/download?path=$src"
        echo
        ;;

    health)
        curl_bridge GET "/api/health"
        echo
        ;;

    *)
        echo "Unknown command: $1" >&2
        echo "Run '$0 --help' for usage." >&2
        exit 1
        ;;
esac