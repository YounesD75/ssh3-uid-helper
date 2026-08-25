# Secure socket management in modular proxying

A C++ privilege-separation helper for SSH3, restoring per-user socket ownership for proxied connections.

## Problem

SSH3 proxies TCP/UDP connections on behalf of authenticated users, but the relayed sockets are created by the server process, which runs as root. Since socket ownership is fixed by the kernel at creation time, this breaks per-user traffic accountability (e.g. iptables --uid-owner rules never match).

## Goal

A lightweight C++ daemon that creates sockets under the correct UID/GID and hands the resulting file descriptor to SSH3 via SCM_RIGHTS, plus a Go-side DialUID() to replace the net.DialTCP() call in handleTCPForwardingChannel (cmd/ssh3-server.go, SSH3 upstream).

## Status

Work in progress (internship project, June-September 2026).

## Structure

- `src/` - C++ proof-of-concept programs (sockets, UID switching, IPC).
- `src/run_all_tests.sh` - automated end-to-end test suite (see below).
- `ssh3-uid-helper.patch` - the SSH3 server patch replacing `net.DialTCP()` with `dialUID()`.
- `src/dial_uid.go` - Go-side helper client, to be dropped into `ssh3/cmd/`.

## Running the test suite

Everything can be validated with a single script. It builds nothing - it assumes
the binaries are already compiled - starts every component in the right order,
runs all eight tests, prints a PASS/FAIL summary, and writes `test_report.txt`.

```bash
# Run the full suite (needs root: the daemon and the SSH3 server run as root)
sudo ./src/run_all_tests.sh

# Stop everything the suite left running
sudo ./src/run_all_tests.sh --clean
```

The suite deliberately leaves the daemon, the SSH3 server and the forwarded
connection alive when it finishes, so the evidence stays on screen for
inspection (`ss`, `ps`) and screenshots.

### What each test covers

| Test | What it proves |
|------|----------------|
| TEST 1 | The daemon starts and creates `/tmp/ssh3-helper.sock` with SO_PEERCRED auth |
| TEST 2 | An unprivileged caller may request a socket under its **own** UID |
| TEST 3 | A root caller may request a socket under **any** UID (this is how SSH3 calls it) |
| TEST 4 | A caller requesting a **different** UID is rejected, and no fd is passed |
| TEST 5 | The resulting socket is owned by the target user, not by the root daemon |
| TEST 6 | The patched SSH3 server actually invokes `dialUID()` when forwarding TCP |
| TEST 7 | All binaries exist and are newer than their sources |
| TEST 8 | The patch file and the `dialUID` call site are present in the tree |

TEST 7 and TEST 8 are static checks and need no root; TEST 1-6 are runtime checks.

### Prerequisites for the suite

The suite expects these to already exist:

```
src/helper_daemon_v2          g++ -Wall -o helper_daemon_v2 helper_daemon_v2.cpp
src/helper_client             g++ -Wall -o helper_client helper_client.cpp
ssh3/ssh3-server-patched      go build -o ssh3-server-patched ./cmd/ssh3-server
ssh3/ssh3                     the stock SSH3 client
ssh3/cert.pem, ssh3/priv.key  server certificate and key
~/.ssh/id_ed25519             the SSH key used by the client
```

Note: `go build ./cmd/...` fails because `cmd/` holds several `main` packages -
build `./cmd/ssh3-server` specifically.

## How to use the SSH3 patch

### Prerequisites
- Ubuntu Linux (tested on 24.04 LTS)
- Go 1.21+ and a C compiler (gcc) installed
- The SSH3 source code: `git clone https://github.com/francoismichel/ssh3`

### Step 1 — Build and launch the helper daemon
```bash
cd ssh3-uid-helper/src
g++ -Wall -o helper_daemon_v2 helper_daemon_v2.cpp
sudo ./helper_daemon_v2
# [daemon] Listening on /tmp/ssh3-helper.sock
```

### Step 2 — Apply the patch and build the SSH3 server
```bash
cd ssh3
git apply ../ssh3-uid-helper/ssh3-uid-helper.patch
cp ../ssh3-uid-helper/src/dial_uid.go cmd/
CGO_ENABLED=1 go build -o ssh3-server cmd/ssh3-server/main.go
```

### Step 3 — Run the SSH3 server
```bash
sudo ./ssh3-server -generate-selfsigned-cert -bind 127.0.0.1:4443 -url-path /ssh3
```

When a user connects and requests TCP port forwarding, the server now contacts
the helper daemon to create the relayed socket under the correct UID instead of root.
Verify with: `sudo ss -tnp | grep <port>` — the socket owner should match the
authenticated user, not the server process.
