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
    /*
    explicit 关键字用来修饰只有一个参数的类构造函数，以表明该构造函数是显式的，而非隐式的 
    当使用 explicit 修饰构造函数时，它将禁止类对象之间的隐式转换，以及禁止隐式调用拷贝构造函数  
    比如防止出现 Semaphore sem = 3，将数字3转换成 Semaphore 对象的这种情况，必须标准：Semaphore sem(3);
    */
    
    // P操作
    void wait() 
    {
        std::unique_lock<std::mutex> lock(mtx); // 没有选择使用 lock_guard 是因为它不允许手动解锁，并且无法在 wait 中将锁释放，只能等待l ock_guard 函数结束
        while (count == 0) { // 为了防止虚假唤醒，直到 count > 0 才跳出循环
            /*
            这里体现了 uique_lock 允许在 cv.wait(lock) 内手动解锁 mtx，而 lock_guard 不允许这样做 
            cv.wait(lock) 在等待时会自动释放 mtx，让其他线程可以获取锁并修改 count
            当 cv.wait(lock) 满足条件变量的条件被 notify_one() 或 notify_all() 唤醒后 
            lock 会自动重新获取 mtx，确保 count-- 操作是安全的
            */
            cv.wait(lock); // wait for signal
        }
        count--;
    }

    // V操作：负责给 count++，然后通知 wait 唤醒等待的线程
    void signal() 
    {
        std::unique_lock<std::mutex> lock(mtx); // 加锁
        count++;
        cv.notify_one();  // signal：注意这里的 one 指的不一定是一个线程，有可能是多个
    }
};

// 一共两种线程: 1.由系统自动创建的主线程 2.由Thread类创建的线程 
class Thread 
{
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();

    pid_t getId() const { return m_id; } // 获取进程ID
    const std::string& getName() const { return m_name; } // 获取线程名

    void join();

public:
    // 获取系统分配的线程ID
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

    std::function<void()> m_cb; // 线程需要运行的函数
    std::string m_name; // 线程名
    
    Semaphore m_semaphore; // 引入信号量的类来完成线程的同步创建
};

}

#endif
