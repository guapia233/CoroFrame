#ifndef _THREAD_H_
#define _THREAD_H_

#include <mutex>
#include <condition_variable>
#include <functional>     

namespace sylar // 将后续的类、函数等代码封装在 sylar 这个命名空间下，避免不同模块下的同名冲突
{

// 用于线程方法间的同步
class Semaphore 
{
private:
    std::mutex mtx;                
    std::condition_variable cv;    
    int count;                   

public:
    // 信号量初始化为0
    explicit Semaphore(int count_ = 0) : count(count_) {}
    
    // P操作
    void wait() 
    {
        std::unique_lock<std::mutex> lock(mtx); // 没有选择使用lock_guard是因为它不允许手动解决，并且无法在wait中将锁释放，只能等待lock_guard函数结束
        while (count == 0) { // 为了防止虚假唤醒，直到count > 0才跳出循环
            cv.wait(lock); // wait for signals
        }
        count--;
    }

    // V操作 负责给count++,然后通知wait唤醒等待的线程
    void signal() 
    {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();  // signal 注意这里的one指的不一定是一个线程，有可能是多个
    }
};

// 一共两种线程: 1.由系统自动创建的主线程 2.由Thread类创建的线程 
class Thread 
{
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();

    pid_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }

    void join();

public:
    // 获取系统分配的线程id
	static pid_t GetThreadId();
    // 获取当前所在线程
    static Thread* GetThis();

    // 获取当前线程的名字
    static const std::string& GetName();
    // 设置当前线程的名字
    static void SetName(const std::string& name);

private:
	// 线程函数
    static void* run(void* arg);

private:
    pid_t m_id = -1; // 进程ID
    pthread_t m_thread = 0; // 线程ID

    // 线程需要运行的函数
    std::function<void()> m_cb;
    std::string m_name; // 线程名
    
    Semaphore m_semaphore; // 引入信号量的类来完成线程的同步创建
};

























}



#endif
