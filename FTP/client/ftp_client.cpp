#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
#include <cstdio>
#include <sys/stat.h>
using namespace std;

bool logged_in = false;

void PrintMenu() {
    cout << "\n===== FTP Client =====" << endl;
    cout << "1. login" << endl;
    cout << "2. ls" << endl;
    cout << "3. cd" << endl;
    cout << "4. pwd" << endl;
    cout << "5. download" << endl;
    cout << "6. upload" << endl;
    cout << "7. quit" << endl;
    cout << "choice: ";
}

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

int ConnectServer() {
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd == -1) {
        cerr << "socket error" << endl;
        exit(1);
    }

    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(2100);
    inet_pton(AF_INET, "127.0.0.1", &client_addr.sin_addr.s_addr);
    socklen_t client_len = sizeof(client_addr);

    if (connect(cfd, (struct sockaddr*)&client_addr, client_len) == -1) {
        cerr << "connect error" << endl;
        exit(1);
    }

    return cfd;
}

string ReadResponse(int fd) {
    string line;
    char ch;

    while (true) {
        int n = recv(fd, &ch, 1, 0);

        if (n <= 0) break;

        line += ch;

        if (line.size() >= 2 &&
            line.substr(line.size() - 2) == "\r\n") {
            break;
        }
    }

    return line;
}

void SendCommand(int fd, const string& cmd) {
    string msg = cmd + "\r\n";

    int total = 0;

    while (total < msg.size()) {

        int sent = send(fd,msg.c_str() + total,msg.size() - total,0);

        if (sent <= 0) {
            perror("send");
            return;
        }

        total += sent;
    }
}

void Login(int fd) {
    string user;
    string pass;

    cout << "username: ";
    cin >> user;

    SendCommand(fd, "USER " + user);

    cout << ReadResponse(fd);

    cout << "password: ";
    cin >> pass;

    SendCommand(fd, "PASS " + pass);

    string resp = ReadResponse(fd);
    cout << resp;
    if (resp.find("230") != string::npos) logged_in = true;
} 

void PWD(int fd) {
    SendCommand(fd, "PWD");
    cout << ReadResponse(fd);
}

void Quit(int fd) {
    SendCommand(fd, "QUIT");
    cout << ReadResponse(fd);
    logged_in = false;
    close(fd);
}


int PASV(int fd) {
    SendCommand(fd, "PASV");
    string response = ReadResponse(fd);
    int a, b, c, d, p1, p2;
    if (sscanf(response.c_str(), "%*[^(](%d,%d,%d,%d,%d,%d)", &a, &b, &c, &d, &p1, &p2) != 6) {
        cerr << "PASV failed: " << response << endl;
        return -1;
    }
    int port = p1 * 256 + p2;
    int data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd == -1) {
        cerr << "socket error" << endl;
        return -1;
    }

    struct sockaddr_in data_addr;
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &data_addr.sin_addr.s_addr);
    socklen_t data_len = sizeof(data_addr);

    if (connect(data_fd, (struct sockaddr*)&data_addr, data_len) == -1) {
        cerr << "connect error" << endl;
        close(data_fd);
        return -1;
    }
    return data_fd;
}

void List(int fd) {
    if (!logged_in) {
    cout << "please login first" << endl;
    return;
    }

    string path;
    cout << "path(. for current): ";
    cin >> path;

    int data_fd = PASV(fd);
    if (data_fd == -1) return;

    SendCommand(fd, "LIST " + path);

    string resp = ReadResponse(fd);
    cout << resp;

    if (resp.find("150") == string::npos) {
        close(data_fd);
        return;
    }

    char buf[4096];
    int n;
    while ((n = recv(data_fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        cout << buf;
    }
    close(data_fd);
    cout << ReadResponse(fd);
}

void Download(int fd) {
    if (!logged_in) {
        cout << "please login first" << endl;
        return;
    }

    string remote_file;

    cout << "remote file path(example: /tmp/test/a.txt): ";
    cin >> remote_file;

    string save_dir;

    cout << "save dir(example: /home/hgc/downloads): ";
    cin >> save_dir;

    string filename;

    size_t pos = remote_file.find_last_of('/');

    if (pos == string::npos)
        filename = remote_file;
    else
        filename = remote_file.substr(pos + 1);

    string local_file = save_dir + "/" + filename;

    int data_fd = PASV(fd);
    if (data_fd == -1) return;

    SendCommand(fd, "RETR " + remote_file);

    string resp = ReadResponse(fd);
    cout << resp;

    if (resp.find("150") == string::npos) {
        close(data_fd);
        return;
    }

    size_t pos2 = local_file.find_last_of('/');

    if (pos2 != string::npos) {
        string dir = local_file.substr(0, pos2);

        MakeDirs(dir);
    }

    FILE* fp = fopen(local_file.c_str(), "wb");

    if (!fp) {
        perror("fopen");
        close(data_fd);
        return;
    }

    char buf[4096];
    int n;

    while ((n = recv(data_fd, buf, sizeof(buf), 0)) > 0) {
        size_t written = fwrite(buf, 1, n, fp);

        if (written != (size_t)n) {
            perror("fwrite");
            break;
        }
    }

    fclose(fp);
    shutdown(data_fd, SHUT_RDWR);
    close(data_fd);

    cout << ReadResponse(fd);

    cout << "saved to: " << local_file << endl;
}

void Upload(int fd) {
    if (!logged_in) {
    cout << "please login first" << endl;
    return;
    }

    string local_file;

    cout << "local file(example: /home/xxx/upload/a.txt): ";
    cin >> local_file;

    string filename;
    size_t pos = local_file.find_last_of('/');

    if (pos == string::npos)
        filename = local_file;
    else
        filename = local_file.substr(pos + 1);

    string remote_file = filename;
    FILE* fp = fopen(local_file.c_str(), "rb");
    if (!fp) {
        perror("fopen");
        return;
    }

    int data_fd = PASV(fd);
    if (data_fd == -1) {
        fclose(fp);
        return;
    }

    SendCommand(fd, "STOR " + remote_file);
    string resp = ReadResponse(fd);
    cout << resp;
    if (resp.find("150") == string::npos) {
        fclose(fp);
        close(data_fd);
        return;
    }
    
    char buf[4096];
    int n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {

        int total = 0;

        while (total < n) {

            int sent = send(data_fd,buf + total,n - total,0);

            if (sent <= 0) {
                perror("send");
                fclose(fp);
                shutdown(data_fd, SHUT_RDWR);
                close(data_fd);
                return;
            }

            total += sent;
        }
    }
    if (ferror(fp)) {
        perror("fread");
    }

    fclose(fp);
    shutdown(data_fd, SHUT_WR);
    close(data_fd);
    cout << ReadResponse(fd);
}

void ChangeDir(int fd) {
    if (!logged_in) {
    cout << "please login first" << endl;
    return;
    }

    string path;
    cout << "path: ";
    cin >> path;

    SendCommand(fd, "CWD " + path);
    cout << ReadResponse(fd);
}

int main() {
    int cfd = ConnectServer();
    cout << ReadResponse(cfd);

    while (true) {
        PrintMenu();

        int choice;
        cin >> choice;

        switch(choice) {
            case 1:
                Login(cfd);
                break;
            case 2:
                List(cfd);
                break;
            case 3:
                ChangeDir(cfd);
                break;
            case 4:
                PWD(cfd);
                break;
            case 5:
                Download(cfd);
                break;
            case 6:
                Upload(cfd);
                break;
            case 7:
                Quit(cfd);
                return 0;
        }
    }
}