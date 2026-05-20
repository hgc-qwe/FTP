#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <thread>
#include <dirent.h>
#include <cstdio>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
using namespace std;
mutex cout_mtx;

struct ClientSession {
    string root_path = ".";
    string current_dir = "/";
    bool logged_in = false;
    string username;

    string normalize(const string& path) {
        if (path.empty()) return current_dir;

        string full;
        if (path[0] == '/') {
            full = path;
        } else {
            full = current_dir;
            if (full.back() != '/') full += '/';
            full += path;
        }

        vector<string> parts;
        size_t start = 0;
        while (start < full.size()) {
            if (full[start] == '/') {
                start++;
                continue;
            }
            size_t end = full.find('/', start);
            string part = full.substr(start, end - start);
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (part != "." && !part.empty()) {
                parts.push_back(part);
            }
            start = (end == string::npos) ? full.size() : end;
        }

        string res = "/";
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) res += "/";
            res += parts[i];
        }
        return res;
    }

    string getRealPath(const string& filepath) {
        string virtual_path = normalize(filepath);
        string real = root_path + virtual_path;
        if (real.find(root_path) != 0) return "";
        return real;
    }
};

int CreateServerSocket(int port);
int AcceptClient(int server_fd, std::string& client_ip, int& client_port);
bool SendResponse(int client_fd, const std::string& response);
bool SendGreeting(int client_fd);
void ProcessCommand(int client_fd, const std::string& cmd, bool& quit, int& data_listen_fd, ClientSession& session);
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
    return SendResponse(client_fd, "220 ready");
}

void ProcessCommand(int client_fd, const string& cmd, bool& quit, int& data_listen_fd, ClientSession& session) {
    size_t space = cmd.find(' ');
    string op = cmd.substr(0, space);
    transform(op.begin(), op.end(), op.begin(), ::toupper);
    string arg = (space == string::npos) ? "" : cmd.substr(space+1);

    if (op == "USER") {
        if (arg.empty()) {
            SendResponse(client_fd, "501 No username given.");
        } else {
            session.username = arg;
            session.logged_in = false;
            SendResponse(client_fd, "331 User name okay, need password.");
        }
    }
    else if (op == "PASS") {
        if (session.username.empty()) {
            SendResponse(client_fd, "503 Login with USER first.");
        } else if (session.username == "hgc-qwe" && arg == "1234567") {
            session.logged_in = true;
            SendResponse(client_fd, "230 User logged in.");
        } else if (session.username == "anonymous") {
            session.logged_in = true;
            SendResponse(client_fd, "230 Anonymous login ok.");
        } else {
            SendResponse(client_fd, "530 Login incorrect.");
            session.username.clear();
        }
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
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        if (data_listen_fd == -1) {
            SendResponse(client_fd, "425 Use PORT or PASV first.");
            return;
        }

        string real_path = session.getRealPath(arg.empty() ? "." : arg);
        string path = real_path.empty() ? "." : real_path;
        if (real_path.empty()) {
            SendResponse(client_fd, "550 Permission denied.");
            return;
        }

        DIR* dir = opendir(real_path.c_str());
        if (dir == nullptr) {
            SendResponse(client_fd, "550 Failed to open directory.");
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }
        SendResponse(client_fd, "150 Opening data connection.");
        
        int data_client_fd = accept(data_listen_fd, nullptr, nullptr);
        if (data_client_fd == -1) {
            SendResponse(client_fd, "425 Can't open data_connection.");
            closedir(dir);
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        string listing;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            string name = entry->d_name;
            if (name == "." || name == "..") continue;
            listing += name + "\r\n";
        }
        closedir(dir);
        send(data_client_fd, listing.c_str(), listing.size(), 0);

        close(data_client_fd);
        close(data_listen_fd);
        data_listen_fd = -1;

        SendResponse(client_fd, "226 Transfer complete.");
    }
    else if (op == "RETR") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        if (data_listen_fd == -1) {
            SendResponse(client_fd, "425 Use PORT or PASV first.");
            return;
        }

        string real_path = session.getRealPath(arg.empty() ? "." : arg);
        if (real_path.empty()) {
            SendResponse(client_fd, "501 No filename given.");
            return;
        }

        int file_fd = open(real_path.c_str(), O_RDONLY);
        if (file_fd == -1) {
            SendResponse(client_fd, "550 Failed to open file.");
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        struct stat st;
        fstat(file_fd, &st);
        off_t offset = 0;
        ssize_t remaining = st.st_size;
        SendResponse(client_fd, "150 Opening data connection.");

        int data_client_fd = accept(data_listen_fd, nullptr, nullptr);
        if (data_client_fd == -1) {
            SendResponse(client_fd, "425 Can't open data connection.");
            close(file_fd);
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        while (remaining > 0) {
            ssize_t sent = sendfile(data_client_fd, file_fd, &offset, remaining);
            if (sent <= 0) {
                if (errno == EINTR) continue;
                break;
            }
            remaining -= sent;
        }
        close(file_fd);
        close(data_client_fd);
        close(data_listen_fd);
        data_listen_fd = -1;
        SendResponse(client_fd, "226 Transfer complete.");
    }
    else if (op == "STOR") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        if (data_listen_fd == -1) {
            SendResponse(client_fd, "425 Use PORT or PASV first.");
            return;
        }

        string real_path = session.getRealPath(arg.empty() ? "." : arg);
        if (real_path.empty()) {
            SendResponse(client_fd, "501 No filename given.");
            return;
        }

        FILE* fp = fopen(real_path.c_str(), "wb");
        if (fp == nullptr) {
            SendResponse(client_fd, "550 Failed to creat file.");
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        SendResponse(client_fd, "150 Opening data connection.");

        int data_client_fd = accept(data_listen_fd, nullptr, nullptr);
        if (data_client_fd == -1) {
            SendResponse(client_fd, "425 Can't open data connection.");
            fclose(fp);
            close(data_listen_fd);
            data_listen_fd = -1;
            return;
        }

        char buf[8192];
        size_t n;
        while ((n = recv(data_client_fd, buf, sizeof(buf), 0)) > 0) {
            fwrite(buf, 1, n, fp);
        }
        fclose(fp);
        close(data_client_fd);
        close(data_listen_fd);
        data_listen_fd = -1;

        SendResponse(client_fd, "226 Transfer complete.");
    }
    else if (op == "QUIT") {
        SendResponse(client_fd, "221 Goodbye.");
        quit = true;
    }
    else if (op == "FEAT") {
        SendResponse(client_fd, "211 No features.");
    }
    else if (op == "SYST") {
        SendResponse(client_fd, "215 UNIX Type: L8");
    }
    else if (op == "TYPE") {
        SendResponse(client_fd, "200 Type set to " + arg + ".");
    }
    else if (op == "PWD") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        SendResponse(client_fd, "257 \"" + session.current_dir + "\" is current directory.");
    }
    else if (op == "SIZE") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        struct stat st;
        string real_path = session.getRealPath(arg.empty() ? "." : arg);
        if (stat(real_path.c_str(), &st) == 0) {
            SendResponse(client_fd, "213 " + to_string(st.st_size));
        } else {
            SendResponse(client_fd, "550 File not found.");
        }
    }
    else if (op == "MDTM") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        struct stat st;
        string real_path = session.getRealPath(arg.empty() ? "." : arg);
        if (stat(real_path.c_str(), &st) == 0) {
            char t[20];
            strftime(t, sizeof(t), "%Y%m%d%H%M%S", gmtime(&st.st_mtime));
            SendResponse(client_fd, "213 " + string(t));
        } else {
            SendResponse(client_fd, "550 File not found.");
        }
    }
    else if (op == "CWD") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        string new_virtual = session.normalize(arg);
        string real_path = session.root_path + new_virtual;

        struct stat st;
        if (stat(real_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            session.current_dir = new_virtual;
            SendResponse(client_fd, "250 Directory changed.");
        } else {
            SendResponse(client_fd, "550 Directory not found.");
        }
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
    ClientSession session;

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
                if (cmd.find("PASS") == 0) {
                    cout << "[Received] : " << "PASS ***" << endl;
                } else {
                    cout << "[Received] : " << cmd << endl;
                }
            }

            ProcessCommand(client_fd, cmd, quit, data_listen_fd, session);
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