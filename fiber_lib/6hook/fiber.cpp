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

// 首先运行该函数初始化当前线程的协程功能，即创建主协程（内部调用了 Fiber 的无参构造函数）
std::shared_ptr<Fiber> Fiber::GetThis()
{	// 如果 t_fiber 已经被初始化，则直接返回正在运行的协程的自身指针 
	if(t_fiber)
	{	
		return t_fiber->shared_from_this(); // 安全地返回 this 指针
	}

	// 如果 t_fiber 尚未初始化，说明当前主协程尚未创建 
	// 这时需要创建一个新的 Fiber 对象充当主协程，并使用 shared_ptr 管理它
	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_thread_fiber = main_fiber;
	t_scheduler_fiber = main_fiber.get(); // 除非主动设置，否则主协程默认充当调度协程
	/**
	 * get() 返回的是 shared_ptr<Fiber> 所管理的原始指针，而不是 shared_ptr<Fiber> 本身
	 * 而 t_thread_fiber 的类型本来就是 shared_ptr<Fiber>，所以它可以直接赋值，不需要 get()
	 */
	
	// 断言检查：判断当前协程是否为主协程，是则继续执行，否则程序终止
	assert(t_fiber == main_fiber.get()); // 确保在没有其他协程运行时，主协程是唯一有效的协程 

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
Fiber::Fiber() // 定义为 private，只能被 GetThis()调用，不允许类外调用
{
	SetThis(this); // 将新创建的 fiber 对象设置为 t_fiber 
	m_state = RUNNING; // 设置协程的状态为运行（因为是主协程，直接跑起来即可）
	
	// 若返回值为0，表示 getcontext() 成功地保存了当前协程的上下文
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
	}
	
	m_id = s_fiber_id++; // 从0开始分配协程ID 
	s_fiber_count++; // 活跃的协程数量+1
	if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

// 创建一个新协程并指定其入口函数、栈的大小和是否受调度，初始化 ucontext_t上下文，分配栈空间，并通过 make 将上下文与入口函数绑定
// 当 set 或 swap 激活上下文 m_ctx 时，会执行该入口函数
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
	m_state = READY; // 初始化状态为就绪

	// 分配协程栈空间
	m_stacksize = stacksize ? stacksize : 128000;
	m_stack = malloc(m_stacksize);

	if(getcontext(&m_ctx)) // 存储协程上下文
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}
	
	m_ctx.uc_link = nullptr; // 因为没有设置后继，所以在运行完该协程入口函数后，协程退出并调用一次 yield 返回主协程 
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0); // 将上下文与入口函数绑定
	
	m_id = s_fiber_id++;
	s_fiber_count++;
	if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
	s_fiber_count--; // 活跃的协程数量-1
	if(m_stack) // 有独立栈，说明是子协程（关于主协程的析构还没实现）
	{
		free(m_stack);
	}
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}
 
// 复用一个已终止的协程对象：重置协程的入口函数，重新设置上下文，将协程状态从 TERM 改为 READY，从而避免频繁创建和销毁对象带来的开销
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

// 将协程的状态设置为 RUNNING，并恢复协程的执行
void Fiber::resume()
{
	assert(m_state==READY); // 使用断言确保当前协程的状态是 READY，即协程已经准备好执行
	
	m_state = RUNNING;

	// 如果 m_runInScheduler 为 true，说明该协程（即调用 resume 方法的协程）受调度协程的调度，则由调度协程 t_scheduler_fiber 切换到该协程恢复执行
	if(m_runInScheduler) 
	{
		SetThis(this); // 将该协程赋值给变量 t_fiber，方便输出调试信息

		// swapcontext 会保存当前协程的上下文 t_scheduler_fiber->m_ctx，并切换到指定协程的上下文 m_ctx 
		if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	// 如果 m_runInScheduler 为 false，说明该协程不受调度协程的调度，此时会将执行权从主协程 t_thread_fiber 切换到该协程
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

// 让出该协程的执行权
void Fiber::yield()
{
	assert(m_state==RUNNING || m_state==TERM);

	if(m_state != TERM)
	{
		m_state = READY;
	}

	if(m_runInScheduler)
	{
		SetThis(t_scheduler_fiber);   
		if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx))) // 将当前协程的执行权交给调度协程  
		{
			std::cerr << "yield() to to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	else
	{
		SetThis(t_thread_fiber.get());  
		if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) // 将当前协程的执行权交给主协程  
		{
			std::cerr << "yield() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}	
}

// 协程入口函数的真正执行地，在协程恢复执行时会调用这个函数
// 通过封装入口函数，可以实现协程在结束时自动执行 yield 操作
void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis(); // GetThis()的 shared_from_this 方法让引用计数+1
	assert(curr != nullptr);

	curr->m_cb(); // 执行协程的回调函数
	curr->m_cb = nullptr; // 表示协程不再需要执行回调函数，这样做是为了释放回调函数的资源，避免重复执行
	curr->m_state = TERM; // 表示协程已经执行完毕，生命周期结束

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get(); // 获取当前协程对象的裸指针
	curr.reset(); // 将入口函数 m_cb 指向 nullptr，以便其他线程可以再次复用这个协程对象
	raw_ptr->yield(); // 调用协程的 yield 函数，让出当前协程的执行权
}

}