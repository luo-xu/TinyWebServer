#ifndef HTTP_CONN_H
#define HTTP_CONN_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>

class http_conn {
public:
    static const int FILENAME_LEN = 200;      // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 2048;  // 写缓冲区的大小

    // HTTP请求方法 (这里只支持GET)
    enum METHOD {
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
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,  // 正在分析请求行
        CHECK_STATE_HEADER,           // 正在分析请求头
        CHECK_STATE_CONTENT           // 正在分析请求体
    };
    // 报文解析的结果
    enum HTTP_CODE {
        NO_REQUEST,              // 请求不完整，需要继续读取客户数据
        GET_REQUEST,             // 获得了一个完成的客户请求
        BAD_REQUEST,             // 请求有语法错误
        NO_RESOURCE,             // 服务器没有资源
        FORBIDDEN_REQUEST,       // 客户对请求的资源没有访问权限
        FILE_REQUEST,            // 文件请求
        INTERNAL_ERROR,          // 服务器内部错误
        CLOSED_CONNECTION        // 客户端已关闭连接 (未使用)
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
    void init(int sockfd, const sockaddr_in& addr);  // 初始化新接受的连接
    void close_conn();   // 关闭客户端
    void process();      // 处理客户端请求
    bool read();         // 非阻塞读
    bool write();        // 非阻塞写

private:
    void init();   // 初始连接
    HTTP_CODE process_read();            // 解析http请求 (主状态机)
    bool process_write(HTTP_CODE ret);   // 填充http应答

    // 这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line(char* text);                  // 主状态机解析HTTP请求行
    HTTP_CODE parse_headers(char* text);                       // 主状态机解析HTTP请求头
    HTTP_CODE parse_content(char* text);                       // 主状态机解析HTTP请求体
    HTTP_CODE do_request();                                    // 生成响应报文
    char* get_line() { return m_read_buf + m_start_line; }     // 获取当前处理的行的第一个字符的下标
    LINE_STATUS parse_line();                                  // 解析一行内容 (从状态机)

    // 这一组函数被process_write调用以填充HTTP应答 (根据响应报文格式，生成对应8个部分)
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;      // epoll实例（所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的）
    static int m_user_count;   // 客户数量

private:
    // 客户端socket信息
    int m_sockfd;                    // 客户连接套接字描述符
    struct sockaddr_in m_address;    // 客户socket地址

    // 与读相关
    char m_read_buf[READ_BUFFER_SIZE];   // 读缓冲区
    int m_read_idx;                      // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置 (m_read_buf当前的长度)
    int m_checked_idx;                   // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                    // 当前正在解析的行的起始位置 (已解析的字符数)

    // 状态机相关
    CHECK_STATE m_check_state;           // 当前主状态的状态
    METHOD m_method;                     // 请求方法

    // 解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];  // 客户请求的目标文件的完整路径
    char* m_url;                     // 客户请求的目标文件的文件名
    char* m_version;                 // HTTP协议版本号 (我们仅支持HTTP1.1)
    char* m_host;                    // 主机名
    int m_content_length;            // HTTP请求体的字节数
    bool m_linger;                   // HTTP是否需要保持连接

    // 与写相关
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int m_write_idx;                     // 写缓冲区中待发送的字节数 (m_write_buf当前的长度)
    int bytes_to_send;                   // 将要发送的字节数
    int bytes_have_send;                 // 已经发送的字节数
    char* m_file_address;                // 客户请求的目标文件的完整路径 (客户请求的目标文件被mmap到内存中的起始位置)
    struct stat m_file_stat;             // 目标文件的信息 (通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息)
    struct iovec m_iv[2];                // 采用writev来执行写操作
    int m_iv_count;                      // 被写内存块的数量

};




#endif // !HTTP_CONN_H