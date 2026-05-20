#include <iostream>
#include <string>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>
using namespace std;
mutex cout_mtx;

int CreateServerSocket(int port);
int AcceptClient(int server_fd, std::string& client_ip, int& client_port);
bool SendResponse(int client_fd, const std::string& response);
bool SendGreeting(int client_fd);
void ProcessCommand(int client_fd, const std::string& cmd, bool& quit, int& data_listen_fd);
void ReceiveClientData(int client_fd);
void HandleClient(int client_fd, const std::string& client_ip, int client_port);
int CreateDataListener(int& data_port);
void RunServer(int port);

int CreateServerSocket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        cerr << "socket failed" << endl;
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "bind failed: " << strerror(errno) << endl;
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) == -1) {
        cerr << "listen failed" << endl;
        close(server_fd);
        return -1;
    }
    
    return server_fd;
}

int AcceptClient(int server_fd, string& client_ip, int& client_port) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        cerr << "accept failed" << endl;
        return -1;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    client_ip = string(ip_str);
    client_port = ntohs(client_addr.sin_port);

    return client_fd;
}

bool SendResponse(int client_fd, const string& response) {
    string msg = response + "\r\n";
    return send(client_fd, msg.c_str(), msg.size(), 0) != -1;
}

bool SendGreeting(int client_fd) {
    const char* greeting = "220 ready\r\n";
    if (send(client_fd, greeting, strlen(greeting), 0) == -1) {
        cerr << "send failed" << endl;
        return false;
    }
    return true;
}

void ProcessCommand(int client_fd, const string& cmd, bool& quit, int& data_listen_fd) {
    size_t space = cmd.find(' ');
    string op = cmd.substr(0, space);
    transform(op.begin(), op.end(), op.begin(), ::toupper);
    string arg = (space == string::npos) ? "" : cmd.substr(space+1);

    if (op == "USER") {
    SendResponse(client_fd, "331 User name okay, need password.");
    }
    else if (op == "PASS") {
        SendResponse(client_fd, "230 User logged in.");
    }
    else if (op == "PASV") {
        if (data_listen_fd != -1) {
            close(data_listen_fd);
            data_listen_fd = -1;
        }
        int data_port;
        data_listen_fd = CreateDataListener(data_port);
        if (data_listen_fd == -1) {
            cerr << "data_listen failed" << endl;
            return;
        }
        int p1 = data_port / 256;
        int p2 = data_port % 256;
        string response = "227 Entering Passive Mode (127,0,0,1," + to_string(p1) + "," + to_string(p2) +").";
        SendResponse(client_fd, response);
    }
    else if (op == "LIST") {
        if (data_listen_fd == -1) {
            SendResponse(client_fd, "425 Use PORT or PASV first.");
            return;
        }
        SendResponse(client_fd, "150 Opening data connection.");

        int data_client_fd = accept(data_listen_fd, nullptr, nullptr);
        if (data_client_fd == -1) {
            SendResponse(client_fd, "425 Can't open data_connection.");
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        const char* listing = "f1.txt\r\nf2.txt\r\nhello.cpp\r\n";
        send(data_client_fd, listing, strlen(listing), 0);

        close(data_client_fd);
        close(data_listen_fd);
        data_listen_fd = -1;

        SendResponse(client_fd, "226 Transfer complete.");
    }
    else if (op == "QUIT") {
        SendResponse(client_fd, "221 Goodbye.");
        quit = true;
    }
    else {
        SendResponse(client_fd, "500 Unknown command.");
    }
}

void ReceiveClientData(int client_fd) {
    char buf[1024];
    string line_buf;
    bool quit = false;
    int data_listen_fd = -1;

    while (!quit) {
        memset(buf, 0, sizeof(buf));
        int count = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (count <= 0) break;

        line_buf.append(buf, count);
        size_t pos;
        while ((pos = line_buf.find("\r\n")) != string::npos) {
            string cmd = line_buf.substr(0, pos);
            line_buf.erase(0, pos+2);

            {
                lock_guard<mutex> lock(cout_mtx);
                cout << "[Received] : " << cmd << endl;
            }

            ProcessCommand(client_fd, cmd, quit, data_listen_fd);
            if (quit) break;
        }
    }

    if (data_listen_fd != -1) {
        close(data_listen_fd);
    }
}

void HandleClient(int client_fd, const string& client_ip, int client_port) {
    if (!SendGreeting(client_fd)) {
        close(client_fd);
        return;
    }

    ReceiveClientData(client_fd);
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "客户端已断开" << endl;
    }
    close(client_fd);
}

int CreateDataListener(int& data_port) {
    struct sockaddr_in data_addr;
    int data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd == -1) {
        cerr << "socket failed" << endl;
        return -1;
    }

    int opt = 1;
    setsockopt(data_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = INADDR_ANY;
    data_addr.sin_port = htons(0);

    if (bind(data_fd, (struct sockaddr*)&data_addr, sizeof(data_addr)) == -1) {
        cerr << "bind failed" << endl;
        close(data_fd);
        return -1;
    }

    if (listen(data_fd, 1) == -1) {
        cerr << "listen failed" << endl;
        close(data_fd);
        return -1;
    }

    socklen_t addr_len = sizeof(data_addr);
    if (getsockname(data_fd, (struct sockaddr*)&data_addr, &addr_len) == -1) {
        cerr << "getsockname failed" << endl;
        close(data_fd);
        return -1;
    }
    data_port = ntohs(data_addr.sin_port);

    return data_fd;
}

void RunServer(int port) {
    int server_fd = CreateServerSocket(port);
    if (server_fd == -1) return;

    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "listening " << port << "..." << endl;
    }

    while (true) {
        string client_ip;
        int client_port;

        int client_fd = AcceptClient(server_fd, client_ip, client_port);
        if (client_fd == -1) continue;

        {
            lock_guard<mutex> lock(cout_mtx);
            cout << client_ip << " : " << client_port << endl;
        }

        thread thr(HandleClient, client_fd, client_ip, client_port);
        thr.detach();
    }
    close(server_fd);
}

int main() {
    const int port = 2100;
    RunServer(port);
    return 0;
}