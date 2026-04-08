#include "RiskManager.h"
#include "Logger.h"

RiskManager::RiskManager(const AccountMetrics& account,
                         OrderManager& oms,
                         const RiskConfig& cfg)
    : m_account(account), m_oms(oms), m_cfg(cfg) {}

// 七道风控检查，全部使用 __builtin_expect 标注小概率拒绝分支
// 编译器将通过路径放在 fall-through，减少 branch misprediction penalty
bool RiskManager::CheckOrder(const char* inst, double price, int vol,
                              int current_net_pos, char direction, bool is_maker) {
    // 1. 日内亏损熔断：超过阈值后停止所有报单
    if (__builtin_expect(m_daily_loss.load(std::memory_order_relaxed) >= m_cfg.max_daily_loss, 0)) {
        LOG_ERROR("[Risk] 日内亏损熔断: %.2f >= %.2f，停止交易",
                  m_daily_loss.load(), m_cfg.max_daily_loss);
        return false;
    }

    // 2. 名义价值上限：price * vol 不得超过单笔限额
    double notional = price * vol;
    if (__builtin_expect(notional > m_cfg.max_order_value, 0)) {
        LOG_WARN("[Risk] 名义价值超限: %.2f > %.2f", notional, m_cfg.max_order_value);
        return false;
    }

    // 3. 可用资金检查：预估保证金不得超过当前可用资金
    double est_margin = price * m_account.multiplier * m_account.margin_ratio * vol;
    if (__builtin_expect(m_account.available.load(std::memory_order_relaxed) < est_margin, 0)) {
        LOG_WARN("[Risk] 资金不足: 需=%.2f 可用=%.2f",
                 est_margin, m_account.available.load());
        return false;
    }

    // 4. 全局挂单数上限
    if (__builtin_expect(m_oms.GetActiveCount() >= m_cfg.max_active_orders, 0)) {
        LOG_WARN("[Risk] 全局挂单数达上限(%d)", m_cfg.max_active_orders);
        return false;
    }

    // 5. 单合约挂单数上限（做市撤单重报时跳过，旧槽位异步释放中）
    if (!is_maker &&
        __builtin_expect(m_oms.GetActiveCountByInst(inst) >= m_cfg.max_active_per_inst, 0)) {
        LOG_WARN("[Risk] %s 挂单数达上限(%d)", inst, m_cfg.max_active_per_inst);
        return false;
    }

    // 6. 净持仓上限：当前净持仓 + 本次报量不得超过限制
    if (__builtin_expect(std::abs(current_net_pos) + vol > m_cfg.max_pos_net, 0)) {
        LOG_WARN("[Risk] %s 净持仓将超限(%d)", inst, m_cfg.max_pos_net);
        return false;
    }

    // 7. 自成交保护：新报单不得与已有挂单形成对手方成交
    if (__builtin_expect(m_oms.WouldSelfMatch(inst, price, direction), 0)) {
        LOG_WARN("[Risk] %s 自成交保护拦截: 价=%.2f 方向=%c", inst, price, direction);
        return false;
    }

    return true;
}

bool RiskManager::CheckCancel() {
    int now_sec = (int)(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    // 弹出队列头部超过 60 秒的过期记录
    while (m_cancel_head != m_cancel_tail &&
           now_sec - m_cancel_ts[m_cancel_head] >= 60) {
        m_cancel_head = (m_cancel_head + 1) & (CANCEL_BUF_SIZE - 1);
        --m_cancel_sum;
    }

    if (__builtin_expect(m_cancel_sum >= m_cfg.max_cancel_per_min, 0)) {
        LOG_WARN("[Risk] 撤单频率超限(%d次/分钟)", m_cfg.max_cancel_per_min);
        return false;
    }

    int next_tail = (m_cancel_tail + 1) & (CANCEL_BUF_SIZE - 1);
    if (__builtin_expect(next_tail == m_cancel_head, 0)) {
        LOG_ERROR("[Risk] 撤单队列溢出，拒绝撤单");
        return false;
    }
    m_cancel_ts[m_cancel_tail] = now_sec;
    m_cancel_tail = next_tail;
    ++m_cancel_sum;
    return true;
}

// 平仓成交后更新日内累计亏损
// 使用 compare_exchange_weak 循环实现 double 的原子累加（C++11 兼容写法）
void RiskManager::UpdatePnl(double pnl_delta) {
    if (__builtin_expect(pnl_delta < 0, 0)) {
        double old_val = m_daily_loss.load(std::memory_order_relaxed);
        double new_val;
        do {
            new_val = old_val + (-pnl_delta);
        } while (!m_daily_loss.compare_exchange_weak(
                     old_val, new_val,
                     std::memory_order_release, std::memory_order_relaxed));
        LOG_INFO("[Risk] 日内累计亏损: %.2f", new_val);
    }
} 

// 每日开盘前重置所有风控计数器
void RiskManager::DailyReset() {
    m_daily_loss.store(0.0, std::memory_order_relaxed);
    m_cancel_head = 0;
    m_cancel_tail = 0;
    m_cancel_sum  = 0;
    LOG_INFO("[Risk] 日内风控数据已重置");
}

// 返回当前日内累计亏损金额
double RiskManager::GetDailyLoss() const {
    return m_daily_loss.load();
}
