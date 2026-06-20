#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/termux-adb-bridge"
DEFAULT_PORT=10099
DEFAULT_DEVICE=""

PORT="$DEFAULT_PORT"
DEVICE=""
CA_CERT=""
CLIENT_CERT=""
CLIENT_KEY=""
ADDR="127.0.0.1"

usage() {
    cat <<EOF
Termux ADB Bridge — Desktop-side CLI

Usage: bridge.sh <command> [options]

Commands:
  install              Build, push, and start the daemon on device
  exec     <cmd>       Run a shell command via the bridge
  exec-st  <cmd>       Run a command with streaming output
  upload   <src> <dst> Upload a local file to the device
  download <src>       Download a file from the device to stdout
  health               Check bridge health

Global options:
  -p, --port <port>    Port (default: $DEFAULT_PORT)
  -d, --device <id>    ADB device serial
  --ca <file>          CA certificate path
  --cert <file>        Client certificate path
  --key <file>         Client key path
  -h, --help           Show this help

Environment:
  ADB_DEVICE           Default ADB device serial
  BRIDGE_PORT          Default port (overrides -p default)
  BRIDGE_CA            Default CA cert path
  BRIDGE_CERT          Default client cert path
  BRIDGE_KEY           Default client key path
EOF
    exit 0
}

# Parse global options (before command)
GLOBAL_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -d|--device) DEVICE="$2"; shift 2 ;;
        --ca) CA_CERT="$2"; shift 2 ;;
        --cert) CLIENT_CERT="$2"; shift 2 ;;
        --key) CLIENT_KEY="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) GLOBAL_ARGS+=("$1"); shift ;;
    esac
done
set -- "${GLOBAL_ARGS[@]}"

# Fallback to env
: "${PORT:=${BRIDGE_PORT:-$DEFAULT_PORT}}"
: "${DEVICE:=${ADB_DEVICE:-}}"
: "${CA_CERT:=${BRIDGE_CA:-}}"
: "${CLIENT_CERT:=${BRIDGE_CERT:-}}"
: "${CLIENT_KEY:=${BRIDGE_KEY:-}}"

# Build adb prefix
ADB_CMD="adb ${DEVICE:+-s $DEVICE}"

curl_bridge() {
    local method="$1" path="$2" data="$3" extra="$4"
    local curl_args=(-s)
    [[ -n "$CA_CERT" ]] && curl_args+=(--cacert "$CA_CERT")
    [[ -n "$CLIENT_CERT" ]] && curl_args+=(--cert "$CLIENT_CERT")
    [[ -n "$CLIENT_KEY" ]] && curl_args+=(--key "$CLIENT_KEY")
    curl_args+=(-X "$method")
    [[ -n "$data" ]] && curl_args+=(-d "$data")
    [[ -n "$extra" ]] && curl_args+=($extra)
    curl_args+=("https://${ADDR}:${PORT}${path}")
    curl "${curl_args[@]}"
}

cmd_install() {
    echo "=== Installing Termux ADB Bridge ==="
    echo ""

    # Determine certs directory on device
    local device_cert_dir="/data/local/tmp/bridge-certs"
    local binary="/data/local/tmp/termux-adb-bridge"

    if [[ ! -f "$BINARY" ]]; then
        echo "Error: binary not found at $BINARY. Run install.sh on device first."
        exit 1
    fi

    # Find certs
    local local_cert_dir="${HOME}/.termux-adb-bridge/certs"
    if [[ -z "$CA_CERT" ]]; then
        CA_CERT="$local_cert_dir/ca.crt"
        CLIENT_CERT="$local_cert_dir/client.crt"
        CLIENT_KEY="$local_cert_dir/client.key"
    fi

    if [[ ! -f "$CA_CERT" || ! -f "$CLIENT_CERT" || ! -f "$CLIENT_KEY" ]]; then
        echo "Error: client certificates not found."
        echo "Generate them by running install.sh on the device first."
        exit 1
    fi

    echo "[1/4] Pushing binary to device..."
    $ADB_CMD push "$BINARY" /data/local/tmp/termux-adb-bridge
    $ADB_CMD shell chmod +x /data/local/tmp/termux-adb-bridge

    echo "[2/4] Pushing certificates to device..."
    $ADB_CMD shell mkdir -p "$device_cert_dir"
    $ADB_CMD push "$local_cert_dir/ca.crt"     "$device_cert_dir/ca.crt"
    $ADB_CMD push "$local_cert_dir/server.crt" "$device_cert_dir/server.crt"
    $ADB_CMD push "$local_cert_dir/server.key" "$device_cert_dir/server.key"

    echo "[3/4] Starting daemon on device..."
    $ADB_CMD shell "setsid /data/local/tmp/termux-adb-bridge --daemon --cert-dir $device_cert_dir > /dev/null 2>&1 &"
    sleep 1

    echo "[4/4] Setting up port forwarding..."
    $ADB_CMD forward tcp:$PORT tcp:$PORT

    echo ""
    echo "Bridge is running on 127.0.0.1:$PORT"
    echo "Client certs on desktop:"
    echo "  CA:   $CA_CERT"
    echo "  Cert: $CLIENT_CERT"
    echo "  Key:  $CLIENT_KEY"
    echo ""
    echo "Try: $0 health"
}

cmd_exec() {
    if [[ $# -lt 1 ]]; then
        echo "Usage: $0 exec <command>"
        exit 1
    fi
    local cmd="$*"
    curl_bridge POST "/api/exec" "{\"command\":\"${cmd//\"/\\\"}\"}"
    echo
}

cmd_exec_st() {
    if [[ $# -lt 1 ]]; then
        echo "Usage: $0 exec-st <command>"
        exit 1
    fi
    local cmd="$*"
    local curl_args=(-s -N)
    [[ -n "$CA_CERT" ]] && curl_args+=(--cacert "$CA_CERT")
    [[ -n "$CLIENT_CERT" ]] && curl_args+=(--cert "$CLIENT_CERT")
    [[ -n "$CLIENT_KEY" ]] && curl_args+=(--key "$CLIENT_KEY")
    curl_args+=(-X POST)
    curl_args+=(-d "{\"command\":\"${cmd//\"/\\\"}\"}")
    curl_args+=("https://${ADDR}:${PORT}/api/exec/stream")
    curl "${curl_args[@]}"
    echo
}

cmd_upload() {
    if [[ $# -lt 2 ]]; then
        echo "Usage: $0 upload <local-file> <remote-path>"
        exit 1
    fi
    local src="$1" dst="$2"
    if [[ ! -f "$src" ]]; then
        echo "Error: file not found: $src"
        exit 1
    fi
    curl_bridge POST "/api/upload?path=$dst" "" "--data-binary @$src"
    echo
}

cmd_download() {
    if [[ $# -lt 1 ]]; then
        echo "Usage: $0 download <remote-path>"
        exit 1
    fi
    local src="$1"
    local curl_args=(-s)
    [[ -n "$CA_CERT" ]] && curl_args+=(--cacert "$CA_CERT")
    [[ -n "$CLIENT_CERT" ]] && curl_args+=(--cert "$CLIENT_CERT")
    [[ -n "$CLIENT_KEY" ]] && curl_args+=(--key "$CLIENT_KEY")
    curl_args+=("https://${ADDR}:${PORT}/api/download?path=$src")
    curl "${curl_args[@]}"
    echo
}

cmd_health() {
    curl_bridge GET "/api/health"
    echo
}

# Dispatch
COMMAND="${1:-help}"
shift 2>/dev/null || true

case "$COMMAND" in
    install)   cmd_install "$@" ;;
    exec)      cmd_exec "$@" ;;
    exec-st)   cmd_exec_st "$@" ;;
    upload)    cmd_upload "$@" ;;
    download)  cmd_download "$@" ;;
    health)    cmd_health "$@" ;;
    help|--help|-h) usage ;;
    *)
        echo "Unknown command: $COMMAND"
        echo "Run '$0 --help' for usage."
        exit 1
        ;;
esac
