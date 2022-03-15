#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*actor_model用于模型切换，connPool是数据库连接池指针，thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);             // 主线程将新任务插入请求队列

private:
    static void *worker(void *arg);        // 工作线程运行的函数 (调用run执行任务)
    void run();                            // 主要实现 (工作线程从请求队列中取出某个任务进行处理)

private:
    int m_thread_number;          // 线程池中的线程数
    int m_max_requests;           // 请求队列中允许的最大请求数
    pthread_t *m_threads;         // 描述线程池的数组 (线程id数组，其大小为m_thread_number)
    std::list<T *> m_workqueue;   // 请求队列
    locker m_queuelocker;         // 互斥锁 (保护请求队列)
    sem m_queuestat;              // 信号量 (是否有任务需要处理)
    connection_pool *m_connPool;  // 数据库连接池
    int m_actor_model;    // 模型切换
};

// 构造函数
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_number];   // 创建线程id数组
    if (!m_threads)
        throw std::exception();

    for (int i = 0; i < thread_number; ++i)       // 循环创建thread_number条线程
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)    // 创建一个线程 (参数意义分别为: 线程id、线程属性、线程函数、传入线程函数的实参。worker函数为静态成员函数，没有this指针，因此将this指针作为参数 以访问线程池的成员变量)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))         // 将线程设置为unjoinable状态，从而不用单独对工作线程进行回收 (当脱离线程退出时，系统会自动回收该线程的资源。而无需要调用pthread_join() 来回收该线程的资源)
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

// reactor: 主线程将新任务插入请求队列 (socket可读可写事件。state: 读为0，写为1。工作线程负责处理逻辑和读写数据)
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();    // 加锁

    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;         // 设置当前事件 (是读还是写)
    m_workqueue.push_back(request);   // 将任务对象插入请求队列

    m_queuelocker.unlock();  // 解锁
    m_queuestat.post();      // 插入任务对象，信号量原子+1 (如果>0，则唤醒等待该信号量的工作线程)
    return true;
}

// proactor: 主线程将新任务插入请求队列 (工作线程仅负责处理逻辑)
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();

    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);

    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 线程函数
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;   // arg即传入的this指针，类型为threadpool* (在静态函数worker中 引用这个threadpool对象，并调用其动态方法run)
    pool->run();
    return pool;
}

// 主要实现 (工作线程从请求队列中取出某个任务进行处理)
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();       // 信号量等待 (如果信号量原子>0，则信号量原子-1，并向下执行；否则阻塞)

        m_queuelocker.lock();     // 加锁
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();   // 从请求队列中取出第一个任务
        m_workqueue.pop_front();            // 将该任务从请求队列中删除
        m_queuelocker.unlock();   // 解锁

        if (!request)
            continue;    
        // Reactor模型
        if (1 == m_actor_model)    
        {
            // 读工作线程
            if (0 == request->m_state)    
            {
                if (request->read_once())   // 工作线程循环读取客户数据，直到无数据可读或对方关闭连接
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);   // 从数据库连接池中 获取一条数据库连接 来处理该任务对象request
                    request->process();                                     // 线程通过process函数对任务进行处理 (报文解析和响应)
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            // 写工作线程
            else
            {
                if (request->write())       // 写入响应报文
                {
                    request->improv = 1;
                }
                else              
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        // Proactor模型 (同步I/O模拟proactor模式)
        else                      
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);    
            request->process();                                      
        }
    }
}
#endif
