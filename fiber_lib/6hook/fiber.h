#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>     
#include <memory>       
#include <atomic>       
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>

namespace sylar {

// std::enable_shared_from_this<T>是C++标准库提供的一个模板类
// 它允许对象在其成员函数中获取指向自身的智能指针 std::shared_ptr 
class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
	// 定义协程状态 在上下文切换时需要被保存
	enum State
	{
		READY, 
		RUNNING, 
		TERM 
	};

private:
	Fiber(); // 定义为私有函数，只能被GetThis()调用，用于创建主协程  

public:
	// 用于创建指定回调函数、栈大小和run_in_scheduler(即本协程是否参与调度器调度，默认为true)
	Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true); 
	~Fiber();

	// 重用一个协程：重置协程状态和入口函数，复用栈空间，不重新创建栈
	void reset(std::function<void()> cb);

	
	void resume(); // 任务协程恢复执行
	void yield(); // 任务协程让出执行权
 
	uint64_t getId() const {return m_id;} //获取唯一标识
	State getState() const {return m_state;}

public:
	// 设置当前运行的协程
	static void SetThis(Fiber *f);

	// 获取当前运行的协程的shared_ptr实例
	static std::shared_ptr<Fiber> GetThis();

	// 设置调度协程（默认为主协程）
	static void SetSchedulerFiber(Fiber* f);
	
	// 获取当前运行的协程ID
	static uint64_t GetFiberId();

	// 协程的函数，入口点
	static void MainFunc();	

private:
	// 协程唯一标识符
	uint64_t m_id = 0;
	// 栈大小
	uint32_t m_stacksize = 0;
	// 协程状态(初始为ready)
	State m_state = READY;
	// 协程上下文
	ucontext_t m_ctx;
	// 协程栈指针
	void* m_stack = nullptr;
	// 协程的回调函数
	std::function<void()> m_cb;
	// 是否让出执行权交给调度协程
	bool m_runInScheduler;

public:
	std::mutex m_mutex;
};

}

#endif

