#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {

// 1.注册事件 -> 2.等待事件 -> 3.事件触发，调度回调函数 -> 4.从 epoll 中注销事件 -> 5.执行回调函数
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
    // 每个 socket fd 都对应一个 Fdcontext，包括 fd 的值，fd 上的事件以及 fd 的读写事件的上下文
    struct FdContext // 文件描述符的事件上下文 
    {
        // 具体事件的上下文 
        struct EventContext 
        {
            Scheduler *scheduler = nullptr; // 关联的调度器
            std::shared_ptr<Fiber> fiber; // 关联的回调协程
            std::function<void()> cb; // 关联的回调函数（会被封装为协程对象）
        };

        EventContext read; // 读事件的上下文
        EventContext write; // 写事件的上下文
        int fd = 0; // 事件关联的 fd（句柄）

        // 注册事件
        Event events = NONE; // 当前注册的事件，可能是 READ、WRITE 或二者的组合

        std::mutex mutex;

        // 根据事件类型获取相应的事件上下文（如读事件上下文或写事件上下文）
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

    // 添加一个事件到文件描述符 fd 上，并关联一个回调函数 cb
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // 删除文件描述符 fd 上的某个事件
    bool delEvent(int fd, Event event);
    // 取消文件描述符 fd 上的某个事件，并触发回调函数
    bool cancelEvent(int fd, Event event);
    // 取消文件描述符 fd 上的所有事件，并触发所有回调函数
    bool cancelAll(int fd);

    static IOManager* GetThis();

protected:
    // 通知调度器有任务调度
    // 写管道让 idle 协程从 epoll_wait 退出，待 idle 协程 yield 之后，scheduler::run 就可以调度其他任务
    void tickle() override; // 重写 scheduler 类中的虚函数
    
    // 判断调度器是否可以停止
    // 判断条件是 scheduler::stopping() 外加 I0Manager 的 m_pendingEventcount 为0，表示没有 I0 事件可调度
    bool stopping() override;
    
    // 当没有事件处理时，线程处于空闲状态时的处理逻辑
    // idle 协程只负责收集所有已触发的 fd 的回调函数并将其加入调度器的任务队列，真正的执行时机是 idle 协程退出后，调度器在下一轮调度时执行
    void idle() override; 

    // 重写 Timer 类的虚函数，当有新的定时器插入到最前面时的处理逻辑
    void onTimerInsertedAtFront() override;

    // 调整文件描述符上下文数组的大小
    void contextResize(size_t size);

private:
    int m_epfd = 0; // 用于 epoll 的文件描述符
    int m_tickleFds[2]; // 用于线程间通信的管道文件描述符，fd[0]是读端，fd[1]是写端 

    // 使用 atomic 的好处是这个变量进行+或-操作时不会被多线程影响，确保线程安全
    std::atomic<size_t> m_pendingEventCount = {0}; // 原子计数器，用于记录待处理的事件数量
    std::shared_mutex m_mutex; // 读写锁
    std::vector<FdContext *> m_fdContexts; // 文件描述符上下文数组，用于存储每个文件描述符的 Fdcontext
};

}  

#endif