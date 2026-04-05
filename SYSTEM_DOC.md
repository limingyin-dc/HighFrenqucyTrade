# 高频量化交易系统 — 技术文档

## 目录

1. [系统概览](#1-系统概览)
2. [整体架构与数据流](#2-整体架构与数据流)
3. [文件结构](#3-文件结构)
4. [模块详解](#4-模块详解)
5. [延迟测量与优化](#5-延迟测量与优化)
6. [C++ 特性使用](#6-c-特性使用)
7. [线程模型与并发安全](#7-线程模型与并发安全)
8. [简历描述参考](#8-简历描述参考)

---

## 1. 系统概览

基于 CTP API 的 C++20 高频量化交易框架，对接国内期货市场（SimNow 仿真环境）。

核心设计目标：
- 低延迟：tick 到报单全链路延迟 < 5 微秒（软件层）
- 无锁：热路径全程无互斥锁
- 无分配：热路径零堆内存分配，启动阶段预分配完毕
- 可观测：共享内存段对外暴露实时状态，外部监控进程零侵入

技术栈：C++20 / CTP API / pthread / CMake / POSIX Shared Memory / Linux

---

## 2. 整体架构与数据流

```
┌──────────────────────────────────────────────────────────────────┐
│                           main.cpp                               │
│  TSC校准 → 内存预热 → 信号处理 → 优雅退出 → Logger::Flush()      │
└──────────┬───────────────────────────┬───────────────────────────┘
           │                           │
    ┌──────▼──────┐             ┌──────▼──────┐
    │  MdEngine   │             │  TdEngine   │
    │  行情线程    │             │  交易线程    │
    │  CTP 回调   │             │  CTP 回调   │
    └──────┬──────┘             └──────▲──────┘
           │ T1打点(rdtsc)             │ SendOrder/CancelOrder
           │ Write(SlimTick,tsc)       │
           ▼                           │
    ┌─────────────┐                    │
    │  TickPool   │  ← Ring Buffer（1024槽）
    │  write_seq  │    行情写入后递增原子序号
    └──────┬──────┘
           │ 策略线程轮询 write_seq（acquire）
           │ 直接通过序号读取最新槽位，无队列
    ┌──────▼──────┐
    │  Strategy   │  ← 策略线程（绑核 core 2）
    │  T2打点      │    整数价格比较，开仓/平仓
    │  延迟统计    │    每100tick打印T1→T2均值
    └─────────────┘

    ┌──────────────────────────────────────────┐
    │  OrderManager  (O(1) 下标编码查找)        │
    │  RiskManager   (原子风控 + 自成交保护)    │
    │  LockFreeLogger(MPSC 异步日志队列)        │
    │  ShmMonitor    (共享内存实时快照)         │
    └──────────────────────────────────────────┘
```

数据流：
1. CTP 推送 tick → `OnRtnDepthMarketData` 入口打 T1（rdtsc）
2. `IsValid` 过滤异常值，`IsDuplicate` 去重（固定数组按合约下标索引，无哈希）
3. `TickPool::Write` 提取 SlimTick 写入 Ring Buffer 槽位，递增 `write_seq`（release）
4. 策略线程轮询 `write_seq`（acquire），变化时直接通过序号读取槽位，打 T2，计算 T1→T2 延迟
5. 整数价格比较触发信号 → `TdEngine::SendOrder` 七道风控 → `ReqOrderInsert`
6. CTP 回报 → `OnRtnOrder` O(1) 解码 order_ref 末3位定位 OMS 槽位
7. 成交回报更新持仓均价，触发 PnL 计算，刷新共享内存快照

---

## 3. 文件结构

```
src/
├── main.cpp
├── common/
│   ├── spsc.h            SPSC 无锁队列（保留备用）
│   ├── mpsc.h            MPSC 无锁队列（多线程→Logger）
│   ├── Logger.h/cpp      异步无锁日志器
│   ├── future_struct.h   SlimTick / AccountMetrics / PositionMetrics
│   ├── ShmMonitor.h/cpp  共享内存监控
│   ├── OrderManager.h/cpp 无锁订单管理器（O(1)查找）
│   ├── RiskManager.h/cpp  七道无锁风控
│   ├── ConfigLoader.h/cpp XML 配置加载
│   └── Tsc.h             rdtsc 时间戳工具
├── md_engine/
│   ├── TickPool.h/cpp    SlimTick 对象池（零拷贝传递）
│   └── MdEngine.h/cpp    行情引擎
├── td_engine/
│   └── TdEngine.h/cpp    交易引擎
└── strategy/
    └── Strategy.h/cpp    策略引擎
```

---

## 4. 模块详解

### 4.1 SPSC 无锁队列 (`spsc.h`)

通用 SPSC 无锁环形队列，当前系统行情传递已改为 Ring Buffer + write_seq 轮询，SPSC 保留备用。

关键设计：
- `head`/`tail` 各自 `alignas(64)` 独占 cache line，防止 false sharing
- Push 用 `release`，Pop 用 `acquire`，建立 happens-before 保证数据可见性
- `WarmUp()` 启动时遍历缓冲区，触发物理页分配，避免开盘 Page Fault
- Size 必须是 2 的幂，用 `& (Size-1)` 代替取模，消除除法指令

### 4.2 MPSC 无锁队列 (`mpsc.h`)

Logger 专用，修复原 SPSC 在多生产者场景下的数据竞争。

关键设计：
- 生产者用 `fetch_add`（x86 XADD 指令）原子抢占 tail 槽位，无需 CAS 循环
- 每个槽位有独立 `ready` flag，消费者按 head 顺序消费，保证日志顺序性
- 每个 Slot `alignas(64)` 独占 cache line，避免生产者之间 false sharing

### 4.3 SlimTick 与 TickPool (`future_struct.h` / `TickPool.h`)

零拷贝行情传递的核心。

SlimTick 只保留策略需要的字段：
```cpp
struct alignas(64) SlimTick {
    char   instrument[32];
    double last_price;
    double upper_limit;   // 涨停价（一天内不变，首次 tick 写入）
    double lower_limit;   // 跌停价
    double bid1, ask1;
    int    update_ms;
    char   update_time[9];
};
```

TickPool 是预分配的 Ring Buffer（1024槽），每个 TickSlot 存储 SlimTick + T1 时间戳：
```
MdEngine 回调 → TickPool::Write(SlimTick, recv_tsc) → write_seq++ (release)

Strategy 线程 → 轮询 WriteSeq() (acquire) → SlotBySeq(cur_seq) 直接读槽位
```

### 4.4 行情引擎 (`MdEngine.h/cpp`)

两道过滤：
1. `IsValid`：零/负价格、超涨跌停过滤
2. `IsDuplicate`：量变或价变才认为是新 tick，用固定数组按合约下标索引，替代 unordered_map，消除哈希开销

T1 打点在 `OnRtnDepthMarketData` 第一行，用 `rdtsc`（~5ns 开销）。

### 4.5 交易引擎 (`TdEngine.h/cpp`)

连接流程：
```
Init → ConnectApi
  ↓ OnFrontConnected → ReqAuthenticate
  ↓ OnRspAuthenticate → ReqUserLogin
  ↓ OnRspUserLogin → 同步 MaxOrderRef → ReqSettlementInfoConfirm
  ↓ OnRspSettlementInfoConfirm → isReady=true → QueryAccount + QueryPosition + QueryOpenOrders
```

断线重连：`OnFrontDisconnected` 置 `isReady=false`，CTP 自动重连后重走上述流程，`QueryOpenOrders` 重建 OMS。

报单接口 `SendOrder`：七道风控 → `AllocSlot`（下标编码进 order_ref 末3位）→ 预冻结保证金 → `ReqOrderInsert`。

### 4.6 订单管理器 (`OrderManager.h/cpp`)

O(1) 查找核心：order_ref 末3位 = 槽位下标。

```
发单：snprintf(ref, "%09d%03d", seq, slot_idx)  → 末3位是下标
回调：atoi(ref + len - 3)                        → 直接定位，O(1)
```

查找时间从 O(N) 几百纳秒 → O(1) 几纳秒。

自成交保护：`WouldSelfMatch` 检查新报单是否与已有挂单价格方向匹配。

内存预热：`WarmUp()` 遍历 128 个 OrderSlot（8KB），确保物理页已分配。

断线重连：`Reset()` 清空所有槽位，配合 `QueryOpenOrders` 重建。

### 4.7 风控管理器 (`RiskManager.h/cpp`)

七道无锁检查（Strategy 线程单线程调用）：

| 检查项 | 实现 |
|--------|------|
| 日内亏损熔断 | `atomic<double>` relaxed load |
| 名义价值上限 | 纯计算 |
| 可用资金检查 | `atomic<double>` relaxed load |
| 全局挂单数 | 遍历 OMS 槽位 |
| 单合约挂单数 | 同上，加合约名过滤 |
| 净持仓上限 | 传入当前净持仓，纯计算 |
| 自成交保护 | `OMS::WouldSelfMatch` |

撤单频率：滑动窗口队列，维护时间戳环形数组 + sum，弹出过期记录，O(1) 均摊复杂度，修复了时间轮旧桶数据不清零的 bug。

分支预测：所有拒绝条件用 `__builtin_expect(..., 0)` 标注为小概率，减少 branch misprediction penalty。

### 4.8 策略引擎 (`Strategy.h/cpp`)

整数价格：`PriceUtil::ToInt` 将 double 价格放大 10000 倍转为 `int64_t`，策略内部全程整数比较，消除浮点精度抖动。

自旋等待：`write_seq` 无变化时用 `__asm__ __volatile__("pause")` 替代 `yield()`，避免 OS 调度延迟。

延迟统计：每 100 个 tick 打印一次 T1→T2 均值（行情接收延迟）。

### 4.9 TSC 时间戳工具 (`Tsc.h`)

```cpp
Tsc::Init()    // 启动时忙等 200ms 校准 CPU 频率（避免 sleep 导致降频）
Tsc::Now()     // rdtsc，~5ns 开销
Tsc::ToNs(delta)  // TSC 差值转纳秒
```

实测 CPU 频率：2.494 GHz，换算精度约 0.4ns/tick。

### 4.10 共享内存监控 (`ShmMonitor.h/cpp`)

外部 Python/GUI 进程通过 `mmap` 只读映射 `ShmSnapshot`，获取账户、持仓、订单状态，对交易主线程零侵入。

`TdEngine` 在成交回报、资金查询完成后调用 `UpdateShm()` 刷新快照，同时更新系统状态字符串（"RUNNING"/"RECONNECTING"/"HALTED"）。

---

## 5. 延迟测量与优化

### 5.1 测量点定义

```
T1：OnRtnDepthMarketData 入口（rdtsc，CTP 推送时刻）
T2：Strategy::Run() Pop 出 tick 下标（rdtsc）
T3：ReqOrderInsert 返回（报单已发出）
```

当前只统计 T1→T2（行情接收延迟），每 100 个 tick 打印均值：
```
[Latency] 行情接收延迟(T1→T2) 近100tick均值=3258ns  (CPU=2.494GHz)
```

### 5.2 实测结果

| 阶段 | 延迟 | 说明 |
|------|------|------|
| T1→T2（行情接收） | ~3 微秒 | 核间 cache 同步为主要来源 |
| 首批 100 tick | ~36 微秒 | unordered_map 首次插入触发堆分配，Init 预热后消除 |

### 5.3 优化历程

1. `yield()` → `pause` 指令：消除 OS 调度延迟（1~10 微秒）
2. 整个结构体（~600字节）→ SlimTick（~85字节）：减少 80% cache line 写入
3. `clock_gettime` → `rdtsc`：打点开销从 ~25ns 降至 ~5ns
4. `nanosleep` 校准 → 忙等校准：避免 CPU 降频导致频率测量偏低
5. unordered_map 预热：消除首批 tick 的堆分配延迟

### 5.4 硬件层面的限制

当前服务器：单 NUMA 节点，8 个物理核，无超线程。

3 微秒的主要来源是核间 cache 同步（MESI 协议），这是硬件决定的，软件层面已无明显瓶颈。

进一步优化需要：
- co-location 机房 + 专线（减少 CTP 网络延迟）
- Solarflare 网卡 + OpenOnload（绕过内核网络栈，延迟从几十微秒降至几微秒）
- 更高端 CPU（Intel Xeon 低延迟型号，核间延迟 ~200ns）

---

## 6. C++ 特性使用

### C++11

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `std::atomic<T>` | 全局 | 无锁并发基础 |
| `memory_order` | 所有原子操作 | 精确控制可见性，避免 seq_cst 全局屏障 |
| `compare_exchange_strong/weak` | OrderManager / RiskManager | CAS 原语，对应 x86 CMPXCHG |
| `fetch_add` | mpsc.h / TdEngine | 对应 x86 XADD，MPSC 多生产者原子抢占 |
| `std::thread` | Logger / TdEngine / Strategy | flush 线程、守护线程、策略线程 |
| `std::chrono` | OrderManager / RiskManager | 报单时间戳、时间轮 |
| `alignas(64)` | OrderSlot / AccountMetrics / TickSlot 等 | cache line 对齐，防止 false sharing |
| `static_assert` | spsc.h / mpsc.h | 编译期强制 Size 为 2 的幂 |
| `constexpr` | PriceUtil::SCALE / MAX_ORDERS 等 | 编译期常量 |
| `std::array` | OrderManager / TickPool / Strategy | 固定大小数组，栈分配 |
| Lambda | TdEngine / Strategy | ForEachTimeout 回调、线程启动 |
| Magic Static | Logger::getInstance() | 线程安全单例 |

### C++17

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `[[nodiscard]]` | OrderManager::AllocSlot / TickPool::Write | 返回值不能被忽略 |
| `inline` 变量 | TickPool.h 的 `inline TickPool g_tick_pool` | 避免多编译单元重复定义 |

### C++20

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `[[likely]]` / `[[unlikely]]` | RiskManager.cpp | 分支预测标注（GCC 11 在 if 条件内不支持，改用 `__builtin_expect`） |

### GCC 扩展

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `__builtin_expect` | Strategy.cpp / RiskManager.cpp | 分支预测，等价于 `[[likely]]`/`[[unlikely]]` |
| `rdtsc` / `rdtscp` 内联汇编 | Tsc.h | CPU 周期计数器，~5ns 开销 |
| `pthread_setaffinity_np` | Strategy.cpp | CPU 绑核 |
| `__asm__ __volatile__("pause")` | Strategy.cpp | 自旋等待降功耗，避免内存序冲突 |

---

## 7. 线程模型与并发安全

系统共 6 个线程：

| 线程 | 职责 | 绑核 |
|------|------|------|
| 主线程 | 信号监听、优雅退出 | 无 |
| 行情线程 | CTP 行情回调、写 TickPool、Push 下标 | 无（CTP 内部管理） |
| 交易线程 | CTP 交易回调、更新持仓/OMS/共享内存 | 无（CTP 内部管理） |
| 策略线程 | 轮询 write_seq、读 TickPool、信号判断、报单 | core 2 |
| 超时撤单线程 | 每秒扫描 OMS、撤超时单 | 无 |
| 日志 flush 线程 | Pop LogEntry、写文件 | 无 |

共享数据并发安全：

| 共享数据 | 写线程 | 读线程 | 保护方式 |
|----------|--------|--------|----------|
| TickPool | 行情线程（单生产者） | 策略线程 | write_seq 原子序号（release/acquire）同步 |
| OrderSlot::state | 策略线程（CAS 占槽）、交易线程（更新） | 超时撤单线程 | `std::atomic<int>` |
| AccountMetrics | 交易线程（回报）、策略线程（预冻结） | 风控检查 | `std::atomic<double>` + CAS |
| PositionMetrics | 交易线程（成交回报） | 策略线程 | `std::atomic<int32_t>` |
| m_daily_loss | 交易线程（UpdatePnl） | 策略线程（CheckOrder） | `std::atomic<double>` |
| LogQueue | 所有线程（多生产者） | flush 线程 | MPSC 无锁队列 |
| ShmSnapshot | 交易线程（UpdateShm） | 外部监控进程 | 非原子写（监控允许轻微撕裂） |

---

## 8. 简历描述参考

**项目名称：** 基于 CTP API 的 C++ 高频量化交易系统

**技术栈：** C++20 / CTP API / pthread / CMake / POSIX Shared Memory / Linux

**项目描述：**

基于上期技术 SimNow 仿真交易平台（CTP API）设计并实现了一套面向国内期货市场的低延迟量化交易框架，覆盖行情接入、策略执行、报单/撤单、风控管理、断线重连全链路。

**核心技术亮点：**

- 基于 Ring Buffer 实现无锁行情池，通过单原子序号（write_seq）同步生产消费，策略线程直接轮询读取最新 tick，消除队列 Push/Pop 开销；行情去重用固定数组替代 unordered_map，消除哈希开销
- 实现 MPSC 无锁日志队列，用 `fetch_add`（x86 XADD）原子抢占槽位，修复多生产者场景下 SPSC 的数据竞争，业务线程热路径零 I/O
- 实现 O(1) 订单查找：将 OMS 槽位下标编码进 OrderRef，CTP 回调直接解码定位；`alignas(64)` 消除 false sharing；支持断线重连后重建 OMS 状态
- 实现七道无锁风控，含自成交保护和滑动窗口撤单频率限制（修复时间轮旧桶不清零 bug）；拒绝分支用 `__builtin_expect` 标注降低 branch misprediction penalty
- 使用 `rdtsc` 内联汇编打延迟时间戳，策略线程绑核（`pthread_setaffinity_np`），`pause` 指令替代 `yield()` 消除调度延迟
- 基于 POSIX `shm_open` + `mmap` 实现共享内存监控，外部进程实时读取账户持仓状态，对主线程零侵入；启动阶段内存预热避免开盘 Page Fault
