#include "thread.h"

#include <sys/syscall.h> 
#include <iostream>
#include <unistd.h>  

namespace sylar {

// 线程信息
static thread_local Thread* t_thread = nullptr; // 当前线程的thread对象指针
static thread_local std::string t_thread_name = "UNKNOWN"; // 当前线程的名字
// static代表生命周期直到程序运行结束时候销毁 
// thread_local表示线程是本地的，也就是每一个访问到这个类的线程都具有一个副本，都会有Thread的指针及当前线程的名字，并且多个线程独立的副本互不影响 


pid_t Thread::GetThreadId() // 获取系统分配的线程ID
{
	return syscall(SYS_gettid); // 系统调用，用于获取当前线程的唯一ID，其中是Linux特定的系统调用编号
}

Thread* Thread::GetThis() // 获取当前线程
{
    return t_thread;
}

const std::string& Thread::GetName() // 获取当前线程的名字
{
    return t_thread_name;
}

void Thread::SetName(const std::string &name) // 给当前线程设置名字 
{
    if (t_thread) 
    {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string &name): 
m_cb(cb), m_name(name) 
{
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this); // 参4是参3回调函数的参数，将当前Thread对象的指针作为参数传递给run函数，这样run函数就能访问当前对象的成员
    if (rt) // rt不为0 表示线程创建失败
    {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    // 等待线程函数完成初始化
    m_semaphore.wait();
}

Thread::~Thread() 
{
    if (m_thread) 
    {
        // 实现线程分离，让线程在结束后自动释放其资源，而不需要调用pthread_join()函数来回收这些资源
        pthread_detach(m_thread); // 适用于不需要等待线程完成并且希望线程在结束后自动清理资源的场景
        m_thread = 0;
    }
}

void Thread::join() // 适用于需要等待线程结束并且显式回收其资源的场景
{
    if (m_thread) 
    {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) 
        {
            std::cerr << "pthread_join failed, rt = " << rt << ", name = " << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) 
{
    Thread* thread = (Thread*)arg; // 由上面创建线程可知 传入的参数是this线程对象指针

    t_thread       = thread;
    t_thread_name  = thread->m_name;
    thread->m_id   = GetThreadId();
    // pthread_self()获取当前线程的ID，设置m_name是取前15个字节，目的是设置线程的名字方便调试
    // 为什么是0，15：因为操作系统对线程名长度的限制，在linux中线程名最长为15，后面还有一个\0总共16
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb; // std::function是智能指针，通常通过std::shared_ptr来实现
    cb.swap(thread->m_cb); // swap可以避免增加m_cb中智能指针的引用计数
    //ps：如果不使用swap，而是直接将thread->m_cb赋值给cb，这种赋值操作会导致一次额外的引用计数增加和对象的拷贝。通过swap，我们直接交换指针，避免了这个问题
    
    // 初始化完成，这里确保主线程创建出来一个工作线程，提供给协程使用，否则可能会出现协程在未初始化的线程上使用的情况
    thread->m_semaphore.signal();

    cb(); // 真正执行函数的地方

    return 0;
}

} 

