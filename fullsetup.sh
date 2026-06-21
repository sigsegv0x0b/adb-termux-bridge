#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/termux-adb-bridge-secure"

usage() {
    cat <<EOF
Usage: fullsetup.sh [--help]

One-command setup: install dependencies, build the secure binary,
detect device IP, connect to ADB, and inject the bridge.

Steps:
  1. Install Termux packages (openssl, make, clang, etc.)
  2. Build termux-adb-bridge-secure
  3. Check for online ADB device
  4. If not connected, detect device IP and prompt for ADB address
  5. Run inject.sh to push and start the bridge

No flags needed for normal use.
EOF
    exit 0
}

case "${1:-}" in
    --help|-h) usage ;;
esac

# --- 1. Install packages ---
echo "=== Installing dependencies ==="
pkg update -y
pkg install -y openssl openssl-static make clang curl android-tools

# --- 2. Build secure binary ---
if [ ! -f "$BINARY" ]; then
    echo "=== Building secure binary ==="
    cd "$SCRIPT_DIR"
    make clean 2>/dev/null || true
    make -j$(nproc)
    make secure
else
    echo "Binary already built: $BINARY"
fi

# --- 3. Detect device IP ---
detect_ip() {
    ifconfig 2>/dev/null | awk '
        /^[a-z]/   { iface=$1; sub(/:$/,"",iface) }
        /inet / && iface != "lo" { print $2; exit }
    '
}

# --- 4. Check ADB connection ---
ADB_DEVICE=""
select_device() {
    ADB_DEVICE=$(adb devices | awk '$2=="device"{print $1; exit}')
}

select_device

if [ -n "$ADB_DEVICE" ]; then
    echo "Device $ADB_DEVICE already connected"
else
    echo "=== No ADB device connected ==="
    detected=$(detect_ip)
    if [ -n "$detected" ]; then
        echo "Detected device IP: $detected"
        default_ip="$detected"
    else
        echo "Could not detect device IP automatically."
        default_ip=""
    fi

    echo ""
    echo "Enable Wireless debugging on your phone:"
    echo "  Settings → Developer options → Wireless debugging"
    echo "  Note the IP address and port shown."
    echo ""

    read -p "ADB address [${default_ip}]: " input_ip
    IP="${input_ip:-$default_ip}"

    while [ -z "$IP" ]; do
        read -p "ADB address (required): " IP
    done

    read -p "ADB port: " PORT
    while [ -z "$PORT" ]; do
        read -p "ADB port (required): " PORT
    done

    echo ""
    echo "Connecting to $IP:$PORT..."
    adb connect "$IP:$PORT"

    select_device
    if [ -z "$ADB_DEVICE" ]; then
        echo "Connection failed. Check the address and port and try again."
        echo "Also ensure USB debugging is enabled and the pairing code is accepted."
        read -p "Retry? [Y/n] " retry
        case "$retry" in
            [nN]|[nN][oO]) exit 1 ;;
            *)
                adb connect "$IP:$PORT"
                select_device
                if [ -z "$ADB_DEVICE" ]; then
                    echo "Connection failed again. Exiting."
                    exit 1
                fi
                ;;
        esac
    fi
    echo "Connected to $ADB_DEVICE"
fi

# --- 5. Inject ---
echo ""
echo "=== Injecting bridge ==="
cd "$SCRIPT_DIR"
exec ./inject.sh
