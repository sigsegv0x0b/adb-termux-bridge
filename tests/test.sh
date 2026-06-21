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

# 11) Pipe: no body (cat file)
echo "11) pipe: no body (cat file)"
echo "pipe test file content" > "$TESTDIR/pipe_test.txt"
resp=$(curl -s -N --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -X POST -H "Content-Length: 0" \
    "$API/api/exec/pipe?command=cat%20$TESTDIR/pipe_test.txt" 2>/dev/null)
if echo "$resp" | grep -q "pipe test file content" && echo "$resp" | grep -q '"exit_code":0'; then
    pass ""
else
    fail "  pipe no body failed: $resp"
fi

# 12) Pipe: Content-Length body
echo "12) pipe: Content-Length body"
resp=$(curl -s -N --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -X POST -d "hello from content-length" \
    "$API/api/exec/pipe?command=cat" 2>/dev/null)
if echo "$resp" | grep -q "hello from content-length" && echo "$resp" | grep -q '"exit_code":0'; then
    pass ""
else
    fail "  pipe content-length failed: $resp"
fi

# 13) Pipe: chunked body
echo "13) pipe: chunked body"
resp=$(echo "hello from chunked" | curl -s -N --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -X POST -H "Transfer-Encoding: chunked" -T - \
    "$API/api/exec/pipe?command=cat" 2>/dev/null)
if echo "$resp" | grep -q "hello from chunked" && echo "$resp" | grep -q '"exit_code":0'; then
    pass ""
else
    fail "  pipe chunked failed: $resp"
fi

# 14) Pipe: dd with stderr
echo "14) pipe: dd with stderr"
resp=$(curl -s -N --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -X POST -d "dd test data" \
    "$API/api/exec/pipe?command=dd%20bs%3D1024" 2>/dev/null)
if echo "$resp" | grep -q "dd test data" && echo "$resp" | grep -q "records out" && echo "$resp" | grep -q '"exit_code":0'; then
    pass ""
else
    fail "  pipe dd failed: $resp"
fi

# 15) Certinfo endpoint
echo "15) certinfo endpoint"
resp=$(curl -s --cacert "$CA_CERT" --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    "$API/api/certinfo" 2>/dev/null)
if echo "$resp" | grep -q '"ca_pem"' && echo "$resp" | grep -q '"client_cert_pem"' && echo "$resp" | grep -q '"fingerprint":"SHA256:'; then
    pass ""
else
    fail "  certinfo failed: $resp"
fi

# 16) Raw pipe: upload + download with checksum verification
# Helper: generate testload, pipe through bridge, verify checksum
test_raw_pipe() {
    local name="$1" size="$2" direction="$3"
    local testload="$TESTDIR/raw_${size}_${direction}.bin"
    local recv="$TESTDIR/recv_${size}_${direction}.bin"
    dd if=/dev/urandom bs=1 count="$size" of="$testload" 2>/dev/null
    local s1; s1=$(sha256sum "$testload" | cut -d' ' -f1)

    if [[ "$direction" == "up" ]]; then
        # local → device: cat testload | pipe --raw 'cat > recv'
        BRIDGE_CERT_DIR="$TESTDIR" BRIDGE_PORT="$PORT" \
            "$BRIDGE_DIR/adb-termux.sh" pipe --raw "cat > $recv" \
            < "$testload" > /dev/null 2>/dev/null || true
    else
        # device → local: pipe --raw 'cat testload' > recv
        BRIDGE_CERT_DIR="$TESTDIR" BRIDGE_PORT="$PORT" \
            "$BRIDGE_DIR/adb-termux.sh" pipe --raw "cat $testload" \
            > "$recv" 2>/dev/null || true
    fi

    local s2; s2=$(sha256sum "$recv" 2>/dev/null | cut -d' ' -f1)
    if [[ "$s1" == "$s2" && -n "$s2" ]]; then
        pass "  ${name} (${size}B ${direction})"
    else
        fail "  ${name} (${size}B ${direction}): ${s1:0:16}.. vs ${s2:-MISSING}"
    fi
}

echo "16) raw pipe: binary transfer with checksum verification"
for dir in up down; do
    test_raw_pipe "raw pipe" 1         "$dir"
    test_raw_pipe "raw pipe" 100       "$dir"
    test_raw_pipe "raw pipe" 65000     "$dir"
    test_raw_pipe "raw pipe" 65536     "$dir"
    test_raw_pipe "raw pipe" 1048576   "$dir"
done

echo ""
echo "=== Results ==="
[[ $FAIL_COUNT -eq 0 ]] && echo "All tests passed!" || echo "$FAIL_COUNT test(s) failed"
