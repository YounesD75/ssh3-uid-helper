// uid_socket.cpp — create a socket AFTER switching to a target UID
// Compile: g++ -Wall -o uid_socket uid_socket.cpp
// Run:     sudo ./uid_socket <target_uid>     (must start as root)
// Find your own uid first with:  id -u

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: sudo %s <target_uid>\n", argv[0]);
        return 1;
    }
    uid_t target_uid = (uid_t)atoi(argv[1]);

    printf("Before switch: real uid=%d effective uid=%d\n", getuid(), geteuid());

    // Drop ALL three UIDs (real, effective, saved) to target_uid.
    // After this call there is NO way back to root — that's the point
    // (least privilege: once dropped, privileges can't be regained).
    if (setresuid(target_uid, target_uid, target_uid) < 0) {
        perror("setresuid");
        return 1;
    }
    printf("After switch:  real uid=%d effective uid=%d\n", getuid(), geteuid());

    // NOW create the socket. Its owning UID is fixed at THIS exact instant.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(9100);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(fd, 1) < 0) { perror("listen"); return 1; }

    printf("Listening on 127.0.0.1:9100 as uid=%d (pid=%d).\n", getuid(), getpid());
    printf("Sleeping 30s so you can inspect it with ss/ps...\n");
    sleep(30);

    close(fd);
    return 0;
}
