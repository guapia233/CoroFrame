## What’s inside
- 设计并实现了基于 ucontext_t 的非对称协程，定义了三种协程状态，实现了调度协程与任务协程的切换。
- 设计并实现了三类协程：主协程、调度协程和任务协程，降低不同功能之间的耦合。
- 开发了 N-M 协程调度器，使 main 函数线程能够参与调度，并结合 epoll 和定时器实现了 I/O 协程调度。
- 对 sleep、Socket IO、fd 等系统调用进行了 hook 封装，以达到异步非阻塞的效果。
- 基于 RAII 思想，封装了 pthread，实现了互斥量、信号量、读写锁等线程同步机制。
- 实现了基于时间堆的定时器功能，支持定时事件的管理。

## How to run

0. 运行环境：Ubuntu 20.04 LTS

1. 进入文件所在目录
```shell
cd coroutine-lib && cd fiber_lib && cd 6hook 
```

2. 在 6hook 文件下编译链接可执行文件
```shell
g++ *.cpp -std=c++17 -o main -ldl -lpthread
```

3. 执行可执行文件
```shell
./main
```
4. 测试工具可使用 ApacheBench
```shell
ab -n 1000 -c 10  http://127.0.0.1:8080/
```

ps：可以忽略 1thread ~ 5iomanager，统一在 6hook 中实现。