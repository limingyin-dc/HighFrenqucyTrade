#pragma once
#include <cstdint>

// 共享内存快照结构体
// 外部 Python/GUI 监控进程通过 mmap 只读映射，零侵入交易主线程
struct ShmSnapshot {
    double   available;     // 可用资金
    double   frozen_margin; // 冻结保证金
    double   curr_margin;   // 占用保证金
    double   daily_pnl;     // 日内盈亏

    static constexpr int MAX_INST = 16;
    struct InstSnap {
        char   name[32];    // 合约代码
        int    net_long;    // 净多头手数
        double avg_price;   // 多头均价
    } positions[MAX_INST];
    int inst_count; // 当前持仓合约数

    int64_t last_tick_ns;   // 最近一次 tick 的纳秒时间戳
    int64_t last_order_ns;  // 最近一次报单的纳秒时间戳
    int     active_orders;  // 当前活跃挂单数
    char    status[32];     // 系统状态："RUNNING"/"RECONNECTING"/"HALTED"
};

// 共享内存生命周期管理
// 主进程调用 Create() 创建并映射，监控进程调用 Attach() 只读附加
class ShmMonitor {
public:
    static constexpr const char* SHM_NAME = "/trading_monitor";

    // 创建并映射共享内存段，返回可读写指针；失败返回 nullptr
    static ShmSnapshot*       Create();

    // 以只读方式附加到已有共享内存段，供监控进程使用
    static const ShmSnapshot* Attach();

    // 删除共享内存段（进程退出时调用）
    static void               Destroy();
};
