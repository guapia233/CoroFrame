#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// 宏 HOOK_FUN(XX) 是一个宏展开机制，通过将 XX 依次应用于宏定义中的每一个函数名称来生成一系列代码，可以有效减少重复代码，提高代码的可读性和维护性 
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar{

// 使用线程局部变量，每个线程都会判断一下是否启用了 hook
static thread_local bool t_hook_enable = false; // 当前线程是否启用了 hook 功能，初始值为 false，即 hook 功能默认关闭

// 返回当前线程的 hook 功能是否启用
bool is_hook_enable()
{
    return t_hook_enable;
}

// 设置当前线程的 hook 功能启用状态 
void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

// hook 初始化
void hook_init()
{
	static bool is_inited = false; // 通过一个静态变量来确保 hook init() 只初始化一次，防止重复初始化 
	if(is_inited)
	{
		return;
	}
	is_inited = true;

// dlsym + RTLD_NEXT 用于绕过 hook，调用原始系统调用
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
	HOOK_FUN(XX)
#undef XX // 提前取消 XX 的作用域
}

struct HookIniter
{
	HookIniter() // 钩子函数
	{
		hook_init(); // 初始化 hook，让原始调用绑定到宏展开的函数指针中
	}
};

// 定义一个静态的 HookIniter 实例，由于静态变量的初始化发生在 main 函数之前，所以 hook_init() 会在程序开始时被调用，从而初始化钩子函数
static HookIniter s_hook_initer;

} // end namespace sylar


// 跟踪定时器的状态，cancelled 成员变量用于表示定时器是否已经被取消 
struct timer_info 
{
    int cancelled = 0;
};

/**
 * 自定义的系统调用都要将其参数放入 do_io 模板来做统一的规范化处理：
 * do_io 主要是判断全局 hook 是否启用，并且根据文件描述符是否有效和是否设置了非阻塞来选择是否使用原始系统调用
 * 通过超时管理和取消操作来判断，如果 IO 操作资源不足，会添加一个事件监听器来等待资源可用并让出协程
 * 同时，如果有超时设置，还会启动一个条件计时器来取消事件，超时事件主要是处理当超时时间到达后还没处理的情况
 * 然后添加一个读或写事件来监听相应的事件调用协程处理，所以 do_io 是对 read 等系统调用的封装以实现非阻塞
 * 协程和超时机制保证了可靠性和健壮性，具体在于如果出现资源暂时不可用，我们就注册新事件监听，然后主动 yield 让出执行权，
 * 当定时器超时或者资源满足就会唤醒 epoll_wait，将任务放入到调度器中执行，然后取消相应定时器，这就是 do_io 函数的作用 
 */
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args) 
{
    if(!sylar::t_hook_enable) // 如果全局 hook 功能未启用，则直接调用原始系统调用
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取与文件描述符 fd 相关联的上下文对象 FdCtx，如果上下文不存在，则直接调用原始系统调用
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);  
    if(!ctx) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 如果 FdCtx 已经关闭，设置 errno 为 EBADF 并返回-1 
    if(ctx->isClosed()) 
    {
        errno = EBADF; // 表示文件描述符无效或已经关闭
        return -1;
    }

    // 如果文件描述符不是一个 socket 或者用户已经设置了非阻塞模式，则直接调用原始系统调用
    if(!ctx->isSocket() || ctx->getUserNonblock()) 
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取超时设置并初始化 timer_info 结构体，用于后续的超时管理和取消操作 
    uint64_t timeout = ctx->getTimeout(timeout_so);
    // timer condition
    std::shared_ptr<timer_info> tinfo(new timer_info);

// 调用原始系统调用，如果由于系统中断(EINTR)导致操作失败，函数会重试
retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    
    while(n == -1 && errno == EINTR) 
    {
        n = fun(fd, std::forward<Args>(args)...);
    }
    
    // 0.如果 I/O 操作因为资源暂时不可用(EAGAIN)而失败，函数会添加一个事件监听器来等待资源可用
    // 同时，如果有超时设置，还会启动一个条件计时器来取消事件
    if(n == -1 && errno == EAGAIN) 
    {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1.如果执行的 read 等函数在 FdManager 管理的 FdCtx 中设置了超时时间，就通过 addConditionTimer 创建条件定时器
        if(timeout != (uint64_t)-1) // 检查是否设置了超时时间
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]() 
            {
                auto t = winfo.lock();
                if(!t || t->cancelled) // 如果 timer_info 对象已被释放，或者操作已被取消，则直接返回
                {
                    return;
                }
                t->cancelled = ETIMEDOUT; // 如果超时时间到达并且事件尚未被处理，即 cancelled 仍然是0
                 
                // 取消该文件描述符上的事件，并立即触发一次事件(即恢复被挂起的协程)
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }

        // 2.将 fd 和 event 添加到 IOManager 中进行管理，IOManager 会监听这个文件描述符上的事件，当事件触发时，它会调度相应的协程来处理
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if(-1 == rt) // 如果 rt 为-1，说明 addEvent 失败，会打印一条调试信息 
        {
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer) 
            {
                timer->cancel(); // 并且因为添加事件失败，所以要取消之前设置的定时器，避免误触发
            }
            return -1;
        } 
        else 
        {
            //如果 addEvent 成功，当前协程会调用 yield() 函数，将自己挂起，让出执行权，等待事件的触发 
            sylar::Fiber::GetThis()->yield();
     
            // 3.当协程被恢复（例如事件触发后），它会继续执行 yield() 之后的代码，如果之前设置了定时器，则在事件处理完毕后取消该定时器
            if(timer) 
            {
                timer->cancel();
            }
            
            // 接下来检査 tinfo->cancelled 是否等于 ETIMEDOUT
            // 如果等于，说明该操作因超时而被取消，因此设置 errno 为 ETIMEDOUT 并返回 -1，表示操作失败
            if(tinfo->cancelled == ETIMEDOUT) 
            {
                errno = tinfo->cancelled;
                return -1;
            }
            // 如果没有超时，则跳转到 retry 标签，重新尝试这个操作
            goto retry;
        }
    }
    return n;
}



extern "C"{

// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name ## _fun name ## _f = nullptr;
	HOOK_FUN(XX)
#undef XX 


// 下面三个 sleep 函数的实现过程类似，目的都是将时间转换成毫秒，添加到超时时间堆中，然后让出协程，方便其他任务执行 

// 实现了一个协程版本的 sleep，通过 hook 机制拦截 sleep 的调用，并将其改为使用协程来实现非阻塞的休眠
unsigned int sleep(unsigned int seconds)
{
	if(!sylar::t_hook_enable) // 如果 hook 未启用，则调用原始的系统调用
	{
		return sleep_f(seconds);
	}

    // 获取当前正在执行的协程，将其保存到 fiber 变量中
	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    
    // 获取 IOManager 实例
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	
    // 添加一个定时器模拟睡眠，注意要转换为微秒
	iom->addTimer(seconds*1000, [fiber, iom](){iom->scheduleLock(fiber, -1);});  
	
    // 主动挂起当前协程的执行，将控制权交还给调度器，等待下一次 resume 恢复
	fiber->yield();  

	return 0;
}

// 微秒版 sleep
int usleep(useconds_t usec)
{
	if(!sylar::t_hook_enable)
	{
		return usleep_f(usec);
	}

    // 和上面的 sleep 类似
	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();

	iom->addTimer(usec/1000, [fiber, iom](){iom->scheduleLock(fiber);}); // 转换为毫秒
 
	fiber->yield();

	return 0;
}

// 纳秒版 sleep
int nanosleep(const struct timespec* req, struct timespec* rem)
{
	if(!sylar::t_hook_enable)
	{
		return nanosleep_f(req, rem);
	}	

	int timeout_ms = req->tv_sec*1000 + req->tv_nsec/1000/1000; // 转换为毫秒

	std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
	sylar::IOManager* iom = sylar::IOManager::GetThis();
	 
	iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	 
	fiber->yield();	
    
	return 0;
}

// 对系统调用socket的封装，同时添加了一些额外的逻辑，用于处理自定义的钩子和文件描述符管理 
int socket(int domain, int type, int protocol)
{
	if(!sylar::t_hook_enable) // 如果钩子未启用，则调用原始的系统调用
	{
		return socket_f(domain, type, protocol);
	}	

    //如果钩子启用了，则通过调用原始的socket函数创建套接字 
	int fd = socket_f(domain, type, protocol);
	if(fd==-1) // socket创建失败
	{
		std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		return fd;
	}

    // 如果socket创建成功，会利用FdManager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中 
    // 如果不在则扩展存储文件描述数组大小，并且利用 FdCtx 进行初始化判断是是不是套接字，是不是系统非阻塞模式
	sylar::FdMgr::GetInstance()->get(fd, true);
	return fd;
}

// 用于在连接超时的情况下处理非阻塞套接字连接的实现，首先尝试使用钩子功能来捕获并管理连接请求的行为
// 然后使用 IOManager 和 Timer 来管理超时机制，具体的逻辑实现上和 do_io 类似 
int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) 
{
    // 注意:如果没有启用hook或者不是一个套接字或者用户启用了非阻塞，都直接去调用connect系统调用
    // 因为 connect_with_timeout 本身就是在connect系统调用基础上进行调用的 
    if(!sylar::t_hook_enable) // 没启用hook
    {
        return connect_f(fd, addr, addrlen);
    }

    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd); // 获取文件描述符fd的上下文信息FdCtx
    
    // 检查文件描述符上下文是否存在或是否已关闭
    if(!ctx || ctx->isClosed()) 
    {
        errno = EBADF; // EBAD表示一个无效的文件描述符
        return -1;
    }

    if(!ctx->isSocket()) // 文件描述符不是一个套接字
    {
        return connect_f(fd, addr, addrlen);
    }

    if(ctx->getUserNonblock()) // 用户设置了非阻塞
    {

        return connect_f(fd, addr, addrlen);
    }

    // 尝试进行connect操作 
    int n = connect_f(fd, addr, addrlen);
    if(n == 0) 
    {
        return 0;
    } 
    else if(n != -1 || errno != EINPROGRESS) // 说明连接请求未处于等待状态，直接返回结果
    {
        return n;
    }

    // wait for write event is ready -> connect succeeds
    sylar::IOManager* iom = sylar::IOManager::GetThis(); // 获取当前线程的 IOManager 实例 
    std::shared_ptr<sylar::Timer> timer; // 声明一个定时器对象
    std::shared_ptr<timer_info> tinfo(new timer_info); // 创建一个追踪定时器是否取消的对象
    std::weak_ptr<timer_info> winfo(tinfo); // 判断追踪定时器对象是否存在

    // 检査是否设置了超时时间，如果 timeout_ms 不等于-1，则创建一个定时器
    if(timeout_ms != (uint64_t)-1) 
    {
        // 添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态 
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() 
        {
            auto t = winfo.lock(); 
            // 判断追踪定时器对象是否存在或者追踪定时器的成员变量是否大于0，大于0就意味着取消了
            if(!t || t->cancelled) 
            {
                return;
            }
            t->cancelled = ETIMEDOUT; // 如果超时了但事件仍未处理
            iom->cancelEvent(fd, sylar::IOManager::WRITE); // 将指定的fd的事件触发，将事件处理
        }, winfo);
    }

    // 为文件描述符fd添加一个写事件监听器，目的是为了上面的回调函数处理指定文件描述符
    int rt = iom->addEvent(fd, sylar::IOManager::WRITE); 
    if(rt == 0) // 表示添加事件成功
    {
        sylar::Fiber::GetThis()->yield();

        // resume either by addEvent or cancelEvent
        if(timer) // 如果有定时器，取消定时器 
        {
            timer->cancel();
        }

        if(tinfo->cancelled) // 如果发生超时错误或者用户取消
        {
            errno = tinfo->cancelled; // 赋值给errno，通过其查看具体错误原因 
            return -1;
        }
    } 
    else // 如果添加事件失败
    {
        if(timer) 
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // check out if the connection socket established 
    int error = 0;
    socklen_t len = sizeof(int);

    // 通过 getsocketopt 检查套接字实际错误状态，来判断连接是否成功 
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) 
    {
        return -1;
    }
    if(!error) // 如果没有错误，表示连接成功返回0
    {
        return 0;
    } 
    else // 如果有错误，设置errno并返回错误
    {
        errno = error;
        return -1;
    }
}

// connect_with_timeout函数实际上是在原始connect系统调用基础上，增加了超时控制的逻辑
// 在超时时间为-1时，表示不启用超时功能，也就是不会调用 addConditionTimer 函数放入到超时时间堆中
// 等待超时唤醒 tickle 触发 IOManager::idle函数中epoll，而是只监听这个事件，这个事件没到就一直阻塞直到成功或失败
static uint64_t s_connect_timeout = -1; // 表示默认的连接超时时间
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    // 调用hook启用后的 connect_with_timeout 函数
	return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

// 用于处理套接字接受连接的操作，同时支持超时连接控制，和recv等函数一样使用了do_io的模板，实现了非阻塞accpet的操作
// 并且如果成功接受了一个新的连接，则将新的文件描述符fd添加到文件描述符管理器(FdManager)中进行跟踪管理 
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);	
    /* 参数说明：
    sockfd：监听套接字的文件描述符
    accept_f：原始的accpet系统调用函数指针 
    "accept"：操作名称，用于调试和日志记录 
    sylar::IOManager::READ：表示READ事件(即有新的连接可接受时触发) 
    SO_RCVTIMEO：接收超时时间选项，用于处理超时逻辑 
    addr和addrlen：用于保存新连接的客户端地址信息 
    */
	if(fd>=0)
	{
		sylar::FdMgr::GetInstance()->get(fd, true); // 添加到文件描述符管理器 FdManager 中
	}
	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);	
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

// 将所有文件描述符上的事件处理，调用IOManager的cancelAll函数将fd上的读写事件全部处理，最后从FdManger文件描述符管理中移除该fd
int close(int fd)
{
	if(!sylar::t_hook_enable)
	{
		return close_f(fd);
	}	

	std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

	if(ctx)
	{
		auto iom = sylar::IOManager::GetThis();
		if(iom)
		{	
			iom->cancelAll(fd);
		}
		// del fdctx
		sylar::FdMgr::GetInstance()->del(fd); 
	}
	return close_f(fd); // 处理完后调用原始系统调用
}

// 封装fcntl函数，对某些操作进行自定义处理，比如处理非阻塞模式表示，同时保留了对原始fcntl的调用
int fcntl(int fd, int cmd, ... /* arg */ )
{
  	va_list va; // to access a list of mutable parameters

    va_start(va, cmd); // 使其指向第一个可变参数(在cmd之后的参数)
    switch(cmd) 
    {
        case F_SETFL: // 用于设置文件描述符的状态标志(例如，设置非阻塞模式)
            {
                int arg = va_arg(va, int); // Access the next int argument
                va_end(va);
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

                // 如果ctx无效，或者文件描述符关闭或者不是一个套接字，就调用原始调用
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return fcntl_f(fd, cmd, arg);
                }

                // 用户是否设置了非阻塞
                ctx->setUserNonblock(arg & O_NONBLOCK);

                // 最后是否阻塞根据系统设置决定
                if(ctx->getSysNonblock()) 
                {
                    arg |= O_NONBLOCK;
                } 
                else 
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd); // 调用原始的 fcntl 函数获取文件描述符的当前状态标志 
                std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

                // 如果上下文无效、文件描述符已关闭或不是套接字，则直接返回状态标志
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
                {
                    return arg;
                }

                // 这里是呈现给用户，显示的是用户设定的值，但是底层还是根据系统设置决定的
                if(ctx->getUserNonblock()) 
                {
                    return arg | O_NONBLOCK;
                } else 
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int); // 从va获取标志位
                va_end(va); // 清理va
                return fcntl_f(fd, cmd, arg); // 调用原始调用 
            }
            break;


        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va); // 清理va变量
                return fcntl_f(fd, cmd); // 返回原始调用的结果
            }
            break;

        case F_SETLK: // 设置文件锁，如果不能立即获得锁，则返回失败 
        case F_SETLKW: // 设置文件锁，如果不能立即获得锁，则阻塞等待
        case F_GETLK: // 获取文件锁的状态，如果fd关联的文件已经被锁定，那么该命令会填充f1ock结构体，指示锁的状态
            {
                // 从可变参数列表中获取 struct flock* 类型的指针，这个指针指向一个flock结构体，包含锁定操作相关的信息(如锁的类型、偏移量、锁的长度等)
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        case F_GETOWN_EX: // 获取文件描述符fd所属的所有者信息，这通常用于与信号处理相关的操作，尤其是在异步IO操作中
        case F_SETOWN_EX: // 设置文件描述符fd的所有者信息
            {
                // 和上面的思路类似
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*); // 从可变参数中提取相应类型的结构体指针
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }	
}

// 实际处理了文件描述符上的ioctl系统调用，并在特定条件下对 FIONBIO(用于设置非阻塞模式)进行了特殊处理
int ioctl(int fd, unsigned long request, ...)
{
    va_list va; // va持有处理可变参数的状态信息
    va_start(va, request); // 给va初始化让它指向可变参数的第一个参数位置 
    void* arg = va_arg(va, void*); // 将va指向的参数以 void* 类型取出并存放到arg中
    va_end(va); // 用于结束对 va_list 变量的操作，清理va占用的资源

    if(FIONBIO == request) // 用于设置非阻塞模式的命令 
    {
        bool user_nonblock = !!*(int*)arg; // 当前ioctl调用是为了设置或清除非阻塞模式 
        std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

        // 检查获取的上下文对象是否有效，如果上下文对象无效或文件描述符已关闭或不是一个套接字，则直接调用原始的ioct1函数 
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) 
        {
            return ioctl_f(fd, request, arg);
        }

        // 如果上下文对象有效，调用其 setUserNonblock 方法，将非阻塞模式设置为 user_nonblock 指定的值，这将更新fd的非阻塞状态 
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(fd, request, arg);
}

// 一个用于获取套接字选项值的函数，它允许你检查指定套接字的某些选项的当前设置 
int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
	return getsockopt_f(sockfd, level, optname, optval, optlen);
    /* 参数说明：
    sockfd：套接字文件描述符，表示要操作的套接字 
    level：指定选项的协议层次，常见的值是SOL_SOCKET，表示通用套接字选项，还有可能是IPPROTO_TCP等协议相关选项
    optname：表示你要获取的选项的名称，例如可以是SO_RCVBUF，表示接收缓冲区大小或SO_RESUEADDR，表示地址重用
    optval：指向一个缓冲区，该缓冲区将存储选项的值
    optlen：指向一个变量，该变量指定optval缓冲区的大小，并在函数返回时包含实际存储的选项值的大小
    */
}

// 用于设置套接字的选项，它允许你对套接字的行为进行配置，如设置超时时间、缓冲区大小、地址重用等
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if(!sylar::t_hook_enable) 
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    // 如果level是SOL_SOCKET且optname是SO_RCVTIMEO(接收超时)或SO_SNDTIMEO(发送超时)，代码会获取与该fd关联的FdCtx上下文对象
    if(level == SOL_SOCKET) 
    {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) 
        {
            std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            // 读取传入的timeval结构体，将其转化为毫秒数，并调用 ctx->setTimeout 方法，记录超时设置 
            if(ctx) 
            {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }

    // 无论是否执行了超时处理，最后都会调用原始的 setsockopt_f 函数来设置实际的套接字选项 
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}

}
