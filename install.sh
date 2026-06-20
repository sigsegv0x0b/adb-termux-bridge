#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/termux-adb-bridge"
CERTS_DIR="$HOME/.termux-adb-bridge/certs"

echo "=== Termux ADB Bridge Installer ==="
echo ""

# Build
echo "[1/3] Building binary..."
cd "$SCRIPT_DIR"
make clean 2>/dev/null || true
make -j$(nproc)
echo "  Binary: $BINARY"

# Generate certs
echo "[2/3] Generating ED25519 certificates..."
mkdir -p "$CERTS_DIR"
"$BINARY" --init-certs --cert-dir "$CERTS_DIR"
echo "  Certs: $CERTS_DIR"

# Print client info for desktop
echo "[3/3] Done!"
echo ""
echo "=== To deploy from desktop ==="
echo "Copy these files to your desktop machine:"
echo "  $CERTS_DIR/ca.crt"
echo "  $CERTS_DIR/client.crt"
echo "  $CERTS_DIR/client.key"
echo "  $BINARY"
echo ""
echo "Then run on desktop:"
echo "  adb push $BINARY /data/local/tmp/"
echo "  adb push $CERTS_DIR/ca.crt     /data/local/tmp/"
echo "  adb push $CERTS_DIR/client.crt  /data/local/tmp/"
echo "  adb push $CERTS_DIR/client.key /data/local/tmp/"
echo "  adb shell 'chmod +x /data/local/tmp/termux-adb-bridge'"
echo "  adb shell 'setsid /data/local/tmp/termux-adb-bridge --daemon --cert-dir /data/local/tmp > /dev/null 2>&1 &'"
echo "  adb forward tcp:10099 tcp:10099"
echo ""
echo "Or use bridge.sh for convenience (see --help)"
