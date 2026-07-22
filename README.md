# Secure socket management in modular proxying

A C++ privilege-separation helper for SSH3, restoring per-user socket ownership for proxied connections.

## Problem

SSH3 proxies TCP/UDP connections on behalf of authenticated users, but the relayed sockets are created by the server process, which runs as root. Since socket ownership is fixed by the kernel at creation time, this breaks per-user traffic accountability (e.g. iptables --uid-owner rules never match).

## Goal

A lightweight C++ daemon that creates sockets under the correct UID/GID and hands the resulting file descriptor to SSH3 via SCM_RIGHTS, plus a Go-side DialUID() to replace the net.DialTCP() call in handleTCPForwardingChannel (cmd/ssh3-server.go, SSH3 upstream).

## Status

Work in progress (internship project, June-September 2026).

## Structure

- src/ - C++ proof-of-concept programs (sockets, UID switching, IPC).

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
