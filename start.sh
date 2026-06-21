#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOCAL_CERT_DIR="$HOME/.termux-adb-bridge/certs"
DEVICE_BINARY="/data/local/tmp/termux-adb-bridge-secure"
PORT=10099
ADDR="127.0.0.1"

CURL_CA=""
CURL_CERT=""
CURL_KEY=""

usage() {
    cat <<EOF
Usage: start.sh [--kill|--status|--help]

  --kill      Kill the running bridge on device
  --status    Check if bridge is running and exit
  --help      Show this help

Default (no flags): start the bridge on the device using the
already-injected binary. Does not build or push. Run inject.sh first.
EOF
    exit 0
}

find_certs() {
    local fp_dir
    fp_dir=$(ls -dt "$LOCAL_CERT_DIR"/*/ 2>/dev/null | head -1)
    if [ -n "$fp_dir" ] && [ -f "$fp_dir/ca.crt" ] && [ -f "$fp_dir/client.crt" ] && [ -f "$fp_dir/client.key" ]; then
        CURL_CA="$fp_dir/ca.crt"
        CURL_CERT="$fp_dir/client.crt"
        CURL_KEY="$fp_dir/client.key"
        return 0
    fi
    return 1
}

curl_bridge() {
    if [ -z "$CURL_CA" ]; then
        find_certs || return 1
    fi
    curl -sf --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
        "https://${ADDR}:${PORT}${1}" 2>/dev/null
}

select_device() {
    local offline
    offline=$(adb devices | awk '$2=="offline"{print $1}')
    if [ -n "$offline" ]; then
        echo "Note: ignoring offline device(s): $offline" >&2
    fi
    DEVICE_SERIAL=$(adb devices | awk '$2=="device"{print $1; exit}')
    if [ -z "$DEVICE_SERIAL" ]; then
        echo "Error: no online device connected" >&2
        echo "Connect via: adb connect <ip>:<port>" >&2
        exit 1
    fi
    ADB_CMD=(adb -s "$DEVICE_SERIAL")
}

get_remote_pid() {
    "${ADB_CMD[@]}" shell 'pidof termux-adb-bridge-secure' 2>/dev/null | tr -d ' \n\r'
}

# --- Parse args ---
case "${1:-}" in
    --help|-h) usage ;;
    --kill)
        select_device
        pid=$(get_remote_pid)
        if [ -n "$pid" ]; then
            echo "Killing bridge (PID $pid)..."
            "${ADB_CMD[@]}" shell "kill $pid" 2>/dev/null || true
        else
            echo "Bridge not running"
        fi
        exit 0
        ;;
    --status)
        if curl_bridge "/api/health" >/dev/null 2>&1; then
            echo "Bridge is running"
            exit 0
        else
            echo "Bridge is not running"
            exit 1
        fi
        ;;
esac

# --- Check adb ---
if ! which adb >/dev/null 2>&1; then
    echo "Error: adb not found"
    exit 1
fi

select_device

# --- Check binary exists on device ---
if ! "${ADB_CMD[@]}" shell "test -x '$DEVICE_BINARY'" 2>/dev/null; then
    echo "Error: $DEVICE_BINARY not found on device" >&2
    echo "Run inject.sh first to deploy the bridge." >&2
    exit 1
fi

# --- Check if already running ---
find_certs || true
if curl_bridge "/api/health" >/dev/null 2>&1; then
    echo "Service is already running"
    if [ -n "$CURL_CA" ]; then
        fp_dir=$(dirname "$CURL_CA")
        echo "  Certificates: $fp_dir"
    fi
    exit 0
fi

# --- Start daemon ---
echo "Starting daemon on device $DEVICE_SERIAL..."
"${ADB_CMD[@]}" shell "setsid $DEVICE_BINARY --daemon > /dev/null 2>&1 &"

# --- Wait for health ---
echo "Waiting for bridge..."
for i in $(seq 1 10); do
    sleep 1
    find_certs || true
    if curl_bridge "/api/health" >/dev/null 2>&1; then
        break
    fi
done

# --- Show results ---
if curl_bridge "/api/health" >/dev/null 2>&1; then
    fp_dir=$(dirname "$CURL_CA")
    echo ""
    echo "=== Bridge is running ==="
    echo "  PID: $(get_remote_pid)"
    echo "  Address: $ADDR:$PORT"
    echo ""
    echo "Certificates:"
    echo "  CA:   $fp_dir/ca.crt"
    echo "  Cert: $fp_dir/client.crt"
    echo "  Key:  $fp_dir/client.key"
    echo ""
    echo "Use adb-termux to issue commands:"
    echo "  alias adb-termux='$SCRIPT_DIR/adb-termux.sh'"
else
    echo "Error: bridge did not start" >&2
    exit 1
fi
