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
	if(m_isInit) // 如果已经初始化过了就直接返回 true
	{
		return true;
	}
	
	struct stat statbuf;
	// fstat() 用于获取与文件描述符 m_fd 关联的文件状态信息并存放到 statbuf 中
	if(-1 == fstat(m_fd, &statbuf)) // 如果 fstat() 返回 -1，说明文件描述符无效或在获取文件状态时发生错误
	{
		m_isInit = false;
		m_isSocket = false;
	}
	else
	{
		m_isInit = true;	
		m_isSocket = S_ISSOCK(statbuf.st_mode);	// S_ISSOCK() 用于判断文件类型是否为套接字
	}

	if(m_isSocket) // 如果是套接字，就设置为非阻塞
	{
		int flags = fcntl_f(m_fd, F_GETFL, 0); // 获取该文件描述符的状态
		if(!(flags & O_NONBLOCK)) // 检查是否已经设置了非阻塞
		{
			fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK); // 如果还没设置，就设置为非阻塞
		}
		m_sysNonblock = true; // 设置完将系统非阻塞标志位置为 true
	}
	else
	{
		m_sysNonblock = false;
	}

	return m_isInit; // 返回初始化是否成功
}

// 设置该 fd 的超时时间，type 指定超时类型，包括读事件超时 SO_RCVTIMEO 和写事件超时 SO_SNDTIMEO，v 代表设置的毫秒级超时时间 
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

// 获取该 fd 的超时时间
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
	m_datas.resize(64); // 给 std::vector<std:shared ptr<FdCtx>>m datas 数组分配空间 
}

// 用于获取 m_datas 数组中指定 fd 对应的 FdCtx 对象
std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
{
	if(fd == -1) // 文件描述符无效则直接返回
	{
		return nullptr;
	}

	std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 获取读锁

	// 如果 fd 超出了 m_datas 的范围（fd 是 m_datas 数组的下标），并且 auto_create 为 false，则返回 nullptr，表示没有创建新对象的需求
	if(m_datas.size() <= fd) 
	{
		if(auto_create == false)
		{
			return nullptr;
		}
	}
	else 
	// 如果没超出 m_datas 的范围且该 fd 对应的 FdCtx 存在，或者即使不存在也不打算自动创建，则返回该 FdCtx 对象指针（可能是 nullptr）
	{
		if(m_datas[fd] || !auto_create)
		{
			return m_datas[fd];
		}
	}

	// 当 fd 的大小超出了 m_data.size 的值，也就是 m_datas[fd] 数组中没找到对应的 FdCtx，并且 auto_create 为 true 时会走到这里
	read_lock.unlock(); // 释放读锁
	std::unique_lock<std::shared_mutex> write_lock(m_mutex); // 获取写锁

	if(m_datas.size() <= fd)
	{
		m_datas.resize(fd * 1.5); // 先扩展数组大小
	}
	m_datas[fd] = std::make_shared<FdCtx>(fd); // 然后通过 make_shared 安全创建 FdCtx 对象指针存放到 m_datas 数组中 
	
	return m_datas[fd];
}

// 删除指定 fd 的 FdCtx 对象 
void FdManager::del(int fd)
{
	std::unique_lock<std::shared_mutex> write_lock(m_mutex); // 获取写锁
	if(m_datas.size() <= fd)
	{
		return;
	}

	// reset() 用于释放 shared_ptr 所管理的对象，减少引用计数，并将智能指针置为 nullptr，当引用计数为0时销毁对象
	m_datas[fd].reset();
}

}