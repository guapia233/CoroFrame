#include "thread.h"

#include <sys/syscall.h> 
#include <iostream>
#include <unistd.h>  

namespace sylar {

// 线程信息
static thread_local Thread* t_thread = nullptr; // 当前线程的 Thread 对象指针，初始值为 nullptr
static thread_local std::string t_thread_name = "UNKNOWN"; // 当前线程的名字，初始值为"UNKNOWN"
/*
关键字说明：
   static        - 表示变量是静态的（生命周期扩展，变量在程序运行期间持续存在）
   thread_local  - 表示变量是线程局部的，每个线程有独立副本，都会有 Thread 对象的指针及线程的名字
*/
 
pid_t Thread::GetThreadId() // 获取系统分配的线程ID
{
	return syscall(SYS_gettid); // Linux 特定的系统调用，用于获取当前线程的唯一ID 
}

Thread* Thread::GetThis() // 获取当前线程对象的指针
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
    // 参4是参3回调函数的参数，将当前 Thread 对象的指针作为参数传递给 run 函数，这样 run 函数就能访问当前对象的成员
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this); // 绑定 run 函数为线程的入口函数（再在 run 函数中执行真正的回调函数）
    if (rt) // rt不为0，表示线程创建失败
    {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    // 等待线程函数完成初始化，必须等 run 那边完成初始化，这边才能结束，这就是同步
    m_semaphore.wait();
}

Thread::~Thread() 
{
    if (m_thread) 
    {
        // 实现线程分离，让线程在结束后自动释放其资源，而不需要调用 pthread_join() 函数来回收这些资源
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

// 负责线程的初始化以及执行真正的回调函数
/**
 * 为什么 run 函数要设置为 static？
 * run 函数作为 pthread_create 的参3必须为静态函数，因为 pthread_create 无法识别 C++ 的 this 指针机制，
 * 它只接受标准的 C 风格函数，所以使用 static 隐去 this 指针，但该函数需要用到 this 指针，所以在参数中进行传递
 */
void* Thread::run(void* arg) 
{
    Thread* thread = (Thread*)arg; // 由上面创建线程可知 传入的参数是 this 线程对象指针，用 thread 变量来接收

    t_thread       = thread;
    t_thread_name  = thread->m_name;
    thread->m_id   = GetThreadId();

    /**
     * pthread_setname_np 是 POSIX 线程库中的一个非标准扩展函数，用于设置线程的名称，目的是便于后续日志追踪与调试
     * 参1 pthread_self() 用于获取当前线程的ID，参2取 m_name 的前15个字节 
     * 为什么是(0, 15)：因为操作系统对线程名长度有限制，在 Linux 中线程名最长为15，后面还有一个\0，总共16个字节
     */
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb; // 创建一个未绑定任何实际函数的空包装器 
    cb.swap(thread->m_cb); // 不是直接将 cb 赋值给 m_cb，而是用 swap 直接交换两者，可以避免不必要的拷贝或资源释放
     
    // 唤醒构造函数中的 wait ，代表线程初始化完成
    // 确保主线程创建出了一个工作线程，提供给协程使用，否则可能会出现协程在未初始化的线程上执行的情况
    thread->m_semaphore.signal();

    cb(); // 执行真正的回调函数

    return 0;
}

} 

