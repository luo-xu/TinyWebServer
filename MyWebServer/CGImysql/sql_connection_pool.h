#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
	MYSQL *GetConnection();				     // 获取一条连接 
	bool ReleaseConnection(MYSQL *conn);     // 释放当前连接
	int GetFreeConn();					     // 获取当前的空闲连接数
	void DestroyPool();					     // 销毁数据库连接池

	// 局部静态变量 (单例模式)
	static connection_pool *GetInstance();   // 创建数据库连接池对象

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);   // 初始化

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;           // 最大连接数
	int m_CurConn;           // 当前已使用的连接数
	int m_FreeConn;          // 当前空闲的连接数
	list<MYSQL *> connList;  // 连接池
	locker lock;      // 互斥锁
	sem reserve;      // 信号量

public:
	string m_url;			// 主机地址
	string m_Port;		    // 数据库端口号
	string m_User;		    // 登陆数据库用户名
	string m_PassWord;	    // 登陆数据库密码
	string m_DatabaseName;  // 使用数据库名
	int m_close_log;  // 日志开关
};


// 将数据库连接的获取与释放 通过“RALL机制”封装，避免手动释放
class connectionRAII{  

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);  // 封装了“获取连接”的接口 GetConnection() (注: 用双指针来修改MYSQL *con)
	~connectionRAII();                                       // 封装了“释放连接”的接口 ReleaseConnection()
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif


/**********注解**********/
// 单例模式: 一种常见的软件设计模式。它的核心结构只包含一个 被称为单例的特殊类。它的目的是 保证一个类仅有一个实例，并提供一个访问它的全局访问点，该实例被所有程序模块共享
// RALL机制 (资源获取就是初始化): 资源用类进行封装起来，对资源的操作 都封装在类的内部，在析构函数中进行释放资源。当定义的局部变量的生命结束时，它的析构函数就会自动的被调用。如此，程序员就无需显示地去调用 释放资源的操作。
