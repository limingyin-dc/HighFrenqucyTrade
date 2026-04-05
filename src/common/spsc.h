#pragma once
#include <atomic>
#include <array>

// Single-Producer Single-Consumer 无锁环形队列
// 用于行情线程（生产者）和策略线程（消费者）之间传递 tick 下标
// Size 必须是 2 的幂（编译期 static_assert 强制检查）
template<typename T, size_t Size = 4096>
class LockFreeQueue {
    static_assert((Size & (Size - 1)) == 0, "LockFreeQueue: Size must be a power of 2");
public:
    LockFreeQueue() : m_head(0), m_tail(0) {}

    // 生产者调用（单线程）
    // 将 item 写入队尾，队列满时返回 false（静默丢弃，不阻塞热路径）
    bool Push(const T& item) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & (Size - 1);
        if (next == m_head.load(std::memory_order_acquire))
            return false; // 队列满
        m_buffer[tail] = item;
        m_tail.store(next, std::memory_order_release);
        return true;
    }

    // 消费者调用（单线程）
    // 从队头取出一个元素，队列空时返回 false
    bool Pop(T& item) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire))
            return false; // 队列空
        item = m_buffer[head];
        m_head.store((head + 1) & (Size - 1), std::memory_order_release);
        return true;
    }

    // 判断队列是否为空
    bool empty() const {
        return m_head.load(std::memory_order_acquire) ==
               m_tail.load(std::memory_order_acquire);
    }

    // 返回当前队列中的元素数量
    size_t size() const {
        size_t t = m_tail.load(std::memory_order_acquire);
        size_t h = m_head.load(std::memory_order_acquire);
        return (t - h + Size) & (Size - 1);
    }

    // 内存预热：启动阶段遍历整个缓冲区，触发物理页分配和 TLB 加载
    // 避免开盘第一个 tick 到来时触发 Page Fault 导致延迟抖动
    void WarmUp() {
        volatile T* p = m_buffer.data();
        for (size_t i = 0; i < Size; ++i) (void)p[i];
    }

private:
    alignas(64) std::atomic<size_t> m_head; // 消费者写，独占 cache line
    alignas(64) std::atomic<size_t> m_tail; // 生产者写，独占 cache line
    std::array<T, Size> m_buffer;
};
