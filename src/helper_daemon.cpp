// helper_daemon.cpp — the privilege-separation helper daemon (Phase 3)
//
// Architecture:
//   - Listens on a UNIX socket (/tmp/ssh3-helper.sock)
//   - Receives text requests: "uid=1000 gid=1000 host=127.0.0.1 port=8000\n"
//   - For each request: fork() -> child drops to target uid/gid, creates a
//     TCP socket, connects to remote, passes the fd via SCM_RIGHTS, exits.
//   - Parent stays root and loops back to accept the next request.
//
// Compile: g++ -Wall -o helper_daemon helper_daemon.cpp
// Run:     sudo ./helper_daemon
// Test:    ./helper_client 1000 1000 127.0.0.1 8000

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define DAEMON_SOCK "/tmp/ssh3-helper.sock"

// ── RAII file-descriptor wrapper ─────────────────────────────────────────────
// Ensures every fd is closed when it goes out of scope, even on error paths.
// This is the C++ "good practice" that makes code leak-free automatically.
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() { if (fd >= 0) close(fd); }
    Fd(const Fd&) = delete;             // non-copyable
    Fd& operator=(const Fd&) = delete;
    operator int() const { return fd; } // implicit cast: use Fd where int needed
};

// ── send_fd: pass a file descriptor via SCM_RIGHTS ───────────────────────────
// (Same mechanism as fd_sender_uid.cpp — now packaged as a reusable function)
static int send_fd(int sock, int fd_to_send) {
    char dummy = '!';
    struct iovec iov { &dummy, 1 };

    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));

    struct msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd_to_send, sizeof(int));

    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

// ── handle_request: runs in the CHILD process after fork() ───────────────────
static void handle_request(int client_sock,
                            uid_t uid, gid_t gid,
                            const char* host, int port) {
    // 1. Drop ALL privileges — irreversible, but we're in a child process
    if (setresgid(gid, gid, gid) < 0) { perror("[child] setresgid"); exit(1); }
    if (setresuid(uid, uid, uid) < 0) { perror("[child] setresuid"); exit(1); }
    printf("[child %d] running as uid=%d gid=%d\n", getpid(), getuid(), getgid());

    // 2. Create the TCP socket — NOW owned by uid/gid (kernel stamps at creation)
    Fd tcp_fd(socket(AF_INET, SOCK_STREAM, 0));
    if (tcp_fd < 0) { perror("[child] socket"); exit(1); }

    // 3. Connect to the remote address
    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(tcp_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[child] connect");
        exit(1);
    }
    printf("[child %d] connected to %s:%d, fd=%d\n", getpid(), host, port, (int)tcp_fd);

    // 4. Pass the connected fd back to the caller via SCM_RIGHTS
    if (send_fd(client_sock, tcp_fd) < 0) { perror("[child] send_fd"); exit(1); }
    printf("[child %d] fd sent. Exiting.\n", getpid());
    exit(0);
}

// ── main: the daemon loop ─────────────────────────────────────────────────────
int main() {
    if (getuid() != 0) {
        fprintf(stderr, "Must run as root (sudo ./helper_daemon)\n");
        return 1;
    }

    // Create the UNIX listening socket
    Fd server(socket(AF_UNIX, SOCK_STREAM, 0));
    if (server < 0) { perror("socket"); return 1; }

    unlink(DAEMON_SOCK);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", DAEMON_SOCK);

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server, 8) < 0)  { perror("listen"); return 1; }

    // Allow any local user to connect for now.
    // Phase 4 will restrict this via SO_PEERCRED (check caller UID at accept time).
    chmod(DAEMON_SOCK, 0666);

    printf("[daemon] Listening on %s\n", DAEMON_SOCK);

    // Main accept loop
    for (;;) {
        Fd client(accept(server, nullptr, nullptr));
        if (client < 0) { perror("accept"); continue; }
        printf("[daemon] New connection.\n");

        // Read the request line: "uid=1000 gid=1000 host=127.0.0.1 port=8000\n"
        char buf[256] = {};
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        if (n <= 0) { fprintf(stderr, "[daemon] empty request\n"); continue; }
        buf[n] = '\0';
        printf("[daemon] Request: %s", buf);

        uid_t uid = 0; gid_t gid = 0;
        char host[64] = {}; int port = 0;
        if (sscanf(buf, "uid=%u gid=%u host=%63s port=%d", &uid, &gid, host, &port) != 4) {
            fprintf(stderr, "[daemon] malformed request\n"); continue;
        }

        // Fork: child handles the request, parent loops back immediately
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            // CHILD — close the server fd (child doesn't need it)
            close(server);
            handle_request(client, uid, gid, host, port);
            // handle_request calls exit() — never reaches here
        }

        // PARENT — close our copy of client fd (child has its own)
        // and reap finished children to avoid zombies
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }

    unlink(DAEMON_SOCK);
    return 0;
}
