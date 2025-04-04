#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory> // 智能指针头文件
#include <vector>
#include <set>
#include <shared_mutex> // 读写锁头文件
#include <assert.h> // 断言
#include <functional> // 函数对象
#include <mutex> // 互斥锁

namespace sylar {

class TimerManager; // 定时器管理类

/**
 * enable_shared_from_this<T> 是一个辅助基类，它可以让类T在成员函数中安全地获取指向自身的 shared_ptr
 * 在 shared_ptr 管理的对象内部，如果想要获取指向自身的 shared_ptr，不能直接用 std::shared_ptr<T>(this)
 * 因为这会创建一个新的 shared_ptr，导致两个 shared_ptr 共享同一个原始指针，可能会导致双重释放的问题
 * std::enable_shared_from_this<T> 解决了这个问题，它提供了 shared_from_this() 方法，可以安全地返回 shared_ptr<T>
 */
class Timer : public std::enable_shared_from_this<Timer> 
{
    friend class TimerManager; // TimerManager 作为友元类，意味着 TimerManager 可以访问 Timer 的私有成员
public:
    // 从时间堆中删除一个 Timer
    bool cancel();
    // 刷新 Timer
    bool refresh();
    // 重设 Timer 的超时时间，参1 ms 指定时器执行间隔时间，参2 from now 指是否从当前时间开始计算
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);
 
private:
    // 是否循环
    bool m_recurring = false;
    // 相对超时时间
    uint64_t m_ms = 0;
    // 绝对超时时间，即该定时器下一次触发的时间点
    std::chrono::time_point<std::chrono::system_clock> m_next;
    // 超时时触发的回调函数
    std::function<void()> m_cb;
    // 管理该 Timer 的管理器
    TimerManager* m_manager = nullptr;

private:
    // 最小堆的比较函数，用于比较两个 Timer 对象，比较的依据是绝对超时时间
    struct Comparator 
    {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager 
{
    friend class Timer;
public:
    TimerManager();  
    virtual ~TimerManager();

    // 添加 Timer：参1 ms指定时器执行间隔时间，参2 cb指定时器回调函数，参3 recurring指是否循环定时器
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件 Timer
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    // 获取堆中最近的超时时间
    uint64_t getNextTimer();

    // 获取所有超时定时器的回调函数
    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    // 堆中是否有定时器，即判断 set 是否为空
    bool hasTimer();

protected:
    // 当一个最早的 Timer 加入到堆中（即该 Timer 加入了堆顶） -> 调用该函数
    virtual void onTimerInsertedAtFront() {};

    // 添加 Timer
    void addTimer(std::shared_ptr<Timer> timer);

private:
    // 当系统时间改变时 -> 调用该函数
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex; // 读写锁

    // 时间堆：存储所有的 Timer 对象，并使用 Timer::Comparator 进行排序，确保最早超时的 Timer 在最前面（堆顶）
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

    // 在下次 getNextTime()执行前，onTimerInsertedAtFront()是否已经被触发了 --> 在此过程中，onTimerInsertedAtFront()只执行一次，防止重复调用
    // m_tickled 是一个标志，用于指示是否需要在定时器插入到时间堆的前端时触发额外的处理操作，例如唤醒一个等待的线程或进行其他管理操作
    bool m_tickled = false;

    // 上一次检查定时器管理器时的系统绝对时间，用于检测系统时间是否发生了回退
    std::chrono::time_point<std::chrono::system_clock> m_previouseTime;
};

}

#endif