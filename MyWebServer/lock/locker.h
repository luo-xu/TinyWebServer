#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 封装POSIX信号量的类 
class sem       
{
public:
    // 创建并始化信号量m_sem为0 (sem_init函数第二个参数为0: 表示该信号量为当前进程的 局部信号量)
    sem()              
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }

    // 创建并始化信号量m_sem为num         
    sem(int num)       
    {
        if (sem_init(&m_sem, 0, num) != 0)          
        {
            throw std::exception();
        }
    }

    // 销毁信号量m_sem
    ~sem()             
    {
        sem_destroy(&m_sem);    
    }
    
    // 等待信号量 (sem_wait函数以原子操作的方式 将信号量m_sem的值-1。如果m_sem==0，则该函数一直被阻塞，直到m_sem!=0)
    bool wait()       
    {
        return sem_wait(&m_sem) == 0;
    }

    // 增加信号量 (sem_post函数以原子操作的方式 将信号量m_sem的值+1。当m_sem>0时，其他正在调用sem_wait等待信号量的线程 将被唤醒)
    bool post()       
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;     // 信号量m_sem
};

// 封装互斥锁的类 
class locker
{
public:
    // 创建并初始化互斥锁m_mutex (pthread_mutex_init函数的第二个参数为NULL: 表示默认属性)
    locker()           
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)        
        {
            throw std::exception();
        }
    }

    // 销毁互斥锁m_mutex
    ~locker()          
    {
        pthread_mutex_destroy(&m_mutex);
    }

    // 获取互斥锁 (pthread_mutex_lock以原子操作的方式 给互斥锁m_mutex加锁。如果m_mutex在这之前 已经被锁上，则该函数将被阻塞，直到该互斥锁的占有者将其解锁)
    bool lock()        
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    // 释放互斥锁 (pthread_mutex_unlock以原子操作的方式 给互斥锁m_mutex解锁。如果此时有其他线程 正在等待这个互斥锁，则这些线程中的某一个将获得它)
    bool unlock()      
    { 
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    // 返回互斥锁变量m_mutex
    pthread_mutex_t *get()         
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;     // 互斥锁m_mutex
};

// 封装条件变量的类
class cond
{
public:
    // 创建并初始化条件变量m_cond (pthread_cond_init函数的第二个参数为NULL: 表示使用默认属性)
    cond()         
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }

    // 销毁条件变量m_cond
    ~cond()        
    {
        pthread_cond_destroy(&m_cond);
    }

    // 等待条件变量 (mutex参数是用于 保护条件变量的互斥锁，以确保pthread_cond_wait操作的原子性)
    bool wait(pthread_mutex_t *m_mutex)                             
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    // 在指定时间内，等待条件变量 (当在指定时间内 有信号传过来时，pthread_cond_timedwait返回0; 否则返回一个非0数)
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)      
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    // 唤醒一个 等待目标条件变量的线程 (至于哪个线程被唤醒，则取决于 线程的优先级和调度策略)
    bool signal()           
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    // 唤醒所有等待目标条件变量的线程 
    bool broadcast()        
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;     // 条件变量m_cond
};
#endif
