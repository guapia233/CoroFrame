#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {

// work flow
// 1.注册事件 -> 2.等待事件 -> 3.事件触发 调度回调函数 -> 4.从epoll中注销事件 -> 5.执行回调函数
class IOManager : public Scheduler, public TimerManager 
{
public:
    enum Event 
    {
        NONE = 0x0, // 表示没有事件
        READ = 0x1, // 表示读事件，对应 epoll 的 EPOLLIN 事件
        WRITE = 0x4 // 表示写事件，对应 epoll 的 EPOLLOUT 事件
    };

private:
    // 文件描述符的事件上下文
    // 每个socket fd都对应一个Fdcontext，包括fd的值，fd上的事件以及fd的读写事件的上下文
    struct FdContext 
    {
        // 具体事件的上下文，比如读事件或写事件
        struct EventContext 
        {
            Scheduler *scheduler = nullptr; // 关联的调度器
            std::shared_ptr<Fiber> fiber; // 关联的回调线程（协程）
            std::function<void()> cb; // 关联的回调函数（都会被封装为协程对象）
        };

        EventContext read; // 读事件的上下文
        EventContext write; // 写事件的上下文
        int fd = 0; // 事件关联的fd（句柄）

        // 注册事件
        Event events = NONE; // 当前注册的事件，可能是 READ、WRITE 或二者的组合
        std::mutex mutex;
        // 根据事件类型获取相应的事件上下文(如读事件上下文或写事件上下文)
        EventContext& getEventContext(Event event);
        // 重置事件上下文
        void resetEventContext(EventContext &ctx);
        // 触发事件，根据事件类型调用对应上下文结构的调度器去调度协程或函数
        void triggerEvent(Event event);        
    };

public:
    //threads：线程数量，use caller：主线程是否参与调度，name：调度器的名字
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    // 事件管理方法
    // 添加一个事件到文件描述符 fd 上，并关联一个回调函数 cb
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // 添加文件描述符 fd 上的某个事件
    bool delEvent(int fd, Event event);
    // 取消文件描述符 fd 上的某个事件，并触发回调函数
    bool cancelEvent(int fd, Event event);
    // 取消文件描述符 fd 上的所有事件，并触发所有回调函数
    bool cancelAll(int fd);

    static IOManager* GetThis();

protected:
    // 通知调度器有任务调度
    // 写pipe让idle协程从epoll wait退出，待idle协程yield之后，scheduler::run 就可以调度其他任务
    void tickle() override; // 重写scheduler类中的虚函数
    
    // 判断调度器是否可以停止
    // 判断条件是 scheduler::stopping() 外加 I0Manager 的 m_pendingEventcount 为0，表示没有I0事件可调度
    bool stopping() override;
    
    // 当没有事件处理时，线程处于空闲状态时的处理逻辑
    // idle协程只负责收集所有已触发的fd的回调函数并将其加入调度器的任务队列，真正的执行时机是idle协程退出后，调度器在下一轮调度时执行
    void idle() override; 

    //重写Timer类的虚函数，当有新的定时器插入到最前面时的处理逻辑
    void onTimerInsertedAtFront() override;

    // 调整文件描述符上下文数组的大小
    void contextResize(size_t size);

private:
    int m_epfd = 0; // 用于epoll的文件描述符
    int m_tickleFds[2]; // 用于线程间通信的管道文件描述符，fd[0]是读端，fd[1]是写端 

    // 使用atomic的好处是这个变量再进行+或-都不会被多线程影响
    std::atomic<size_t> m_pendingEventCount = {0}; // 原子计数器，用于记录待处理的事件数量
    std::shared_mutex m_mutex; // 读写锁
    std::vector<FdContext *> m_fdContexts; // 文件描述符上下文数组，用于存储每个文件描述符的Fdcontext
};

}  

#endif