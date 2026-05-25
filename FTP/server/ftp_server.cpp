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
#include <queue>
#include <condition_variable>
#include <functional>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unordered_map>
#include <sys/sendfile.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <sys/sysmacros.h>
using namespace std;
mutex cout_mtx;

struct ClientSession {
    string root_path = "";
    string current_dir = "/";
    bool logged_in = false;
    string username;
    off_t restart_offset = 0;

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
        string real_path;

        if (root_path.empty()) {
            real_path = virtual_path;
        } else {
            real_path = root_path + virtual_path;
        }

        char resolved[PATH_MAX];

        if (realpath(real_path.c_str(), resolved) == nullptr) {
            return "";
        }

        string final_path = resolved;

        if (!root_path.empty() &&
            final_path.find(root_path) != 0) {
            return "";
        }

        return final_path;
    }
};

struct ClientContext {
    ClientSession session;
    string read_buf;
    int data_listen_fd = -1;
};

mutex session_mtx;
unordered_map<int, ClientContext> g_session;

class ThreadPool {
public:
    ThreadPool(size_t num_threads) {
        for (size_t i = 0; i < num_threads; i++) {
            workers.emplace_back([this]() {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queue_mtx);
                        cv.wait(lock, [this]() {
                            return stop || !tasks.empty();
                        });

                        if (stop && tasks.empty()) return;

                        task = move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    void Push(function<void()> task) {
        {
            lock_guard<mutex> lock(queue_mtx);
            tasks.push(move(task));
        }
        cv.notify_one();
    }

    ~ThreadPool() {
        {
            lock_guard<mutex> lock(queue_mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queue_mtx;
    condition_variable cv;
    bool stop = false;
};

typedef struct {
    char* name;
    time_t mtime;
}File;

#define MAX_FILES 1000

bool SetNonBlocking(int fd);
int CreateServerSocket(int port);
int AcceptClient(int server_fd, std::string& client_ip, int& client_port);
bool SendResponse(int client_fd, const std::string& response);
bool SendGreeting(int client_fd);
void ProcessCommand(int client_fd, const std::string& cmd, bool& quit, int& data_listen_fd, ClientSession& session);
void ReceiveClientData(int client_fd);
void HandleClient(int client_fd, const std::string& client_ip, int client_port);
void HandleClientNonBlocking(int client_fd, int epoll_fd);
int CreateDataListener(int& data_port);
void RunServer(int port);

bool MakeDirs(const string& dir) {
    if (dir.empty()) return false;

    string path;

    for (size_t i = 0; i < dir.size(); i++) {
        path += dir[i];

        if (dir[i] == '/') {
            if (path.size() == 1) continue;

            mkdir(path.c_str(), 0755);
        }
    }

    mkdir(dir.c_str(), 0755);

    return true;
}

bool SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

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
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
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
    size_t total = 0;
    while (total < msg.size()) {
        ssize_t sent = send(client_fd, msg.c_str() + total, msg.size() - total, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        total += sent;
    }
    return true;
}

bool SendGreeting(int client_fd) {
    return SendResponse(client_fd, "220 ready");
}

int compare(const void* a, const void* b) {
    const File* fa = (File*)a;
    const File* fb = (File*)b;
    return strcoll(fa->name, fb->name);
}

string GenerateList(const string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return "550 Failed to open directory.\r\n";

    File entries[MAX_FILES];
    int count = 0;

    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr && count < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        entries[count].name = strdup(entry->d_name);

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path.c_str(), entry->d_name);

        struct stat st;

        if (lstat(fullpath, &st) == 0) {
            entries[count].mtime = st.st_mtime;
        } else {
            entries[count].mtime = 0;
        }

        count++;
    }
    closedir(dir);

    qsort(entries, count, sizeof(File), compare);

    string listing;

    for (int i = 0; i < count; i++) {
        listing += entries[i].name;
        listing += "\r\n";

        free(entries[i].name);
    }
    return listing;
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

            char cwd[PATH_MAX];
            getcwd(cwd, sizeof(cwd));

            session.root_path = "";
            session.current_dir = "/";

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

        string listing = GenerateList(real_path);
        
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

        string real_path = session.getRealPath(arg);
        if (real_path.empty()) {
            SendResponse(client_fd, "550 Invalid file path.");
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
        off_t offset = session.restart_offset;
        if (offset > st.st_size) {
            SendResponse(client_fd, "550 offset exceeds file size.");
            close(file_fd);
            close(data_listen_fd);
            data_listen_fd = -1;
            session.restart_offset = 0;
            return;
        }
        ssize_t remaining = st.st_size - offset;
        SendResponse(client_fd, "150 Opening data connection.");

        int data_client_fd;

        while (true) {
            data_client_fd = accept(data_listen_fd, nullptr, nullptr);
            if (data_client_fd >= 0)
                break;
            if (errno == EINTR)
                continue;

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
        session.restart_offset = 0;
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

        if (arg.empty()) {
            SendResponse(client_fd, "501 No filename given.");
            return;
        }

        
        const string SERVER_UPLOAD_ROOT = "./ftp_storage";
        string filename;
        size_t pos2 = arg.find_last_of('/');

        if (pos2 == string::npos)
            filename = arg;
        else
            filename = arg.substr(pos2 + 1);

        string real_path = SERVER_UPLOAD_ROOT + "/" + filename;
        size_t pos = real_path.find_last_of('/');

        if (pos != string::npos) {
            string dir = real_path.substr(0, pos);
            MakeDirs(dir);
        }

        FILE* fp = nullptr;

        if (session.restart_offset > 0) {

            fp = fopen(real_path.c_str(), "r+b");

            if (fp != nullptr) {
                fseek(fp, session.restart_offset, SEEK_SET);
            } else {
                fp = fopen(real_path.c_str(), "wb");
            }

        } else {

            fp = fopen(real_path.c_str(), "wb");
        }

        if (fp == nullptr) {
            SendResponse(client_fd, "550 Failed to create file.");

            close(data_listen_fd);
            data_listen_fd = -1;

            session.restart_offset = 0;
            return;
        }

        SendResponse(client_fd, "150 Opening data connection.");

        int data_client_fd;

        while (true) {
            data_client_fd = accept(data_listen_fd, nullptr, nullptr);
            if (data_client_fd >= 0)
                break;
            if (errno == EINTR)
                continue;

            SendResponse(client_fd, "425 Can't open data connection.");
            fclose(fp);
            close(data_listen_fd);
            data_listen_fd = -1;
            session.restart_offset = 0;
            return;
        }

        char buf[8192];
        ssize_t n;

        while ((n = recv(data_client_fd, buf, sizeof(buf), 0)) > 0) {
            size_t written = fwrite(buf, 1, n, fp);
            if (written != (size_t)n) {
                SendResponse(client_fd, "451 Write file failed.");
                fclose(fp);
                close(data_client_fd);
                close(data_listen_fd);
                data_listen_fd = -1;
                session.restart_offset = 0;
                return;
            }
        }

        fclose(fp);
        close(data_client_fd);
        close(data_listen_fd);
        data_listen_fd = -1;
        session.restart_offset = 0;
        SendResponse(client_fd, "226 Transfer complete.");
    }
    else if (op == "REST") {
        if (!session.logged_in) {
            SendResponse(client_fd, "530 Please login with USER and PASS.");
            return;
        }
        if (arg.empty()) {
            SendResponse(client_fd, "501 No offset given.");
            return;
        }try {
            session.restart_offset = stoll(arg);
            SendResponse(client_fd, "350 Restarting at " + arg + ". Send STORE or RETRIEVE.");
        } catch (...) {
            SendResponse(client_fd, "501 Bad offset.");
        }
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
        if (stat(real_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
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

        string real_path = session.getRealPath(arg);

        if (real_path.empty()) {
            SendResponse(client_fd, "550 Permission denied.");
            return;
        }

        struct stat st;

        if (stat(real_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {

            session.current_dir = session.normalize(arg);
            SendResponse(client_fd, "250 Directory changed.");
        } else {
            SendResponse(client_fd, "550 Directory not found.");
        }
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

void HandleClientNonBlocking(int client_fd, int epoll_fd) {
    char buf[512];
    bool should_close = false;
    bool quit = false;

    ClientContext* ctx = nullptr;
    {
        lock_guard<mutex> lock(session_mtx);
        ctx = &g_session[client_fd];
    }

    while (true) {
        ssize_t count = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            should_close = true;
            break;
        } else if (count == 0) {
            should_close = true;
            break;
        }
        buf[count] = '\0';
        ctx->read_buf.append(buf, count);
    }

    size_t pos;
    while (!should_close && (pos = ctx->read_buf.find('\n')) != string::npos) {
        string cmd = ctx->read_buf.substr(0, pos);
        ctx->read_buf.erase(0, pos + 1);

        if (!cmd.empty() && cmd.back() == '\r') cmd.pop_back();

        {
            lock_guard<mutex> lock(cout_mtx);
            cout << "[Epoll Worker] Process Cmd: " << cmd << endl;
        }

        ProcessCommand(client_fd, cmd, quit, ctx->data_listen_fd, ctx->session);

        if (quit) {
            should_close = true;
            break;
        }
    }

    if (should_close) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
        if (ctx->data_listen_fd != -1) close(ctx->data_listen_fd);

        {
            lock_guard<mutex> lock(session_mtx);
            g_session.erase(client_fd);
        }

        {
            lock_guard<mutex> lock(cout_mtx);
            cout << "Client disconnected, contexts cleard." << endl;
        }
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.fd = client_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
    }
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
    
    SetNonBlocking(server_fd);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        cerr << "epoll_creat1 failed" << endl;
        close(server_fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        cerr << "epoll_ctl listen fd failed" << endl;
        close(server_fd);
        close(epoll_fd);
        return;
    }

    ThreadPool pool (4);
    struct epoll_event events[1024];

    cout << "FTP Server started on port " << port << " with epoll..." << endl;

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, 1024, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            cerr << "epoll_wait failed" << endl;
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                string client_ip;
                int client_port;

                while (true) {
                    int client_fd = AcceptClient(server_fd, client_ip, client_port);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    SetNonBlocking(client_fd);
                    SendGreeting(client_fd);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLONESHOT;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else if (events[i].events & EPOLLIN) {
                pool.Push([fd, epoll_fd]() {
                    HandleClientNonBlocking(fd, epoll_fd);
                });
            }
        }
    }
    close(server_fd);
    close(epoll_fd);
}

int main() {
    const int port = 2100;
    RunServer(port);
    return 0;
}