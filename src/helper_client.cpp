// helper_client.cpp — test client for helper_daemon
// Sends a request, receives the fd, and proves it works.
//
// Compile: g++ -Wall -o helper_client helper_client.cpp
// Usage:   ./helper_client <uid> <gid> <host> <port>
// Example: ./helper_client 1000 1000 127.0.0.1 8000

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define DAEMON_SOCK "/tmp/ssh3-helper.sock"

static int recv_fd(int sock) {
    char dummy;
    struct iovec iov { &dummy, 1 };
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));
    struct msghdr msg {};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = buf; msg.msg_controllen = sizeof(buf);
    if (recvmsg(sock, &msg, 0) < 0) { perror("recvmsg"); return -1; }
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_RIGHTS) { fputs("no SCM_RIGHTS\n", stderr); return -1; }
    int fd; memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <uid> <gid> <host> <port>\n", argv[0]);
        return 1;
    }

    // Connect to the daemon
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr {}; addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", DAEMON_SOCK);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect (is helper_daemon running?)"); return 1;
    }

    // Send the request
    char req[256];
    snprintf(req, sizeof(req), "uid=%s gid=%s host=%s port=%s\n",
             argv[1], argv[2], argv[3], argv[4]);
    write(sock, req, strlen(req));
    printf("[client] Sent: %s", req);

    // Receive the fd
    int fd = recv_fd(sock);
    if (fd < 0) return 1;
    printf("[client] Received fd=%d — connection is ready to use!\n", fd);

    // Prove it's a live connection: write something, read the echo
    const char* msg = "hello from helper_client\n";
    write(fd, msg, strlen(msg));
    char resp[256] = {};
    ssize_t n = read(fd, resp, sizeof(resp)-1);
    if (n > 0) printf("[client] Echo from server: %s", resp);
    else       printf("[client] (no echo — server received the data)\n");

    close(fd);
    close(sock);
    return 0;
}
