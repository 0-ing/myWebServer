#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>


// 线程池类，定义成模板类是为了代码的复用
// 这个线程池类，是将线程池，和任务的请求队列，都放在一起了
template<typename T>
class threadpool {
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);
    void run();

private:
    static void* worker(void* arg);
     
private:

    // 线程的数量
    int m_thread_number;

    // 线程池数组， 大小为m_thread_number
    pthread_t* m_threads;

    // 请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    // 请求队列
    std :: list<T*> m_workqueue;

    // 互斥锁, 保护请求队列的
    locker m_queuelocker;

    // 信号量用来判断是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程，这个是结束所有的线程
    bool m_stop;

};

template <typename T>
threadpool<T> :: threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL) {
        if ((thread_number <= 0) || (max_requests <= 0)) {
            throw std :: exception();
        }
    
        m_threads = new pthread_t[m_thread_number];
        if (!m_threads) {
            throw std:: exception();
        }

        // 创建thread_number个线程，并将它们设置为线程脱离
        for (int i = 0; i < thread_number; i++){
            printf("create the %dth thread\n", i);

            // 由于这里的worker是静态函数，静态成员函数里是不能调用成员变量的，所以直接将对象作为参数
            // 导进静态成员函数中去，即将this作为参数，这样就可以调用该对象的成员变量了
            if (pthread_create(m_threads + i, NULL, worker, this) != 0 ) {
                delete [] m_threads;
                throw std :: exception();
            }

            if (pthread_detach(m_threads[i])) {
                delete [] m_threads;
                throw std:: exception();
            };
            
        }

    }

template <typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
    m_stop = true;

}

template <typename T>
bool threadpool<T>:: append(T* request){

    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 信号量增加
    m_queuestat.post();

    return true;
    
}

template <typename T>
void* threadpool<T>::worker(void* arg){

    threadpool* pool = (threadpool*)arg;
    // 创建出线程，目的是为了拿出请求队列中的任务，这也正是worker中要干的事
    pool -> run();
    return pool;
}

template <typename T>
void threadpool<T>::run (){
    while(!m_stop) {
        // 判断是否需要堵塞，信号值减一
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        
        T* request = m_workqueue.front();
        m_workqueue.pop_front();  // 取出之后，删掉这个任务
        m_queuelocker.unlock();

        // 感觉这步有点多余，因为上面已经判读过m_workqueue工作队列是否为空，所以这里坑定是拿到请求了
        if (!request) {
            continue;
        }
        // 这个才是针对一个任务，要进行的处理
        request -> process();



    }
}

#endif