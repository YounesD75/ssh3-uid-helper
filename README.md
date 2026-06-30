# Secure socket management in modular proxying

A C++ privilege-separation helper for [SSH3](https://github.com/francoismichel/ssh3), restoring per-user
socket ownership for proxied connections.

## Problem

SSH3 proxies TCP/UDP connections on behalf of authenticated users, but the relayed sockets are created
by the server process, which runs as root. Since socket ownership is fixed by the kernel at creation
time, this breaks per-user traffic accountability (e.g. `iptables --uid-owner` rules never match).

## Goal

A lightweight C++ daemon that creates sockets under the correct UID/GID and hands the resulting file
descriptor to SSH3 via `SCM_RIGHTS`, plus a Go-side `DialUID()` to replace the `net.DialTCP()` call in
`handleTCPForwardingChannel` (`cmd/ssh3-server.go`, SSH3 upstream).

## Status

Work in progress (internship project, June-September 2026).

## Structure

- `src/` - C++ proof-of-concept programs (sockets, UID switching, IPC).
