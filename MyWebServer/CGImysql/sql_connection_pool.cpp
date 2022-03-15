#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

// 构造函数
connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

// 创建数据库连接池对象 (单例模式)
connection_pool* connection_pool::GetInstance()
{
	static connection_pool connPool;     // 创建数据库连接池对象 (注: 使用局部静态变量 懒汉模式创建连接池)
	return &connPool;
}

// 初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	// 初始化数据库信息
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	// 创建MaxConn条数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL* con = NULL;
		con = mysql_init(con);       // 分配一个MYSQL对象

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error: mysql_init");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);    // 建立一个连接 到"运行host上的一个 MySQL数据库引擎" (如果连接成功，则返回一个 MYSQL*连接句柄)

		if (con == NULL)
		{
			string err_info(mysql_error(con));
			err_info = (string("MySQL Error[errno=")
				+ std::to_string(mysql_errno(con)) + string("]: ") + err_info);
			LOG_ERROR(err_info.c_str());

			//LOG_ERROR("MySQL Error: mysql_real_connect");
			exit(1);
		}
		connList.push_back(con);    // 将建立好的连接 插入数据库连接池
		++m_FreeConn;               // 当前空闲的连接数+1
	}

	reserve = sem(m_FreeConn);      // 信号量reserve初始化为 数据库的连接总数 (使用信号量实现 多线程争夺连接的同步机制)

	m_MaxConn = m_FreeConn;         // 设置最大连接数m_MaxConn为 当前建立的连接的总数m_FreeConn
}


// 获取一条数据库连接 (当有请求时，从数据库连接池中 返回一条可用连接，同时更新使用和空闲连接数)
MYSQL* connection_pool::GetConnection()
{
	MYSQL* con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();    // 取出连接，信号量原子减1，为0则等待

	lock.lock();       // 加锁

	con = connList.front();   // 获取连接 (获取"连接池链表"头部元素)
	connList.pop_front();     // 从连接池中拿出该连接 (将"连接池链表"头部元素 从链表中删除)
	--m_FreeConn;             // 当前空闲连接数-1
	++m_CurConn;              // 当前已使用连接数+1

	lock.unlock();     // 解锁

	return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL* con)
{
	if (NULL == con)
		return false;

	lock.lock();       // 加锁

	connList.push_back(con);   // 将该连接放入连接池 (将该连接 插入"连接池链表"尾部)
	++m_FreeConn;              // 当前空闲连接数+1
	--m_CurConn;               // 当前已使用连接数-1

	lock.unlock();     // 解锁

	reserve.post();    // 释放连接，信号量原子加1
	return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();      // 加锁
	if (connList.size() > 0)
	{
		list<MYSQL*>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)    // 通过迭代器it 遍历"连接池链表"，关闭对应的数据库连接，清空链表
		{
			MYSQL* con = *it;
			mysql_close(con);    // 关闭 MYSQL*连接句柄con 对应的MYSQL连接
		}

		m_CurConn = 0;           // 当前空闲连接数置0
		m_FreeConn = 0;          // 当前已使用连接数置0
		connList.clear();        // 清空链表
	}

	lock.unlock();    // 解锁
}

// 获取当前的空闲连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

// 析构函数 (使用"RAII机制"销毁连接池)
connection_pool::~connection_pool()
{
	DestroyPool();
}



// 构造函数
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {   // 封装了“获取连接”的接口 GetConnection()
	*SQL = connPool->GetConnection();     // 获取一条数据库连接

	conRAII = *SQL;
	poolRAII = connPool;
}
// 析构函数
connectionRAII::~connectionRAII() {                                        // 封装了“释放连接”的接口 ReleaseConnection()
	poolRAII->ReleaseConnection(conRAII);
}