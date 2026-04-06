#include "TdEngine.h"
#include "Logger.h"
#include <cstring>
#include <chrono>
#include <algorithm>

// 默认构造，使用默认风控参数
TdEngine::TdEngine() : m_risk(m_account, m_oms) {}

// 自定义风控参数构造
TdEngine::TdEngine(const RiskConfig& cfg) : m_risk(m_account, m_oms, cfg) {}

// 析构：停止守护线程，更新共享内存状态为 STOPPED，销毁共享内存段
TdEngine::~TdEngine() {
    m_cancel_guard_running = false;
    if (m_shm) {
        strncpy(m_shm->status, "STOPPED", 31);
        ShmMonitor::Destroy();
    }
}

// 初始化交易引擎：预热 OMS 内存，创建共享内存，连接 CTP，启动守护线程
void TdEngine::Init(const char* front, const char* b, const char* u,
                    const char* p, const char* app, const char* auth) {
    m_front_str = front;
    m_broker = b; m_user = u; m_pw = p; m_app = app; m_auth = auth;

    m_oms.WarmUp(); // 预热 OMS 槽位物理页

    m_shm = ShmMonitor::Create();
    if (m_shm) {
        strncpy(m_shm->status, "CONNECTING", 31);
        LOG_INFO("[Td] 共享内存监控已启动: %s", ShmMonitor::SHM_NAME);
    }

    ConnectApi();

    m_cancel_guard_running = true;
    m_cancel_guard = std::thread(&TdEngine::CancelGuardLoop, this);
    m_cancel_guard.detach();
}

// 前置连接成功，发起客户端认证（AppID + AuthCode）
void TdEngine::OnFrontConnected() {
    LOG_INFO("[Td] 连接成功，正在认证...");
    if (m_shm) strncpy(m_shm->status, "AUTHENTICATING", 31);
    CThostFtdcReqAuthenticateField req = {};
    strcpy(req.BrokerID, m_broker.c_str());
    strcpy(req.UserID,   m_user.c_str());
    strcpy(req.AppID,    m_app.c_str());
    strcpy(req.AuthCode, m_auth.c_str());
    m_api->ReqAuthenticate(&req, ++m_requestID);
}

// 前置断线：重置 isReady，阻止策略线程继续报单，等待 CTP 自动重连
void TdEngine::OnFrontDisconnected(int nReason) {
    isReady = false;
    LOG_WARN("[Td] 交易断线，原因=%d，等待重连...", nReason);
    if (m_shm) strncpy(m_shm->status, "RECONNECTING", 31);
}

// 认证成功，发起账号密码登录
void TdEngine::OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField*, int, bool) {
    CThostFtdcReqUserLoginField req = {};
    strcpy(req.BrokerID, m_broker.c_str());
    strcpy(req.UserID,   m_user.c_str());
    strcpy(req.Password, m_pw.c_str());
    m_api->ReqUserLogin(&req, ++m_requestID);
}

// 登录成功：同步 MaxOrderRef（断线重连时取较大值，避免 OrderRef 重复），发起结算单确认
void TdEngine::OnRspUserLogin(CThostFtdcRspUserLoginField* p, CThostFtdcRspInfoField*, int, bool) {
    if (p) {
        int new_max = atoi(p->MaxOrderRef);
        if (new_max > m_maxOrderRef) {
            m_maxOrderRef = new_max;
            LOG_INFO("[Td] 重连同步 MaxOrderRef=%d", m_maxOrderRef);
        }
        m_front_id   = p->FrontID;
        m_session_id = p->SessionID;
    }
    CThostFtdcSettlementInfoConfirmField req = {};
    strcpy(req.BrokerID,   m_broker.c_str());
    strcpy(req.InvestorID, m_user.c_str());
    m_api->ReqSettlementInfoConfirm(&req, ++m_requestID);
}

// 结算单确认完成：置 isReady=true，查询资金持仓，重建 OMS
void TdEngine::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField*, int, bool) {
    LOG_INFO("[Td] 交易链路就绪，查询资金与持仓...");
    isReady = true;
    if (m_shm) strncpy(m_shm->status, "RUNNING", 31);
    QueryAccount();
    QueryPosition();
    QueryOpenOrders(); // 断线重连后重建 OMS
}

// 发起资金查询请求
void TdEngine::QueryAccount() {
    CThostFtdcQryTradingAccountField req = {};
    strcpy(req.BrokerID,   m_broker.c_str());
    strcpy(req.InvestorID, m_user.c_str());
    m_api->ReqQryTradingAccount(&req, ++m_requestID);
}

// 资金查询回报：更新 AccountMetrics 三个原子字段，刷新共享内存
void TdEngine::OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField*, int, bool) {
    if (!p) return;
    m_account.available.store(p->Available);
    m_account.curr_margin.store(p->CurrMargin);
    m_account.frozen_margin.store(p->FrozenMargin);
    LOG_INFO("[Td] 资金: 可用=%.2f 占用保证金=%.2f", p->Available, p->CurrMargin);
    UpdateShm();
}

// 发起持仓查询请求
void TdEngine::QueryPosition() {
    CThostFtdcQryInvestorPositionField req = {};
    strcpy(req.BrokerID,   m_broker.c_str());
    strcpy(req.InvestorID, m_user.c_str());
    m_api->ReqQryInvestorPosition(&req, ++m_requestID);
}

// 持仓查询回报：按多空方向更新昨仓、今仓和均价
void TdEngine::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p, CThostFtdcRspInfoField*, int, bool bIsLast) {
    if (p) {
        auto& ip = GetOrCreateInstPos(p->InstrumentID);
        if (p->PosiDirection == THOST_FTDC_PD_Long) {
            ip.pos.long_yd.store(p->YdPosition);
            ip.pos.long_td.store(p->TodayPosition);
            ip.pos.long_avg_price.store(p->OpenCost / std::max(p->Position, 1));
        } else {
            ip.pos.short_yd.store(p->YdPosition);
            ip.pos.short_td.store(p->TodayPosition);
        }
        LOG_INFO("[Td] 持仓: %s %s 昨=%d 今=%d",
            p->InstrumentID,
            p->PosiDirection == THOST_FTDC_PD_Long ? "多" : "空",
            p->YdPosition, p->TodayPosition);
    }
    if (bIsLast) { LOG_INFO("[Td] 持仓查询完毕"); UpdateShm(); }
}

// 断线重连后重建 OMS：先重置所有槽位，再查询交易所挂单
void TdEngine::QueryOpenOrders() {
    m_oms.Reset();
    CThostFtdcQryOrderField req = {};
    strcpy(req.BrokerID,   m_broker.c_str());
    strcpy(req.InvestorID, m_user.c_str());
    m_api->ReqQryOrder(&req, ++m_requestID);
    LOG_INFO("[Td] 查询挂单以重建 OMS...");
}

// 挂单查询回报：只重建仍在挂单中的订单（NoTradeQueueing / PartTradedQueueing）
void TdEngine::OnRspQryOrder(CThostFtdcOrderField* p, CThostFtdcRspInfoField*, int, bool bIsLast) {
    if (p) {
        if (p->OrderStatus == THOST_FTDC_OST_NoTradeQueueing ||
            p->OrderStatus == THOST_FTDC_OST_PartTradedQueueing) {
            m_oms.OnOrderSent(p->OrderRef, p->InstrumentID,
                              p->Direction, p->CombOffsetFlag[0],
                              p->LimitPrice, p->VolumeTotalOriginal);
            LOG_INFO("[Td] 重建挂单: ref=%s %s", p->OrderRef, p->InstrumentID);
        }
    }
    if (bIsLast) LOG_INFO("[Td] OMS 重建完毕");
}

// 报单被拒：通知 OMS 释放槽位，解冻预估保证金
void TdEngine::OnRspOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* r, int, bool) {
    if (r && r->ErrorID != 0) {
        const char* ref = p ? p->OrderRef : "unknown";
        m_oms.OnOrderRejected(ref, r->ErrorID, r->ErrorMsg);
        if (p) {
            double est = p->LimitPrice * m_account.multiplier
                         * m_account.margin_ratio * p->VolumeTotalOriginal;
            m_account.update_available(est);
            m_account.frozen_margin.store(
                std::max(0.0, m_account.frozen_margin.load() - est));
        }
    }
}

// 报单状态变化：通知 OMS 更新槽位状态，刷新共享内存
void TdEngine::OnRtnOrder(CThostFtdcOrderField* p) {
    if (!p) return;
    m_oms.OnOrderUpdate(p);
    UpdateShm();
}

// 成交回报：更新持仓均价，计算 PnL，触发资金刷新
void TdEngine::OnRtnTrade(CThostFtdcTradeField* p) {
    if (!p) return;
    LOG_INFO("[Td] 成交: %s 价=%.2f 量=%d 方向=%c",
        p->InstrumentID, p->Price, p->Volume, p->Direction);

    auto& ip = GetOrCreateInstPos(p->InstrumentID);
    auto& pos = ip.pos;
    if (p->Direction == THOST_FTDC_D_Buy) {
        if (p->OffsetFlag == THOST_FTDC_OF_Open) {
            // 买开：更新多头今仓和加权均价
            int old_vol = pos.long_td.load();
            pos.long_td.fetch_add(p->Volume);
            int new_vol = pos.long_td.load();
            if (new_vol > 0)
                pos.long_avg_price.store(
                    (pos.long_avg_price.load() * old_vol + p->Price * p->Volume) / new_vol);
        } else {
            // 买平：减少空头持仓，计算平仓盈亏
            double avg = pos.long_avg_price.load();
            m_risk.UpdatePnl((p->Price - avg) * p->Volume * m_account.multiplier);
            pos.short_td.fetch_add(-p->Volume);
            pos.short_yd.fetch_add(-p->Volume);
        }
    } else {
        if (p->OffsetFlag == THOST_FTDC_OF_Open) {
            // 卖开：增加空头今仓
            pos.short_td.fetch_add(p->Volume);
        } else {
            // 卖平：减少多头持仓，计算平仓盈亏
            double avg = pos.long_avg_price.load();
            m_risk.UpdatePnl((p->Price - avg) * p->Volume * m_account.multiplier);
            pos.long_td.fetch_add(-p->Volume);
            pos.long_yd.fetch_add(-p->Volume);
        }
    }
    QueryAccount(); // 成交后刷新资金
}

// 撤单失败回报，记录错误日志
void TdEngine::OnRspOrderAction(CThostFtdcInputOrderActionField*, CThostFtdcRspInfoField* r, int, bool) {
    if (r && r->ErrorID != 0)
        LOG_ERROR("[Td] 撤单失败: ErrID=%d Msg=%s", r->ErrorID, r->ErrorMsg);
}

// 风控前置报单：七道风控检查 → 分配 OMS 槽位 → 预冻结保证金 → 发送限价单
// 返回 order_ref，风控拦截或槽位满时返回空字符串
std::string TdEngine::SendOrder(const char* inst, double price, char dir,
                                 char offset, int vol) {
    int net = GetNetLong(inst);
    if (!m_risk.CheckOrder(inst, price, vol, net, dir)) return "";

    CThostFtdcInputOrderField req = {};
    strcpy(req.BrokerID,     m_broker.c_str());
    strcpy(req.InvestorID,   m_user.c_str());
    strcpy(req.InstrumentID, inst);

    // AllocSlot 将槽位下标编码进 order_ref 末3位，实现 O(1) 回调查找
    const char* ref_str = m_oms.AllocSlot(++m_maxOrderRef, inst, dir, offset, price, vol);
    if (!ref_str) return "";

    snprintf(req.OrderRef, sizeof(req.OrderRef), "%s", ref_str);
    req.OrderPriceType      = THOST_FTDC_OPT_LimitPrice;
    req.LimitPrice          = price;
    req.VolumeTotalOriginal = vol;
    req.Direction           = dir;
    req.CombOffsetFlag[0]   = offset;
    req.CombHedgeFlag[0]    = THOST_FTDC_HF_Speculation;
    req.TimeCondition       = THOST_FTDC_TC_GFD;
    req.VolumeCondition     = THOST_FTDC_VC_AV;
    req.ContingentCondition = THOST_FTDC_CC_Immediately;
    req.ForceCloseReason    = THOST_FTDC_FCC_NotForceClose;
    req.IsAutoSuspend       = 0;
    req.MinVolume           = 0;

    // 预冻结保证金，报单被拒时在 OnRspOrderInsert 中解冻
    double est = price * m_account.multiplier * m_account.margin_ratio * vol;
    m_account.frozen_margin.store(m_account.frozen_margin.load() + est);
    m_account.update_available(-est);

    m_api->ReqOrderInsert(&req, ++m_requestID);

    // 更新共享内存中的最近报单时间戳
    if (m_shm) {
        m_shm->last_order_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    return req.OrderRef;
}

// 撤单：通过撤单频率风控检查后发送撤单请求
bool TdEngine::CancelOrder(const OrderSlot& slot) {
    if (!m_risk.CheckCancel()) return false;

    CThostFtdcInputOrderActionField req = {};
    strcpy(req.BrokerID,     m_broker.c_str());
    strcpy(req.InvestorID,   m_user.c_str());
    strcpy(req.InstrumentID, slot.instrument);
    strcpy(req.OrderRef,     slot.order_ref);
    req.FrontID    = m_front_id;
    req.SessionID  = m_session_id;
    req.ActionFlag = THOST_FTDC_AF_Delete;

    LOG_INFO("[Td] 发起撤单: ref=%s %s", slot.order_ref, slot.instrument);
    m_api->ReqOrderAction(&req, ++m_requestID);
    return true;
}

// 平多仓：遵循上期所规则，优先平今仓（CloseToday），再平昨仓（Close）
std::string TdEngine::CloseOrder(const char* inst, double price, int vol) {
    auto* ip = FindInstPos(inst);
    if (!ip) return "";

    auto& pos = ip->pos;
    int td = pos.long_td.load(std::memory_order_relaxed);
    int yd = pos.long_yd.load(std::memory_order_relaxed);

    char offset   = (td > 0) ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Close;
    int close_vol = std::min(vol, td > 0 ? td : yd);
    if (close_vol <= 0) {
        LOG_WARN("[Td] %s 无可平多仓", inst);
        return "";
    }
    return SendOrder(inst, price, THOST_FTDC_D_Sell, offset, close_vol);
}

// 快照式读取净多头：四次 relaxed load，策略线程单线程调用无撕裂风险
int TdEngine::GetNetLong(const char* inst) {
    auto* ip = FindInstPos(inst);
    if (!ip) return 0;
    auto& p = ip->pos;
    return (p.long_yd.load(std::memory_order_relaxed) +
            p.long_td.load(std::memory_order_relaxed)) -
           (p.short_yd.load(std::memory_order_relaxed) +
            p.short_td.load(std::memory_order_relaxed));
}

// 创建 CTP TraderApi 实例，注册 SPI 和前置地址，发起连接
void TdEngine::ConnectApi() {
    m_api = CThostFtdcTraderApi::CreateFtdcTraderApi("./flow/td/");
    m_api->RegisterSpi(this);
    m_api->SubscribePrivateTopic(THOST_TERT_QUICK);
    m_api->RegisterFront((char*)m_front_str.c_str());
    m_api->Init();
}

// 将账户资金、持仓、活跃挂单数写入共享内存快照（非热路径，CTP 回调线程调用）
void TdEngine::UpdateShm() {
    if (!m_shm) return;
    m_shm->available     = m_account.available.load(std::memory_order_relaxed);
    m_shm->frozen_margin = m_account.frozen_margin.load(std::memory_order_relaxed);
    m_shm->curr_margin   = m_account.curr_margin.load(std::memory_order_relaxed);
    m_shm->active_orders = m_oms.GetActiveCount();

    int cnt = m_inst_count.load(std::memory_order_relaxed);
    m_shm->inst_count = cnt;
    for (int i = 0; i < cnt && i < ShmSnapshot::MAX_INST; ++i) {
        strncpy(m_shm->positions[i].name, m_positions[i].name, 31);
        m_shm->positions[i].net_long  = GetNetLong(m_positions[i].name);
        m_shm->positions[i].avg_price =
            m_positions[i].pos.long_avg_price.load(std::memory_order_relaxed);
    }
}

// 按合约名查找持仓槽位，不存在时原子递增 m_inst_count 创建新槽位
TdEngine::InstPos& TdEngine::GetOrCreateInstPos(const char* name) {
    int cnt = m_inst_count.load(std::memory_order_relaxed);
    for (int i = 0; i < cnt; ++i)
        if (strncmp(m_positions[i].name, name, 31) == 0) return m_positions[i];
    int idx = m_inst_count.fetch_add(1, std::memory_order_relaxed);
    snprintf(m_positions[idx].name, sizeof(m_positions[idx].name), "%.31s", name);
    return m_positions[idx];
}

// 按合约名查找持仓槽位，不存在时返回 nullptr
TdEngine::InstPos* TdEngine::FindInstPos(const char* name) {
    int cnt = m_inst_count.load(std::memory_order_relaxed);
    for (int i = 0; i < cnt; ++i)
        if (strncmp(m_positions[i].name, name, 31) == 0) return &m_positions[i];
    return nullptr;
}

// 超时撤单守护线程：每秒扫描 OMS，对超过 3000ms 未成交的挂单自动撤单
void TdEngine::CancelGuardLoop() {
    while (m_cancel_guard_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!isReady) continue;
        m_oms.ForEachTimeout(3000, [this](OrderSlot& slot) {
            LOG_WARN("[OMS] 超时挂单撤销: ref=%s %s", slot.order_ref, slot.instrument);
            CancelOrder(slot);
        });
    }
}
