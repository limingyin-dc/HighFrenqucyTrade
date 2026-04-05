#pragma once
#include "ThostFtdcMdApi.h"
#include "future_struct.h"
#include <atomic>
#include <array>
#include <cstring>

constexpr size_t TICK_POOL_SIZE = 1024; // 必须是 2 的幂

struct alignas(64) TickSlot {
    SlimTick tick;
    uint64_t recv_tsc{0};
};

// 无队列、无索引环形缓冲池
// 行情线程写入槽位后递增 write_seq
// 策略线程直接通过 Latest() 拿到最新 tick 的指针，无需任何索引计算
class TickPool {
public:
    TickPool() = default;

    void WarmUp();

    // 行情线程：写入数据，递增 write_seq（release 保证数据可见）
    void Write(const CThostFtdcDepthMarketDataField& p, uint64_t recv_tsc);

    // 策略线程：返回当前写入序号，用于检测是否有新 tick
    uint64_t WriteSeq() const {
        return m_write_seq.load(std::memory_order_acquire);
    }

    // 策略线程：用已知的 seq 直接定位槽位，避免重复 load write_seq
    const TickSlot& SlotBySeq(uint64_t seq) const {
        return m_slots[(seq - 1) & (TICK_POOL_SIZE - 1)];
    }

private:
    std::array<TickSlot, TICK_POOL_SIZE> m_slots{};
    alignas(64) std::atomic<uint64_t> m_write_seq{0};
};

inline TickPool g_tick_pool;
