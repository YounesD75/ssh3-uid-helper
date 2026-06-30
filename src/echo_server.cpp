// echo_server.cpp — a minimal TCP "echo" server
// Compile:  g++ -Wall -o echo_server echo_server.cpp
// Run:      ./echo_server
// Test:     in another terminal ->  nc localhost 9000   (then type something)

#include <sys/socket.h>  // socket, bind, listen, accept
#include <netinet/in.h>  // sockaddr_in, htons
#include <arpa/inet.h>   // inet_addr
#include <unistd.h>      // read, write, close
#include <stdio.h>       // printf, perror

int main() {
    // 1. Create the socket (AF_INET = IPv4, SOCK_STREAM = TCP)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // Let us re-run immediately without "address already in use"
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind: choose the address (localhost) and port (9000)
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(9000);          // htons = put the port in network byte order
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    // 3. Listen: start waiting for clients (backlog of 1 pending connection)
    if (listen(server_fd, 1) < 0) { perror("listen"); return 1; }
    printf("Listening on 127.0.0.1:9000 ...\n");

    // 4. Accept one client -> gives us a NEW fd for that conversation
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) { perror("accept"); return 1; }
    printf("Client connected!\n");

    // 5. Echo loop: read bytes, then write the same bytes back
    char buffer[1024];
    for (;;) {
        ssize_t n = read(client_fd, buffer, sizeof(buffer));
        if (n <= 0) break;                 // 0 = client closed, <0 = error
        write(client_fd, buffer, n);       // send the exact same bytes back
        printf("Echoed %zd bytes\n", n);
    }

    printf("Client disconnected.\n");
    close(client_fd);
    close(server_fd);
    return 0;
}
