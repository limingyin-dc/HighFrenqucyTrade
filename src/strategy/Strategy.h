#pragma once
#include "TdEngine.h"
#include "TickPool.h"
#include "Tsc.h"
#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <cstring>

#ifndef LIKELY
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

namespace PriceUtil {
    constexpr int64_t SCALE = 10000;

    inline int64_t ToInt(double price) {
        return static_cast<int64_t>(price * SCALE + 0.5);
    }
    inline double ToDouble(int64_t price_int) {
        return static_cast<double>(price_int) / SCALE;
    }
    inline int64_t AddTick(int64_t price_int, int ticks, int64_t tick_size = 2) {
        return price_int + ticks * tick_size;
    }
}

class Strategy {
public:
    Strategy(TdEngine& td, double threshold,
             const std::vector<std::string>& instruments);
    void Start(int cpu_core = -1);

private:
    static constexpr int MAX_LONG_POS   = 5;
    static constexpr int MAX_INST       = 16;

    TdEngine& m_td;
    int64_t   m_threshold_int;

    // 合约名→下标映射，固定数组，线性查找，无堆分配
    int  m_inst_count = 0;
    char m_inst_names[MAX_INST][32]{};

    // 去重缓存：按合约下标索引，数组访问 O(1)，无哈希开销
    // 用成交量变化判断是否有新 tick（量只增不减，直接比不等）
    struct TickCache {
        double last_price = 0.0;
        int    volume     = 0;
    };
    std::array<TickCache, MAX_INST> m_cache{};

    // 返回合约下标，未找到返回 -1
    int FindInstIdx(const char* inst) const;

    void Run();
};
