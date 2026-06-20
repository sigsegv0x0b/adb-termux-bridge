# Termux ADB Bridge

A lightweight HTTP-over-mTLS daemon that runs inside Termux, providing a REST API to execute arbitrary shell commands, stream output, upload, and download files on an Android device.

## Why

Termux on Android cannot use `/dev/binder` for Binder IPC (the kernel does not expose it to app processes), so the standard Android inter-process communication mechanism is unavailable. Additionally, Termux runs as a regular Android app process with no special privileges.

This bridge solves both problems by:

1. Running as a daemon that listens on `127.0.0.1` (loopback only, no network exposure)
2. Providing a REST API that any local process (including scripts) can call over TLS
3. Using mutual TLS (mTLS) with ED25519 certificates so that only clients holding a CA-signed certificate can issue commands

The bridge itself has no special privileges. It runs at the same UID as Termux. To perform privileged operations, pair it with ADB, `su`, or Shizuku.

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

The bridge runs inside Termux on Android, which ships with a full GCC/Clang toolchain but no pre-installed Go or Rust cross-compilers. A single C file compiles in seconds with no dependencies beyond OpenSSL (already installed in Termux). C also gives precise control over memory, signals, and forking — all critical for a daemon that manages subprocesses.

### Why custom HTTP, not libmicrohttpd or libcurl

The bridge serves exactly one concurrent client (loopback) with a tiny API surface. Adding a dependency on libmicrohttpd would pull in dozens of source files and complicate cross-builds. The HTTP parser in `server.c` is ~150 lines — it only needs to parse the request line, headers, and Content-Length.

### Why mTLS over plain TCP or SSH

- **Plain TCP**: Anyone on the device could connect and issue arbitrary commands as Termux. The bridge binds to `127.0.0.1` only, but other apps on the same device can still connect to `127.0.0.1:10099`. mTLS ensures only whitelisted clients can talk to the bridge.
- **SSH**: SSH requires a user account, password/key management, and a separate SSH server. The bridge is a single binary that starts in under 10ms.
- **mTLS with ED25519**: No passwords, no server-side session state. The client presents a certificate signed by the bridge's own CA. The bridge verifies the signature against its trusted CA cert. ED25519 keys are 32 bytes, signature verification is extremely fast, and OpenSSL supports them natively.

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
- **The Termux user**: Can start/stop the bridge at will
- **Clients holding a CA-signed certificate**: Can execute commands
- **localhost (`127.0.0.1`)**: The only interface the bridge listens on

The bridge does **not** trust:
- **Other apps on the device**: Blocked by mTLS (they need a signed client cert)
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

## Build Modes

### Normal mode

```
make
./termux-adb-bridge --init-certs
./termux-adb-bridge
```

Certificates are loaded from `~/.termux-adb-bridge/certs/` at runtime. Useful for development and environments where the filesystem is available.

### Secure mode

```
make
./termux-adb-bridge --init-certs
make secure
```

The `make secure` target:
1. Builds the normal binary
2. Saves all PEM files to `<cert-dir>/<fingerprint>/` via `--save-certs`
3. Generates `src/certs_data.h` with all certs and keys as embedded DER byte arrays
4. Rebuilds the binary as `termux-adb-bridge-secure` with `-DSECURE_BUILD`

The secure binary contains all cryptographic material baked in. It ignores the filesystem entirely — no PEM files need to be present at runtime. This is useful when:
- Deploying to a read-only filesystem
- You want to ensure the private key never appears on disk as a separate file
- You want a single-file deployment

In secure mode, `--cert`, `--ca`, and `--save-certs` still work — they extract the embedded certs and save them to disk. Server startup also auto-saves to the fingerprint directory.

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

## Deployment

### On-device (Termux)

```
git clone <this repo>
cd termux-adb-bridge
./install.sh
```

`install.sh` builds the binary and generates certificates. The bridge can then be started with:

```
./termux-adb-bridge --daemon
```

### Via ADB from desktop

```
bridge.sh install            # pushes binary + certs, starts daemon, sets up forward
bridge.sh health             # check if running
bridge.sh exec 'echo hi'     # run a command
```

## Dependencies

- **OpenSSL ≥ 3.0** (`libssl-dev`, `libcrypto`): TLS, X.509, ED25519
- **POSIX threads** (`-pthread`): Connection concurrency
- **Standard C library**: Fork, exec, pipes, poll

## Comparison with Shizuku

| Aspect | Shizuku | termux-adb-bridge |
|--------|---------|-------------------|
| Language | Java (C++ injector) | C |
| IPC | Android Binder | HTTP + mTLS |
| Privilege elevation | ADB shell + app_process | None (runs at Termux UID) |
| Certificates | None (UID-based auth) | ED25519 mTLS |
| Process model | fork + setsid + app_process | fork + setsid + execve |
| Dependencies | Android framework | OpenSSL only |
| Target | Granting apps system-level Binder access | Executing shell commands via REST API |

Shizuku is the right choice if you need Binder-level IPC or privileged system API access. The bridge is the right choice if you need a simple HTTP API to run commands, upload files, and download files — especially from scripts or automation.

## License

MIT
