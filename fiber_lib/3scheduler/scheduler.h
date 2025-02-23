#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

//#include "hook.h"
#include "fiber.h"
#include "thread.h"

#include <mutex>
#include <vector>

namespace sylar {

class Scheduler
{
public:
	// threads指定线程池的线程数量，use_caller指定是否将主线程作为工作线程，name指定调度器的名称
	Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name="Scheduler");
	virtual ~Scheduler(); // 防止出现资源泄露，基类指针删除派生类对象时不完全销毁的情况
	
	const std::string& getName() const {return m_name;} // 获取调度器的名称

public:	
	// 获取正在运行的调度器
	static Scheduler* GetThis();

protected:
	// 设置正在运行的调度器
	void SetThis();
	
public:	
	// 添加任务到任务队列
    template <class FiberOrCb> // Fiberorcb是调度任务类型，可以是协程对象或函数指针
    void scheduleLock(FiberOrCb fc, int thread = -1) 
    {
    	bool need_tickle; // 标记任务队列是否为空，从而判断是否需要唤醒线程
		// 用大括号的作用是缩短锁的作用域
		//std::lock_guard是一个RAII机制的锁，在构造时会自动加锁，当lock变量离开作用域后，它的析构函数会自动释放锁
    	{
    		std::lock_guard<std::mutex> lock(m_mutex);
    		// empty ->  all thread is idle -> need to be waken up
    		need_tickle = m_tasks.empty();
	        // 创建Task的任务对象
	        ScheduleTask task(fc, thread);
	        if (task.fiber || task.cb) // 存在就加入  
	        {
	            m_tasks.push_back(task);
	        }
    	}
    	
    	if(need_tickle) // 任务队列原本为空，但新任务来了 -> 唤醒线程
    	{
    		tickle();
    	}
    }
	
	// 启动线程池，启动调度器
	virtual void start();
	// 关闭线程池，停止调度器，等所有调度任务都执行完后再返回
	virtual void stop();	
	
protected:
	// 唤醒线程
	virtual void tickle();
	
	// 线程函数
	virtual void run();

	// 空闲协程函数，无任务调度时执行idle协程
	virtual void idle();
	
	// 是否可以关闭
	virtual bool stopping();

	// 返回是否有空闲线程
	// 当调度协程进入idle时空闲线程数+1，从idle协程返回时空闲线程数-1
	bool hasIdleThreads() {return m_idleThreadCount>0;}

private:
	// 任务结构体
	struct ScheduleTask
	{
		std::shared_ptr<Fiber> fiber;
		std::function<void()> cb;
		int thread; // 指定任务需要运行的线程ID

		ScheduleTask()
		{
			fiber = nullptr;
			cb = nullptr;
			thread = -1;
		}

		ScheduleTask(std::shared_ptr<Fiber> f, int thr)
		{
			fiber = f;
			thread = thr;
		}

		ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
		{
			fiber.swap(*f); //将内容转移也就是指针内部的转移和上面的赋值不同，引用计数不会增加
			thread = thr;
		}	

		ScheduleTask(std::function<void()> f, int thr)
		{
			cb = f;
			thread = thr;
		}		

		ScheduleTask(std::function<void()>* f, int thr)
		{
			cb.swap(*f); // 同理
			thread = thr;
		}

		void reset() // 重置
		{
			fiber = nullptr;
			cb = nullptr;
			thread = -1;
		}	
	};

private:
	// 调度器的名称
	std::string m_name;
	// 互斥锁 -> 保护任务队列
	std::mutex m_mutex;
	// 线程池，存储初始化好的线程
	std::vector<std::shared_ptr<Thread>> m_threads;
	// 任务队列
	std::vector<ScheduleTask> m_tasks;
	// 存储工作线程的线程ID
	std::vector<int> m_threadIds;
	// 需要额外创建的线程数
	size_t m_threadCount = 0;
	// 活跃线程数
	std::atomic<size_t> m_activeThreadCount = {0};
	// 空闲线程数
	std::atomic<size_t> m_idleThreadCount = {0};

	// 主线程是否用作工作线程
	bool m_useCaller;
	// 如果是 -> 需要额外创建调度协程
	std::shared_ptr<Fiber> m_schedulerFiber;
	// 如果是 -> 记录主线程的线程ID
	int m_rootThread = -1;
	// 是否正在关闭
	bool m_stopping = false;	
};

}

#endif