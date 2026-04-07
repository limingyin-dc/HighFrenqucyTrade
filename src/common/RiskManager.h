#pragma once
#include "future_struct.h"
#include "OrderManager.h"
#include <atomic>
#include <chrono>
#include <cmath>

// 风控参数配置
struct RiskConfig {
    double max_order_value     = 500000.0; // 单笔报单名义价值上限（元）
    int    max_active_orders   = 10;       // 全局最大活跃挂单数
    int    max_active_per_inst = 3;        // 单合约最大活跃挂单数
    double max_daily_loss      = 50000.0;  // 日内最大亏损熔断线（元）
    int    max_cancel_per_min  = 20;       // 每分钟最大撤单次数
    int    max_pos_net         = 10;       // 单合约最大净持仓手数
};

// 无锁风控管理器
// 所有检查均为原子操作，Strategy 线程单线程调用，无互斥锁
class RiskManager {
public:
    explicit RiskManager(const AccountMetrics& account,
                         OrderManager& oms,
                         const RiskConfig& cfg = RiskConfig());

    // 报单前七道风控检查（Strategy 线程调用）
    // 依次检查：熔断 / 名义价值 / 可用资金 / 全局挂单数 /
    //           单合约挂单数 / 净持仓上限 / 自成交保护
    // 全部通过返回 true，任一失败返回 false 并记录日志
    bool CheckOrder(const char* inst, double price, int vol,
                    int current_net_pos, char direction = '0');

    // 撤单频率检查：基于 60 桶时间轮统计每分钟撤单次数
    // 超过 max_cancel_per_min 时返回 false
    bool CheckCancel();

    // 更新日内累计亏损（CTP 回调线程调用）
    // pnl_delta 为负时累加亏损，使用 CAS 循环保证原子性
    void UpdatePnl(double pnl_delta);

    // 每日开盘前重置日内亏损和撤单计数器
    void DailyReset();

    // 返回当前日内累计亏损金额
    double GetDailyLoss() const;

private:
    const AccountMetrics& m_account;
    OrderManager&         m_oms;
    RiskConfig            m_cfg;

    std::atomic<double>  m_daily_loss{0.0};

    // 滑动窗口撤单频率统计
    // 用环形队列存每次撤单的时间戳（秒），维护一个 sum
    // CheckCancel 是单线程调用，无需原子操作
    // 最多支持 max_cancel_per_min * 2 条记录，足够容纳滑动窗口内的所有撤单
    static constexpr int CANCEL_BUF_SIZE = 256; // 必须大于 max_cancel_per_min
    int      m_cancel_ts[CANCEL_BUF_SIZE]{};    // 每次撤单的时间戳（秒）
    int      m_cancel_head = 0;                 // 队列头（最旧的记录）
    int      m_cancel_tail = 0;                 // 队列尾（下一个写入位置）
    int      m_cancel_sum  = 0;                 // 当前窗口内的撤单次数
    };