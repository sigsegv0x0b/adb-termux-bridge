# adb-termux-bridge

A lightweight HTTP-over-mTLS daemon that is **injected via ADB and runs as the ADB shell UID (2000)**, providing a REST API on `127.0.0.1` to execute shell commands with ADB-level privileges — all accessible from Termux without a desktop.

## What is this?

```
┌───────────────────────────────────────────────────┐
│                   Android Device                   │
│                                                   │
│  ┌─────────────────────────────────────────────┐  │
│  │  Termux (app UID)                          │  │
│  │  ┌──────────────┐  ┌─────────────────────┐ │  │
│  │  │ adb-termux   │  │ curl, scripts, etc. │ │  │
│  │  │ shell pm     │  │ POST /api/exec      │ │  │
│  │  │ list packages│  │ mTLS client certs   │ │  │
│  │  └──────┬───────┘  └──────────┬──────────┘ │  │
│  └─────────┼─────────────────────┼────────────┘  │
│            │     loopback        │                │
│            │   127.0.0.1:10099   │                │
│  ┌─────────▼─────────────────────▼────────────┐  │
│  │  Bridge daemon (UID 2000 — ADB shell)      │  │
│  │  runs at /data/local/tmp/                  │  │
│  │  injected via: adb shell setsid ./binary   │  │
│  │                                            │  │
│  │  Commands execute as UID 2000              │  │
│  │  → pm, dumpsys, settings, input, content   │  │
│  │  → same as running "adb shell <cmd>"       │  │
│  └────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────┘
```

## Why

Termux on Android runs as an unprivileged app (UID `u0_aXXX`). It cannot:
- Run `pm`, `dumpsys`, `settings`, or other ADB shell commands
- Access `/dev/binder` for Binder IPC (the kernel does not expose it to app processes)
- Elevate privileges without external help

The standard workaround is to pair Termux with a desktop via `adb shell`, but that requires a USB cable or WiFi, and a desktop machine.

**This bridge flips the model**: instead of Termux reaching out to ADB, ADB injects a daemon into the device that Termux can talk to. The daemon runs at the `shell` UID (2000) — the same as `adb shell` — and executes commands with full ADB-level privileges.

The result: **Termux gains the ability to run any ADB shell command programmatically, without a desktop, over loopback.**

## How injection works

The secure binary (`termux-adb-bridge-secure`) is a fully self-contained, statically-linked executable with embedded ED25519 certificates. It is deployed via:

```
adb connect <phone-ip>:<port>    # wireless debugging
adb push termux-adb-bridge-secure /data/local/tmp/
adb shell 'setsid /data/local/tmp/termux-adb-bridge-secure --daemon'
```

1. **`adb push`** places the binary in `/data/local/tmp/` (the ADB shell user's writable directory)
2. **`adb shell`** starts the binary on the device
3. **`setsid`** creates a new session — the daemon detaches from the ADB connection
4. **`--daemon`** forks again — the parent exits, the child orphans to `init`
5. The daemon now runs permanently as **UID 2000** (the ADB shell user), listening on `127.0.0.1:10099`
6. On startup, it auto-extracts its embedded certs to `~/.termux-adb-bridge/certs/<fingerprint>/`

Now any process on the device (Termux, scripts, apps) can call the daemon via `127.0.0.1:10099` with mTLS and execute commands at the ADB shell privilege level.

## What you can do

| Command | Equivalent ADB command |
|---------|----------------------|
| `adb-termux shell pm list packages` | `adb shell pm list packages` |
| `adb-termux shell dumpsys battery` | `adb shell dumpsys battery` |
| `adb-termux shell settings put global airplane_mode_on 1` | `adb shell settings put global airplane_mode_on 1` |
| `adb-termux shell input keyevent 26` | `adb shell input keyevent 26` |
| `adb-termux shell content query --uri content://settings/secure` | `adb shell content query ...` |
| `adb-termux shell cmd wifi set-wifi-enabled 1` | `adb shell cmd wifi set-wifi-enabled 1` |

Any command that works in `adb shell` works through the bridge.

## Architecture

```
┌──────────────────────────┐
│     Client (curl, etc.)  │
│  ┌─────────────────────┐ │
│  │ CA cert + client    │ │
│  │ cert + client key   │ │
│  └─────┬───────────────┘ │
│        │ mTLS (TLS 1.3)  │
└────────┼─────────────────┘
         │
         │ loopback (127.0.0.1:10099)
         ▼
┌──────────────────────────┐
│  termux-adb-bridge       │
│  (UID 2000, /data/local) │
│  ┌────────────────────┐  │
│  │ SSL_CTX (mTLS)     │  │
│  │ ├─ Server cert     │  │
│  │ ├─ Server key      │  │
│  │ └─ CA cert (trust) │  │
│  └────────┬───────────┘  │
│           │               │
│  ┌────────▼───────────┐  │
│  │ HTTP/1.1 parser    │  │
│  │ + router           │  │
│  │ exec / upload /    │  │
│  │ download / health  │  │
│  └────────┬───────────┘  │
│           │               │
│  ┌────────▼───────────┐  │
│  │ executor (fork+    │  │
│  │ exec + pipe capture│  │
│  │ + poll streaming)  │  │
│  └────────────────────┘  │
└──────────────────────────┘
```

## API

All requests require mTLS. Only clients presenting a certificate signed by the bridge's CA are accepted.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/health` | Returns `{"status":"ok","version":"0.1.0"}` |
| `POST` | `/api/exec` | Execute command, return full output |
| `POST` | `/api/exec/stream` | Execute command, stream output as it arrives |
| `POST` | `/api/upload?path=<remote>` | Upload file to device |
| `GET` | `/api/download?path=<remote>` | Download file from device |

### `/api/exec`

Request:
```
POST /api/exec
Content-Type: application/json

{"command": "echo hello; exit 42"}
```

Response:
```json
{"stdout":"hello\n","stderr":"","exit_code":42}
```

### `/api/exec/stream`

Same request format. Response is a chunked JSON stream:
```
{"stdout":"hello\n"}
{"exit_code":0}
```

## Design Choices

### Why C, not Rust/Go/Python

C produces a small (~50 KB dynamically linked, ~6 MB with static OpenSSL) and fast binary with no runtime overhead or garbage collection. Static linking is trivial — just `-l:libssl.a` — which allows the binary to run from `/data/local/tmp/` with zero dependencies beyond what the Android system provides (`libc.so`, `libdl.so`).

### Why custom HTTP, not libmicrohttpd or libcurl

The bridge serves exactly one concurrent client (loopback) with a tiny API surface. Adding a dependency on libmicrohttpd would pull in dozens of source files and complicate cross-builds. The HTTP parser in `server.c` is ~150 lines — it only needs to parse the request line, headers, and Content-Length.

### Why mTLS

Only clients with a signed certificate can talk to the bridge — no passwords, no server-side state. The bridge never leaves `127.0.0.1`, so it's not network-reachable, but any process on the device can still reach loopback. mTLS ensures the bridge only trusts clients that present a cert signed by its own CA.

### Why ED25519, not RSA or ECDSA

- ED25519 keys are 32 bytes (vs 256+ for RSA)
- Signing and verification are ~3x faster than ECDSA P-256
- ED25519 is immune to timing side-channel attacks
- OpenSSL 3.x on Termux supports ED25519 with a simple `EVP_PKEY_keygen_init` call

**Note for OpenSSL 3.x**: ED25519 requires `X509_sign(x, key, NULL)` — passing any digest algorithm (e.g. `EVP_sha256()`) causes a silent failure. This is because ED25519 uses a fixed internal hash and does not accept an external digest.

### Why fork+exec, not popen or libssh2

`popen()` captures only stdout and does not provide streaming or separate stdout/stderr. `libssh2` is designed for remote SSH sessions, not local subprocess management. The executor in `executor.c` uses raw `fork()` + `execvp()` with a pipe for stdout and a separate pipe for stderr, then `poll()` with a 500ms timeout for non-blocking streaming — giving full control over process lifecycle and output delivery.

### Why pthreads, not an event loop

With a single daemon handling loopback clients, thread-per-connection is the simplest correct model. An event loop (libevent, libuv) would add complexity with no benefit — there is no I/O multiplexing problem at this scale.

## Security Model

### Threat model

The bridge trusts:
- **The ADB shell user (UID 2000)**: Injects and runs the binary
- **Clients holding a CA-signed certificate**: Can execute commands as UID 2000
- **localhost (`127.0.0.1`)**: The only interface the bridge listens on

The bridge does **not** trust:
- **Other apps on the device**: Blocked by mTLS (they need a signed client cert)
- **Termux**: Despite being on the same device, Termux still needs a valid client cert to issue commands
- **Remote network clients**: The bridge is not reachable from outside `127.0.0.1` unless port forwarding is set up
- **ADB forward from desktop**: If `adb forward tcp:10099 tcp:10099` is used, the desktop client still needs a valid client cert

### Certificate chain

```
CA (self-signed)
├── server.crt  — identifies the bridge itself (CN=127.0.0.1)
└── client.crt  — identifies the caller (CN=client)
```

- The CA key is used only to sign certificates and can be kept offline
- The server key is on the device and proves the bridge's identity
- The client key is given to authorized clients
- All keys are ED25519 (32-byte seed)

### TLS configuration

- Minimum TLS 1.2, maximum TLS 1.3
- `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` — every connection must present a valid client cert
- Verify depth = 1 (only direct CA-signed certs, no intermediate chains)
- ED25519 signature scheme (no ciphersuites to negotiate — TLS 1.3 auto-negotiates)

### Certificate fingerprint

The SHA-256 fingerprint of the server certificate identifies a specific bridge instance. Certs are saved to `<cert-dir>/<fingerprint>/` so multiple instances (or rebuilds) do not collide. When using the secure build, the fingerprint directory is created automatically on startup.

## Build

Running `make` produces the secure binary by default:

```
make
# → termux-adb-bridge-secure  (statically linked, certs embedded)
```

The build:
1. Compiles the normal binary (`termux-adb-bridge`)
2. Auto-generates ED25519 certificates on first build if missing
3. Saves PEM files to `<cert-dir>/<fingerprint>/` via `--save-certs`
4. Generates `src/certs_data.h` with all certs and keys as embedded DER byte arrays
5. Rebuilds as `termux-adb-bridge-secure` with `-DSECURE_BUILD` and `-l:libssl.a` (static OpenSSL)

The secure binary is fully self-contained with zero runtime dependencies. It extracts its certs to the fingerprint directory on startup.

### Targets

| Target | Output | Description |
|--------|--------|-------------|
| `make` (default) | `termux-adb-bridge-secure` | Static binary with embedded certs |
| `make normal` | `termux-adb-bridge` | Dynamic binary (PEM files at runtime) |
| `make clean` | — | Remove build artifacts |

## Process Lifecycle

### Daemonization

When `--daemon` is passed:
1. The process forks — the parent prints the child PID and exits
2. The child calls `setsid()` to create a new session, detaching from the ADB shell's process group
3. stdin/stdout/stderr are redirected to `/dev/null`
4. SIGINT/SIGTERM are handled for graceful shutdown

This is the same pattern used by Shizuku's `starter.cpp`.

### Kill previous instance

On startup, the bridge scans `/proc/<pid>/cmdline` for existing processes whose path contains `termux-adb-bridge`. Any match (excluding itself) receives `SIGTERM` with a 500ms grace period, then `SIGKILL`. This prevents "address already in use" errors on restart and mirrors the approach Shizuku uses to kill stale `shizuku_server` processes.

### Signal handling

- `SIGHUP`: Ignored (daemon does not reload config)
- `SIGPIPE`: Ignored (handled gracefully by SSL_write return value)
- `SIGINT`/`SIGTERM`: Sets a shutdown flag, stops the accept loop, and exits cleanly

## Quick start

### 1. Build on device (Termux)

```
git clone https://github.com/sigsegv0x0b/adb-termux-bridge
cd adb-termux-bridge
pkg install openssl openssl-static
make
```

This builds the secure binary (`termux-adb-bridge-secure`) — a statically-linked daemon with ED25519 certificates baked in. Certs are auto-generated on the first build.

### 2. Enable wireless debugging

On your phone: **Settings → Developer options → Wireless debugging** → enable it and note the IP:port.

### 3. Inject

```
adb connect <phone-ip>:<port>
./inject.sh
```

This pushes the statically-linked secure binary to `/data/local/tmp/`, starts it as a daemon via `setsid`, and shows the certificate paths.

### 4. Use adb-termux

```
alias adb-termux='$PWD/adb-termux.sh'
adb-termux shell pm list packages
adb-termux shell dumpsys battery
adb-termux shell settings put global airplane_mode_on 1
```

## Scripts

| Script | Description |
|--------|-------------|
| `inject.sh` | Push secure binary to device and start daemon |
| `adb-termux.sh` | CLI wrapper that routes `shell`, `push`, `pull` through the bridge API |
| `install.sh` | Build + generate certs (on-device) |
| `bridge.sh` | Desktop-side deploy CLI |

## Dependencies

- **OpenSSL ≥ 3.0** (`libssl-dev`, `libcrypto`): TLS, X.509, ED25519
- **POSIX threads** (`-pthread`): Connection concurrency
- **Standard C library**: Fork, exec, pipes, poll

## Comparison with Shizuku

| Aspect | Shizuku | termux-adb-bridge |
|--------|---------|-------------------|
| Language | Java (C++ injector) | C |
| IPC | Android Binder | HTTP + mTLS |
| Privilege elevation | ADB shell + app_process | Injected via ADB (runs at UID 2000) |
| Certificates | None (UID-based auth) | ED25519 mTLS |
| Process model | fork + setsid + app_process | fork + setsid + execve |
| Dependencies | Android framework | OpenSSL only |
| Target | Granting apps system-level Binder access | Executing shell commands via REST API |

Shizuku is the right choice if you need Binder-level IPC or privileged system API access. The bridge is the right choice if you need a simple HTTP API to run commands, upload files, and download files — especially from scripts or automation.

## License

MIT
