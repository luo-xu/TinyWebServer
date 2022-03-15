#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;    // 数据库读取表 (存储用户名和密码)

// 主线程初始化数据库读取表 (将数据库中的用户名和密码载入到服务器的map中)
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);    

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))    // mysql_query()执行一条mysql查询语句
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));        // mysql_error()返回上一个MySQL操作产生的文本错误信息
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);        // 获取文件描述符旧的状态标志
    int new_option = old_option | O_NONBLOCK;    
    fcntl(fd, F_SETFL, new_option);             // 设置非阻塞标志
    return old_option;                          // 返回文件描述符旧的状态标志，以便日后恢复该状态标志
}

// 向内核事件表注册connfd读事件 (ET模式，则选择开启EPOLLONESHOT)
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)   // ET
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else                 // LT
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;   // 一个socket连接在任一时刻都只被一个线程处理 (对于注册了EPOLLONESHOT事件的文件描述符，操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次)

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核事件表中删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 内核事件表注册新事件，开启EPOLLONESHOT (注册了EPOLLONESHOT事件的socket一旦被某个线程处理完毕，该线程应该立即重置这个socket上的EPOLLONESHOT事件，以确保这个socket下一次可读时，其EPOLLIN事件能被触发，进而让其他线程有机会继续处理这个socket)
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)   // ET
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else                 // LT
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接 (关闭一个连接，客户总量减一)
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);    // 从内核事件表中删除客户端socket描述符
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化新连接 (外部调用初始化套接字地址)
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新连接 (check_state默认为分析请求行状态)
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}


// 从状态机: 用于分析出一行内容 (返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN)
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];     // temp为将要分析的字符

        // 如果当前字节是'\r'字符，则有可能会读取到完整行
        if (temp == '\r')
        {   
            if ((m_checked_idx + 1) == m_read_idx)            // 下一个字符达到了buffer结尾('/r'为当前buffer中的最后一个字节)，则说明没有读取到一个完整的行，需要继续接收
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')   // 下一个字符是\n，说明读取到一个完整的行。则将\r\n改为\0\0，且m_checked_idx指向下一行的开头
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;                                  
        }
        // 如果当前字节是'\n'字符，则有可能会读取到完整行 (一般是上次读取到\r就到了buffer末尾，没有接收完整，再次接收时会出现这种情况)
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')    // 如果前一个字符是'\r'，则将\r\n改成\0\0，且m_checked_idx指向下一行的开头
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;    // 如果分析完所有内容也没遇到'\r'、'\n'，则说明没有读取到一个完整的行，需要继续接收
}


// 循环读取客户数据，直到无数据可读或对方关闭连接 (非阻塞ET工作模式下，需要一次性将数据读完)
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)     // 正常情况下，0 <= m_read_idx < READ_BUFFER_SIZE
    {
        return false;
    }
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)     
        {
            return false;
        }

        return true;
    }
    // ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);  // 循环读取
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)    // EAGIN、EWOULDBLOCK 表示当前没有数据可读，不需要重新读 (读完了)
                    break;
                return false;
            }
            else if (bytes_read == 0)  // recv返回0，意味着客户端已关闭连接
            {
                return false;
            }
            m_read_idx += bytes_read;  // 循环累加
        }
        return true;
    }
}


// 主状态机解析HTTP请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过'\t'或' '分隔。
    m_url = strpbrk(text, " \t");    // strpbrk(): 返回两个字符串中首个相同字符的位置
    if (!m_url)                      // 如果请求行中没有' '或'\t'，则说明格式有误
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';                 // 将该位置改为\0，用于将前面数据取出
    
    // 取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    if (strcasecmp(method, "GET") == 0)    // strcasecmp(): 忽略大小写比较字符串   
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;    // 启用POST
    }
    else
        return BAD_REQUEST;

    // m_url此时跳过了第一个' '或'\t'，但之后可能还有。因此将m_url向后偏移，通过查找，继续跳过' '和'\t'字符，从而指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");     // strspn(): 计算字符串m_url中连续有几个字符都属于字符串" \t"，其返回值是字符串m_url开头连续包含字符串" \t"内的字符数目

    // 使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)    // 仅支持HTTP/1.1
        return BAD_REQUEST;

    if (strncasecmp(m_url, "http://", 7) == 0)     // 对请求资源前7个字符进行判断 (因为有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');                // strchr(): 在字符串str中寻找字符'/'第一次出现的位置，并返回其位置(地址指针)，若失败则返回NULL
    }
    if (strncasecmp(m_url, "https://", 8) == 0)    // 对请求资源前8个字符进行判断 (因为有些报文的请求资源中会带有https://，这里需要对这种情况进行单独处理)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般的不会带有上述两种前缀，而是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')      // 如果上述两种前缀后的一个字节不是'\'，这说明格式有误
        return BAD_REQUEST;

    // 当url为/时，显示欢迎界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");    // strcat: 将字符串"judge.html"连接到字符串m_url后，然后得到一个组合后的字符串m_url (即客户请求的目标文件的文件名"/judge.html")

    // 请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


// 主状态机解析HTTP请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前text首位是不是'\0'来判断处理对象(从状态机解析一行的时候，将'\r'和'\n'都换成了'\0'a)。若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
    if (text[0] == '\0')
    {
        // 判断是GET还是POST请求
        if (m_content_length != 0)    
        {
            m_check_state = CHECK_STATE_CONTENT;   // POST请求，则需要跳转到消息体处理状态
            return NO_REQUEST;
        }
        return GET_REQUEST;                        // GET请求，则解析结束 (接收到一个完整的请求)    
    }
    // 解析请求头的连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;    // 如果是长连接，则将linger标志设置为true
        }
    }
    // 解析请求头的内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);    // atol(): 用于将给定的字符串值转换为整数值。它接受包含"整数"的字符串，并返回其长整数值。 
    }
    // 解析请求头的HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)   
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    // 其他字段直接跳过(该项目只检查以上几个字段)
    else
    {
        LOG_INFO("oop!unknow header: %s", text);    // log打印未解析的字段
    }

    return NO_REQUEST;    // 继续读取头部字段，直到遇到空行，则说明头部字段解析完毕
}


// 主状态机解析HTTP请求体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;    // POST请求体中的数据为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


// 主状态机: 从m_read_buf读缓冲区中读取数据，并处理请求报文
http_conn::HTTP_CODE http_conn::process_read()
{
    // 初始化从状态机状态(LINE_OK)、HTTP请求解析结果(NO_REQUEST)
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    
    // 在POST请求报文中，消息体的末尾没有任何字符('\r','\n')，不能仅靠从状态机的状态来判断解析结果(GET请求报文可以)，所以当解析请求体时不使用从状态机状态
    // (m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)表示开始解析请求体且上一行(空行)解析成功，||后面表达式就不会执行; 相反没有解析到请求体部分时，就仅执行||后面的从状态机。妙！
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();                // get_line用于将指针向后偏移，指向读缓冲中未处理的字符 (text指针指向读缓冲中未处理的字符，"从状态机解析出的那一行")
        m_start_line = m_checked_idx;     // (从状态机在主状态机解析内容之前，已经先解析出一行，在这个过程中m_chaecked_idx已经被移动到 该行最后一个字符的下一个位置，也就是下下行的开始!)
        LOG_INFO("%s", text);             // log打印请求内容

        // 主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);   // 解析请求行
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);        // 解析请求头
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 完整解析GET请求后，跳转到报文响应函数 (GET请求没有请求体)
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: 
        {
            ret = parse_content(text);        // 解析请求体
            // 完整解析POST请求后，跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;    // 客户请求不完整 (需要继续读取客户数据)
}


// 生成响应报文
http_conn::HTTP_CODE http_conn::do_request()
{
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

     // 找到请求文件地址m_url中最后一个'/'的位置
    const char *p = strrchr(m_url, '/');    // strrchr(): 返回字符串m_url中最后一次出现字符'/'的位置。

    // 处理cgi，实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        char flag = m_url[1];    // 根据标志判断是登录检测还是注册检测

        // 将网站根目录和url文件拼接
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);   // 把m_url_real所指由NULL结束的字符串的前(FILENAME_LEN-len-1)个字节复制到(m_real_file+len)所指的数组中
        free(m_url_real);

        // 将用户名和密码提取出来 (user=123&passwd=123)
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // /3CGISQL.cgi: POST请求，进行注册校验。注册成功跳转到log.html，即登录页面; 注册失败跳转到registerError.html，即注册失败页面
        if (*(p + 1) == '3')     
        {
            // 如果是注册，先检测数据库中是否有重名的，如果没有重名的，则增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);     
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 查询哈希表users，查看该用户名是否注册过
            if (users.find(name) == users.end())    
            {
                m_lock.lock();      // 加锁
                int res = mysql_query(mysql, sql_insert);             // mysql_query(): 执行一条MySQL查询 (将用户名和密码插入到数据库中)
                users.insert(pair<string, string>(name, password));   // 同时也插入到哈希表users中
                m_lock.unlock();    // 解锁

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        
        // /2CGISQL.cgi: POST请求，进行登录校验。验证成功跳转到welcome.html，即资源请求成功页面; 验证失败跳转到logError.html，即登录失败页面
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    // 如果请求资源为/0，表示跳转到注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));   // 将网站目录和/log.html进行拼接，更新到m_real_file中

        free(m_url_real);
    }
    // 如果请求资源为/1，表示跳转到登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));   // 将网站目录和/log.html进行拼接，更新到m_real_file中

        free(m_url_real);
    }
    // 如果请求资源为/5，表示跳转到图片请求界面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/6，表示跳转到视频请求界面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/7，表示跳转到关注界面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接。这里的情况是welcome界面，请求服务器上的一个图片"judge.html"
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 通过stat获取请求资源文件信息。成功则将信息更新到m_file_stat结构体，失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)     // stat(): 取得指定文件的文件信息
        return NO_RESOURCE;

    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))     // S_ISDIR(): 判断一个路径是否为目录
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);    // mmap(): 用于将一个文件或其他对象映射到内存，提高文件的访问速度
    close(fd);    // 避免文件描述符的浪费和占用

    return FILE_REQUEST;    //表示请求文件存在，且可以访问
}


// 取消目标文件到内存的映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);   // munmap: 释放由mmap创建的这段内存空间
        m_file_address = 0;
    }
}


// 写入响应报文    
bool http_conn::write()
{
    int temp = 0;

    //若要发送的数据长度为0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    // 重新注册可读事件 (开启EPOLLONESHOT)
        init();                                             // 重新初始化HTTP对象
        return true;
    }

    while (1)
    {
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);     // writev(): 以顺序iov[0],iov[1]至iov[iovcnt-1]从缓冲区中聚集输出数据，返回输出的字节总数

        if (temp < 0)
        {
            // 判断TCP写缓冲区是否满了 (如果TCP写缓冲没有空间，则重新注册写事件，等待下一轮EPOLLOUT事件，因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性)
            if (errno == EAGAIN)   
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);    // 重新注册可写事件
                return true;
            }

            unmap();    // 如果发送失败，但不是缓冲区问题，取消映射，关闭连接
            return false;
        }

        // 更新已发送字节数和待发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;   // 不再继续发送头部信息
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 否则，继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 判断数据是否已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();                                            // 取消映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    // 重新注册可读事件 (开启EPOLLONESHOT)

            // 判断浏览器的请求是否为长连接
            if (m_linger)
            {
                init();    // 如果是长连接，则重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}


// 用于更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char *format, ...)
{
     // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    va_list arg_list;             // 定义可变参数列表
    va_start(arg_list, format);   // 将变量arg_list初始化为传入参数
    // 将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);    // vsnprintf(): 将可变参数格式化输出到一个字符数组
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;   // 更新m_write_idx位置
    va_end(arg_list);     // 清空可变参列表
 
    LOG_INFO("request:%s", m_write_buf);     // log打印输出内容

    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加消息报头 (具体的添加文本长度、连接状态和空行)
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// 添加文本类型 (这里是html)
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
// 添加连接状态 (通知浏览器端是保持连接还是关闭)
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
// 添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


// 为发送响应报文做准备 (向m_write_buf写入响应报文数据，第一个iovec指针指向响应报文缓冲区，第二个iovec指针指向mmap返回的文件指针)
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:     // 内部错误，500
    {
        add_status_line(500, error_500_title);   // 状态行
        add_headers(strlen(error_500_form));     // 消息头
        if (!add_content(error_500_form))        // 消息体
            return false;
        break;
    }
    case BAD_REQUEST:        // 报文语法有误，404
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:  // 资源没有访问权限，403
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:       // 文件存在，200
    {
        add_status_line(200, ok_200_title);   

        // 如果请求的资源存在
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;   // 发送的全部数据为响应报文头部信息和文件大小
            return true;
        }
        // 如果请求的资源不存在，则返回空白html文件
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }

    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


// 线程通过process函数对任务进行处理 (处理客户请求)
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();         // HTTP报文解析
    // NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);   // 重新注册可读事件 (开启EPOLLONESHOT)
        return;
    }

    bool write_ret = process_write(read_ret);   // HTTP报文响应 (此时表示接收并解析了一个完整的请求)
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);      // 注册可写事件 (开启EPOLLONESHOT)
}
