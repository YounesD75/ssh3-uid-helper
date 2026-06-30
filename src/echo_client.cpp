// echo_client.cpp — a minimal TCP client that talks to echo_server
// Compile:  g++ -Wall -o echo_client echo_client.cpp
// Run:      ./echo_client      (echo_server must be running in another terminal)

#include <arpa/inet.h>   // sockaddr_in, htons, inet_addr
#include <unistd.h>      // read, write, close
#include <cstdio>        // printf, perror, fgets
#include <cstring>       // strlen

int main() {
    // 1. Create the socket (same kind as the server: IPv4, TCP)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // 2. Describe WHO we want to reach: 127.0.0.1 on port 9000
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(9000);

    // 3. connect() = dial the server and wait until it accepts
    //    (no bind() needed: the OS picks a temporary local port for us)
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
    printf("Connected to 127.0.0.1:9000. Type something (Ctrl-D to quit):\n");

    // 4. Loop: read a line from the keyboard, send it, read the echo back
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        ssize_t len = (ssize_t)strlen(line);
        write(sock, line, len);                          // send what you typed

        char buffer[1024];
        ssize_t n = read(sock, buffer, sizeof(buffer));  // wait for the echo
        if (n <= 0) break;                               // server closed or error
        printf("Echo: %.*s", (int)n, buffer);            // print exactly n bytes
    }

    close(sock);
    return 0;
}
