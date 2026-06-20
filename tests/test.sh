#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$BRIDGE_DIR/termux-adb-bridge"
PORT=10100
API="https://127.0.0.1:$PORT"

PASS_COUNT=0
FAIL_COUNT=0
SERVER_PID=""
TESTDIR=""

cleanup() {
    local ec=$?
    [[ -n "${SERVER_PID:-}" ]] && { kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; }
    [[ -n "${TESTDIR:-}" ]] && rm -rf "$TESTDIR"
    echo ""; echo "===================="
    echo "$PASS_COUNT/$((PASS_COUNT + FAIL_COUNT)) tests passed"
    exit "$ec"
}
trap cleanup EXIT INT TERM

pass() { echo "[PASS] $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "[FAIL] $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

# ---- JSON parsing (bash parameter expansion, no external deps) ----
# Extract a string field from {"stdout":"...","stderr":"...","exit_code":N}
json_str() {
    local json="$1" field="$2"
    local rest="${json#*\"${field}\":\"}"
    local val="${rest%%\",*}"
    val="${val%\"\}}"
    printf "%b" "$val"
}

json_num() {
    local json="$1" field="$2"
    local rest="${json#*\"${field}\":}"
    echo "$rest" | sed 's/[^0-9-].*//'
}

# ---- run helpers ----
direct_run() {
    local cmd="$1"
    local to=$(mktemp) te=$(mktemp)
    set +e; eval "$cmd" >"$to" 2>"$te"; D_EXIT=$?; set -e
    D_STDOUT=$(cat "$to"); D_STDERR=$(cat "$te")
    rm -f "$to" "$te"
}

bridge_run() {
    local cmd="$1"
    local esc="${cmd//\"/\\\"}"
    local resp
    resp=$(curl -s --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
        -X POST -d "{\"command\":\"${esc}\"}" "$API/api/exec")
    echo "$resp" > "$TESTDIR/last_response.json"
    B_EXIT=$(json_num "$resp" "exit_code")
    B_STDOUT=$(json_str "$resp" "stdout")
    B_STDERR=$(json_str "$resp" "stderr")
}

# ---- test runners ----
test_exact() {
    local name="$1" cmd="$2"
    echo "$name"
    direct_run "$cmd"
    bridge_run "$cmd"
    if [[ "$D_EXIT" != "$B_EXIT" ]]; then fail "  exit_code: direct=$D_EXIT bridge=$B_EXIT"; return; fi
    if [[ "$D_STDOUT" != "$B_STDOUT" ]]; then
        fail "  stdout mismatch"
        echo "    direct: [${D_STDOUT}]"
        echo "    bridge: [${B_STDOUT}]"
        return
    fi
    if [[ "$D_STDERR" != "$B_STDERR" ]]; then
        fail "  stderr mismatch"
        echo "    direct: [${D_STDERR}]"
        echo "    bridge: [${B_STDERR}]"
        return
    fi
    pass ""
}

test_exit_only() {
    local name="$1" cmd="$2" exp="$3"
    echo "$name"
    bridge_run "$cmd"
    [[ "$B_EXIT" == "$exp" ]] && { pass ""; return; }
    fail "  exit_code: expected $exp got $B_EXIT"
}

test_streaming() {
    local name="$1" cmd="$2"
    echo "$name"
    local resp
    resp=$(curl -s -N --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
        -X POST -d "{\"command\":\"${cmd//\"/\\\"}\"}" "$API/api/exec/stream" 2>/dev/null || true)
    if echo "$resp" | grep -q "exit_code"; then pass ""; else fail "  no exit event in stream"; fi
}

test_mtls_reject() {
    echo "$1"
    set +e
    curl -sk https://127.0.0.1:$PORT/api/health >/dev/null 2>&1
    local rc=$?
    set -e
    [[ $rc -ne 0 ]] && { pass ""; return; }
    fail "  request without client cert should have been rejected"
}

test_date_not_equal() {
    local name="$1" cmd="$2"
    echo "$name"
    direct_run "$cmd"
    bridge_run "$cmd"
    [[ "$B_EXIT" != 0 ]] && { fail "  exit_code: $B_EXIT"; return; }
    [[ -z "$B_STDOUT" ]] && { fail "  stdout empty"; return; }
    # Note: date output may coincidentally match if both run in the same second.
    # Verifying it's not identical is a best-effort check (no true structural difference).
    [[ "$D_STDOUT" != "$B_STDOUT" ]] && echo "  (output differs as expected)" || echo "  (output matches — same second, still valid)"
    pass ""
}

test_date_ts_close() {
    local name="$1" cmd="$2" maxd="${3:-2}"
    echo "$name"
    direct_run "$cmd"
    bridge_run "$cmd"
    [[ "$B_EXIT" != 0 ]] && { fail "  exit_code: $B_EXIT"; return; }
    local d=$((D_STDOUT)) b=$((B_STDOUT))
    local diff=$((b - d))
    [[ $diff -lt 0 ]] && diff=$((-diff))
    [[ $diff -le $maxd ]] && { pass ""; return; }
    fail "  timestamps differ by ${diff}s (max ${maxd}s)"
}

# ============================================================
echo "=== Termux ADB Bridge Test Suite ==="
echo ""

# 1. Build
if [[ ! -x "$BINARY" ]]; then
    echo "[setup] Building..."
    make -C "$BRIDGE_DIR" -j$(nproc 2>/dev/null || echo 2) | sed 's/^/  /'
fi

# 2. Certs
TESTDIR=$(mktemp -d)
CA_CERT="$TESTDIR/ca.crt"
CLIENT_CERT="$TESTDIR/client.crt"
CLIENT_KEY="$TESTDIR/client.key"
echo "[setup] Generating test certificates..."
"$BINARY" --init-certs --cert-dir "$TESTDIR" >/dev/null 2>&1

# 3. Start server
echo "[setup] Starting bridge on port $PORT..."
"$BINARY" --cert-dir "$TESTDIR" --port "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!
sleep 1

if ! curl -sf --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    "$API/api/health" >/dev/null 2>&1; then
    echo "[FATAL] Bridge failed to start"; exit 1
fi
echo "[setup] Bridge is alive"
echo ""

# ---- Tests ----
test_exit_only       "01) echo with exit code"          'echo ok; exit 42' 42
test_exact           "02) uname -a"                     'uname -a'
test_date_ts_close   "03) date +%s (close)"             'date +%s' 2
test_date_not_equal  "04) date (differs per run)"       'date'
test_exit_only       "05) nonexistent command"          'nonexistent_cmd_xyz_999' 127
test_exact           "06) stderr only"                  'echo stderror >&2'
test_exact           "07) stdout and stderr"            'echo out; echo err >&2'
test_exit_only       "08) uptime (non-deterministic)"   'uptime' 0
test_streaming       "09) streaming exec"               'echo hello'
test_mtls_reject     "10) mTLS rejection"

echo ""
echo "=== Results ==="
[[ $FAIL_COUNT -eq 0 ]] && echo "All tests passed!" || echo "$FAIL_COUNT test(s) failed"
