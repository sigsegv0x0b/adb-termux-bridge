#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/termux-adb-bridge-secure"
LOCAL_CERT_DIR="$HOME/.termux-adb-bridge/certs"
DEVICE_BINARY="/data/local/tmp/termux-adb-bridge-secure"
PORT=10099
ADDR="127.0.0.1"

CURL_CA=""
CURL_CERT=""
CURL_KEY=""

usage() {
    cat <<EOF
Usage: inject.sh [--kill|--status|--help]

  --kill      Kill the running bridge on device
  --status    Check if bridge is running and exit
  --help      Show this help

Default (no flags): inject the secure binary and start the bridge.
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

get_remote_pid() {
    adb shell 'pidof termux-adb-bridge-secure' 2>/dev/null | tr -d ' \n\r'
}

# --- Parse args ---
case "${1:-}" in
    --help|-h) usage ;;
    --kill)
        pid=$(get_remote_pid)
        if [ -n "$pid" ]; then
            echo "Killing bridge (PID $pid)..."
            adb shell "kill $pid" 2>/dev/null || true
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

DEVICE_COUNT=$(adb devices | grep -v "List" | grep "device$" | wc -l)
if [ "$DEVICE_COUNT" -eq 0 ]; then
    echo "Error: no device connected"
    echo "Connect via: adb connect <ip>:<port>"
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
    read -p "Re-inject? [y/N] " answer
    case "$answer" in
        [yY]|[yY][eE][sS]) ;;
        *)
            echo "Exiting"
            exit 0
            ;;
    esac
    pid=$(get_remote_pid)
    if [ -n "$pid" ]; then
        echo "Killing existing bridge (PID $pid)..."
        adb shell "kill $pid" 2>/dev/null || true
        sleep 1
    fi
fi

# --- Build if needed ---
if [ ! -f "$BINARY" ]; then
    echo "Building secure binary..."
    cd "$SCRIPT_DIR"
    make clean 2>/dev/null || true
    make -j$(nproc) 2>&1
    make secure 2>&1
fi

# --- Push and start ---
echo "Pushing binary to device..."
adb push "$BINARY" /data/local/tmp/termux-adb-bridge-secure 2>&1
adb shell chmod +x "$DEVICE_BINARY"

echo "Starting daemon..."
adb shell "setsid $DEVICE_BINARY --daemon > /dev/null 2>&1 &"

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
    echo "Error: bridge did not start"
    exit 1
fi