#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;           // 设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;      // 设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;     // 设置写缓冲区m_write_buf大小

    // 报文的请求方法 (本项目只用到GET和POST)
    enum METHOD
    {
        GET = 0,          
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,    // 正在分析请求行
        CHECK_STATE_HEADER,             // 正在分析请求头
        CHECK_STATE_CONTENT             // 正在分析请求体
    };
    // 报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,           // 客户请求不完整 
        GET_REQUEST,          // 获得了一个完整的客户请求
        BAD_REQUEST,          // 客户请求有语法错误
        NO_RESOURCE,          // 没有资源
        FORBIDDEN_REQUEST,    // 客户对请求的资源没有访问权限
        FILE_REQUEST,         // 文件请求
        INTERNAL_ERROR,       // 服务器内部错误
        CLOSED_CONNECTION     // 客户端已关闭连接 (未使用)
    };
    // 从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,    // 读取到一个完整的行  
        LINE_BAD,       // 行出错 
        LINE_OPEN       // 行数据尚且不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);   // 初始化新连接 (函数内部会调用私有方法init)
    void close_conn(bool real_close = true);                                                                        // 关闭http连接
    void process();                                                                                                 // 处理客户请求
    bool read_once();                                                                                               // 读取浏览器端发来的全部数据
    bool write();                                                                                                   // 写入响应报文
    sockaddr_in *get_address()    // 获取客户端socket地址
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);    // 同步线程初始化数据库读取表 (CGI使用线程池初始化数据库表)
    int timer_flag;
    int improv;


private:
    void init();
    HTTP_CODE process_read();                    // 从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);           // 向m_write_buf写入响应报文数据
    HTTP_CODE parse_request_line(char *text);    // 主状态机解析HTTP请求行
    HTTP_CODE parse_headers(char *text);         // 主状态机解析HTTP请求头
    HTTP_CODE parse_content(char *text);         // 主状态机解析HTTP请求体
    HTTP_CODE do_request();                      // 生成响应报文

    char *get_line() { return m_read_buf + m_start_line; };    // get_line用于将指针向后偏移，指向未处理的字符 (m_start_line是已解析的字符数)
    LINE_STATUS parse_line();     // 从状态机分析一行内容
    void unmap();

    // 根据响应报文格式，生成对应8个部分 (以下函数均由do_request调用)
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;      // 内核事件表 (所以socket上的事件都被注册到同一个epoll内核事件表中，所以epoll文件描述符设置为static)
    static int m_user_count;   // 用户数量
    MYSQL *mysql;              // MYSQL*连接句柄
    int m_state;   // 读为0, 写为1 (Reactor模式下，工作线程需要进行I/O读写数据，读线程或者写线程)

private:
    int m_sockfd;                          // 该HTTP连接的socket
    sockaddr_in m_address;                 // 客户socket地址

    char m_read_buf[READ_BUFFER_SIZE];     // 读缓冲区 (存储读取的请求报文数据)
    int m_read_idx;                        // 标识读缓冲区中数据的最后一个字节的下一个位置 (m_read_buf当前的长度)
    int m_checked_idx;                     // 从状态机正在解析的字符在读缓冲区中的位置 
    int m_start_line;                      // 已解析的字符数 (当前正在解析的行的起始位置)

    char m_write_buf[WRITE_BUFFER_SIZE];   // 写缓冲区 (存储发出的响应报文数据)
    int m_write_idx;                       // 写缓冲区中待发送的字节数 (w_write_buf当前的长度)

    CHECK_STATE m_check_state;             // 主状态机的状态
    METHOD m_method;                       // 请求方法

    // 以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];   // 客户请求的目标文件的完整路径
    char *m_url;                      // 客户请求的目标文件的文件名
    char *m_version;                  // HTTP协议版本号
    char *m_host;                     // 主机名
    int m_content_length;             // HTTP请求的消息体的长度
    bool m_linger;                    // HTTP是否需要保持连接

    char *m_file_address;      // 读取服务器上的文件地址 (客户请求的目标文件被mmap到内存中的起始位置)
    struct stat m_file_stat;   // 目标文件的信息 (通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息)

    // 我们将采用writev来执行写操作，所以定义如下两个成员，其中m_iv_count表示被写内存块的数量
    struct iovec m_iv[2];       
    int m_iv_count;

    int cgi;                   // 是否启用的POST
    char *m_string;            // 存储请求体数据
    int bytes_to_send;         // 剩余发送字节数
    int bytes_have_send;       // 已发送字节数
    char *doc_root;   // 网站的根目录

    map<string, string> m_users;
    int m_TRIGMode;                 // 连接套接字LT或ET模式 (0为LT,1为ET)
    int m_close_log;                // 是否关闭日志

    char sql_user[100];      // 登陆数据库用户名
    char sql_passwd[100];    // 登陆数据库密码
    char sql_name[100];      // 使用数据库名
};

#endif
