#include "lst_timer.h"
#include "../http/http_conn.h"

// 构造函数
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

// 析构函数 (常规销毁链表)
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 添加定时器 (内部调用私有成员add_timer)
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 如果新的定时器超时时间小于当前头部结点,直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)  
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 否则调用私有成员，调整内部结点
    add_timer(timer, head);
}

// 调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    util_timer *tmp = timer->next;
    // 被调整的定时器是链表尾结点，或定时器超时值仍然小于下一个定时器超时值，则不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    // 被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    // 被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

// 删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    // 链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    // 被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    // 被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    // 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 定时任务处理函数
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);   // 获取当前时间
    util_timer *tmp = head;
    // 遍历定时器链表
    while (tmp)
    {   
        // 链表容器为升序排列，若当前时间小于定时器的超时时间，则后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }

        tmp->cb_func(tmp->user_data);  // 当前定时器到期，则调用回调函数，执行定时事件
        head = tmp->next;              // 将处理后的定时器从链表容器中删除，并重置头结点
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

// 用于调整链表内部结点 (私有成员，被公有成员add_timer和adjust_time调用)
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;

    // 遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 遍历完发现，目标定时器需要放到尾结点处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

// 初始化
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件 (ET模式，选择开启EPOLLONESHOT)
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;  // 针对connfd，开启EPOLLONESHOT，因为我们希望每个socket在任意时刻都只被一个线程处理
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig)
{
    int save_errno = errno;   // 为保证函数的可重入性，保留原来的errno (可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据)
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);   // 工作线程将信号值从u_pipefd[1]写入，主线程从u_pipefd[0]读取 (传输字符类型，而非整型)
    errno = save_errno;       // 将原来的errno赋值为当前的errno
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;              // 创建sigaction结构体变量
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;          // 设置信号处理函数 (信号处理函数中仅仅发送信号值，不做对应逻辑处理)  
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);          // 将所有信号添加到信号集中    
    assert(sigaction(sig, &sa, NULL) != -1);   // 执行sigaction函数
}

// 定时处理任务 (执行定时处理函数，并再次触发SIGALRM信号，重新定时)
void Utils::timer_handler()
{
    m_timer_lst.tick();   // 到时，执行定时处理函数
    alarm(m_TIMESLOT);    // 再次触发SIGALRM信号，重新定时
}

// 打印错误
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;


class Utils;
// 定时器回调函数 (关闭非活动连接)
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);   // 删除非活动连接在socket上的注册事件
    assert(user_data);
    close(user_data->sockfd);    // 关闭文件描述符
    http_conn::m_user_count--;   // 减少连接数
}
