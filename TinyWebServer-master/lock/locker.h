#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem//信号量类
{
public:
    sem()//构造函数
    {
        if (sem_init(&m_sem, 0, 0) != 0)//信号量初始化为0
        {
            throw std::exception(); //信号量初始化失败 抛出异常 并把控制权转移给能够处理该异常的代码
        }
    }
    sem(int num)//有参构造函数
    {
        if (sem_init(&m_sem, 0, num) != 0)//信号量初始化为num
        {
            throw std::exception();//信号量初始化失败 抛出异常 并把控制权转移给能够处理该异常的代码
        }
    }
    ~sem()//析构函数 
    {
        sem_destroy(&m_sem);//信号量销毁
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;//sem_wait函数将以原子操作方式将信号量减一 信号量为0时 sem_wait阻塞 成功时返回0
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;//sem_post函数以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程 成功时返回0
    }

private:
    sem_t m_sem;
};
class locker//互斥锁类
{
public:
    locker()//构造函数
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();//若构造失败 抛出异常 并把控制权转移给能够处理该异常的代码
        }
    }
    ~locker()//析构函数
    {
        pthread_mutex_destroy(&m_mutex);//销毁互斥锁
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;//以原子操作方式给互斥锁加锁 成功返回0 失败返回errno
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;//以原子操作方式给互斥锁解锁 成功返回0 失败返回errno
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; //互斥锁变量 
};
class cond //条件变量类 条件变量提供了一种线程间的通知机制,当某个共享数据达到某个值时,唤醒等待这个共享数据的线程
{
public:
    cond()//构造函数
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)//初始化条件变量
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()//析构
    {
        pthread_cond_destroy(&m_cond);//销毁条件变量
    }
    bool wait(pthread_mutex_t *m_mutex) //条件等待 返回0表示成功 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        //该函数用于等待目标的条件变量 mutex参数是为了用于保护条件变量的互斥锁 以确保wait操作的原子性
        //在调用wait前 必须确保互斥锁加锁 wait函数执行时 首先把调用线程放入条件变量的等待队列中 
        //然后将mutex解锁 可见 从wait开始执行到其调用线程被放入等待队列之间的时间内 signal和broadcvast等函数
        //不会修改条件变量 当wait成功返回时 互斥锁mutex会再次被锁上
        ret = pthread_cond_wait(&m_cond, m_mutex);//该函数调用时需要传入 mutex参数(加锁的互斥锁) ,函数执行时,先把调用线程放入条件变量的请求队列,然后将互斥锁mutex解锁,当函数成功返回为0时,互斥锁会再次被锁上. 也就是说函数内部会有一次解锁和加锁操作
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)//计时等待  返回0表示成功 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);//超过时间会返回 ETIMEDOUT
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()//激活一个等待该条件的线程，存在多个等待线程时按入队顺序激活其中一个
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()// 以广播的方式唤醒所有等待目标变量的线程
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
