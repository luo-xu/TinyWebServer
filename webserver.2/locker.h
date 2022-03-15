#ifndef LOCKER_H
#define LOCKER_H



#include <pthread.h>
#include <semaphore.h>
#include <exception>

// 互斥锁类
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;   // 互斥锁m_mutex
};


// 条件变量类
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t* m_mutex) {   // 等待条件变量 (mutex参数是用于 保护条件变量的互斥锁，以确保pthread_cond_wait操作的原子性)
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
    }

    bool timewait(pthread_mutex_t* m_mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, m_mutex, &t) == 0;
    }

    bool signal() {   // 唤醒一个 等待目标条件变量的线程 (至于哪个线程被唤醒，则取决于 线程的优先级和调度策略)
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {   // 唤醒所有等待目标条件变量的线程 
        return pthread_cond_broadcast(&m_cond) == 0;
    }


private:
    pthread_cond_t m_cond;
};


// 信号量类
class sem {
public:
    sem() {   // 创建并始化信号量m_sem为0 (sem_init函数第二个参数为0: 表示该信号量用于 多个线程)
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    bool wait() {   // 等待信号量 (sem_wait函数以原子操作的方式 将信号量m_sem的值-1。如果m_sem==0，则该函数一直被阻塞，直到m_sem!=0)
        return sem_wait(&m_sem) == 0;
    }

    bool post() {   // 增加信号量 (sem_post函数以原子操作的方式 将信号量m_sem的值+1。当m_sem>0时，其他正在调用sem_wait等待信号量的线程 将被唤醒)
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};



#endif  // !LOCKER_H
