# C++ 知识点总结

本文档统计本高频交易系统中用到的所有 C++ 知识点，按类别整理。

---

## 一、内存模型与原子操作

### 1.1 std::atomic<T>
用于多线程间无锁共享数据，避免互斥锁开销。

```cpp
std::atomic<int>    m_write_seq{0};   // TickPool 写入序号
std::atomic<double> m_daily_loss{0.0}; // 日内亏损
std::atomic<bool>   m_running{false};  // Logger 运行标志
```

支持的类型：整数、指针、bool、double（需要平台支持）。

### 1.2 memory_order 内存序

| 内存序 | 含义 | 使用场景 |
|--------|------|----------|
| `relaxed` | 只保证原子性，不保证顺序 | 读计数器、读状态 |
| `acquire` | 之后的读写不能重排到之前 | Pop 读 tail、读 write_seq |
| `release` | 之前的读写不能重排到之后 | Push 写 tail、写 write_seq |
| `acq_rel` | acquire + release | CAS 占槽 |
| `seq_cst` | 全局顺序一致（最慢） | 本系统不使用 |

```cpp
// release：保证数据写完后再递增 seq，消费者读到 seq 时数据一定可见
m_write_seq.store(seq + 1, std::memory_order_release);

// acquire：读到 seq 后，对应槽位的数据一定已经写入
uint64_t cur = m_write_seq.load(std::memory_order_acquire);
```

### 1.3 CAS（Compare-And-Swap）

```cpp
// strong：保证值相等时一定成功，用于单次占槽
m_slots[i].state.compare_exchange_strong(expected, OrderState::Pending,
                                          std::memory_order_acq_rel);

// weak：允许伪失败，循环中性能更好（ARM LL/SC 天然 weak 语义）
m_daily_loss.compare_exchange_weak(old_val, new_val,
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
```

### 1.4 fetch_add

对应 x86 `XADD` 指令，原子加法，MPSC 队列多生产者抢占槽位：

```cpp
const size_t pos = m_tail.fetch_add(1, std::memory_order_acq_rel) & (Size - 1);
```

---

## 二、并发与多线程

### 2.1 std::thread

```cpp
std::thread t([this, cpu_core]() { Run(); });
t.detach(); // 分离线程，不等待结束
```

### 2.2 CPU 绑核（pthread_setaffinity_np）

POSIX 扩展，将线程绑定到指定 CPU 核心，减少调度抖动和 cache 迁移：

```cpp
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(cpu_core, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

### 2.3 std::this_thread::yield / pause 指令

```cpp
// yield：让出 CPU，OS 重新调度（会引入 1~10 微秒延迟）
std::this_thread::yield();

// pause：x86 自旋等待指令，降低功耗，避免内存序冲突，延迟更低
__asm__ __volatile__("pause");
```

### 2.4 std::chrono

```cpp
auto now = std::chrono::steady_clock::now();
auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    now - insert_time).count();
```

---

## 三、模板与泛型编程

### 3.1 类模板

```cpp
template<typename T, size_t Size = 4096>
class LockFreeQueue { ... };
```

### 3.2 static_assert（编译期断言）

```cpp
static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");
```

编译期强制检查，不满足条件直接报错，零运行时开销。

### 3.3 模板函数

```cpp
template<typename Fn>
void ForEachTimeout(int timeout_ms, Fn cb) {
    // cb 是任意可调用对象（lambda、函数指针等）
}
```

---

## 四、面向对象

### 4.1 继承与虚函数

CTP 的 SPI 模式：继承接口类，重写回调：

```cpp
class MdEngine : public CThostFtdcMdSpi {
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) override;
};
```

`override` 关键字（C++11）：编译器验证确实在重写虚函数，防止拼写错误。

### 4.2 单例模式（Magic Static）

C++11 保证局部静态变量线程安全初始化：

```cpp
static LockFreeLogger& getInstance() {
    static LockFreeLogger instance; // 线程安全，只初始化一次
    return instance;
}
```

### 4.3 explicit 构造函数

防止隐式类型转换：

```cpp
explicit TdEngine(const RiskConfig& cfg);
```

### 4.4 default / delete

```cpp
TickPool() = default;          // 使用编译器生成的默认构造
LockFreeLogger(const LockFreeLogger&) = delete; // 禁止拷贝
```

---

## 五、内存管理与对齐

### 5.1 alignas

将结构体或变量对齐到指定字节边界，防止 false sharing：

```cpp
struct alignas(64) OrderSlot { ... };  // 独占一条 cache line（64字节）
alignas(64) std::atomic<size_t> m_head;
```

**False sharing**：两个变量在同一 cache line，一个线程修改会导致另一个线程的 cache 失效。

### 5.2 std::array

固定大小数组，栈分配，无堆开销，比 `std::vector` 更适合热路径：

```cpp
std::array<OrderSlot, MAX_ORDERS> m_slots{};
std::array<TickSlot, TICK_POOL_SIZE> m_slots{};
```

### 5.3 placement new / 内存预热

启动阶段遍历缓冲区，触发物理页分配，避免开盘 Page Fault：

```cpp
void WarmUp() {
    volatile T* p = m_buffer.data();
    for (size_t i = 0; i < Size; ++i) (void)p[i];
}
```

---

## 六、现代 C++ 特性

### 6.1 constexpr（C++11）

编译期常量，零运行时开销：

```cpp
constexpr int64_t SCALE = 10000;
constexpr int MAX_ORDERS = 128;
constexpr size_t TICK_POOL_SIZE = 1024;
```

### 6.2 auto 类型推导（C++11）

```cpp
auto& last = m_last_tick[p->InstrumentID];
auto elapsed = std::chrono::duration_cast<...>(...).count();
```

### 6.3 Lambda 表达式（C++11）

```cpp
// 捕获 this，作为线程函数
std::thread t([this, cpu_core]() { Run(); });

// 作为回调传入模板函数
m_oms.ForEachTimeout(3000, [this](OrderSlot& slot) {
    CancelOrder(slot);
});
```

### 6.4 Range-based for（C++11）

```cpp
for (const auto& inst : instruments)
    m_last_tick[inst] = {};
```

### 6.5 nullptr（C++11）

替代 `NULL`，类型安全：

```cpp
CThostFtdcMdApi* m_api = nullptr;
ShmSnapshot* m_shm = nullptr;
```

### 6.6 using 类型别名（C++11）

```cpp
using LogQueue = MPSCQueue<LogEntry, 8192>;
```

### 6.7 [[nodiscard]]（C++17）

返回值不能被忽略，编译器发出警告：

```cpp
[[nodiscard]] const char* AllocSlot(...);
```

### 6.8 inline 变量（C++17）

头文件中定义全局变量，避免多编译单元重复定义：

```cpp
inline TickPool g_tick_pool;
```

### 6.9 [[likely]] / [[unlikely]]（C++20）

分支预测标注，编译器将 likely 分支放在 fall-through 路径：

```cpp
if ([[unlikely]] m_daily_loss.load() >= m_cfg.max_daily_loss) return false;
```

---

## 七、GCC 扩展与内联汇编

### 7.1 __builtin_expect

等价于 `[[likely]]`/`[[unlikely]]`，兼容 C++17：

```cpp
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
```

### 7.2 rdtsc / rdtscp 内联汇编

直接读取 CPU 周期计数器，开销约 5ns，远低于 `clock_gettime`（~25ns）：

```cpp
static inline uint64_t Now() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// rdtscp：序列化版本，等待之前指令完成后再读
static inline uint64_t NowSerialized() {
    uint32_t lo, hi, aux;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}
```

### 7.3 pause 指令

x86 自旋等待优化，降低功耗，避免内存序冲突：

```cpp
__asm__ __volatile__("pause");
```

---

## 八、标准库

### 8.1 std::string / std::string_view

```cpp
std::string ref = m_td.SendOrder(...);
if (!ref.empty()) { ... }
```

### 8.2 std::unordered_map

哈希表，O(1) 平均查找，用于行情去重：

```cpp
std::unordered_map<std::string, CThostFtdcDepthMarketDataField> m_last_tick;
```

注意：首次插入会触发堆分配，需要在 `Init` 阶段预热。

### 8.3 std::vector

动态数组，用于合约列表：

```cpp
std::vector<std::string> m_instruments;
```

### 8.4 std::function

类型擦除的可调用对象包装器，用于撤单回调：

```cpp
std::function<void(const OrderSlot&)> on_cancel_cb;
```

### 8.5 std::ofstream

文件输出流，Logger 写日志文件：

```cpp
std::ofstream m_outFile;
m_outFile.open(path, std::ios::app);
m_outFile << time_buf << " [" << entry.level << "] " << entry.msg << "\n";
```

### 8.6 std::signal / POSIX 信号

```cpp
std::signal(SIGINT,  OnSignal);
std::signal(SIGTERM, OnSignal);
```

### 8.7 POSIX 共享内存

```cpp
int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
ftruncate(fd, sizeof(ShmSnapshot));
void* ptr = mmap(nullptr, sizeof(ShmSnapshot), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

---

## 九、设计模式

### 9.1 SPSC 无锁队列

Single Producer Single Consumer，利用 acquire/release 内存序，无需互斥锁：

- 生产者只写 tail，消费者只写 head，天然无竞争
- head/tail 各自 alignas(64) 防止 false sharing

### 9.2 MPSC 无锁队列

Multi Producer Single Consumer，用 fetch_add 原子抢占槽位：

- 每个槽位有独立 ready flag，消费者按序消费
- 解决 Logger 多线程写入的数据竞争

### 9.3 环形缓冲区（Ring Buffer）

用 `& (Size-1)` 代替取模（Size 必须是 2 的幂）：

```cpp
int idx = seq & (TICK_POOL_SIZE - 1); // 等价于 seq % TICK_POOL_SIZE，但更快
```

### 9.4 对象池（Object Pool）

预分配固定数量的对象，避免运行时堆分配：

```cpp
std::array<OrderSlot, MAX_ORDERS> m_slots{};  // 128 个订单槽
std::array<TickSlot, TICK_POOL_SIZE> m_slots{}; // 1024 个 tick 槽
```

### 9.5 时间轮（Timing Wheel）

O(1) 更新的滑动窗口计数器，用于撤单频率限制：

```cpp
// 环形队列存时间戳，维护 sum，弹出过期记录
while (now_sec - m_cancel_ts[m_cancel_head] >= 60) {
    m_cancel_head = (m_cancel_head + 1) & (CANCEL_BUF_SIZE - 1);
    --m_cancel_sum;
}
```

### 9.6 SPI 模式（Service Provider Interface）

CTP 的回调机制：继承接口类，注册到 API，API 在事件发生时调用：

```cpp
m_api->RegisterSpi(this); // 注册自己为回调接收者
// CTP 内部在收到行情时调用 this->OnRtnDepthMarketData(p)
```

---

## 十、编译与构建

### 10.1 CMake

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -pthread")
file(GLOB_RECURSE ALL_SRCS "src/*.cpp")  # 自动收集所有 .cpp
add_executable(SimNowTrader ${ALL_SRCS})
target_link_libraries(SimNowTrader thostmduserapi_se thosttraderapi_sm pthread dl)
```

### 10.2 编译优化选项

- `-O3`：最高优化级别，开启向量化、内联等
- `-Wall -Wextra`：开启所有警告
- `-pthread`：启用 POSIX 线程支持

### 10.3 pragma once

防止头文件重复包含，比 `#ifndef` 更简洁：

```cpp
#pragma once
```
