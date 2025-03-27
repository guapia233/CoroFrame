#include "fiber.h"

static bool debug = false;

namespace sylar {

// 当前线程上的协程控制信息

// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;


static std::atomic<uint64_t> s_fiber_id{0}; // 全局协程ID计数器
static std::atomic<uint64_t> s_fiber_count{0}; // 活跃协程计数器

// 设置当前运行的协程
void Fiber::SetThis(Fiber *f)
{
	t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{	// 如果t_fiber已经被初始化，则直接返回当前协程的shared_ptr，作为主协程
	if(t_fiber)
	{	
		return t_fiber->shared_from_this();
	}
	// 如果t_fiber尚未初始化，说明当前没有主协程，或者主协程尚未创建 
	// 在这种情况下，创建一个新的Fiber对象，并使用std::shared_ptr管理它，这个新的Fiber对象就是主协程
	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_thread_fiber = main_fiber;
	t_scheduler_fiber = main_fiber.get(); // 除非主动设置,否则主协程默认为调度协程
	
	// 用于判断当前协程是否为主协程，是则继续执行，否则程序终止
	assert(t_fiber == main_fiber.get()); // 断言检查的目的是确保在没有其他协程运行时，主协程是唯一有效的协程 
	return t_fiber->shared_from_this();
}

// 设置调度协程
void Fiber::SetSchedulerFiber(Fiber* f)
{
	t_scheduler_fiber = f;
}

// 获取当前运行的协程ID 
uint64_t Fiber::GetFiberId()
{
	if(t_fiber)
	{
		return t_fiber->getId();
	}
	return (uint64_t)-1; // 返回-1，并且是(Uint64_t)-1，会转换成UINT64_max，用来表示错误的情况
}

// 创建主协程，设置状态，初始化上下文，并分配ID 
Fiber::Fiber() // 只能被GetThis()调用，不允许类外部调用，因为定义为private
{
	SetThis(this); // 在GetThis中使用了无参的Fiber来构造t_fiber
	m_state = RUNNING; // 设置协程的状态为可运行
	
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
	}
	
	m_id = s_fiber_id++; // 分配协程ID，从0开始，用完+1
	s_fiber_count ++; // 活跃的协程数量+1
	if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

// 创建一个新协程，初始化回调函数、栈的大小和状态，分配栈空间，并通过make修改上下文
// 当set或swap激活上下文m_ctx时，会执行make的参2：MainFunc函数
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
	m_state = READY; // 初始化状态

	// 分配协程栈空间
	m_stacksize = stacksize ? stacksize : 128000;
	m_stack = malloc(m_stacksize);

	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}
	
	m_ctx.uc_link = nullptr; // 这里因为没有设置后继，所以在运行完mainfunc后协程退出，会调用一次yield返回主协程 
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
	s_fiber_count--; // 活跃的协程数量-1
	if(m_stack)
	{
		free(m_stack);
	}
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}
 
// 重置协程的回调函数，并重新设置上下文，将协程从TERM状态重置为READY、
// 目的：复用一个已经终止的协程对象，从而避免频繁创建和销毁对象带来的开销
void Fiber::reset(std::function<void()> cb)
{
	assert(m_stack != nullptr && m_state == TERM); 

	m_state = READY;
	m_cb = cb;

	if(getcontext(&m_ctx))
	{
		std::cerr << "reset() failed\n";
		pthread_exit(NULL);
	}

	m_ctx.uc_link = nullptr;
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

// 将协程的状态设置为running，并恢复协程的执行
// 如果m_runInScheduler为true，则将上下文切换到调度协程；否则，切换到主线程的协程
void Fiber::resume()
{
	assert(m_state==READY); // 使用断言确保当前协程的状态是READY，即协程已经准备好执行
	
	m_state = RUNNING;

	// 下面的切换类似于非对称的协程切换模型，即一个协程的执行会交给另一个协程来处理 

	// 如果m_runInScheduler为true，说明当前协程参与调度器的调度 
	if(m_runInScheduler) 
	{
		SetThis(this); // 将该协程设置为"当前执行的协程"
		// 下面是进行上下文切换的关键操作，swapcontext会保存当前协程的上下文(m_ctx)并切换到指定协程的
		// 上下文(t_scheduler_fiber->m_ctx)。如果 m_runInScheduler为true，则切换到调度协程(t_scheduler_fiber)
		if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	// 如果 m_runInScheduler 为false，说明当前协程不参与调度器的调度，而是由主线程管理，此时会将执行权切换到主线程的协程(t_thread_fiber)
	else
	{
		SetThis(this);
		if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}
}

// 让出协程的执行权
void Fiber::yield()
{
	assert(m_state==RUNNING || m_state==TERM);

	if(m_state != TERM)
	{
		m_state = READY;
	}

	if(m_runInScheduler)
	{
		SetThis(t_scheduler_fiber); // 将当前协程设置为调度协程，这样后续操作会使用调度协程的上下文 
		if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx))) // 将当前协程的执行权交给调度协程  
		{
			std::cerr << "yield() to to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	else
	{
		SetThis(t_thread_fiber.get()); // 将当前协程设置为主协程 
		if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) // 将当前协程的执行权交给主协程  
		{
			std::cerr << "yield() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}	
}

// 协程的入口函数，在协程恢复执行时会调用这个函数
// 通过封装入口函数，可以实现协程在结束时自动执行yield操作
void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis(); // GetThis()的shared_from_this方法让引用计数加1
	assert(curr != nullptr);

	curr->m_cb(); // 执行协程的回调函数
	curr->m_cb = nullptr; // 表示协程不再需要执行回调函数，这样做是为了释放回调函数的资源，避免重复执行
	curr->m_state = TERM; // 表示协程已经执行完毕，生命周期结束

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get(); // 获取当前协程的裸指针
	curr.reset(); // 这里留意一个细节：重置的cb回调函数希望它指向nullptr，因为方便其他线程再次调用这个协程对象
	raw_ptr->yield(); // 调用协程的yield函数，让出当前协程的执行权
}

}