#include "http_conn.h"

// 定义http响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/shanshan/NiuKe/webserver/resources";

// 初始化静态变量
int http_conn::m_user_count = 0;  // 所有的客户数
int http_conn::m_epollfd = -1;    // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的

// 设置非阻塞
int setnonblocking(int fd) {
    int oldOption = fcntl(fd, F_GETFL);
    int newOption = oldOption | O_NONBLOCK;
    fcntl(fd, F_SETFL, newOption);

    return oldOption;
}

// 向epoll中注册需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;  // 读事件，对端断开连接事件
    if (one_shot) {
        event.events |= EPOLLONESHOT;  // 针对connfd，开启EPOLLONESHOT，因为我们希望每个socket在任意时刻都只被一个线程处理
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    setnonblocking(fd);  // 设置文件描述符非阻塞
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}



// 初始化连接 (外部调用初始化套接字地址)
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int optval = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    // 向epoll实例注册客户端套接字
    addfd(m_epollfd, m_sockfd, true);
    // 用户数+1
    ++m_user_count;
    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始状态为检查请求行
    m_method = GET;     // 默认请求方式为GET

    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_content_length = 0;
    m_linger = false;   // 默认不保持连接 (Connection : keep-alive保持连接)

    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;

    m_write_idx = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);  // 从epoll中移除监听的客户端套接字
        m_sockfd = -1;
        --m_user_count;
    }
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {  // (可以进一步处理)
        return false;
    }

    while (true) {

        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        int bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {  // 没有数据可读
                break;
            }
            perror("recv");
            return false;
        }
        else if (bytes_read == 0) {  // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;  // 更新数据结尾位置
    }

    printf("[read]: 读取到的数据: \n%s", m_read_buf);
    return true;
}



// 从状态机：解析一行内容 (判断依据：\r\n)
http_conn::LINE_STATUS http_conn::parse_line() {
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        char temp = m_read_buf[m_checked_idx];   // temp为当前要分析的字符

        // 如果当前字节是'\r'字符，则有可能会读取到完整行
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {            // 下一个字符达到了buffer结尾 ('/r'为当前buffer中的最后一个字节)，则说明没有读取到一个完整的行，需要继续接收
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') {   // 下一个字符是\n，说明读取到一个完整的行。则将\r\n改为\0\0，且m_checked_idx指向下一行的开头
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 如果当前字节是'\n'字符，则有可能会读取到完整行 (一般是上次读取到\r就到了buffer末尾，没有接收完整，再次接收时会出现这种情况)
        else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;   // 没遇到'\r','\n'的情况，说明需要继续读
}

// 主状态机解析"HTTP请求行" (获得请求方法，目标url及http版本号)
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过'\t'或' '分隔。

    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");  // 返回字符串text 第一个匹配" \t"中任何一个字符 的字符的下标
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';               // 置位空字符，字符串结束符

    // GET\0/index.html HTTP/1.1
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {  // 比较字符串method和"GET"是否相同 (忽略大小写)
        m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    // /index.html\0HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // http://192.168.110.129:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');  //在字符串m_url中寻找字符'/'第一次出现的位置
    }

    // /index.html
    if (!m_url || m_url[0] != '/') {
        printf("[LOG]: 没有请求资源文件路径！\n");
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;  // 检查状态变成检查头
    return NO_REQUEST;
}

// 主状态机解析"HTTP请求头"
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    // 处理Connection头部字段  
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");    // strspn: 返回text字符串起始部分 " \t"中任意字符 的字符数 (中间可能隔多个' '或'\t'，都要跳过)
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    // 处理Content-Length头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    // 其他字段不予处理
    else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;   // 继续读取头部字段，直到遇到空行
}

// 主状态机解析"HTTP请求体" (仅判断它是否被完整的读入)
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    // 判断消息体是否被读入buffer中
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机：解析http请求 
http_conn::HTTP_CODE http_conn::process_read() {
    // 初始化从状态机状态(LINE_OK)、HTTP请求解析结果(NO_REQUEST)
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    // 在POST请求报文中，消息体的末尾没有任何字符('\r','\n')，不能仅靠从状态机的状态来判断解析结果(GET请求报文可以)，所以当解析请求体时不使用从状态机状态
    // (m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)表示开始解析请求体且上一行(空行)解析成功，||后面表达式就不会执行; 相反没有解析到请求体部分时，就仅执行||后面的从状态机。妙！
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
        || ((line_status = parse_line()) == LINE_OK)) {

        char* text = get_line();       // text指针指向读缓冲中未处理的字符，"从状态机解析出的那一行 的起始位置"
        m_start_line = m_checked_idx;  // "下一次"从状态机解析行的起始位置
        printf("got one http line: %s\n", text);

        // 主状态机的三种状态转移逻辑
        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST) {  // 完整解析GET请求后，跳转到报文响应函数 (GET请求没有请求体)
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST) {   // 完整解析POST请求后，跳转到报文响应函数 (如果支持)
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }

    return NO_REQUEST;
}

// 生成响应报文 (当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功)
http_conn::HTTP_CODE http_conn::do_request() {
    // "/home/nowcoder/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_real_file文件的相关的状态信息
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断访问权限 (读权限)
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        printf("[LOG]: 访问的是目录\n");
        return BAD_REQUEST;
    }

    // 以只读权限打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建文件映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  //此时可以直接关闭描述符，因为mem已经代替其作用

    printf("[do_request]: 访问的文件路径为　%s\n", m_real_file);
    return FILE_REQUEST;
}





// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;              // 定义可变参数列表
    va_start(arg_list, format);    // 将arg_list初始化为传入参数

    // 将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len > (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {   // 如果写入的数据长度超过缓冲区剩余空间，则报错
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;   // 更新m_write_idx位置

    va_end(arg_list);     // 清空可变参列表
    return true;
}
// 添加状态行 
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加消息报头 (具体的添加文本长度、连接状态和空行)
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_content_type() && add_linger() && add_blank_line();
}
// 添加Content-Length字段
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
// 添加文本类型
bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}
// 添加连接状态 (通知浏览器端是保持连接还是关闭)
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
// 添加文本
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}



// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容 (向m_write_buf写入响应报文数据，第一个iovec指针指向响应报文缓冲区，第二个iovec指针指向mmap返回的文件指针)
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
    case INTERNAL_ERROR:         // 内部错误，500
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form)) {
            return false;
        }
        break;
    }
    case BAD_REQUEST:           // 报文语法有误，400
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form)) {
            return false;
        }
        break;
    }
    case NO_RESOURCE:           // 请求资源不存在，404
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form)) {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:     // 资源没有访问权限，403
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_403_form)) {
            return false;
        }
        break;
    }
    case FILE_REQUEST:         // 请求资源存在，200
    {
        add_status_line(200, ok_200_title);
        add_headers(m_file_stat.st_size);
        // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        // 待发送的全部数据为写缓冲+文件大小
        bytes_to_send = m_write_idx + m_file_stat.st_size;

        printf("[process_write]: 文件大小为: %ld   待发送数据大小: %d\n", m_file_stat.st_size, bytes_to_send);
        return true;
    }
    default:
    {
        return false;
    }
    }

    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向m_write_buf (没有响应资源文件)
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    // 待发送的全部数据为写缓冲大小
    bytes_to_send = m_write_idx;
    return true;
}

// 取消目标文件到内存的映射
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write() {
    // 将要发送的字节为0，这一次响应结束
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);  // 重新注册可读事件
        init();                               // 重新初始化HTTP对象
        return true;
    }

    while (true) {
        printf("[write]: 写出的数据: \n");
        printf("%s%s\n", m_write_buf, m_file_address);


        // 分散写
        int temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            // 判断TCP写缓冲区是否满了 (如果TCP写缓冲没有空间，则重新注册写事件，等待下一轮EPOLLOUT事件，因此在此期间无法立即接收到 同一用户的下一请求(EPOLLONESHOT)，但可以保证连接的完整性)
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();   // 如果发送失败，但不是缓冲区问题，则取消映射，关闭连接
            return false;
        }

        // 更新已发送字节数和待发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;


        if (bytes_to_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 判断数据是否已全部发送完
        if (bytes_to_send <= 0) {
            unmap();                              // 取消映射
            modfd(m_epollfd, m_sockfd, EPOLLIN);  // 重新注册可读事件 (开启EPOLLONESHOT)

            // 判断http请求是否为长连接
            if (m_linger) {
                init();      // 如果是长连接，则重新初始化HTTP对象 (为下一次读写做准备)
                return true;
            }
            else {           // 否则，关闭连接
                return false;
            }
        }
    }


}

// 线程通过process函数对任务进行处理 (读http报文: 读到完整http报文则解析报文、最后注册可写; 没读到完整http报文则注册可读，可读时其他线程接着再来处理)
void http_conn::process() {
    // HTTP报文解析
    HTTP_CODE read_ret = process_read();
    // NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);  // 重新注册可读事件 (开启EPOLLONESHOT)
        return;
    }

    // HTTP报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
        // retrun;
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);   // 注册可写事件 (开启EPOLLONESHOT)
}

