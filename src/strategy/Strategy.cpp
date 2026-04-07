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

    // 涨跌停时不报价
    if (tick.last_price >= tick.upper_limit || tick.last_price <= tick.lower_limit)
        return;

    // 用买一卖一中间价作为报价基准
    double  mid_price = (tick.bid[0] + tick.ask[0]) * 0.5;
    int64_t mid_int   = PriceUtil::ToInt(mid_price);

    // 中间价没变则不撤单重报
    if (LIKELY(mid_int == ms.last_mid)) return;
    ms.last_mid = mid_int;

    // 撤掉旧的双边挂单（O(1) 槽位直接访问）
    CancelIfActive(ms.bid_slot);
    CancelIfActive(ms.ask_slot);
    ms.bid_slot = -1;
    ms.ask_slot = -1;

    int net = m_td.GetNetLongByIdx(idx);

    constexpr int64_t TICK_SIZE = 2000; // 0.2 点，股指期货最小变动价位
    int64_t bid_int = PriceUtil::AddTick(mid_int, -m_spread_ticks, TICK_SIZE);
    int64_t ask_int = PriceUtil::AddTick(mid_int,  m_spread_ticks, TICK_SIZE);
    double  bid_px  = PriceUtil::ToDouble(bid_int);
    double  ask_px  = PriceUtil::ToDouble(ask_int);

    // 净多头未达上限时挂 bid
    if (net < m_max_net_pos) {
        std::string ref = m_td.SendOrder(inst, bid_px, THOST_FTDC_D_Buy);
        if (!ref.empty()) {
            // 直接存槽位下标，后续撤单 O(1)
            ms.bid_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
            LOG_INFO("[MM] %s BID %.2f slot=%d net=%d", inst, bid_px, ms.bid_slot, net);
        }
    }

    // 净空头未达上限时挂 ask
    if (net > -m_max_net_pos) {
        std::string ref = m_td.SendOrder(inst, ask_px, THOST_FTDC_D_Sell);
        if (!ref.empty()) {
            ms.ask_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
            LOG_INFO("[MM] %s ASK %.2f slot=%d net=%d", inst, ask_px, ms.ask_slot, net);
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
    uint64_t last_seq = 0;

    while (true) {
        uint64_t cur_seq = g_tick_pool.WriteSeq();
        if (UNLIKELY(cur_seq == last_seq)) {
            __asm__ __volatile__("pause");
            continue;
        }

        const TickSlot& slot   = g_tick_pool.SlotBySeq(cur_seq);
        const SlimTick& tick   = slot.tick;
        const uint64_t  t1_tsc = slot.recv_tsc;
        last_seq = cur_seq;

        if (UNLIKELY(!m_td.isReady)) continue;

        // inst_idx 由 MdEngine 写入时填好，O(1) 定位，无字符串比较
        int idx = tick.inst_idx;
        if (UNLIKELY(idx < 0 || idx >= m_inst_count)) continue;

        if (UNLIKELY(tick.bid[0] <= 0.0 || tick.ask[0] <= 0.0)) continue;

        OnTick(idx, tick, t1_tsc);
    }
}
