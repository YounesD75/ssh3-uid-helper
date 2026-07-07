// fd_sender_uid.cpp — combines setresuid + SCM_RIGHTS:
// starts as root (sudo), drops to target UID, creates a TCP socket
// (now owned by that UID), then passes the fd to the receiver via SCM_RIGHTS.
//
// Compile: g++ -Wall -o fd_sender_uid fd_sender_uid.cpp
// Run:     sudo ./fd_sender_uid 1000    (replace 1000 with your uid: id -u)

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define SOCKET_PATH "/tmp/fd_pass.sock"

static int send_fd(int sock, int fd) {
    char dummy = '!';
    struct iovec iov;
    iov.iov_base = &dummy;
    iov.iov_len  = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return (sendmsg(sock, &msg, 0) < 0) ? -1 : 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: sudo %s <target_uid>\n", argv[0]);
        return 1;
    }
    uid_t target_uid = (uid_t)atoi(argv[1]);

    // 1. Connect to the receiver FIRST (while still root, before dropping privs)
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket (unix)"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect (is fd_receiver running?)");
        return 1;
    }
    printf("[sender] Connected to receiver.\n");

    // 2. *** THE KEY STEP *** drop privileges to target_uid BEFORE socket()
    printf("[sender] Before setresuid: uid=%d euid=%d\n", getuid(), geteuid());
    if (setresuid(target_uid, target_uid, target_uid) < 0) {
        perror("setresuid");
        return 1;
    }
    printf("[sender] After  setresuid: uid=%d euid=%d\n", getuid(), geteuid());

    // 3. Create the TCP socket NOW — kernel stamps it with target_uid
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) { perror("socket (tcp)"); return 1; }
    printf("[sender] Created TCP socket (fd=%d) under uid=%d.\n", tcp_fd, getuid());

    // 4. Pass the fd to the receiver via SCM_RIGHTS
    if (send_fd(sock, tcp_fd) < 0) { perror("send_fd"); return 1; }
    printf("[sender] fd sent successfully.\n");

    close(tcp_fd);
    close(sock);
    return 0;
}
