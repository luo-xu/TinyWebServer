#include "threadpool.h"
#include "http_conn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define MAX_FD 65536  // 最大文件描述符个数
#define MAX_EVENT_NUMBER  10000  // 监听的最大事件数

extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);


void addsig(int sig, void (handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //sa.sa_flags = 0;
    //sa.sa_restorer = NULL;
    sigfillset(&sa.sa_mask);  // sigemptyset(&sa.sa_mask);        
    sa.sa_handler = handler;

    if (sigaction(sig, &sa, NULL) == -1) { // 如果表达式其值为假（即为0），那么它先向stderr打印一条出错信息，然后通过调用 abort 来终止程序运行。
        perror("sigaction");
        exit(-1);
    }
}


int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("usage: %s port_number\n", basename(argv[0]));
        exit(1);
    }

    // 获取端口号
    int port = atoi(argv[1]);
    // 屏蔽SIGPIPE信号 (在linux下写socket的程序的时候，如果尝试send到一个disconnected socket上，就会让底层抛出一个SIGPIPE信号。这个信号的缺省处理方法是退出进程)
    addsig(SIGPIPE, SIG_IGN);
    // 创建线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    }
    catch (...) {
        exit(1);
    }
    // 创建客户数据数组
    http_conn* users = new http_conn[MAX_FD];

    // 创建监听套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        exit(-1);
    }

    // 端口复用 (在bind之前)
    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // 绑定服务器地址
    struct sockaddr_in address;
    memset(&address, '\0', sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    if (ret == -1) {
        perror("bind");
        exit(-1);
    }

    // 监听
    ret = listen(listenfd, 5);
    if (ret == -1) {
        perror("listen");
        exit(-1);
    }

    // 创建事件数组，epoll实例对象
    struct epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    // 注册监听套接字 
    addfd(epollfd, listenfd, false);  // 注：监听套接字无需设置EPOLLONESHOT，只在主线程工作
    http_conn::m_epollfd = epollfd;

    // 
    while (true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            perror("epoll");
            break;
        }

        for (int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == listenfd) {
                struct sockaddr_in clientAddr;
                socklen_t clientAddrLen = sizeof(clientAddr);
                int connfd = accept(listenfd, (struct sockaddr*)&clientAddr, &clientAddrLen);
                if (connfd == -1) {
                    perror("accept");
                    exit(-1);
                }

                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }

                users[connfd].init(connfd, clientAddr); // 初始化http新连接
            }
            // 关闭对应连接 (客户关闭TCP连接、挂起、错误)
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                printf("[main]: 客户端断开连接!!\n\n");
                users[sockfd].close_conn();
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                printf("[main]: 有数据可读\n\n");
                if (users[sockfd].read()) {
                    pool->append(users + sockfd);  // 成功读取后，工作队列
                }
            }
            // 处理响应给客户的数据
            else if (events[i].events & EPOLLOUT) {
                printf("[main]: 有数据可写\n\n");
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();  // 发送失败，则关闭客户端连接
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}