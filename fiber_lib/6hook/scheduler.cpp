#include "scheduler.h"

static bool debug = false;

namespace sylar {

// t_scheduler 是 thread_local 变量，每个线程都有自己独立的 t_scheduler 指针，指向同一个 Scheduler 
static thread_local Scheduler* t_scheduler = nullptr;

Scheduler* Scheduler::GetThis()
{
	return t_scheduler;
}

void Scheduler::SetThis()
{
	t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name):
m_useCaller(use_caller), m_name(name)
{
	// 断言判断要创建的线程数量是否大于0，并且调度器的对象是否是空指针，是就调用 setThis()进行设置
	assert(threads > 0 && Scheduler::GetThis() == nullptr); 

	SetThis(); // 设置当前调度器对象

	Thread::SetName(m_name); // 设置当前线程的名称为调度器的名称

	// 判断是否让主线程作为工作线程
	// 使用主线程充当工作线程，创建协程的主要原因是为了实现更高效的任务调度和管理
	if(use_caller) // 如果 use_caller 为 true，表示主线程也要作为一个工作线程使用
	{
		threads--; // 因为主线程充当了工作线程，所以还需要的线程数量-1（因为这个就已经算一个工作线程了）

		// 创建主协程
		Fiber::GetThis();

		// 创建调度协程，参3 false 表示调度协程不受调度器的调度，即退出后将切换到主协程
		m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));  
		Fiber::SetSchedulerFiber(m_schedulerFiber.get()); //设置调度协程
		
		m_rootThread = Thread::GetThreadId(); // 获取主线程ID
		m_threadIds.push_back(m_rootThread); // 将主线程加入存储工作线程的数组
	}

	m_threadCount = threads; // 将需要创建的线程数量赋值给 m_threadcount
	if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler()
{
	assert(stopping() == true); // 断言判断调度器是否终止
	if(GetThis() == this) // 获取调度器对象
	{
        t_scheduler = nullptr; // 将其设置为 nullptr 防止悬空指针
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

void Scheduler::start()
{
	std::lock_guard<std::mutex> lock(m_mutex);   
	if(m_stopping) // 如果调度器退出直接打印报错
	{
		std::cerr << "Scheduler is stopped" << std::endl;
		return;
	}

	assert(m_threads.empty()); // 首先线程池数量必须为空
	m_threads.resize(m_threadCount); // 将线程池的容量设置为需要额外创建的线程数
	for(size_t i = 0; i < m_threadCount; i++)
	{
		m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
		m_threadIds.push_back(m_threads[i]->getId());
	}
	if(debug) std::cout << "Scheduler::start() success\n";
}

// 调度器的核心，负责从任务队列中取出任务并通过协程执行
void Scheduler::run()
{
	int thread_id = Thread::GetThreadId(); // 获取当前线程的ID
	if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;
	
	// set_hook_enable(true);

	SetThis(); // 设置调度器对象

	if(thread_id != m_rootThread) // 如果不是主线程，而是新创建的线程，就创建主协程
	{
		Fiber::GetThis(); // 分配了线程的主协程和调度协程（默认是同一个）
	}

	/**
	 * 创建空闲协程，make_shared 是 C++11 引入的一个函数，用于高效创建 shared_ptr 对象
	   相比于直接使用 shared_ptr 构造函数，make_shared 更高效且更安全
	   因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作
	 */
	std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this)); // 子协程
	ScheduleTask task; // 创建任务对象
	
	while(true)
	{
		task.reset(); 
		bool tickle_me = false; // 是否要唤醒其他线程进行任务调度
		{
			std::lock_guard<std::mutex> lock(m_mutex); // 互斥访问任务队列
			auto it = m_tasks.begin();
			// 1.遍历任务队列
			while(it != m_tasks.end())
			{
				// 如果任务指定了特定的线程执行且不是当前线程，就跳过该线程，唤醒其他线程
				if(it->thread != -1 && it->thread != thread_id) 
				{
					it++;
					tickle_me = true;
					continue;
				}

				// 2.取出任务
				assert(it->fiber || it->cb);
				task = *it;
				m_tasks.erase(it); 
				m_activeThreadCount++;
				break; // 取到任务的线程就直接 break，因此没有遍历到队尾，说明还有剩余任务
			}	
			tickle_me = tickle_me || (it != m_tasks.end());
		}

		if(tickle_me) // 具体的唤醒代码在 ioscheduler.cpp
		{
			tickle();
		}

		// 3.执行任务
		if(task.fiber) // 如果任务对象是协程
		{   // 任务协程调用 resume 将执行权从调度协程切换到任务协程 
			{					
				std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
				if(task.fiber->getState() != Fiber::TERM)
				{
					task.fiber->resume();	
				}
			}
			// resume 返回时此时任务要么执行完了，要么半路 yield 了，总之任务完成了，活跃线程-1
			m_activeThreadCount--; // 线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此将活跃线程数-1
			task.reset();
		}
		else if(task.cb) // 如果任务对象是函数，之前解释过函数也应该被调度，具体做法就是先封装成协程再执行
		{   
			std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
			{
				std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
				cb_fiber->resume();			
			}
			m_activeThreadCount--;
			task.reset();	
		}
		// 4.当前无任务，就执行空闲协程
		else
		{		
			// 系统关闭 -> idle 协程将从死循环跳出并结束 -> 此时的 idle 协程状态为 TERM -> 再次进入将跳出循环并退出 run()
            if(idle_fiber->getState() == Fiber::TERM) 
            {	
            	if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
			/**
			 * 当任务队列为空时，调度器会进入 idle 协程，不断进行 resume/yield，形成“忙等待”
			   idle 协程不会主动结束，只有当调度器被显式停止时才会退出
			   这样可以确保调度器始终保持活跃，并在有新任务时立即恢复执行，不会因为 sleep 而有延迟
			   在这里 idle_fiber 就是不断的和调度协程进行交互的子协程
			 */
			m_idleThreadCount++;
			idle_fiber->resume();				
			m_idleThreadCount--;
		}
	}
	
}

void Scheduler::stop()
{
	if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
	
	if(stopping())
	{
		return ;
	}

	m_stopping = true;	

    if(m_useCaller) 
    {
        assert(GetThis() == this);
    } 
    else 
    {
        assert(GetThis() != this);
    }
	
	// 调用 tickle()的目的是唤醒空闲线程或协程，防止 m_scheduler 或其他线程处于永久阻塞在等待任务的状态中
	for(size_t i = 0; i < m_threadCount; i++)      
	{
		tickle(); // 唤醒空闲线程 
	} 

	if(m_schedulerFiber) 
	{
		tickle(); // 唤醒可能处于挂起状态，等待下一个任务的调度协程
	}

	// 当只有主线程作为工作线程时，只能从 stop()方法开始任务调度
	if(m_schedulerFiber)
	{
		m_schedulerFiber->resume(); // 开始任务调度
		if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
	}

	// 获取线程池中的线程，通过 swap 不增加引用计数的方式加入到 thrs 中，方便下面的 join 保证线程正常退出
	std::vector<std::shared_ptr<Thread>> thrs;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		thrs.swap(m_threads);
	}

	for(auto &i : thrs)
	{
		i->join();
	}
	if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
}

void Scheduler::tickle()
{
}

void Scheduler::idle()
{
	while(!stopping())
	{
		if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
		sleep(1); // 降低空闲协程在无任务时对 CPU 的占用率，避免空转浪费资源 
		Fiber::GetThis()->yield();
	}
}

// 判断调度器是否退出，在 stop 函数中如果 stopping 函数返回为 true，代表调度器已经退出，则直接返回 return，不做任何操作
bool Scheduler::stopping() 
{
	/**
	 * 使用互斥锁的目的是 m_tasks 和 m_activeThreadCount 会被多线程竞争，所以需要互斥锁来保护资源的互斥访问
	   std::lock_guard<std::mutex> 是 C++11 标准库提供的互斥锁 RAII 封装工具，用于实现互斥访问 
	   作用域开始时，自动 lock()，作用域结束时（函数返回、异常退出等），自动 unlock()
	 */
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

}