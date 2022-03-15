#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;

// 连接资源 (用户数据结构体)
struct client_data
{
    sockaddr_in address;   // 客户端socket地址
    int sockfd;            // socket文件描述符
    util_timer *timer;     // 定时器
}; 

// 定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;   // 超时时间
    
    void (* cb_func)(client_data *);  // 回调函数指针
    client_data *user_data;           // 用户数据结构体
    util_timer *prev;                 // 前向定时器
    util_timer *next;                 // 后继定时器
};

// 定时器容器类
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();   // 常规销毁链表

    void add_timer(util_timer *timer);      // 添加定时器 (内部调用私有成员add_timer)
    void adjust_timer(util_timer *timer);   // 调整定时器 (任务发生变化时，调整定时器在链表中的位置)
    void del_timer(util_timer *timer);      // 删除定时器
    void tick();                            // 定时任务处理函数

private:
    void add_timer(util_timer *timer, util_timer *lst_head);  // 用于调整链表内部结点 (私有成员，被公有成员add_timer和adjust_time调用)

    util_timer *head;   // 头结点
    util_timer *tail;   // 尾结点
};


// 工具类
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);  // 打印错误

public:
    static int *u_pipefd;         // 与主线程通信的管道 (主要用于信号处理函数内，通过该管道将信号发送给主线程，主线程对接收到的信号进行处理)
    static int u_epollfd;         // 内核事件表
    sort_timer_lst m_timer_lst;   // 定时器容器
    int m_TIMESLOT;               // 超时时间
};

// 定时器回调函数
void cb_func(client_data *user_data);

#endif
