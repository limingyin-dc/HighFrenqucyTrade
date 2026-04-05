# C++ 特性使用说明

## C++11

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `std::atomic<T>` | `future_struct.h` / `OrderManager.h` / `RiskManager.h` / `spsc.h` / `mpsc.h` | 原子变量，无锁并发基础 |
| `memory_order` | 所有原子操作 | `relaxed` / `acquire` / `release` / `acq_rel`，精确控制多线程可见性，避免 `seq_cst` 的全局屏障开销 |
| `compare_exchange_strong/weak` | `OrderManager.cpp` / `RiskManager.cpp` / `future_struct.h` | CAS 原语，对应 x86 `CMPXCHG` 指令；`weak` 用于循环（ARM LL/SC 友好），`strong` 用于单次占槽 |
| `fetch_add` | `mpsc.h` / `TdEngine.cpp` | 对应 x86 `XADD` 指令，MPSC 队列多生产者原子抢占槽位 |
| `std::thread` | `Logger.h` / `TdEngine.h` / `Strategy.cpp` | flush 线程、撤单守护线程、策略线程 |
| `std::chrono` | `OrderManager.h` / `RiskManager.cpp` / `Strategy.cpp` | 报单时间戳、时间轮桶计算、延迟统计 |
| `alignas(64)` | `OrderSlot` / `AccountMetrics` / `PositionMetrics` / `TickSlot` / MPSC `Slot` | cache line 对齐，防止 false sharing |
| `static_assert` | `spsc.h` / `mpsc.h` | 编译期强制队列 Size 为 2 的幂 |
| `constexpr` | `OrderState` / `PriceUtil::SCALE` / `MAX_ORDERS` / `TICK_POOL_SIZE` | 编译期常量，零运行时开销 |
| `std::array` | `OrderManager` / `Strategy` / `TickPool` | 固定大小数组，栈分配，无堆开销 |
| Lambda | `TdEngine.cpp` / `Strategy.cpp` | `ForEachTimeout` 回调、策略线程启动 |
| Magic Static | `Logger.cpp` `getInstance()` | 局部静态变量线程安全初始化，无需手写 double-checked locking |
| `nullptr` | 全局 | 替代 `NULL`，类型安全 |
| `auto` | `TdEngine.cpp` / `Strategy.cpp` | 类型推导，减少冗余 |
| `using` 类型别名 | `Logger.h` | `using LogQueue = MPSCQueue<LogEntry, 8192>` |
| `va_list` / `vsnprintf` | `Logger.cpp` | 可变参数日志格式化 |

---

## C++17

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `[[nodiscard]]` | `OrderManager::AllocSlot` / `TickPool::Write` | 返回值不能被忽略，编译器发出警告，防止槽位泄漏 |
| `inline` 变量 | `TickPool.h` 的 `inline TickPool g_tick_pool` | 全局变量定义在头文件中，避免多编译单元重复定义链接错误 |

---

## C++20

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `[[likely]]` / `[[unlikely]]` | `RiskManager.cpp` | 分支预测标注，编译器将 likely 分支放在 fall-through 路径，减少 branch misprediction penalty（约 10~20 个时钟周期） |

> 注意：GCC 11 不支持 `if ([[unlikely]] expr)` 的条件内写法，`Strategy.cpp` 改用等价的 GCC 扩展 `__builtin_expect`。

---

## GCC 扩展（非标准）

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `__builtin_expect(expr, 0/1)` | `Strategy.cpp` | 热路径分支预测，GCC/Clang 均支持，C++17 下 `[[likely]]` 的替代方案 |
| `pthread_setaffinity_np` | `Strategy.cpp` | CPU 绑核，POSIX 扩展，将策略线程固定到指定核心 |
