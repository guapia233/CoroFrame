#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;

namespace sylar {

// 获取当前线程的调度器对象，并尝试将其转换为 IOManager* 类型
// 如果当前线程的调度器确实是一个 IOManager 对象，转换成功并返回 IOManager* 指针
// 否则，转换失败时返回 nullptr，抛出std:bad cast异常
IOManager* IOManager::GetThis() 
{
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

// 据传入的事件event，返回对应事件上下文的引用
IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) 
{
    assert(event==READ || event==WRITE);    
    switch (event) 
    {
    case READ:
        return read;
    case WRITE:
        return write;
    }
    throw std::invalid_argument("Unsupported event type"); // 当传入的参数不符合预期时抛出异常
}

// 重置 EventContext 事件的上下文，将其恢复到初始或者空的状态
// 主要作用是清理并重置传入的 EventContext 对象，使其不再与任何调度器、线程或回调函数相关联
void IOManager::FdContext::resetEventContext(EventContext &ctx) 
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

// 在指定的IO事件被触发时，执行相应的回调函数，并且在执行完之后清理相关的事件上下文
void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    assert(events & event); // 确保event中有指定的事件，否则程序中断

    // 清理该事件，表示不再关注，也就是说，注册IO事件是一次性的 
    // 如果想持续关注某个socket fd的读写事件，那么每次触发事件后都要重新添加
    events = (Event)(events & ~event); // 因为使用了十六进制位，所以对标志位取反就相当于将event从events中删除
    
    // 获取相应的EventContext具体的读或写事件对应的上下文
    EventContext& ctx = getEventContext(event);

    // 将fd绑定的具体读或写任务的回调协程或回调函数，放入到任务队列中等待调度器调度
    if (ctx.cb) 
    {
        // call ScheduleTask(std::function<void()>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.cb);
    } 
    else 
    {
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }

    // 重置 EventContext 事件的上下文
    resetEventContext(ctx);
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name): 
Scheduler(threads, use_caller, name), TimerManager()
{
    // 创建 epoll fd
    // epoll create 的参数实际上在现代 Linux 内核中已经被忽略，在最早版本的 Linux 中，该参数用于指定 epoll 内部使用的事件表大小
    m_epfd = epoll_create(5000); 
    assert(m_epfd > 0);

    // 创建 pipe
    int rt = pipe(m_tickleFds); // 创建管道的函数规定了 m_tickleFds[0] 是读端，1是写端
    assert(!rt);

    // 将管道的读事件监听注册到epoll上
    epoll_event event;
    event.events  = EPOLLIN | EPOLLET; // 标志位，设置边缘触发和读事件
    event.data.fd = m_tickleFds[0]; // 读端

    // 将管道文件描述符修改为非阻塞，配合边缘触发
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    // 将 m_tickleFds[0] 作为读事件放入到event监听集合中
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    // 初始化一个包含32个文件描述符上下文的数组
    contextResize(32);

    // 启动 Scheduler，开启线程池，准备处理任务
    start();
}

IOManager::~IOManager() {
    stop(); // 关闭 scheduler 类中的线程池，让任务全部执行完后线程安全退出
    close(m_epfd); // 关闭epoll的句柄
    close(m_tickleFds[0]); // 关闭管道读端
    close(m_tickleFds[1]); // 关闭管道写端

    // 将fdcontext文件描述符上下文逐个关闭
    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]) 
        {
            delete m_fdContexts[i]; 
        }
    }
}

// 主要作用是调整 m_fdContexts 数组的大小，并为新的文件描述符fd创建并初始化相应的 FdContext 对象
void IOManager::contextResize(size_t size) 
{
    m_fdContexts.resize(size);
    
    // 遍历 m_fdContexts 数组，初始化尚未初始化的 FdContext 对象
    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i] == nullptr) 
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i; // 将文件描述符的编号赋值给fd
        }
    }
}

// 为上面 contextResize()分配好的fd添加一个event事件，并在事件触发时执行指定的回调函数或回调协程，具体的触发是在triggerEvent
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) 
{
    // 查找 FdContext 对象 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 读写锁
    if ((int)m_fdContexts.size() > fd) // 说明传入的fd在数组里面，将对应的 FdContext 赋值给fd_ctx
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else // 没在数组里面，则重新分配数组的size来初始化 FdContext 的对象
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd]; // 潜在的bug？当 扩容1.5倍仍然小于fd时，会导致数组越界访问
    }
    
    // 找到或者创建好 FdContext 对象后，加上互斥锁，确保 FdContext 的状态不会被其他线程修改
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // 判断要添加的事件是否已经在fd_ctx中的events中了
    if(fd_ctx->events & event) 
    {
        return -1; // 已经存在就返回-1，因为相同的事件不能重复添加
    }

    // 添加新事件：如果已经存在其它事件就修改已有事件，如果不存在就添加事件
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events   = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    // 将事件添加到epol1中
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    ++m_pendingEventCount; // 原子计数器，待处理的事件+1

    // 更新 FdContext 的 events 成员，记录当前的所有事件
    // 注意 events 可以监听读和写的组合，如果 fd_ctx -> events 为none,就相当于直接是 fd_ctx -> events = event
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // 设置event_ctx事件上下文 
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb); // 确保 EventContext 中没有其他正在执行的调度器、协程或回调函数 
    event_ctx.scheduler = Scheduler::GetThis(); // 设置调度器为当前的调度器实例 
    
    // 如果提供了回调函数，则将其保存到 EventContext 中;否则，将当前正在运行的协程保存到 EventContext 中，并确保协程的状态是正在运行的
    if (cb) 
    {
        event_ctx.cb.swap(cb);
    } 
    else 
    {
        event_ctx.fiber = Fiber::GetThis(); // 需要确保存在主协程
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;
}

// 从 IOManager 中删除某个文件描述符的特定事件
bool IOManager::delEvent(int fd, Event event) {
    // 查找 FdContext 对象，这几步和上面的 addEvent() 类似
    FdContext *fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 读锁
    // 如果找到，则将fd对应的FdContext赋值给fd_ctx；否则说明数组中没有这个文件描述符，也就没有它对应的事件，直接返回false
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    // 添加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // 虽然找到了FdContext 对象，但如果它的事件与参数传来的事件不相同，返回false
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    
    // 删除事件：对原有的事件状态取反就是删除原有的事件，比如说传入参数是读事件，我们取反就是删除了这个读事件，但可能还有写事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    // 如果取反后还剩下其它事件，则修改事件；如果 new_events 为空，说明fd上已经没有其它事件了，直接将fd从监听红黑树上移除
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx; // 为了在epoll事件触发时能够快速找到与该事件相关联的 FdContext 对象

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount; // 原子计数器，待处理的事件-1

    // 更新 FdContext 的 events 成员
    fd_ctx->events = new_events;

    // 重置 EventContext
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

// 取消特定文件描述符上的指定事件，并触发该事件的回调函数
// 与 delEvent 不同之处在于删除事件后，还需要将删除的事件直接交给trigger函数放入到协程调度器中进行触发 
bool IOManager::cancelEvent(int fd, Event event) {
    // 依然是查找 FdContext，和delEvent一致 
    FdContext *fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // 如果要取消的事件不存在
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    // 删除该事件：和上面的delEvent一致，只有最后的处理不同，一个是重置，一个是调用事件的回调函数    
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount;

    // 调用事件的回调函数    
    fd_ctx->triggerEvent(event); 
    return true;
}

// 取消指定文件描述符上的所有事件，并且触发这些事件的回调函数
bool IOManager::cancelAll(int fd) {
    // 依旧查找 FdContext 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    
    // 该fd上没有事件存在
    if (!fd_ctx->events) 
    {
        return false;
    }

    // 删除该fd上的所有事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    // 调用所有事件的回调函数    
    if (fd_ctx->events & READ) 
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if (fd_ctx->events & WRITE) 
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

// 重写scheduler中的tickle()：检测到有空闲线程（即epoll_wait阻塞的线程）时，通过写入一个字符到管道(mtickleFds[1])中，唤醒那些等待任务的线程
// 目的：epoll_wait() 可能会长时间阻塞，如果没有 I/O 事件发生，线程就不会醒过来，导致线程无法执行新的任务
void IOManager::tickle() 
{
    // 检查当前是否有线程处于空闲状态，如果没有空闲线程，函数直接返回，不执行后续操作
    if(!hasIdleThreads()) 
    {
        return;
    }

    // 如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符"T"
    // 这个写操作的目的是：向阻塞在 m_tickleFds[0](管道的另一端)的线程发送一个信号，通知它有新任务可以处理了
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

// 重写了 scheduler 的 stopping()：检查定时器、挂起事件以及调度器状态，以决定是否可以安全地停止运行
bool IOManager::stopping() 
{
    uint64_t timeout = getNextTimer();
    // 没有剩余的定时器 && 没有剩余的事件 && Scheduler::stopping(没有剩余的任务 && 没有活跃的线程)
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

// 重写Scheduler中的idle()
// 通常在没有任务处理时运行，等待和处理IO事件
// 即使当前没有任务处理，线程也会在id1e()中持续休眠并等待新的任务，保证在所有任务完成之前调度器不会退出
void IOManager::idle() 
{    
    static const uint64_t MAX_EVNETS = 256; // 定义 epoll_wait 能同时处理的最大事件数 

    // 使用 std::unique_ptr 动态分配了一个大小为 MAX_EVENTS 的 epoll_event 数组，用于存储从 epoll_wait 获取的事件
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

    while (true) 
    {
        if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl; 

        if(stopping()) 
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            break;
        }

        // blocked at epoll_wait
        int rt = 0;
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000; // 定义最大超时时间为5000毫秒（5s）
            uint64_t next_timeout = getNextTimer(); // 获取下一个（最近一个）超时的定时器
            next_timeout = std::min(next_timeout, MAX_TIMEOUT); // 获取下一个定时器的超时时间，并将其与 MAX_TIMEOUT 取较小值，避免等待时间过长

            // epoll_wait 陷入阻塞，等待tickle信号的唤醒，并且使用了定时器堆中最早超时的定时器作为 epoll_wait 超时时间
            rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);
            if(rt < 0 && errno == EINTR) // rt小于0表示无限阻塞，errno是EINTR(表示信号中断)
            {
                continue;
            } 
            else 
            {
                break; // 超时触发 or 注册的fd有事件发生
            }
        };

        // 收集所有超时的定时器 
        std::vector<std::function<void()>> cbs; // 用于存储超时的回调函数
        listExpiredCb(cbs); // 获取所有超时的定时器的回调函数，并将它们添加到cbs数组中
        if(!cbs.empty()) 
        {
            for(const auto& cb : cbs) 
            {
                scheduleLock(cb);
            }
            cbs.clear();
        }
        
        // 检查完定时器，就要检查响应事件了：遍历计数器rt，表示准备好的事件数
        for (int i = 0; i < rt; ++i) 
        {
            epoll_event& event = events[i]; // 获取第i个epollevent，用于处理该事件 

            // 检査当前事件是否是 tickle 事件(即用于唤醒空闲线程的事件)
            if (event.data.fd == m_tickleFds[0]) 
            {
                uint8_t dummy[256];
                // 边缘触发 -> exhaust
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0); // 不断取数据直到返回-1
                continue;
            }

            // 其它事件就通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息
            FdContext *fd_ctx = (FdContext *)event.data.ptr;

            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // 如果当前事件是错误或挂起(EPOLLERR 或 EPOLLHUP)，则将其转换为可读或可写事件(EPOLLIN或EPOLLOUT)，以便后续处理
            if (event.events & (EPOLLERR | EPOLLHUP)) 
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
             
            // 确定实际发生的事件类型(读、写或both)
            int real_events = NONE;
            if (event.events & EPOLLIN) 
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) 
            {
                real_events |= WRITE;
            }

            if ((fd_ctx->events & real_events) == NONE) 
            {
                continue;
            }

            // 这里进行取反就是计算除响应的该事件外剩下的事件
            int left_events = (fd_ctx->events & ~real_events);
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events; // 如果 left_event 为空，即没有事件了，那么就只剩下边缘触发了 

            // 根据之前计算的操作op，调用 epoll_ctl 修改或删除epoll监听 
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // 触发事件执行
            if (real_events & READ) 
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        } // end for

        // 当前线程的协程主动让出控制权，调度器可以选择执行其它任务或再次进入idle状态
        Fiber::GetThis()->yield();
  
    } // end while(true)
}

// 当定时器被插入到最小堆的最前面时，触发tickle事件，唤醒阻塞的 epoll_wait ，回收超时的定时任务(回调和协程)并放入协程调度器中等待调度
void IOManager::onTimerInsertedAtFront() 
{
    tickle(); // 唤醒可能被阻塞的 epoll_wait 调用
}

} // end namespace sylar