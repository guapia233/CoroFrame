#include "scheduler.h"

static bool debug = false;

namespace sylar {

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
	assert(threads>0 && Scheduler::GetThis()==nullptr); // 首先判断线程的数量是否大于0，并且调度器的对象是否是空指针，是就调用setThis()进行设置

	SetThis(); // 设置当前调度器对象

	Thread::SetName(m_name); //设置当前线程的名称为调度器的名称

	// 判断是否让主线程作为工作线程
	// 使用主线程当作工作线程，创建协程的主要原因是为了实现更高效的任务调度和管理
	if(use_caller) // 如果use_caller为true，表示当前线程也要作为一个工作线程使用
	{
		threads--; // 因为此时作为了工作线程，所以还需要的线程数量-1（因为这个就已经作为一个工作线程了）

		// 创建主协程
		Fiber::GetThis();

		// 创建调度协程
		m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false表示该调度协程退出后将返回主协程
		Fiber::SetSchedulerFiber(m_schedulerFiber.get()); //设置协程的调度器对象
		
		m_rootThread = Thread::GetThreadId(); // 获取主线程ID
		m_threadIds.push_back(m_rootThread);
	}

	m_threadCount = threads; // 将剩余的线程数量(即总线程数量减去是否使用调用者线程)赋值给m_threadcount
	if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler()
{
	assert(stopping()==true); // 判读调度器是否终止
	if (GetThis() == this) // 获取调度器对象
	{
        t_scheduler = nullptr; // 将其设置为nullptr防止悬空指针
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

void Scheduler::start()
{
	std::lock_guard<std::mutex> lock(m_mutex); // 互斥锁防止共享资源的竞争
	if(m_stopping) // 如果调度器退出直接报错打印cerr后面的话
	{
		std::cerr << "Scheduler is stopped" << std::endl;
		return;
	}

	assert(m_threads.empty()); // 首先线程池数量必须为空
	m_threads.resize(m_threadCount); // 将线程池的数量重置成需要额外创建的线程数
	for(size_t i = 0; i < m_threadCount; i++)
	{
		m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
		m_threadIds.push_back(m_threads[i]->getId());
	}
	if(debug) std::cout << "Scheduler::start() success\n";
}

// 作用:调度器的核心，负责从任务队列中取出任务并通过协程执行
void Scheduler::run()
{
	int thread_id = Thread::GetThreadId(); // 获取当前线程的ID
	if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;
	
	//set_hook_enable(true);

	SetThis(); // 设置调度器对象

	if(thread_id != m_rootThread) // 如果不是主线程，而是新创建的线程，创建主协程
	{
		Fiber::GetThis(); // 分配了线程的主协程和调度协程
	}
	// 创建空闲协程，std::make_shared 是C++11引入的一个函数，用于创建 std::shared_ptr 对象
	// 相比于直接使用 std::shared ptr 构造函数，std::make shared 更高效且更安全
	// 因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作
	std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this)); // 子协程
	ScheduleTask task;
	
	while(true)
	{
		task.reset();
		bool tickle_me = false; // 是否要唤醒其他线程进行任务调度
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_tasks.begin();
			// 1.遍历任务队列
			while(it != m_tasks.end())
			{
				if(it->thread != -1 && it->thread != thread_id) // 如果任务指定了特定的线程，但不是当前线程，就跳过（tickle 其他线程）
				{
					it++;
					tickle_me = true;
					continue;
				}

				// 2.取出任务
				assert(it->fiber||it->cb);
				task = *it;
				m_tasks.erase(it); 
				m_activeThreadCount++;
				break; // 这里取到任务的线程就直接break所以并没有遍历到队尾
			}	
			tickle_me = tickle_me || (it != m_tasks.end());
		}

		if(tickle_me) // 这里虽然写了唤醒但并没有具体的逻辑代码，具体的在io+scheduler
		{
			tickle();
		}

		// 3.执行任务
		if(task.fiber)
		{   // resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1 
			{					
				std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
				if(task.fiber->getState() != Fiber::TERM)
				{
					task.fiber->resume();	
				}
			}
			m_activeThreadCount--; // 线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数-1
			task.reset();
		}
		else if(task.cb)
		{   // 上面解释过对于函数也应该被调度，具体做法就封装成协程加入调度
			std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
			{
				std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
				cb_fiber->resume();			
			}
			m_activeThreadCount--;
			task.reset();	
		}
		// 4.无任务 -> 执行空闲协程
		else
		{		
			// 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if (idle_fiber->getState() == Fiber::TERM) 
            {	
            	if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
			// 当任务队列为空时，调度器会进入 idle 协程，不断进行 resume/yield，形成“忙等待”
			// idle 协程不会主动结束，只有当调度器被显式停止时才会退出
			// 这样可以确保调度器始终保持活跃，并在有新任务时立即恢复执行，而不会因为 sleep 而有延迟
			// 在这里 idle fiber 就是不断的和调度协程进行交互的子协程 
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

    if (m_useCaller) 
    {
        assert(GetThis() == this);
    } 
    else 
    {
        assert(GetThis() != this);
    }
	
	// 调用tickle()的目的是唤醒空闲线程或协程，防止m_scheduler或其他线程处于永久阻塞在等待任务的状态中
	for (size_t i = 0; i < m_threadCount; i++) 
	{
		tickle(); // 唤醒空闲线程
	}

	if (m_schedulerFiber) 
	{
		tickle(); // 唤醒可能处于挂起状态，等待下一个任务的调度的协程
	}

	// 当只有主线程或调度线程作为工作线程的情况，只能从stop()方法开始任务调度
	if (m_schedulerFiber)
	{
		m_schedulerFiber->resume(); // 开始任务调度
		if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
	}

	// 获取此时的线程通过swap不会增加引用计数的方式加入到thrs，方便下面的join保持线程正常退出
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
		sleep(1); // 降低空闲协程在无任务时对cpu的占用率，避免空转浪费资源 
		Fiber::GetThis()->yield();
	}
}

// 该函数的目的是为了判断调度器是否退出，在stop函数中如果stopping()返回为true代表调度器已经退出，则直接返回return，不做任何操作
bool Scheduler::stopping() 
{
	// 使用互斥锁的目的是m_tasks，m_activeThreadCount会被多线程竞争所以需要互斥锁来保护资源的访问
	// std::lock_guard<std::mutex> 是 C++ 标准库提供的一个互斥锁（mutex）的管理类
	// 只需要一句就能自动加锁和解锁，因为它遵循 RAII 机制：
	// 作用域开始时，自动 lock()，作用域结束时（函数返回、异常退出等），自动 unlock()
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}


}