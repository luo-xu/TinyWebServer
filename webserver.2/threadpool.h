#ifndef PTHREADPOOL_H
#define PTHREADPOOL_H



#include<iostream>
#include "locker.h"
#include <list>
#include <stdio.h>
#include <pthread.h>
using std::list;

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    // thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量
    threadpool(int thread_umber = 8, int max_requests = 10000);
    ~threadpool();
    // 主线程将新任务插入请求队列
    bool append(T* request);

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行之
    static void* work(void* arg);
    void run();

private:
    int m_thread_number;     // 线程池中的线程数
    pthread_t* m_threads;    // 描述线程池的数组 (线程id数组，其大小为m_thread_number)
    int m_max_requests;      // 请求队列中最多允许的、等待处理的请求的数量
    list<T*> m_workqueue;    // 请求队列
    locker m_queuelocker;    // 互斥锁 (保护请求队列)
    sem m_queuestat;         // 信号量 (是否有任务需要处理)
    bool m_stop;             // 是否结束线程 
};

// 构造函数
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_stop(false) {
    if (thread_number <= 0 || max_requests <= 0) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];  // 创建线程id数组
    if (!m_threads) {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for (int i = 0; i < thread_number; ++i) {
        printf("[threadpool]: creat the %dth thread\n", i);
        if (pthread_create(&m_threads[i], NULL, work, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }

        if (pthread_detach(m_threads[i]) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数
template<typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

// 主线程将新任务插入请求队列
template<typename T>
bool threadpool<T>::append(T* request) {
    m_queuelocker.lock();

    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);

    m_queuelocker.unlock();
    m_queuestat.post();   // 增加信号量 (如果>0，则唤醒等待该信号量的工作线程)
    return true;
}

template <typename T>
void* threadpool<T>::work(void* arg) {
    threadpool* pool = (threadpool*)arg;   // arg即传入的this指针，类型为threadpool* (在静态函数worker中 引用这个threadpool对象，并调用其动态方法run)
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuestat.wait();   // 等待信号量 (如果信号量原子>0，则信号量原子-1，并向下执行；否则阻塞)

        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();  // 从请求队列中取出第一个任务
        m_workqueue.pop_front();           // 将该任务从请求队列中删除
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }

        printf("[run]: 当前线程id为 %ld\n", pthread_self());
        request->process();   // 线线程通过process函数对任务进行处理 (报文解析和响应)
    }
}



#endif // !PTHREADPOOL_H