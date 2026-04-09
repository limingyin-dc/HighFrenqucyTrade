#include "Strategy.h"
#include "Logger.h"
#include <thread>
#include <pthread.h>

Strategy::Strategy(TdEngine& td, int spread_ticks, int max_net_pos,
                   const std::vector<std::string>& instruments)
    : m_td(td), m_spread_ticks(spread_ticks), m_max_net_pos(max_net_pos) {
    m_inst_count = std::min((int)instruments.size(), MAX_INST);
    for (int i = 0; i < m_inst_count; ++i) {
        strncpy(m_inst_names[i], instruments[i].c_str(), 31);
        m_inst_names[i][31] = '\0';
    }
}

// 通过槽位下标直接撤单，O(1)，无字符串解码
void Strategy::CancelIfActive(int slot_idx) {
    if (slot_idx < 0) return;
    auto& slot = m_td.m_oms.GetSlot(slot_idx);
    int st = slot.state.load(std::memory_order_acquire);
    if (st == OrderState::Pending || st == OrderState::PartialFilled)
        m_td.CancelOrder(slot);
}

void Strategy::OnTick(int idx, const SlimTick& tick, uint64_t t1_tsc) {
    MmState& ms = m_state[idx];
    const char* inst = tick.instrument;

    // 1. 基础检查：涨跌停不报价
    if (tick.last_price >= tick.upper_limit || tick.last_price <= tick.lower_limit)
        return;

    // 2. 报价基准计算
    double  mid_price = (tick.bid[0] + tick.ask[0]) * 0.5;
    int64_t mid_int   = PriceUtil::ToInt(mid_price);

    // 频率控制：中间价没变则不刷新，避免过度撤单
    if (mid_int == ms.last_mid) return;
    ms.last_mid = mid_int;

    // 3. 撤掉该合约在 OMS 中的旧双边单
    CancelIfActive(ms.bid_slot);
    CancelIfActive(ms.ask_slot);
    ms.bid_slot = -1;
    ms.ask_slot = -1;

    // 4. 获取当前实时净持仓（基于你今晚修好的 TdEngine 计数器）
    int net = m_td.GetNetLongByIdx(idx);

    // 5. 库存风控与报价倾斜 (Inventory Skewing)
    // 如果多头多，就稍微抬高卖价（容易成交）并压低买价（不容易成交）
    int bid_offset = m_spread_ticks;
    int ask_offset = m_spread_ticks;

    if (net > 0)      ask_offset -= 1; // 多头多，卖单往中间靠，诱导平仓
    else if (net < 0) bid_offset -= 1; // 空头多，买单往中间靠，诱导平仓

    constexpr int64_t TICK_SIZE = 200ULL; // 以实际合约为准，如 0.2
    int64_t bid_int = PriceUtil::AddTick(mid_int, -bid_offset, TICK_SIZE);
    int64_t ask_int = PriceUtil::AddTick(mid_int,  ask_offset, TICK_SIZE);

    // 6. 发送 BID（买入）逻辑
    if (net < m_max_net_pos) {
        // 重要：如果当前有空头(net < 0)，买入动作必须是“平仓”
        char offset = (net < 0) ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Open;
        
        double bid_px = PriceUtil::ToDouble(bid_int);
        std::string ref = m_td.SendOrder(inst, bid_px, THOST_FTDC_D_Buy, offset, 1, true);
        
        if (!ref.empty()) {
            ms.bid_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
        }
    }

    // 7. 发送 ASK（卖出）逻辑
    if (net > -m_max_net_pos) {
        // 重要：如果当前有多头(net > 0)，卖出动作必须是“平仓”
        char offset = (net > 0) ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Open;

        double ask_px = PriceUtil::ToDouble(ask_int);
        std::string ref = m_td.SendOrder(inst, ask_px, THOST_FTDC_D_Sell, offset, 1, true);

        if (!ref.empty()) {
            ms.ask_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
        }
    }
}

void Strategy::Start(int cpu_core) {
    std::thread t([this, cpu_core]() {
        if (cpu_core >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
                LOG_WARN("[MM] 绑核失败 core=%d", cpu_core);
            else
                LOG_INFO("[MM] 已绑定到 CPU core %d", cpu_core);
        }
        Run();
    });
    t.detach();
}

void Strategy::Run() {
    while (true) {
        bool any_new = false;

        for (int idx = 0; idx < m_inst_count; ++idx) {
            uint64_t cur_seq = g_tick_pool.SeqByInst((int8_t)idx);
            if (LIKELY(cur_seq == m_last_seq[idx])) continue;

            m_last_seq[idx] = cur_seq;
            any_new = true;

            if (UNLIKELY(!m_td.isReady)) continue;

            const TickSlot& slot   = g_tick_pool.SlotByInst((int8_t)idx);
            const SlimTick& tick   = slot.tick;
            const uint64_t  t1_tsc = slot.recv_tsc;

            if (UNLIKELY(tick.bid[0] <= 0.0 || tick.ask[0] <= 0.0)) continue;

            OnTick(idx, tick, t1_tsc);
        }

        if (UNLIKELY(!any_new))
            __asm__ __volatile__("pause");
    }
}
