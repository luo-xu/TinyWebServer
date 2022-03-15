Linux平台下实现的一个轻量级Web服务器，访问服务器数据库实现web端用户注册、登录功能，可以请求服务器图片和视频文件。

1. 使用 线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor 和 同步IO模拟Proactor 均实现) 的并发模型； 
2. 使用状态机解析 HTTP 请求报文，支持解析 GET 和 POST 请求； 
3. 使用定时器处理非活动连接； 
4. 访问服务器数据库 实现 web 端用户注册、登录功能，可以请求服务器图片和视频文件; 
5. 实现同步/异步日志系统，记录服务器运行状态; 
6. 经 Webbench 压力测试可以实现上万的并发连接;

启动服务器，创建并初始化log对象、数据库连接池、线程池。
设置监听套接字，监听客户端http连接请求。
接收到新连接后，主线程为之创建定时器、初始化http连接，并向epoll内核事件表注册该socket上的读就绪事件。
主线程调用epoll_wait等待socket上有数据可读。

(1) 同步I/O模拟Proactor模式下：
当socket上有数据可读，epoll_wait通知主线程，主线程从socket循环读取数据，直到没有更多数据可读，然后将读取到的数据封装成一个请求对象并插入请求队列。
睡眠在请求队列上某个工作线程被唤醒，获得一个请求对象，并从数据库连接池中 获取一条数据库连接 来处理该请求对象。
工作处理客户请求（解析http请求报文，并生成相应的相应报文），然后往epoll内核事件表中注册该socket上的写就绪事件。
该socket上有数据可写，epoll_wait通知主线程，主线程往socket上写入服务器处理客户请求的结果（响应报文）。

(2) Rector模式下：
主线程监测到可读事件，直接将该事件放入请求队列，由工作线程来读取客户数据，并处理客户请求，然后往epoll内核事件表中注册该socket上的写就绪事件。
主线程检测到可写事件，直接将该事件放入请求队列，由工作线程来写入响应报文。


* 测试前确认已安装MySQL数据库

    ```C++
    // 建立yourdb库
    create database yourdb;

    // 创建user表
    USE yourdb;
    CREATE TABLE user(
        username char(50) NULL,
        passwd char(50) NULL
    )ENGINE=InnoDB;

    // 添加数据
    INSERT INTO user(username, passwd) VALUES('name', 'passwd');
    ```

* 修改main.cpp中的数据库初始化信息

    ```C++
    //数据库登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "yourdb";
    ```

* build

    ```C++
    sh ./build.sh
    ```

* 启动server

    ```C++
    ./server
    ```

* 浏览器端

    ```C++
    ip:9006
    ```

个性化运行
------

```C++
./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]
```

温馨提示:以上参数不是非必须，不用全部使用，根据个人情况搭配选用即可.

* -p，自定义端口号
	* 默认9006
* -l，选择日志写入方式，默认同步写入
	* 0，同步写入
	* 1，异步写入
* -m，listenfd和connfd的模式组合，默认使用LT + LT
	* 0，表示使用LT + LT
	* 1，表示使用LT + ET
    * 2，表示使用ET + LT
    * 3，表示使用ET + ET
* -o，优雅关闭连接，默认不使用
	* 0，不使用
	* 1，使用
* -s，数据库连接数量
	* 默认为8
* -t，线程数量
	* 默认为8
* -c，关闭日志，默认打开
	* 0，打开日志
	* 1，关闭日志
* -a，选择反应堆模型，默认Proactor
	* 0，Proactor模型
	* 1，Reactor模型

测试示例命令与含义

```C++
./server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1
```

- [x] 端口9007
- [x] 异步写入日志
- [x] 使用LT + LT组合
- [x] 使用优雅关闭连接
- [x] 数据库连接池内有10条连接
- [x] 线程池内有10条线程
- [x] 关闭日志
- [x] Reactor反应堆模型

