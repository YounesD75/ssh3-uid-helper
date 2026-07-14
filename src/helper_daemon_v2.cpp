// helper_daemon_v2.cpp — Phase 4: adds SO_PEERCRED authentication
//
// New security feature: before processing any request, the daemon checks
// the caller's identity using SO_PEERCRED. Only processes running as root
// or as the same user as the target UID are allowed to request a socket.
// Any other caller is rejected immediately.
//
// Compile: g++ -Wall -o helper_daemon_v2 helper_daemon_v2.cpp
// Run:     sudo ./helper_daemon_v2

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define DAEMON_SOCK "/tmp/ssh3-helper.sock"

// ── RAII file-descriptor wrapper ─────────────────────────────────────────────
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() { if (fd >= 0) close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    operator int() const { return fd; }
};

// ── send_fd: pass a file descriptor via SCM_RIGHTS ───────────────────────────
static int send_fd(int sock, int fd_to_send) {
    char dummy = '!';
    struct iovec iov { &dummy, 1 };
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));
    struct msghdr msg {};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = buf; msg.msg_controllen = sizeof(buf);
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd_to_send, sizeof(int));
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

// ── check_caller: SO_PEERCRED authentication ─────────────────────────────────
// Returns true if the caller is authorized, false otherwise.
// Authorization rule: the caller must be root (uid=0) OR the same user
// as the target UID being requested.
static bool check_caller(int client_sock, uid_t requested_uid) {
    struct ucred cred {};
    socklen_t len = sizeof(cred);

    // SO_PEERCRED asks the kernel: "who is on the other end of this socket?"
    // The kernel fills in pid, uid, gid of the connected process.
    if (getsockopt(client_sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        perror("[daemon] getsockopt SO_PEERCRED");
        return false;
    }

    printf("[daemon] Caller: pid=%d uid=%d gid=%d\n",
           cred.pid, cred.uid, cred.gid);

    // Rule 1: root can request any UID (SSH3 server runs as root)
    if (cred.uid == 0) {
        printf("[daemon] Caller is root — authorized.\n");
        return true;
    }

    // Rule 2: a user can only request a socket under their own UID
    if (cred.uid == requested_uid) {
        printf("[daemon] Caller uid matches requested uid — authorized.\n");
        return true;
    }

    // Everyone else is rejected
    printf("[daemon] REJECTED: caller uid=%d tried to request uid=%d\n",
           cred.uid, requested_uid);
    return false;
}

// ── handle_request: runs in the CHILD process after fork() ───────────────────
static void handle_request(int client_sock,
                            uid_t uid, gid_t gid,
                            const char* host, int port) {
    if (setresgid(gid, gid, gid) < 0) { perror("[child] setresgid"); exit(1); }
    if (setresuid(uid, uid, uid) < 0) { perror("[child] setresuid"); exit(1); }
    printf("[child %d] running as uid=%d gid=%d\n", getpid(), getuid(), getgid());

    Fd tcp_fd(socket(AF_INET, SOCK_STREAM, 0));
    if (tcp_fd < 0) { perror("[child] socket"); exit(1); }

    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(tcp_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[child] connect"); exit(1);
    }
    printf("[child %d] connected to %s:%d, fd=%d\n",
           getpid(), host, port, (int)tcp_fd);

    if (send_fd(client_sock, tcp_fd) < 0) { perror("[child] send_fd"); exit(1); }
    printf("[child %d] fd sent. Exiting.\n", getpid());
    exit(0);
}

// ── send_error: notify the caller that the request was rejected ───────────────
static void send_error(int client_sock, const char* reason) {
    char msg[128];
    snprintf(msg, sizeof(msg), "ERROR: %s\n", reason);
    write(client_sock, msg, strlen(msg));
}

// ── main: the daemon loop ─────────────────────────────────────────────────────
int main() {
    if (getuid() != 0) {
        fprintf(stderr, "Must run as root (sudo ./helper_daemon_v2)\n");
        return 1;
    }

    Fd server(socket(AF_UNIX, SOCK_STREAM, 0));
    if (server < 0) { perror("socket"); return 1; }

    unlink(DAEMON_SOCK);
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", DAEMON_SOCK);

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server, 8) < 0) { perror("listen"); return 1; }

    // Only root can connect for now (Phase 4 relaxes this via PEERCRED)
    chmod(DAEMON_SOCK, 0666);  // allow any process to connect — PEERCRED filters them

    printf("[daemon] Listening on %s (with SO_PEERCRED auth)\n", DAEMON_SOCK);

    for (;;) {
        Fd client(accept(server, nullptr, nullptr));
        if (client < 0) { perror("accept"); continue; }

        // Read the request
        char buf[256] = {};
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';
        printf("[daemon] Request: %s", buf);

        uid_t uid = 0; gid_t gid = 0;
        char host[64] = {}; int port = 0;
        if (sscanf(buf, "uid=%u gid=%u host=%63s port=%d",
                   &uid, &gid, host, &port) != 4) {
            fprintf(stderr, "[daemon] malformed request\n");
            send_error(client, "malformed request");
            continue;
        }

        // *** PHASE 4: authenticate the caller before doing anything ***
        if (!check_caller(client, uid)) {
            send_error(client, "unauthorized caller");
            continue;   // close client fd, loop back — no fork, no socket
        }

        // Caller is authorized — fork and handle
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }
        if (pid == 0) {
            close(server);
            handle_request(client, uid, gid, host, port);
        }
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }

    unlink(DAEMON_SOCK);
    return 0;
}
