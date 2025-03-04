#include "fd_manager.h"
#include "hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sylar{

template class Singleton<FdManager>; // FdManager 类有一个全局唯一的单例实例

// 以下四行行代码定义了 Singleton 类模板的静态成员变量 instance 和 mutex，静态成员变量需要在类外部定义和初始化
template<typename T>
T* Singleton<T>::instance = nullptr;

template<typename T>
std::mutex Singleton<T>::mutex;	

FdCtx::FdCtx(int fd):
m_fd(fd)
{
	init();
}

FdCtx::~FdCtx()
{

}

bool FdCtx::init()
{
	if(m_isInit) // 如果已经初始化过了就直接返回true
	{
		return true;
	}
	
	struct stat statbuf;
	// fstat()函数用于获取与文件描述符m_fd关联的文件状态信息并存放到statbuf中
	// 如果fstat()返回-1，表示文件描述符无效或出现错误
	if(-1 == fstat(m_fd, &statbuf))
	{
		m_isInit = false;
		m_isSocket = false;
	}
	else
	{
		m_isInit = true;	
		m_isSocket = S_ISSOCK(statbuf.st_mode);	// S_ISSOCK()用于判断文件类型是否为套接字
	}

	if(m_isSocket) // 如果是套接字 -> 设置非阻塞
	{
		// fcntl_f() -> the original fcntl() -> get the socket info
		int flags = fcntl_f(m_fd, F_GETFL, 0); // 获取文件描述符的状态
		if(!(flags & O_NONBLOCK)) // 检查当前标志中是否已经设置了非阻塞标志 
		{
			fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK); // 如果还没设置 -> 设置非阻塞
		}
		m_sysNonblock = true; 
	}
	else
	{
		m_sysNonblock = false;
	}

	return m_isInit; // 初始化是否成功
}

// 设置与文件描述符相关的超时时间
// type指定超时类型的标志，包括 SO_RCVTIMEO 和 SO_SNDTIMEO，分别用于接收超时和发送超时，v代表设置的超时时间(ms)
void FdCtx::setTimeout(int type, uint64_t v)
{
	if(type == SO_RCVTIMEO)
	{
		m_recvTimeout = v;
	}
	else
	{
		m_sendTimeout = v;
	}
}

// 获取与文件描述符相关的超时时间
uint64_t FdCtx::getTimeout(int type)
{
	if(type == SO_RCVTIMEO)
	{
		return m_recvTimeout;
	}
	else
	{
		return m_sendTimeout;
	}
}

FdManager::FdManager()
{
	m_datas.resize(64); // 给std::vector<std:shared ptr<FdCtx>>m datas;数组分配空间 
}

// 用于获取 m_datas 数组中的 FdCtx 对象，如果 FdCtx 对象不存在且 m_datas.size() <= fd 的话
// 会先扩展数组大小然后通过 FdCtx 的构造函数创建其对象存放到 m_datas 数组中 
std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
{
	if(fd == -1) // 文件描述符无效则直接返回
	{
		return nullptr;
	}

	std::shared_lock<std::shared_mutex> read_lock(m_mutex);
	// 如果fd超出了 m_datas 的范围，并且 auto_create 为 false，则返回 nullptr，表示没有创建新对象的需求
	if(m_datas.size() <= fd) 
	{
		if(auto_create == false)
		{
			return nullptr;
		}
	}
	else // 没超出 m_datas 的范围，则返回 FdCtx 对象指针
	{
		if(m_datas[fd]||!auto_create)
		{
			return m_datas[fd];
		}
	}

	// 当fd的大小超出 m_data.size 的值，也就是 m_datas[fd] 数组中没找到对应的fd，并且 auto_create 为true时会走到这里
	read_lock.unlock();
	std::unique_lock<std::shared_mutex> write_lock(m_mutex);

	if(m_datas.size() <= fd)
	{
		m_datas.resize(fd*1.5); // 先扩展数组大小
	}

	m_datas[fd] = std::make_shared<FdCtx>(fd); // 然后通过 FdCtx 的构造函数创建其对象存放到 m_datas 数组中 
	return m_datas[fd];

}

// 删除指定文件描述的 FdCtx 对象 
void FdManager::del(int fd)
{
	std::unique_lock<std::shared_mutex> write_lock(m_mutex);
	if(m_datas.size() <= fd)
	{
		return;
	}

	// 智能指针shared调用reset()减少其对对象的引用计数，当引用计数为0时销毁对象
	m_datas[fd].reset();
}

}