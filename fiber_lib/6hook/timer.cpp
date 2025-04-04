#include "timer.h"

namespace sylar {

// 取消一个定时器，删除该定时器的回调函数并将其从定时器堆中移除
bool Timer::cancel() 
{
/**
 * std::shared_mutex 是 C++17 引入的一种读写锁，支持：
 *      1.共享锁（std::shared_lock）：多个线程可以同时读取，但不能写入 
 *      2.独占锁（std::unique_lock）：只能有一个线程写入，其他线程必须等待 
 * unique_lock / shared_lock / lock_guard 都是标准库提供的互斥锁 RAII 封装工具
 *      unique_lock 用于获取 shared_mutex 的写锁，它会独占 shared_mutex，让当前线程获得写权限，其他线程无法同时写入 
 *      shared_lock 用于获取 shared_mutex 的读锁，它会共享 shared_mutex，让多个线程获得读权限，支持多个线程同时读取
 *      相比于 lock_guard，unique_lock 更灵活，支持手动解锁  
 */
    
    // m_manager->m_mutex 是 shared_mutex 类型，支持读写锁（共享锁和独占锁）
    // 使用 unique_lock 获取 m_manager->m_mutex 的独占写锁，确保当前线程可以独占访问 m_manager 保护的资源，而其他线程必须等待
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex); // 写锁互斥锁

    if(m_cb == nullptr) 
    {
        return false;
    }
    else
    {
        m_cb = nullptr; // 将回调函数设置为 nullptr
    }

    auto it = m_manager->m_timers.find(shared_from_this()); // 从定时管理器中找到需要删除的定时器
    if(it != m_manager->m_timers.end())
    {
        m_manager->m_timers.erase(it); // 删除该定时器
    }
    return true;
}

// 刷新定时器超时时间，这个刷新操作会将定时器的下次触发延后 
bool Timer::refresh() 
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex); // 触发读写锁的写锁，独占写

    if(!m_cb) 
    {
        return false;
    }

    auto it = m_manager->m_timers.find(shared_from_this()); // 在定时器集合中查找当前定时器
    if(it == m_manager->m_timers.end())  
    {
        return false;
    }

    // 删除当前定时器并更新超时时间
    m_manager->m_timers.erase(it);
    
    // std::chrono::system_clock::now() 是用来获取当前系统时间的标准方法，返回的是系统绝对时间，通常用于记录当前的实际时间点
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);

    m_manager->m_timers.insert(shared_from_this()); // 将新的定时器加入到定时器管理类中
    
    return true;
}

// 重置定时器的超时时间，可以选择从当前时间或上次超时时间开始计算超时时间 
bool Timer::reset(uint64_t ms, bool from_now) 
{   // 如果新超时时间 ms 与原来的超时时间 m_ms 相同，并且 from_now == false，说明不需要变更超时时间
    if(ms == m_ms && !from_now) 
    {
        return true; // 表示无需重置
    }
    // 如果不满足上面的条件需要重置，删除当前的定时器然后重新计算超时时间并插入定时器
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex); // 独占写锁
    
        if(!m_cb) // 如果回调函数为空，说明该定时器已被取消或未初始化，因此无法重置
        {
            return false;
        }
        
        auto it = m_manager->m_timers.find(shared_from_this()); // 寻找该定时器
        if(it==m_manager->m_timers.end())  
        {
            return false;
        }   
        m_manager->m_timers.erase(it); // 删除该定时器
    }

    // 如果 from_now 为 true 则从当前时间开始计算超时时间，为 false 就从上一次的起点开始计算超时时间
    // 这里 m_next 是当前定时器的下一次绝对触发时间点，减去相对超时时间 m_ms，相当于计算出上一次的起点时间点 
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);
    m_manager->addTimer(shared_from_this()); // 重新插入该定时器

    return true;
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager) 
{
    auto now = std::chrono::system_clock::now();  
    m_next = now + std::chrono::milliseconds(m_ms); // 绝对超时时间，即该定时器下一次触发的时间点
}

// 自定义比较器
bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const
{
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;
}

TimerManager::TimerManager() 
{
    m_previouseTime = std::chrono::system_clock::now(); // 初始化当前系统事件，为后续检查系统时间错误进行校对
}

TimerManager::~TimerManager() 
{
}

/**
 * 将一个新定时器，添加到定时器管理器中，并在必要时唤醒线程
 * 详细来说：epoll_wait 不只负责监听 I/O 事件，还负责等待定时器 
 * epoll_wait 是“带超时阻塞”的，并且是按最早超时定时器设置的 timeout，如果你加入了一个更早的定时器，
 * 必须唤醒 epoll_wait，重新设置它的 timeout，否则会错过定时器的触发时机 
 */
std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) 
{
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);
    return timer;
}
void TimerManager::addTimer(std::shared_ptr<Timer> timer)
{
    bool at_front = false;
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex); // 独占写锁

        // std::pair<iterator, bool> insert(const value_type& val);
        // insert 返回值为 pair，取 first 是为了获取迭代器
        auto it = m_timers.insert(timer).first;

        // 检查新插入的定时器是否排在最前面（即下一个要触发的定时器）
        at_front = (it == m_timers.begin()) && !m_tickled;
        
        // 如果排在最前面，并且没有唤醒过调度线程
        if(at_front)
        {
            m_tickled = true; // 标记已经触发过唤醒，避免重复唤醒，提高性能
        }
    }
   
    if(at_front)
    {
        // 唤醒 epoll_wait 或 IO 调度器中的线程
        onTimerInsertedAtFront();
    }
}

/**
 * 为什么需要用 weak ptr 转换成 share ptr？
 * weak_ptr是一种弱引用，不会增加对象的引用计数，通过 weak_ptr 可以持有
 * 对一个对象的非所有权引用，这在避免循环引用和管理对象生命周期时非常有用 
 */
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) // 如果条件存在 -> 执行cb()
{
    std::shared_ptr<void> tmp = weak_cond.lock(); // 确保当前条件的对象仍然存在
    if(tmp)
    {
        cb();
    }
}

// 添加一个条件定时器，并在定时器触发执行 cb 的时候调用 Ontimer，在 Ontimer 中真正执行回调函数
std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) 
{
    // 将 OnTimer 的真正指向交给了 addtimer，然后创建 timer 对象
    // bind 将 OnTimer / weak_cond / cb 三者绑定起来，返回一个新的函数对象
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring); 
}

// 获取定时器管理器中下一个定时器的超时时间 
uint64_t TimerManager::getNextTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex); // shared_lock 获取 shared_mutex 的读锁
    
    // 重置 m_tickled，目的是如果插入的定时器位于堆顶，能正常触发 at_fornt
    m_tickled = false;
    
    if(m_timers.empty())
    {
        // 返回最大值
        return ~0ull;
    }

    auto now = std::chrono::system_clock::now(); // 获取当前系统绝对时间
    auto time = (*m_timers.begin())->m_next; // 获取最小堆中的第一个定时器的绝对超时时间 

    // 判断当前时间是否已经超过了下一个定时器的超时时间
    if(now >= time)
    {
        // 已经有 timer 超时
        return 0;
    }
    else
    {
        // 计算从当前时间到下一个定时器超时时间的时间差，返回一个 std::chrono::milliseconds 对象
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
        return static_cast<uint64_t>(duration.count()); // 将时间差转换为毫秒并返回     
    }  
}

// 处理所有已经超时的定时器，并将它们的回调函数存到 cbs 向量中，该函数还会处理定时器的循环逻辑
void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
{
    auto now = std::chrono::system_clock::now();

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);  // 写锁

    bool rollover = detectClockRollover(); // 判断是否出现系统时间错误
    
    // 回滚 -> 清理所有timer || 存在超时定时器 -> 清理超时timer 
    while (!m_timers.empty() && rollover || !m_timers.empty() && (*m_timers.begin())->m_next <= now)
    {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());
        
        cbs.push_back(temp->m_cb); 

        // 如果该定时器是循环的，则将 m_next 设置为当前时间加上定时器的相对超时时间 m_ms，然后重新插入到定时器集合中
        if(temp->m_recurring)
        {
            // 重新加入时间堆
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else
        {
            // 将回调函数置空
            temp->m_cb = nullptr;
        }
    }
}

// 检查时间堆是否为空
bool TimerManager::hasTimer() 
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex); // 读锁
    return !m_timers.empty();
}

// 检测系统时间是否发生了回滚，即时间是否倒退
bool TimerManager::detectClockRollover() 
{
    bool rollover = false;

    // 比较当前时间 now 与上次记录的时间 m_previouseTime 减去一个小时的时间量（60 *60* 1000 毫秒）
    
    auto now = std::chrono::system_clock::now();
    if(now < (m_previouseTime - std::chrono::milliseconds(60 * 60 * 1000)))  
    {
        rollover = true; // 如果当前时间 now 小于这个时间值，说明系统时间回滚了
    }
    m_previouseTime = now;

    return rollover;
}

}

