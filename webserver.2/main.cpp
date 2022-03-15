#include "threadpool.h"
#include "http_conn.h"
#include "lit_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define MAX_FD 65536  // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大事件数
#define TIMESLOT 5   // 最小超时单位    

extern void addfd(int epollfd, int fd, bool one_shot);
extern int setnonblocking(int fd);
extern void removefd(int epollfd, int fd);

static int pipefd[2];
static int epollfd;

// 定时器回调函数 (关闭非活动连接)
void cb_func(client_data* user_data) {
    if (!user_data) {
        printf("[cb_func]: user_data illegal\n");
        exit(-1);
    }

    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);     // 关闭客户socket
    user_data->sockfd = -1;
    http_conn::m_user_count--;    // 更新连接数
    printf("[cb_func]: close fd %d\n", user_data->sockfd);
}
// 信号处理函数
void sig_handler(int sig) {
    int save_errno = errno;   // 为保证函数的可重入性，保留原来的errno (可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据)
    int msg = sig;
    int ret = send(pipefd[1], (char*)&msg, 1, 0);  // 信号值从u_pipefd[1]写入，主线程从u_pipefd[0]读取 (传输字符类型，而非整型)
    if (ret <= 0) {
        perror("send");
    }
    errno = save_errno;       // 将原来的errno赋值为当前的errno
}
// 设置注册信号函数
void addsig(int sig, void (handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // sa.sa_flags |= SA_RESTART;
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



    // 注册信号SIGGPIPE、SIGALRM、SIGTERM
    addsig(SIGPIPE, SIG_IGN);       // 屏蔽SIGPIPE信号 (在linux下写socket的程序的时候，如果尝试send到一个disconnected socket上，就会让底层抛出一个SIGPIPE信号。这个信号的缺省处理方法是退出进程)
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);

    // 创建线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    }
    catch (...) {
        exit(1);
    }

    // 创建定时器容器
    sort_timer_lst timer_lst;

    // 预先为每个可能的客户连接 分配一个http_conn对象
    http_conn* users = new http_conn[MAX_FD];

    // 预先为每个可能的客户连接 分配一个关于定时器的连接资源
    client_data* users_timer = new client_data[MAX_FD];



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
    epollfd = epoll_create(5);

    // 注册监听套接字 
    addfd(epollfd, listenfd, false);  // 注：监听套接字无需设置EPOLLONESHOT，因其只在主线程工作
    // 统一内核事件表
    http_conn::m_epollfd = epollfd;

    // 建立一对匿名的已经连接的套接字
    // int pipefd[2];
    ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd);
    if (ret == -1) {
        perror("socket");
        exit(-1);
    }
    setnonblocking(pipefd[1]);         // 设置管道写端为非阻塞 (信号处理函数写)
    addfd(epollfd, pipefd[0], false);  // 设置管道读端为ET非阻塞，并添加到epoll内核事件表 (主线程端读)

    // alarm(): 设置信号传送闹钟，即用来设置信号SIGALRM 在经过参数seconds秒数后发送给目前的进程
    alarm(TIMESLOT);



    bool timeout = false;
    bool stop_server = false;
    // 循环监听并处理事件
    while (true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {  // epoll_wait被信号打断阻塞的情况(number=-1 && errno==EINTR)忽略
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

                // 初始化http新连接
                users[connfd].init(connfd, clientAddr);
                // 分配定时器 (初始化client_data数据，创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中)
                users_timer[connfd].address = clientAddr;
                users_timer[connfd].sockfd = sockfd;
                util_timer* timer = new util_timer;       // 创建定时器timer
                timer->user_data = &users_timer[connfd];  // 初始化定时器timer的客户信息
                timer->cb_func = cb_func;                 // 设置定时器回调函数
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;       // 定时时间为 当前时间+三个超时单位
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);               // 插入该定时器timer
            }
            // 关闭对应连接 (客户关闭TCP连接、挂起、错误)
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                printf("[main]: 客户端断开连接!!\n\n");
                users[sockfd].close_conn();
            }
            // 处理信号
            else if (sockfd == pipefd[0] && (events[i].events && EPOLLIN)) {
                char signals[1024] = { '\0' };
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1 || ret == 0) {
                    perror("recv");
                    exit(-1);
                }
                else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                        case SIGALRM:   // 由alarm设置的实时闹钟超时引起
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:   // 终止进程
                        {
                            stop_server = true;
                            break;
                        }
                        }
                    }
                }
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                // 重置用户定时时间
                util_timer* timer = users_timer[sockfd].timer;
                if (timer) {
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    timer_lst.adjust_timer(timer);
                    printf("[main]: remake timer once\n");
                }

                printf("[main]: 有数据可读\n\n");
                if (users[sockfd].read()) {
                    pool->append(users + sockfd);  // 成功读取后，工作队列
                }
            }
            // 处理响应给客户的数据
            else if (events[i].events & EPOLLOUT) {
                // 重置用户定时时间
                util_timer* timer = users_timer[sockfd].timer;
                if (timer) {
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    timer_lst.adjust_timer(timer);
                    printf("[main]: remake timer once\n");
                }

                printf("[main]: 有数据可写\n\n");
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();  // 发送失败，则关闭客户端连接
                }
            }
        }

        // 定时处理任务 (执行定时处理函数，并再次触发SIGALRM信号，重新定时)
        if (timeout) {
            printf("[main]: 有信号发生!\n");
            timer_lst.tick();

            alarm(TIMESLOT);
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users_timer;
    delete[] users;
    delete pool;
    return 0;
}



