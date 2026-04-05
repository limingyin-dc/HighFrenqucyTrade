#include "OrderManager.h"
#include "Logger.h"

// 遍历所有槽位做一次 volatile 读，触发物理页分配和 TLB 加载
// 在系统启动阶段调用，避免开盘第一笔报单触发 Page Fault（约 1~10 微秒延迟）
void OrderManager::WarmUp() {
    for (int i = 0; i < MAX_ORDERS; ++i) {
        volatile int s = m_slots[i].state.load(std::memory_order_relaxed);
        (void)s;
    }
    LOG_INFO("[OMS] 内存预热完成，%d 个 OrderSlot 已加载到物理页", MAX_ORDERS);
}

// CAS 遍历槽位数组，找到第一个 Empty 槽并原子地将其置为 Pending
// order_ref 编码：snprintf("%09d%03d", base_ref, slot_index)
// 末3位存槽位下标，回调时 O(1) 解码，无需遍历
const char* OrderManager::AllocSlot(int base_ref, const char* inst,
                                     char dir, char offset,
                                     double price, int vol) {
    for (int i = 0; i < MAX_ORDERS; ++i) {
        int expected = OrderState::Empty;
        if (m_slots[i].state.compare_exchange_strong(
                expected, OrderState::Pending,
                std::memory_order_acq_rel)) {
            auto& s = m_slots[i];
            snprintf(s.order_ref, sizeof(s.order_ref), "%09d%03d", base_ref, i);
            strncpy(s.instrument, inst, 31); s.instrument[31] = '\0';
            s.direction   = dir;
            s.offset      = offset;
            s.price       = price;
            s.vol_total   = vol;
            s.vol_traded  = 0;
            s.insert_time = std::chrono::steady_clock::now();
            LOG_INFO("[OMS] 新报单: ref=%s %s 方向=%c 价=%.2f 量=%d slot=%d",
                     s.order_ref, inst, dir, price, vol, i);
            return s.order_ref;
        }
    }
    LOG_WARN("[OMS] 订单槽位已满");
    return nullptr;
}

// 通过已编码的 ref 解析槽位下标，用于断线重连后重建 OMS 状态
int OrderManager::OnOrderSent(const char* ref, const char* inst,
                               char dir, char offset, double price, int vol) {
    int idx = DecodeIndex(ref);
    if (idx < 0 || idx >= MAX_ORDERS) return -1;
    (void)inst; (void)dir; (void)offset; (void)price; (void)vol;
    return idx;
}

// CTP 回调线程调用：O(1) 解码下标，根据订单状态更新槽位
// 全部成交或撤单后将槽位重置为 Empty，自动回收
void OrderManager::OnOrderUpdate(CThostFtdcOrderField* p) {
    if (!p) return;
    int idx = DecodeIndex(p->OrderRef);
    if (idx < 0) return;

    auto& s = m_slots[idx];
    s.vol_traded = p->VolumeTraded;

    switch (p->OrderStatus) {
        case THOST_FTDC_OST_AllTraded:
            LOG_INFO("[OMS] 全部成交: ref=%s slot=%d", p->OrderRef, idx);
            s.state.store(OrderState::Empty, std::memory_order_release);
            break;
        case THOST_FTDC_OST_PartTradedQueueing:
        case THOST_FTDC_OST_PartTradedNotQueueing:
            LOG_INFO("[OMS] 部分成交: ref=%s 已成=%d", p->OrderRef, p->VolumeTraded);
            s.state.store(OrderState::PartialFilled, std::memory_order_release);
            break;
        case THOST_FTDC_OST_Canceled:
            LOG_INFO("[OMS] 已撤单: ref=%s", p->OrderRef);
            s.state.store(OrderState::Empty, std::memory_order_release);
            break;
        default:
            break;
    }
}

// 报单被拒时释放槽位，防止槽位泄漏
void OrderManager::OnOrderRejected(const char* ref, int err_id, const char* err_msg) {
    int idx = DecodeIndex(ref);
    if (idx >= 0)
        m_slots[idx].state.store(OrderState::Empty, std::memory_order_release);
    LOG_ERROR("[OMS] 报单被拒: ref=%s ErrID=%d %s", ref, err_id, err_msg);
}

// 遍历所有槽位，统计 Pending + PartialFilled 的数量
int OrderManager::GetActiveCount() const {
    int cnt = 0;
    for (int i = 0; i < MAX_ORDERS; ++i) {
        int st = m_slots[i].state.load(std::memory_order_relaxed);
        if (st == OrderState::Pending || st == OrderState::PartialFilled) ++cnt;
    }
    return cnt;
}

// std::string 重载，转发给 const char* 版本
int OrderManager::GetActiveCount(const std::string& inst) const {
    return GetActiveCountByInst(inst.c_str());
}

// 遍历所有槽位，统计指定合约的活跃挂单数
int OrderManager::GetActiveCountByInst(const char* inst) const {
    int cnt = 0;
    for (int i = 0; i < MAX_ORDERS; ++i) {
        int st = m_slots[i].state.load(std::memory_order_relaxed);
        if ((st == OrderState::Pending || st == OrderState::PartialFilled) &&
            strncmp(m_slots[i].instrument, inst, 31) == 0) ++cnt;
    }
    return cnt;
}

// 断线重连时将所有槽位重置为 Empty
// 调用后需通过 ReqQryOrder 查询交易所挂单来重建状态
void OrderManager::Reset() {
    for (int i = 0; i < MAX_ORDERS; ++i)
        m_slots[i].state.store(OrderState::Empty, std::memory_order_relaxed);
    LOG_INFO("[OMS] 所有槽位已重置（断线重连）");
}

// 自成交保护：检查新报单是否会与已有挂单成交
// 新买单价格 >= 已有卖单价格，或新卖单价格 <= 已有买单价格时返回 true
bool OrderManager::WouldSelfMatch(const char* inst, double price, char direction) const {
    for (int i = 0; i < MAX_ORDERS; ++i) {
        int st = m_slots[i].state.load(std::memory_order_relaxed);
        if (st != OrderState::Pending && st != OrderState::PartialFilled) continue;
        if (strncmp(m_slots[i].instrument, inst, 31) != 0) continue;
        if (direction == '0' && m_slots[i].direction == '1' && price >= m_slots[i].price)
            return true;
        if (direction == '1' && m_slots[i].direction == '0' && price <= m_slots[i].price)
            return true;
    }
    return false;
}

// 取 order_ref 末3位转为整数作为槽位下标，O(1) 复杂度
// 例："000000001042" → atoi("042") = 42
int OrderManager::DecodeIndex(const char* ref) {
    if (!ref || ref[0] == '\0') return -1;
    int len = (int)strnlen(ref, 13);
    if (len < 3) return -1;
    int idx = atoi(ref + len - 3);
    if (idx < 0 || idx >= MAX_ORDERS) return -1;
    return idx;
}
