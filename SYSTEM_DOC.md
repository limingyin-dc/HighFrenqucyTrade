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

基于 CTP API 的 C++ 高频做市交易框架，对接国内期货市场（SimNow 仿真环境），支持多合约同时做市。

核心设计目标：
- 低延迟：tick 到报单全链路延迟 < 5 微秒（软件层）
- 无锁：热路径全程无互斥锁
- 无分配：热路径零堆内存分配，启动阶段预分配完毕
- 可观测：共享内存段对外暴露实时状态，外部监控进程零侵入

技术栈：C++17 / CTP API / pthread / CMake / POSIX Shared Memory / Linux

当前订阅合约：IF2606 / IF2605 / IC2606 / IC2605 / IH2606 / IH2605（股指期货，可配置）

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
           │ Write(SlimTick+inst_idx)  │
           ▼                           │
    ┌─────────────┐                    │
    │  TickPool   │  ← Ring Buffer（1024槽）
    │  write_seq  │    行情写入后递增原子序号
    └──────┬──────┘    SlimTick 内嵌 inst_idx（预计算，O(1)定位）
           │ 策略线程轮询 write_seq（acquire）
    ┌──────▼──────┐
    │  Strategy   │  ← 做市策略线程（绑核 core 2）
    │  (做市商)    │    双边挂单 bid/ask，中间价变动时撤单重报
    │  延迟统计    │    p50/p99/p999 分位数（每1000笔报单）
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
2. `IsValid` 过滤异常值，时间戳去重（`UpdateTime * 1000 + UpdateMillisec`，防乱序/重连误放）
3. `TickPool::Write` 提取 SlimTick（含预计算的 `inst_idx`）写入 Ring Buffer，递增 `write_seq`（release）
4. 策略线程轮询 `write_seq`（acquire），变化时通过 `tick.inst_idx` O(1) 定位合约状态，无字符串比较
5. 中间价变动 → 撤旧双边挂单（O(1) 槽位下标直接访问）→ 重新挂 bid/ask
6. `TdEngine::SendOrder` 七道风控 → `AllocSlot` → `ReqOrderInsert` → T3 打点
7. CTP 回报 → `OnRtnOrder` O(1) 解码 order_ref 末3位定位 OMS 槽位，撤单成功时解冻保证金
8. 成交回报区分平今/平昨更新持仓，触发 PnL 计算，刷新共享内存快照

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
│   ├── TickPool.h/cpp    SlimTick Ring Buffer（含 inst_idx 预计算）
│   └── MdEngine.h/cpp    行情引擎（时间戳去重 + 乱序防护）
├── td_engine/
│   └── TdEngine.h/cpp    交易引擎（平今/平昨区分 + 撤单保证金解冻）
└── strategy/
    └── Strategy.h/cpp    做市策略引擎
config.xml                 网络/账户/策略参数配置
```

---

## 4. 模块详解

### 4.1 SlimTick 与 TickPool (`future_struct.h` / `TickPool.h`)

零拷贝行情传递的核心。

SlimTick 只保留策略需要的字段，新增 `inst_idx` 预计算字段：
```cpp
struct alignas(64) SlimTick {
    char    instrument[32];
    double  last_price;
    double  upper_limit;
    double  lower_limit;
    double  bid[5];  int bid_vol[5];
    double  ask[5];  int ask_vol[5];
    int     update_ms;
    char    update_time[9];
    int8_t  inst_idx;   // MdEngine 写入时预填，策略侧 O(1) 定位，无字符串比较
};
```

`inst_idx` 的意义：MdEngine 在订阅时已知每个合约的下标，写 tick 时直接填入，策略侧和 TdEngine 侧的持仓数组都按同一份 `instruments` 列表顺序初始化，三边下标天然对齐，消灭热路径里所有字符串比较。

TickPool 是预分配的 Ring Buffer（1024槽）：
```
MdEngine 回调 → TickPool::Write(SlimTick+inst_idx, recv_tsc) → write_seq++ (release)
Strategy 线程 → 轮询 WriteSeq() (acquire) → SlotBySeq(cur_seq) 直接读槽位
```

### 4.2 行情引擎 (`MdEngine.h/cpp`)

两道过滤：
1. `IsValid`：零/负价格、超涨跌停过滤
2. 时间戳去重：`UpdateTime(HHMMSS) * 1000 + UpdateMillisec` 构造单调 key，乱序 tick（key <= last_key）直接丢弃；重连时清零缓存，防止重连后旧 tick 被误放

T1 打点在 `OnRtnDepthMarketData` 第一行，用 `rdtsc`（~5ns 开销）。

### 4.3 交易引擎 (`TdEngine.h/cpp`)

连接流程：
```
Init(含 instruments 预填持仓槽位) → ConnectApi
  ↓ OnFrontConnected → ReqAuthenticate
  ↓ OnRspAuthenticate → ReqUserLogin
  ↓ OnRspUserLogin → 同步 MaxOrderRef → ReqSettlementInfoConfirm
  ↓ OnRspSettlementInfoConfirm → isReady=true → QueryAccount + QueryPosition + QueryOpenOrders
```

持仓槽位预初始化：`Init` 按 `instruments` 顺序填好 `m_positions[i].name`，与 MdEngine 的 `inst_idx` 对齐，`GetNetLongByIdx(idx)` O(1) 直接访问，无字符串查找。

平仓今昨区分：`OnRtnTrade` 根据 `OffsetFlag` 分三路处理：
- `OF_Open`：增加今仓，更新加权均价
- `OF_CloseToday`：只减今仓
- `OF_Close`（平昨）：只减昨仓

撤单成功解冻：`OnRtnOrder(Canceled)` 计算未成交手数对应的预估保证金，归还可用资金。

撤单被拒分类处理：
- `ErrorID 25/26/30`（订单已终结）：主动释放 OMS 槽位，不等 `OnRtnOrder`
- 其他错误：标记 `Cancelled`，阻止守护线程重复撤单

持仓均价精度：`OnRspQryInvestorPosition` 用 `PositionCost / (Position * multiplier)` 还原均价，比 `OpenCost / Position` 更准确，隔夜持仓不丢精度。

### 4.4 订单管理器 (`OrderManager.h/cpp`)

O(1) 查找核心：order_ref 末3位 = 槽位下标。

```
发单：snprintf(ref, "%09d%03d", seq, slot_idx)  → 末3位是下标
回调：atoi(ref + len - 3)                        → 直接定位，O(1)
```

自成交保护：`WouldSelfMatch` 检查新报单是否与已有挂单价格方向匹配。

内存预热：`WarmUp()` 遍历 128 个 OrderSlot，确保物理页已分配。

断线重连：`Reset()` 清空所有槽位，配合 `QueryOpenOrders` 重建。

### 4.5 风控管理器 (`RiskManager.h/cpp`)

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

撤单频率：滑动窗口队列，维护时间戳环形数组 + sum，弹出过期记录，O(1) 均摊复杂度。

分支预测：所有拒绝条件用 `__builtin_expect(..., 0)` 标注为小概率，减少 branch misprediction penalty。

### 4.6 做市策略引擎 (`Strategy.h/cpp`)

做市逻辑：
```
每个新 tick（中间价变动时触发）：
  1. 撤掉旧的 bid/ask 挂单（O(1) 槽位下标直接访问）
  2. 计算新报价：bid = mid - spread_ticks * tick_size
                 ask = mid + spread_ticks * tick_size
  3. 净多头 < max_net_pos 时挂 bid
     净多头 > -max_net_pos 时挂 ask
  4. 涨跌停时不报价
```

关键优化：
- `tick.inst_idx` O(1) 定位合约状态，无 `FindInstIdx` 字符串遍历
- `MmState` 存 OMS 槽位下标（`bid_slot`/`ask_slot`），撤单时直接 `GetSlot(idx)`，无 `DecodeIndexPublic`
- `GetNetLongByIdx(idx)` 直接访问 `m_positions[idx]`，无字符串查找
- 中间价未变时直接跳过，减少无效撤单

延迟统计：`LatencyStats<1000>` 固定窗口分位数统计器，每 1000 笔实际报单打印一次 p50/p99/p999，无堆分配。

整数价格：`PriceUtil::ToInt` 将 double 价格放大 10000 倍转为 `int64_t`，策略内部全程整数比较，消除浮点精度抖动。

自旋等待：`write_seq` 无变化时用 `__asm__ __volatile__("pause")` 替代 `yield()`，避免 OS 调度延迟。

### 4.7 TSC 时间戳工具 (`Tsc.h`)

```cpp
Tsc::Init()          // 启动时忙等 200ms 校准 CPU 频率（避免 sleep 导致降频）
Tsc::Now()           // rdtsc，~5ns 开销
Tsc::NowSerialized() // rdtscp，序列化版本，T1 打点用
Tsc::ToNs(delta)     // TSC 差值转纳秒
```

### 4.8 共享内存监控 (`ShmMonitor.h/cpp`)

外部 Python/GUI 进程通过 `mmap` 只读映射 `ShmSnapshot`，获取账户、持仓、订单状态，对交易主线程零侵入。

`TdEngine` 在成交回报、资金查询完成后调用 `UpdateShm()` 刷新快照。

### 4.9 配置文件 (`config.xml`)

```xml
<Strategy>
    <Spread>2</Spread>        <!-- 单边偏移档数，总价差 = Spread * 2 * 0.2点 -->
    <MaxNetPos>3</MaxNetPos>  <!-- 单合约最大净持仓手数 -->
    <Instruments>
        <Instrument>IF2606</Instrument>
        <Instrument>IF2605</Instrument>
        <Instrument>IC2606</Instrument>
        <Instrument>IC2605</Instrument>
        <Instrument>IH2606</Instrument>
        <Instrument>IH2605</Instrument>
    </Instruments>
</Strategy>
```

---

## 5. 延迟测量与优化

### 5.1 测量点定义

```
T1：OnRtnDepthMarketData 入口（rdtsc，CTP 推送时刻）
T3：ReqOrderInsert 返回后立即打点（报单已发出）

统计：T1→T3 全链路延迟，每 1000 笔实际报单打印 p50/p99/p999
```

日志格式：
```
[Latency][tick→报单(T1→T3)] n=1000  p50=XXXns  p99=XXXns  p999=XXXns
```

### 5.2 优化历程

| 优化项 | 效果 |
|--------|------|
| `yield()` → `pause` 指令 | 消除 OS 调度延迟（1~10 微秒） |
| 整个结构体（~600字节）→ SlimTick（~85字节） | 减少 80% cache line 写入 |
| `clock_gettime` → `rdtsc` | 打点开销从 ~25ns 降至 ~5ns |
| `nanosleep` 校准 → 忙等校准 | 避免 CPU 降频导致频率测量偏低 |
| `unordered_map` → 固定数组 | 消除哈希开销和堆分配 |
| `FindInstIdx` 字符串比较 → `inst_idx` 预计算 | 热路径消灭所有 `strncmp` |
| `bid_ref` 字符串 → `bid_slot` 槽位下标 | 撤单路径消灭 `DecodeIndexPublic` |
| `GetNetLong(name)` → `GetNetLongByIdx(idx)` | 持仓查询从线性遍历变为 O(1) |
| 均值统计 → p50/p99/p999 分位数 | 更准确反映尾延迟 |

### 5.3 硬件层面的限制

核间 cache 同步（MESI 协议）是当前主要延迟来源，软件层面已无明显瓶颈。

进一步优化需要：
- co-location 机房 + 专线（减少 CTP 网络延迟）
- Solarflare 网卡 + OpenOnload（绕过内核网络栈）
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
| `[[nodiscard]]` | OrderManager::AllocSlot | 返回值不能被忽略，防止槽位泄漏 |
| `inline` 变量 | TickPool.h 的 `inline TickPool g_tick_pool` | 避免多编译单元重复定义 |

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
| 行情线程 | CTP 行情回调、写 TickPool（含 inst_idx） | 无（CTP 内部管理） |
| 交易线程 | CTP 交易回调、更新持仓/OMS/共享内存 | 无（CTP 内部管理） |
| 策略线程 | 轮询 write_seq、做市双边报价、撤单重报 | core 2 |
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

**项目名称：** 基于 CTP API 的 C++ 高频做市交易系统

**技术栈：** C++17 / CTP API / pthread / CMake / POSIX Shared Memory / Linux

**项目描述：**

基于上期技术 SimNow 仿真交易平台（CTP API）设计并实现了一套面向国内期货市场的低延迟做市交易框架，支持 IF/IC/IH 等多合约同时做市，覆盖行情接入、双边报价、撤单重报、风控管理、断线重连全链路。

**核心技术亮点：**

- 基于 Ring Buffer 实现无锁行情池，SlimTick 内嵌预计算的合约下标（`inst_idx`），策略侧 O(1) 定位合约状态，消灭热路径所有字符串比较；时间戳去重防止乱序/重连误放旧 tick
- 做市策略维护每合约双边挂单槽位下标，中间价变动时 O(1) 直接撤单重报；`GetNetLongByIdx` 按下标直接访问持仓，三边（行情/策略/交易）下标通过同一份订阅列表初始化天然对齐
- 实现 MPSC 无锁日志队列，用 `fetch_add`（x86 XADD）原子抢占槽位，业务线程热路径零 I/O；`LatencyStats<N>` 固定窗口分位数统计器，每 1000 笔报单打印 p50/p99/p999，无堆分配
- 实现 O(1) 订单查找：OMS 槽位下标编码进 OrderRef 末3位，CTP 回调直接解码定位；撤单成功时解冻预估保证金；撤单被拒按错误码分类处理（已终结订单主动释放槽位，防止泄漏）
- 成交回报区分平今/平昨（`OffsetFlag`）分别更新今仓/昨仓，修复原实现双重扣减 bug；持仓均价用 `PositionCost / (Position * multiplier)` 还原，隔夜持仓精度更高
- 七道无锁风控含自成交保护和滑动窗口撤单频率限制；`rdtsc` 内联汇编打延迟时间戳；策略线程绑核 + `pause` 指令自旋，消除 OS 调度延迟
