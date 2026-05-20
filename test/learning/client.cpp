#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
using namespace std;

int main() {
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd == -1) {
        cerr << "socket error" << endl;
        return 1;
    }

    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(2100);
    inet_pton(cfd, "127.0.0.1", &client_addr.sin_addr.s_addr);
    socklen_t client_len = sizeof(client_addr);

    if (connect(cfd, (struct sockaddr*)&client_addr, client_len) == -1) {
        cerr << "connect error" << endl;
        return 1;
    }

    char buf[BUFSIZ];
    char input[BUFSIZ];
    while (true) {
        int n = read(0, input, sizeof(input));
        write(cfd, input, n);
        int ret = read(cfd, buf, sizeof(buf));
        write(1, buf, ret);
    }
    close(cfd);

    return 0;
}