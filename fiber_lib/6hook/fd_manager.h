#ifndef _FD_MANAGER_H_
#define _FD_MANAGER_H_

#include <memory>
#include <shared_mutex>
#include "thread.h"


namespace sylar{

// 主要用于管理与文件描述符相关的状态和操作 
// FdCtx类在用户态记录了fd的读写超时和非阻塞信息，其中非阻塞包括用户显式设置的非阻塞和hook内部设置的非阻塞
// 区分这两种非阻塞可以有效应对用户对fd设置/获取NONBLOCK模式的情形 
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
private:
	bool m_isInit = false; // 标记文件描述符是否已初始化 
	bool m_isSocket = false; // 标记文件描述符是否是一个套接字 
	bool m_sysNonblock = false; // 标记文件描述符是否设置为系统非阻塞模式
	bool m_userNonblock = false; // 标记文件描述符是否设置为用户非阻塞模式 
	bool m_isClosed = false; // 标记文件描述符是否已关闭 
	int m_fd; // 文件描述符的整数值

	uint64_t m_recvTimeout = (uint64_t)-1; // 读事件的超时时间，默认为-1表示没有超时限制
	uint64_t m_sendTimeout = (uint64_t)-1; // 读事件的超时时间，默认为-1表示没有超时限制

public:
	FdCtx(int fd);
	~FdCtx();

	bool init(); // 初始化 FdCtx 对象
	bool isInit() const {return m_isInit;}
	bool isSocket() const {return m_isSocket;}
	bool isClosed() const {return m_isClosed;}

	// 设置和获取用户层面的非阻塞状态 
	void setUserNonblock(bool v) {m_userNonblock = v;}
	bool getUserNonblock() const {return m_userNonblock;}

	// 设置和获取系统层面的非阻塞状态
	void setSysNonblock(bool v) {m_sysNonblock = v;}
	bool getSysNonblock() const {return m_sysNonblock;}

	// 设置和获取超时时间，type用于区分读事件和写事件，v表示毫秒时间
	void setTimeout(int type, uint64_t v);
	uint64_t getTimeout(int type);
};

// 用于管理 Fdctx 对象的集合，提供了对文件描述符上下文的访问和管理功能
class FdManager
{
public:
	FdManager();

	// 获取指定文件描述符的 Fdctx 对象，如果 auto_create 为 true，表示在不存在时自动创建新的 Fdctx 对象
	std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);

	// 删除指定文件描述符的 Fdctx 对象
	void del(int fd);

private:
	std::shared_mutex m_mutex; // 用于保护对 m_datas 的访问，支持共享读锁和独占写锁
	std::vector<std::shared_ptr<FdCtx>> m_datas; // 存储所有 Fdctx 对象的共享指针
};

// 实现了单例模式，确保一个类只有一个实例，并提供全局访问点 
// 如下是懒汉模式 + 互斥锁维持线程安全 
template<typename T>
class Singleton
{
private:
    static T* instance; // 对外提供的实例
    static std::mutex mutex; // 互斥锁

protected:
    Singleton() {}  

public:
    // Delete copy constructor and assignment operation
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

	// 提供对外的访问点
    static T* GetInstance() 
    {
        std::lock_guard<std::mutex> lock(mutex); // 加锁确保线程安全
        if (instance == nullptr) 
        {
            instance = new T();
        }
        return instance;
    }

    static void DestroyInstance() 
    {
        std::lock_guard<std::mutex> lock(mutex);
        delete instance;
        instance = nullptr; // 防止野指针
    } 
};

typedef Singleton<FdManager> FdMgr; // 重定义将 Singleton<FdManager> 缩写为 FdMgr  

}

#endif