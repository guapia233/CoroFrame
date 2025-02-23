/*
在scheduler类中，一开始是主协程和调度协程(也可以理解为特殊的子协程)在互相切换 
因为当下没有任务，resume走不到true的情况，而当有任务来的时候，子协程调用的resume可以进入true的判断
那么SetThis也就是子协程了，然后记录调度协程的上下文(方便yield的时候切回去)，再运行子协程上下文，执行入口函数就可以了 
所以简单来说:调度协程相当于子协程找到主协程的代理人，没事的时候就是主协程和调度协程玩，等到有任务了就让调度协程和子协程玩 
*/
#include "scheduler.h"

using namespace sylar;

static unsigned int test_number;
std::mutex mutex_cout;
void task()
{
	{
		std::lock_guard<std::mutex> lock(mutex_cout);
		std::cout << "task " << test_number ++ << " is under processing in thread: " << Thread::GetThreadId() << std::endl;		
	}
	sleep(1);
}

int main(int argc, char const *argv[])
{
	{
		// 可以尝试把false 变为true 此时调度器所在线程也将加入工作线程
		std::shared_ptr<Scheduler> scheduler = std::make_shared<Scheduler>(3, true, "scheduler_1");
		
		scheduler->start();

		sleep(2);

		std::cout << "\nbegin post\n\n"; 
		for(int i=0;i<5;i++)
		{
			std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
			scheduler->scheduleLock(fiber);
		}

		sleep(6);

		std::cout << "\npost again\n\n"; 
		for(int i=0;i<15;i++)
		{
			std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
			scheduler->scheduleLock(fiber);
		}		

		sleep(3);
		// scheduler如果有设置将加入工作处理
		scheduler->stop();
	}
	return 0;
}