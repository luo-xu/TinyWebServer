#ifndef LST_TIMER_H
#define LST_TIMER_H



#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/epoll.h>

class util_timer;

// 连接资源 (用户数据结构体)
struct client_data {
    struct sockaddr_in address;   // 客户端socket地址
    int sockfd;                   // socket文件描述符
    util_timer* timer;            // 定时器指针
};

// 定时器类
class util_timer {
public:
    util_timer() :prev(NULL), next(NULL) {}
public:
    time_t expire;    // 超时时间

    void(*cb_func)(client_data*);   // 回调函数指针
    client_data* user_data;         // 用户数据结构体 
    util_timer* prev;               // 前向定时器
    util_timer* next;               // 后继定时器
};

// 定时器容器类
class sort_timer_lst {
public:
    sort_timer_lst();
    ~sort_timer_lst();   // 销毁链表，释放资源

    void add_timer(util_timer* timer);      // 添加定时器
    void adjust_timer(util_timer* timer);   // 调整定时器
    void del_timer(util_timer* timer);      // 删除定时器
    void tick();                            // 定时器任务处理函数

//private:
    void add_timer(util_timer* timer, util_timer* lst_head);  // 给其他函数复用 (将结点timer插入head之后的某个位置,用于调整链表内部结点)

    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点
};



# endif // !LST_TIMER_H