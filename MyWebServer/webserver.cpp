#include "webserver.h"

// 构造函数
WebServer::WebServer()
{
    // 预先为每个可能的客户连接分配一个http_conn对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);     //getcwd(): 将当前工作目录的绝对路径复制到参数server_path所指的内存空间中
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 预先为每个可能的客户连接分配一个关于定时器的连接资源
    users_timer = new client_data[MAX_FD];
}

// 析构函数
WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}


void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

// 设置listenfd和connfd的模式组合 (ET或LT)
void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 创建并初始化log对象，同时设置log日志写入方式
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志
        if (1 == m_log_write)   // 异步写入
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);   // 日志文件名、是否关闭日志、日志缓冲区大小、日志最大行数、阻塞队列长度
        else                    // 同步写入
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

// 创建并初始化数据库连接池，以及初始化数据库读取表
void WebServer::sql_pool()
{
    // 初始化并初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);  // 主机地址、登陆数据库用户名、登陆数据库密码、使用数据库名、数据库端口号、数据库连接数、是否关闭日志

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

 // 创建并初始化线程池
void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 设置监听套接字 (设置监听套接字，是否优雅关闭连接，设置定时器超时时间，创建内核时间表、管道、信号注册)
void WebServer::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);   // 创建套接字
    assert(m_listenfd >= 0);

    // 优雅关闭连接 (当调用closesocket关闭套接字时，SO_LINGER将决定系统如何处理残存在套接字发送队列中的数据。处理方式无非两种：丢弃或者将数据继续发送至对端，优雅关闭连接。)
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};   
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);   // 地址为本机中所有的ip
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));   // 让端口释放后立即就可以被再次使用
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));    // 将m_listenfd与地址绑定
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);    // 指定m_listenfd为监听套接字，并创建监听队列(长度为5)
    assert(ret >= 0);

    utils.init(TIMESLOT);   // 设置超时时间

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);   // 向内核事件表注册监听套接字
    http_conn::m_epollfd = m_epollfd;                              // 统一内核事件表

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);   // 创建双向管道
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);                     // 设置管道写端为非阻塞 (工作线程端写)
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);         // 设置管道读端为ET非阻塞，并添加到epoll内核事件表 (主线程端读)

    // 注册信号SIGGPIPE、SIGALRM、SIGTERM
    utils.addsig(SIGPIPE, SIG_IGN);               // 屏蔽SIGPIPE信号 (在linux下写socket的程序的时候，如果尝试send到一个disconnected socket上，就会让底层抛出一个SIGPIPE信号。这个信号的缺省处理方法是退出进程)
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);    // alarm(): 设置信号传送闹钟，即用来设置信号SIGALRM在经过参数seconds秒数后发送给目前的进程

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 创建设置并插入定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
     // 初始化http新连接
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);  

    // 初始化client_data数据 (创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中)
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;       // 创建定时器timer
    timer->user_data = &users_timer[connfd];  // 初始化定时器timer
    timer->cb_func = cb_func;                 // 设置定时器回调函数
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;       // 定时器初始为当前时间+三个超时单位
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);       // 插入该定时器timer
}

// 调整定时器 (若有数据传输，则将定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整)
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");   // log日志打印
}

// 回收资源 (删除对应的定时器、注册事件，关闭对应文件描述符）
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);     // 调用回调函数 (删除对于注册事件，关闭对应文件描述符，减少连接数)
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);   // 删除定时器
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 接收客户连接 (ET,LT)
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    // 监听套接字为LT模式
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);  // log日志打印"连接错误"
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");             // log日志打印"服务器忙碌"
            return false;
        }
        timer(connfd, client_address);   // 接收到新连接，则为之创建对应的定时器
    }
    // 监听套接字为ET模式
    else
    {
        while (1)  // 循环调用accept，直到监听队列为空
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 信号处理 (SIGALRM,SIGTERM)
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:  // 由alarm设置的实时闹钟超时引起
            {
                timeout = true;
                break;
            }
            case SIGTERM:  // 终止进程
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

// 接收客户数据
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor (由工作线程处理可读或可写事件，主线程只负责监听是否有事件发生)
    if (1 == m_actormodel)
    {
        if (timer)   
        {
            adjust_timer(timer);   // 调整对应的定时器
        }

        m_pool->append(users + sockfd, 0);   // 主线程若监测到读事件，将该事件放入请求队列 (读为0)

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    // proactor (工作线程仅负责处理逻辑，I/O操作都交给主线程和内核来处理进行)
    else
    {
        if (users[sockfd].read_once())  // 主线程循环读取客户数据，直到无数据可读或对方关闭连接 
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));   // log日志打印

            m_pool->append_p(users + sockfd);   // 若监测到读事件，将该事件放入请求队列

            if (timer)
            {
                adjust_timer(timer);    // 调整对应的定时器
            }
        }
        else   // 数据读取失败，则关闭连接，并回收资源
        {
            deal_timer(timer, sockfd);
        }
    }
}

// 响应客户数据
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);   // 调整对应的定时器
        }

        m_pool->append(users + sockfd, 1);    // 主线程若监测到写事件，将该事件放入请求队列 (写为1)

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    // proactor
    else
    {
        if (users[sockfd].write())     // 主线程写入响应报文
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));   // log打印日志

            if (timer)
            {
                adjust_timer(timer);   // 调整对应的定时器
            }
        }
        else   // 数据写入失败，则关闭连接，并回收资源
        {
            deal_timer(timer, sockfd);
        }
    }
}

// 循环监听并处理事件
void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            // 关闭对应连接 (客户关闭TCP连接、挂起、错误)
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);    // 服务器端关闭连接，移除对应的定时器
            }
            // 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");    // log日志打印"信号处理错误"
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            // 处理响应给客户的数据
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }

        if (timeout)
        {
            utils.timer_handler();   // 时钟滴答一次，则执行一次定时处理任务，并重新计时

            LOG_INFO("%s", "timer tick");    // log日志打印一次“时钟滴答”

            timeout = false;
        }
    }
}