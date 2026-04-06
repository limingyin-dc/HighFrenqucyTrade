#pragma once
#include "ThostFtdcUserApiStruct.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>

// 订单状态枚举
namespace OrderState {
    constexpr int Empty         = 0; // 槽位空闲，可被占用
    constexpr int Pending       = 1; // 已报单，等待成交
    constexpr int PartialFilled = 2; // 部分成交，仍在挂单
    constexpr int Filled        = 3; // 全部成交
    constexpr int Cancelled     = 4; // 已撤单
    constexpr int Rejected      = 5; // 被拒绝
}

// 单个订单槽位，alignas(64) 独占 cache line，防止不同槽位间的 false sharing
struct alignas(64) OrderSlot {
    std::atomic<int> state{OrderState::Empty}; // 槽位状态，原子操作保证线程安全
    char   order_ref[13];  // 报单引用（12位编码 + \0），末3位为槽位下标
    char   instrument[32]; // 合约代码
    char   direction;      // 方向：'0'=买，'1'=卖
    char   offset;         // 开平标志
    double price;          // 报单价格
    int    vol_total;      // 报单数量
    int    vol_traded;     // 已成交数量
    std::chrono::steady_clock::time_point insert_time; // 报单时间，用于超时检测
};

constexpr int MAX_ORDERS = 128; // 最大并发订单数

// 无锁订单管理器
// 核心优化：OrderRef 末3位编码槽位下标，CTP 回调时 O(1) 定位，无需遍历
class OrderManager {
public:
    // 内存预热：遍历所有槽位触发物理页分配，避免开盘 Page Fault
    void WarmUp();

    // 分配一个空闲槽位并填写订单信息
    // order_ref 格式：前9位为递增序号，末3位为槽位下标（如 "000000001042" → slot[42]）
    // 返回编码后的 order_ref 字符串，槽位满时返回 nullptr
    // [[nodiscard]] 防止调用方忽略返回值导致槽位泄漏
    [[nodiscard]] const char* AllocSlot(int base_ref, const char* inst,
                                         char dir, char offset,
                                         double price, int vol);

    // 兼容接口：通过已编码的 ref 解析槽位下标，返回下标（失败返回 -1）
    int  OnOrderSent(const char* ref, const char* inst,
                     char dir, char offset, double price, int vol);

    // CTP 回调线程调用：O(1) 解码下标，更新槽位状态
    // 全部成交/撤单后自动将槽位状态重置为 Empty（回收）
    void OnOrderUpdate(CThostFtdcOrderField* p);

    // 报单被拒时释放槽位，记录错误日志
    void OnOrderRejected(const char* ref, int err_id, const char* err_msg);

    // 返回全局活跃挂单数（Pending + PartialFilled）
    int  GetActiveCount() const;

    // 返回指定合约的活跃挂单数（std::string 重载）
    int  GetActiveCount(const std::string& inst) const;

    // 返回指定合约的活跃挂单数（const char* 重载，热路径使用）
    int  GetActiveCountByInst(const char* inst) const;

    // 遍历所有超时挂单，对每个超时槽位执行回调 cb(OrderSlot&)
    // 由超时撤单守护线程每秒调用
    template<typename Fn>
    void ForEachTimeout(int timeout_ms, Fn cb) {
        auto now = std::chrono::steady_clock::now();
        for (int i = 0; i < MAX_ORDERS; ++i) {
            int st = m_slots[i].state.load(std::memory_order_acquire);
            if (st != OrderState::Pending && st != OrderState::PartialFilled) continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_slots[i].insert_time).count();
            if (elapsed >= timeout_ms) cb(m_slots[i]);
        }
    }

    // 断线重连时重置所有槽位为 Empty，配合 QueryOpenOrders 重建状态
    void Reset();

    // 自成交保护检查：新报单是否会与已有挂单成交
    // 买单价格 >= 已有卖单价格，或卖单价格 <= 已有买单价格时返回 true
    bool WouldSelfMatch(const char* inst, double price, char direction) const;

    // 直接访问指定下标的槽位（供 TdEngine 撤单时使用）
    OrderSlot& GetSlot(int idx) { return m_slots[idx]; }

    // 公开解码接口，供 TdEngine::OnRspOrderAction 使用
    static int DecodeIndexPublic(const char* ref) { return DecodeIndex(ref); }

private:
    std::array<OrderSlot, MAX_ORDERS> m_slots{};

    // O(1) 解码：取 order_ref 末3位转为整数作为槽位下标
    static int DecodeIndex(const char* ref);
};
