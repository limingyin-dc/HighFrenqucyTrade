#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

// ============================================================
// SlimTick：从 CThostFtdcDepthMarketDataField（~600字节）中
// 提取策略真正需要的字段，压缩到 64 字节（一条 cache line）。
//
// 涨跌停价一天内不变，不放在这里，由 MdEngine 单独维护。
// ============================================================
struct alignas(64) SlimTick {
    char   instrument[32];
    double last_price;
    double upper_limit;
    double lower_limit;
    // 买一到买五价量
    double bid[5];
    int    bid_vol[5];
    // 卖一到卖五价量
    double ask[5];
    int    ask_vol[5];
    int    update_ms;
    char   update_time[9];
};

// 账户资金指标，alignas(64) 独占 cache line 避免 false sharing
// CTP 回调线程写，策略线程和风控模块读
struct alignas(64) AccountMetrics {
    std::atomic<double> available{0.0};     // 可用资金
    std::atomic<double> frozen_margin{0.0}; // 挂单冻结保证金
    std::atomic<double> curr_margin{0.0};   // 当前持仓占用保证金

    double margin_ratio = 0.15; // 保证金率（默认 15%）
    int    multiplier   = 300;  // 合约乘数（如 IF 为 300）

    // 原子更新可用资金（CAS 循环，兼容 C++11 的 double 原子操作）
    // delta 为正表示增加，为负表示减少
    void update_available(double delta) {
        double old_val = available.load(std::memory_order_relaxed);
        while (!available.compare_exchange_weak(
                   old_val, old_val + delta,
                   std::memory_order_release, std::memory_order_relaxed));
    }
};

// 单合约持仓指标，alignas(64) 独占 cache line
// CTP 回调线程写（成交回报），策略线程只读
struct alignas(64) PositionMetrics {
    std::atomic<int32_t> long_yd{0};     // 多头昨仓
    std::atomic<int32_t> long_td{0};     // 多头今仓
    std::atomic<int32_t> long_frozen{0}; // 多头冻结量

    std::atomic<int32_t> short_yd{0};     // 空头昨仓
    std::atomic<int32_t> short_td{0};     // 空头今仓
    std::atomic<int32_t> short_frozen{0}; // 空头冻结量

    std::atomic<double> long_avg_price{0.0}; // 多头持仓均价
};
