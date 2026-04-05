#pragma once
#include <atomic>
#include <array>

// Multi-Producer Single-Consumer 无锁队列
// 专为 Logger 设计：行情、交易、策略三个线程同时写日志，flush 线程单独消费
//
// 原理：生产者用 fetch_add（x86 XADD 指令）原子抢占 tail 槽位，
//       每个槽用 sequence 版本号（而非简单 ready flag）来防止槽位绕圈覆盖（ABA）：
//         - 槽初始 seq[i] = i
//         - 生产者抢到 pos_raw 后，必须等 seq[pos] == pos_raw 才能写入
//         - 写完后 seq = pos_raw + 1，通知消费者
//         - 消费者读完后 seq = head + Size，允许下一轮生产者写入
// Size 必须是 2 的幂。
template<typename T, size_t Size = 8192>
class MPSCQueue {
    static_assert((Size & (Size - 1)) == 0, "MPSCQueue: Size must be a power of 2");

    // 每个槽独占一条 cache line，避免生产者之间的 false sharing
    struct alignas(64) Slot {
        std::atomic<size_t> seq;
        T data;
    };

public:
    MPSCQueue() : m_head(0), m_tail(0) {
        // seq[i] = i：表示第 0 轮第 i 个槽可供生产者写入
        for (size_t i = 0; i < Size; ++i)
            m_slots[i].seq.store(i, std::memory_order_relaxed);
    }

    // 生产者调用（多线程安全）
    // 用 seq 版本号确保绕圈时不会两个生产者同时写同一槽
    bool Push(const T& item) {
        const size_t pos_raw = m_tail.fetch_add(1, std::memory_order_acq_rel);
        const size_t pos     = pos_raw & (Size - 1);
        // 等待该槽的 seq == pos_raw，说明消费者已处理完上一轮，槽可用
        while (m_slots[pos].seq.load(std::memory_order_acquire) != pos_raw) {}
        m_slots[pos].data = item;
        // 写完后 seq = pos_raw+1，通知消费者该槽已就绪
        m_slots[pos].seq.store(pos_raw + 1, std::memory_order_release);
        return true;
    }

    // 消费者调用（单线程）
    // 按 head 顺序检查 seq，保证日志顺序与写入顺序一致
    bool Pop(T& item) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t pos  = head & (Size - 1);
        // seq == head+1 说明生产者已写完
        if (m_slots[pos].seq.load(std::memory_order_acquire) != head + 1)
            return false;
        item = m_slots[pos].data;
        // 消费完后 seq = head+Size，允许下一轮生产者写入该槽
        m_slots[pos].seq.store(head + Size, std::memory_order_release);
        m_head.store(head + 1, std::memory_order_relaxed);
        return true;
    }

    // 内存预热：触发所有槽位的物理页分配，避免首次写日志触发 Page Fault
    void WarmUp() {
        for (size_t i = 0; i < Size; ++i) {
            volatile size_t s = m_slots[i].seq.load(std::memory_order_relaxed);
            (void)s;
        }
    }

private:
    alignas(64) std::atomic<size_t> m_head; // 消费者写，独占 cache line
    alignas(64) std::atomic<size_t> m_tail; // 生产者竞争写，独占 cache line
    std::array<Slot, Size> m_slots;
};
