#include "config.h"

int main(int argc, char *argv[])
{
    // 需要修改的数据库信息 (登录名,密码,库名)
    string user = "root";
    string passwd = "root";
    string databasename = "mydb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化  
    server.init(config.PORT,         // 端口号
                user,                // 数据库登录名
                passwd,              // 数据库密码
                databasename,        // 数据库库名
                config.LOGWrite,     // 日志写入方式
                config.OPT_LINGER,   // 优雅关闭连接
                config.TRIGMode,     // 触发组合模式
                config.sql_num,      // 数据库连接池数量
                config.thread_num,   // 线程池内的线程数量
                config.close_log,    // 是否关闭日志
                config.actor_model   // 并发模型选择
                );  
    

    // 日志 (创建并初始化log对象，同时设置log日志写入方式)
    server.log_write();

    // 数据库 (创建并初始化数据库连接池，以及初始化数据库读取表)
    server.sql_pool();

    // 线程池 (创建并初始化线程池)
    server.thread_pool();

    // 触发模式 (设置listenfd和connfd的模式组合)
    server.trig_mode();

    // 监听 (设置监听套接字)
    server.eventListen();

    // 运行 (循环监听并处理事件)
    server.eventLoop();

    return 0;
}