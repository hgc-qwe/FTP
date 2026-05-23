#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <sys/epoll.h>
#include <cstring>
using namespace std;

int main() {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        cerr << "socket failed" << endl;
        return 1;
    }

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(2100);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(lfd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == -1) {
        cerr << "bind failed" << endl;
        return 1;
    }

    if (listen(lfd, 128) == -1) {
        cerr << "listen failed" << endl;
        return 1;
    }

    int epfd = epoll_create(1);
    if (epfd == -1) {
        cerr << "epoll_create failed" << endl;
        return 1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) == -1) {
        cerr << "epoll_ctl failed" << endl;
        return 1;
    }

    vector<epoll_event> events(64);
    char buf[BUFSIZ];
    while (true) {
        int count = epoll_wait(epfd, events.data(), 64, -1);
        if (count == -1) {
            cerr << "epoll_wait failed" << endl;
            break;
        }

        for (int i = 0; i < count; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                int cfd = accept(lfd, nullptr, nullptr);
                ev.events = EPOLLIN;
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

                cout << "新客户端连接：" << cfd << endl;
            } else {
                if (events[i].events & EPOLLIN) {
                    int ret = read(fd, buf, sizeof(buf));
                    if (ret > 0) {
                        write(1, buf, ret);
                        int i;
                        for (i = 0; i < ret; i++) {
                            buf[i] = toupper(buf[i]);
                        }
                        write(fd, buf, ret);
                    } else if (ret == 0) {
                        close(fd);
                    }
                }
                if (events[i].events & EPOLLOUT) {

                }
                if (events[i].events & EPOLLERR) {
                    close(fd);
                }
            }
        }
    }
    close (epfd);
    close(lfd);
    return 0;
}