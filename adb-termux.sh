#!/data/data/com.termux/files/usr/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CERT_DIR="${BRIDGE_CERT_DIR:-$HOME/.termux-adb-bridge/certs}"
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
  shell [--env KEY=VALUE ...] <cmd>   Execute command (POST /api/exec)
  exec  [--env KEY=VALUE ...] <cmd>   Same as shell
  stream [--env KEY=VALUE ...] <cmd>  Execute with streaming output
  pipe [--raw] [--env KEY=VALUE ...] <cmd>  Execute with piped stdin/stdout
                                            (--raw for binary, no stderr)
  push <src> <dst>                    Upload file to device
  pull <src>                          Download file from device
  health                              Check bridge health
  uptime                              Show server uptime
  --certinfo                          Show certificate info
  help                                Show this help

Environment:
  BRIDGE_PORT        Override port (default: 10099)
  BRIDGE_CERT_DIR    Override cert directory (default: ~/.termux-adb-bridge/certs)

Examples:
  adb-termux shell echo hello
  adb-termux shell --env FOO=bar 'echo \$FOO'
  echo data | adb-termux pipe 'cat > /sdcard/file'
  adb-termux pipe 'dd if=/dev/block/mmcblk0 bs=4k count=100' > backup.img
  adb-termux push ~/file.txt /sdcard/file.txt
  adb-termux pull /sdcard/file.txt
  adb-termux --certinfo
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
        curl_args+=($extra)
    fi
    curl_args+=("https://${ADDR}:${PORT}${path}")
    curl "${curl_args[@]}"
}

build_json() {
    local cmd="$1" env_json="$2"
    if [ -z "$env_json" ]; then
        printf '{"command":"%s"}' "$cmd"
    else
        printf '{"command":"%s","env":%s}' "$cmd" "$env_json"
    fi
}

build_env_json() {
    local env_json="{"
    local first=1
    local key value
    for kv in "$@"; do
        key="${kv%%=*}"
        value="${kv#*=}"
        if [ "$first" = 1 ]; then
            first=0
        else
            env_json="$env_json,"
        fi
        key=$(printf '%s' "$key" | sed 's/"/\\"/g')
        value=$(printf '%s' "$value" | sed 's/"/\\"/g')
        env_json="$env_json\"$key\":\"$value\""
    done
    env_json="$env_json}"
    echo "$env_json"
}

urlencode() {
    local s="$1"
    local len=${#s}
    local out=""
    for ((i=0; i<len; i++)); do
        local c="${s:i:1}"
        case "$c" in
            [a-zA-Z0-9._~-]) out+="$c" ;;
            ' ') out+='+' ;;
            *) printf -v out '%s%%%02X' "$out" "'$c" ;;
        esac
    done
    echo "$out"
}

parse_exec_args() {
    local env_vars=()
    local cmd_parts=()
    local in_env=0

    for arg in "$@"; do
        if [ "$arg" = "--env" ]; then
            in_env=1
        elif [ "$in_env" = 1 ]; then
            env_vars+=("$arg")
            in_env=0
        else
            cmd_parts+=("$arg")
        fi
    done

    local cmd_str=""
    for part in "${cmd_parts[@]}"; do
        if [ -z "$cmd_str" ]; then
            cmd_str="$part"
        else
            cmd_str="$cmd_str $part"
        fi
    done

    local env_json=""
    if [ ${#env_vars[@]} -gt 0 ]; then
        env_json=$(build_env_json "${env_vars[@]}")
    fi

    echo "$cmd_str|||$env_json"
}

# Build query string for pipe endpoint
build_pipe_query() {
    local cmd="$1"
    shift
    local query="command=$(urlencode "$cmd")"
    local key value
    for kv in "$@"; do
        key="${kv%%=*}"
        value="${kv#*=}"
        query="$query&env_$(urlencode "$key")=$(urlencode "$value")"
    done
    echo "$query"
}

# Parse SSE from pipe response
parse_sse() {
    while IFS= read -r line; do
        case "$line" in
            "data: [STDERR] "*)
                echo "${line#data: [STDERR] }" >&2
                ;;
            "data: {"*)
                local exit_code
                exit_code=$(echo "$line" | sed 's/.*"exit_code":\([0-9-]*\).*/\1/')
                if [ -n "$exit_code" ] && [ "$exit_code" != "$line" ]; then
                    return "$exit_code"
                fi
                echo "$line"
                ;;
            "data: "*)
                echo "${line#data: }"
                ;;
        esac
    done
    return 0
}

case "$1" in
    --certinfo)
        response=$(curl_bridge GET "/api/certinfo")
        # Extract fields using sed (no python dependency)
        echo "Fingerprint: $(echo "$response" | sed 's/.*"fingerprint":"\([^"]*\)".*/\1/')"
        echo ""
        echo "CA Certificate:"
        echo "$response" | sed 's/.*"ca_pem":"\([^"]*\)".*/\1/' | sed 's/\\n/\n/g'
        echo ""
        echo "Client Certificate:"
        echo "$response" | sed 's/.*"client_cert_pem":"\([^"]*\)".*/\1/' | sed 's/\\n/\n/g'
        ;;

    shell|exec)
        shift
        if [ $# -eq 0 ]; then
            echo "Usage: adb-termux shell [--env KEY=VALUE ...] <command>" >&2
            exit 1
        fi
        result=$(parse_exec_args "$@")
        cmd="${result%%|||*}"
        env_json="${result#*|||}"
        escaped=$(printf '%s' "$cmd" | sed 's/"/\\"/g')
        json=$(build_json "$escaped" "$env_json")
        curl_bridge POST "/api/exec" "$json"
        echo
        ;;

    stream)
        shift
        if [ $# -eq 0 ]; then
            echo "Usage: adb-termux stream [--env KEY=VALUE ...] <command>" >&2
            exit 1
        fi
        result=$(parse_exec_args "$@")
        cmd="${result%%|||*}"
        env_json="${result#*|||}"
        escaped=$(printf '%s' "$cmd" | sed 's/"/\\"/g')
        json=$(build_json "$escaped" "$env_json")
        curl_bridge POST "/api/exec/stream" "$json" "-N"
        echo
        ;;

    pipe)
        shift
        if [ $# -eq 0 ]; then
            echo "Usage: adb-termux pipe [--raw] [--env KEY=VALUE ...] <command>" >&2
            exit 1
        fi
        raw=0
        if [ "$1" = "--raw" ]; then
            raw=1
            shift
        fi
        result=$(parse_exec_args "$@")
        cmd="${result%%|||*}"
        env_json="${result#*|||}"
        env_json="${env_json#\{}"
        env_json="${env_json%\}}"

        env_vars=()
        if [ -n "$env_json" ]; then
            while IFS='=' read -r key value; do
                key=$(echo "$key" | sed 's/^"//;s/"$//')
                value=$(echo "$value" | sed 's/^"//;s/"$//')
                env_vars+=("$key=$value")
            done < <(echo "$env_json" | sed 's/,/\n/g')
        fi

        query=$(build_pipe_query "$cmd" "${env_vars[@]}")
        if [ "$raw" = 1 ]; then
            query="${query}&raw=1"
        fi

        if [ "$raw" = 1 ]; then
            # Raw mode: stream bytes directly, no SSE parsing
            if [ -t 0 ]; then
                curl -s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
                    -X POST -H "Content-Length: 0" \
                    "https://${ADDR}:${PORT}/api/exec/pipe?${query}"
            else
                curl -s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
                    -X POST -H "Transfer-Encoding: chunked" -T - \
                    "https://${ADDR}:${PORT}/api/exec/pipe?${query}"
            fi
        else
            # SSE mode: parse event stream
            if [ -t 0 ]; then
                curl -s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
                    -X POST -H "Content-Length: 0" \
                    "https://${ADDR}:${PORT}/api/exec/pipe?${query}" | parse_sse
            else
                curl -s --cacert "$CURL_CA" --cert "$CURL_CERT" --key "$CURL_KEY" \
                    -X POST -H "Transfer-Encoding: chunked" -T - \
                    "https://${ADDR}:${PORT}/api/exec/pipe?${query}" | parse_sse
            fi
        fi
        exit $?
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

    uptime)
        curl_bridge GET "/api/uptime"
        echo
        ;;

    *)
        echo "Unknown command: $1" >&2
        echo "Run '$0 --help' for usage." >&2
        exit 1
        ;;
esac
