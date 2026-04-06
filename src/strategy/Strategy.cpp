#include "Strategy.h"
#include "Logger.h"
#include <thread>
#include <pthread.h>

Strategy::Strategy(TdEngine& td, double threshold,
                   const std::vector<std::string>& instruments)
    : m_td(td), m_threshold_int(PriceUtil::ToInt(threshold)) {
    m_inst_count = (int)instruments.size();
    if (m_inst_count > MAX_INST) m_inst_count = MAX_INST;
    for (int i = 0; i < m_inst_count; ++i) {
        strncpy(m_inst_names[i], instruments[i].c_str(), 31);
        m_inst_names[i][31] = '\0';
    }
}

int Strategy::FindInstIdx(const char* inst) const {
    for (int i = 0; i < m_inst_count; ++i)
        if (strncmp(m_inst_names[i], inst, 31) == 0) return i;
    return -1;
}

void Strategy::Start(int cpu_core) {
    std::thread t([this, cpu_core]() {
        if (cpu_core >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
                LOG_WARN("[Strategy] 绑核失败 core=%d", cpu_core);
            else
                LOG_INFO("[Strategy] 已绑定到 CPU core %d", cpu_core);
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

        // 按合约下标找去重缓存，未订阅的合约直接跳过
        int idx = FindInstIdx(tick.instrument);
        if (UNLIKELY(idx < 0)) continue;

        TickCache& cache = m_cache[idx];

        // // 核心去重：量变了或价变了才执行策略逻辑
        // // 量只增不减，直接比不等；价格用整数比较避免浮点精度问题
        // int    cur_vol   = tick.bid_vol[0] + tick.ask_vol[0]; // 用盘口量变化判断
        // double cur_price = tick.last_price;
        // if (LIKELY(cur_vol == cache.volume && cur_price == cache.last_price)) continue;

        cache.volume     = cur_vol;
        cache.last_price = cur_price;

        // ---- 策略逻辑 ----
        int64_t price_int = PriceUtil::ToInt(cur_price);
        int net_long = m_td.GetNetLong(tick.instrument);

        if (LIKELY(price_int < m_threshold_int) && LIKELY(price_int > 10000)) {
            if (UNLIKELY(net_long >= MAX_LONG_POS)) {
                LOG_WARN("[Strategy] %s 持仓已达上限(%d手)", tick.instrument, MAX_LONG_POS);
            } else {
                double order_price = PriceUtil::ToDouble(
                    PriceUtil::AddTick(price_int, 1, 2000));
                std::string ref = m_td.SendOrder(
                    tick.instrument, order_price, THOST_FTDC_D_Buy);
                if (LIKELY(!ref.empty())) {
                    // T1→T3：tick 收到 → ReqOrderInsert 完成
                    m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
                    LOG_INFO("[Strategy] 开仓 ref=%s", ref.c_str());
                }
            }
        }

        int64_t close_threshold = m_threshold_int * 1005 / 1000;
        if (LIKELY(price_int > close_threshold) && LIKELY(net_long > 0)) {
            double close_price = PriceUtil::ToDouble(
                PriceUtil::AddTick(price_int, -1, 2000));
            std::string ref = m_td.CloseOrder(tick.instrument, close_price, net_long);
            if (LIKELY(!ref.empty())) {
                // T1→T3：tick 收到 → ReqOrderInsert 完成
                m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
                LOG_INFO("[Strategy] 平仓 ref=%s 净多=%d", ref.c_str(), net_long);
            }
        }
    }
}
