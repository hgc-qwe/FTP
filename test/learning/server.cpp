#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
using namespace std;

int main() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2100);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        cerr << "socket error" << endl;
        return 1;
    }

    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "bind error" << endl;
        return 1;
    }

    if (listen(listen_fd, 128) == -1) {
        cerr << "listen error" << endl;
        return 1;
    }

    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        cerr << "accept error" << endl;
        return 1;
    }

    char buf[BUFSIZ];
    while (1) {
        int ret = read(client_fd, buf, sizeof(buf));
        write(STDOUT_FILENO, buf, ret);
        for (int i = 0; i < ret; i++) {
            buf[i] = toupper(buf[i]);
        }
        write(client_fd, buf, ret);
    }
    close(listen_fd);
    close(client_fd);

    return 0;
}