#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${1:-$SCRIPT_DIR/termux-adb-bridge-secure}"
CERT_DIR="${BRIDGE_CERT_DIR:-$HOME/.termux-adb-bridge/certs}"
PORT="${BRIDGE_PORT:-10099}"
ADDR="127.0.0.1"

CURL_CA=""
CURL_CERT=""
CURL_KEY=""

usage() {
    cat <<EOF
Usage: update.sh [path-to-binary]

Send a new binary to the running bridge via /api/update.
The server verifies the SHA256 checksum, atomically replaces
its own binary, and execv's the new version.

Options:
  --help          Show this help

Environment:
  BRIDGE_PORT       Override port (default: 10099)
  BRIDGE_CERT_DIR   Override cert directory (default: ~/.termux-adb-bridge/certs)

Default binary: $SCRIPT_DIR/termux-adb-bridge-secure
If the binary doesn't exist, it will be built automatically.
EOF
    exit 0
}

case "${1:-}" in
    --help|-h) usage ;;
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

check_bridge_running() {
    curl -sf --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
        "https://${ADDR}:${PORT}/api/health" >/dev/null 2>&1
}

# --- Find certs ---
find_certs

# --- Check bridge is running ---
if ! check_bridge_running; then
    echo "Error: bridge is not running on ${ADDR}:${PORT}" >&2
    echo "Start it with inject.sh first." >&2
    exit 1
fi

# --- Build if needed ---
if [ ! -f "$BINARY" ]; then
    echo "Binary not found at $BINARY"
    echo "Building..."
    cd "$SCRIPT_DIR"
    make secure 2>&1 | sed 's/^/  /'
    if [ ! -f "$BINARY" ]; then
        echo "Error: build failed" >&2
        exit 1
    fi
fi

# --- Compute SHA256 ---
SHA=$(sha256sum "$BINARY" | cut -d' ' -f1)
SIZE=$(wc -c < "$BINARY")
echo "Binary: $BINARY"
echo "Size:   $SIZE bytes"
echo "SHA256: ${SHA:0:16}...${SHA:48}"

# --- Send to bridge ---
echo "Uploading..."
RESP=$(curl -s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
    -X POST --data-binary @"$BINARY" \
    "https://${ADDR}:${PORT}/api/update?sha256=${SHA}" 2>&1) || true
echo "Response: $RESP"

# --- Poll health until new instance is back ---
echo "Waiting for restart..."
for i in $(seq 1 20); do
    sleep 0.5
    if check_bridge_running; then
        echo "Server is back up (took $((i * 500))ms)"
        exit 0
    fi
done

echo "Error: server did not come back within 10 seconds" >&2
exit 1
