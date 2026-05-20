#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>
using namespace std;
mutex cout_mtx;

void HandleClient(int client_fd, const string& client_ip, int client_port) {
    const char* greeting = "220 ready\r\n";
    if (send(client_fd, greeting, strlen(greeting), 0) == -1) {
        cerr << "send failed" << endl;
    }

    char buf[1024];
    while (true) {
        memset(buf, 0, 1024);
        int count = recv(client_fd, buf, 1024 - 1, 0);
        if (count <= 0) {
            break;
        } else {
            buf[count] = '\0';
        }
        {
            lock_guard<mutex> lock(cout_mtx);
            cout <<"[Received] : " << buf;
        }
        if (buf[count - 1] != '\n') cout << endl;
    }
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "客户端已断开" << endl;
    }
    close(client_fd);
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        cerr << "socket failed" << endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(2100);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "bind failed" << endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) == -1) {
        cerr << "listen failed" << endl;
        close(server_fd);
        return 1;
    }
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "listening " << 2100 << "..." << endl;
    }

    while (true) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == -1) {
            cerr << "accept failed" << endl;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);

        {
            lock_guard<mutex> lock(cout_mtx);
            cout << client_ip << " : " << client_port << endl;
        }

        thread thr(HandleClient, client_fd, string(client_ip), client_port);
        thr.detach();
    }
    close(server_fd);
    return 0;
}