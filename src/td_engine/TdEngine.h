#pragma once
#include "ThostFtdcTraderApi.h"
#include "future_struct.h"
#include "ShmMonitor.h"
#include "OrderManager.h"
#include "RiskManager.h"
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <thread>

// 交易引擎：继承 CThostFtdcTraderSpi，处理 CTP 交易回调
// 职责：连接认证登录 → 查询资金持仓 → 报单撤单 → 维护 OMS 和持仓表 → 断线重连
class TdEngine : public CThostFtdcTraderSpi {
public:
    bool isReady    = false; // 交易链路就绪标志，Strategy 线程轮询
    int  m_requestID   = 0; // 请求序号，每次 API 调用自增
    int  m_maxOrderRef = 0; // 当前最大 OrderRef，断线重连后从交易所同步

    AccountMetrics m_account; // 账户资金，CTP 回调线程写，风控模块读

    static constexpr int MAX_INST = 16; // 最大持仓合约数
    struct InstPos {
        char            name[32]{};  // 合约代码
        PositionMetrics pos{};       // 持仓数据（内部全为原子变量）
    };
    std::array<InstPos, MAX_INST> m_positions{}; // 固定大小持仓表，线性查找，cache 友好
    std::atomic<int> m_inst_count{0};            // 当前持仓合约数

    OrderManager m_oms;          // 订单管理器
    RiskManager  m_risk;         // 风控管理器
    ShmSnapshot* m_shm = nullptr; // 共享内存快照，nullptr 表示未启用

    // 默认构造，使用默认风控参数
    TdEngine();

    // 自定义风控参数构造
    explicit TdEngine(const RiskConfig& cfg);

    // 析构：停止守护线程，更新共享内存状态，销毁共享内存段
    ~TdEngine();

    // 初始化：预热 OMS 内存，创建共享内存，连接 CTP，启动超时撤单守护线程
    // instruments 用于预初始化持仓槽位顺序，与 MdEngine 的 inst_idx 对齐
    void Init(const char* front, const char* b, const char* u,
              const char* p, const char* app, const char* auth,
              const std::vector<std::string>& instruments = {});

    // ---- CTP 回调 ----

    // 前置连接成功，发起客户端认证
    void OnFrontConnected() override;

    // 前置断线，重置 isReady，等待 CTP 自动重连
    void OnFrontDisconnected(int nReason) override;

    // 认证成功，发起登录
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField*, int, bool) override;

    // 登录成功，同步 MaxOrderRef（断线重连时更新），发起结算单确认
    void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*, int, bool) override;

    // 结算单确认完成，置 isReady=true，查询资金持仓，重建 OMS
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField*, int, bool) override;

    // 资金查询回报，更新 AccountMetrics 和共享内存快照
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField*, CThostFtdcRspInfoField*, int, bool) override;

    // 持仓查询回报，更新持仓表
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField*, CThostFtdcRspInfoField*, int, bool) override;

    // 挂单查询回报，断线重连后重建 OMS 内存状态
    void OnRspQryOrder(CThostFtdcOrderField*, CThostFtdcRspInfoField*, int, bool) override;

    // 报单被拒，通知 OMS 释放槽位，解冻预估保证金
    void OnRspOrderInsert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField*, int, bool) override;

    // 报单状态变化回报，通知 OMS 更新槽位状态
    void OnRtnOrder(CThostFtdcOrderField*) override;

    // 成交回报，更新持仓均价，触发 PnL 计算，刷新资金
    void OnRtnTrade(CThostFtdcTradeField*) override;

    // 撤单失败回报，记录错误日志
    void OnRspOrderAction(CThostFtdcInputOrderActionField*, CThostFtdcRspInfoField*, int, bool) override;

    // ---- 业务接口 ----

    // 风控前置报单：通过风控检查后分配 OMS 槽位，发送限价单
    // 返回 order_ref 字符串，风控拦截或槽位满时返回空字符串
    std::string SendOrder(const char* inst, double price, char dir,
                          char offset = THOST_FTDC_OF_Open, int vol = 1);

    // 撤单：通过撤单频率检查后发送撤单请求
    bool CancelOrder(const OrderSlot& slot);

    // 平多仓：上期所规则，优先平昨仓（Close），昨仓不足再平今仓（CloseToday）
    std::string CloseOrder(const char* inst, double price, int vol);

    // 快照式读取指定合约的净多头手数（多头总量 - 空头总量）
    int GetNetLong(const char* inst);

    // 按合约下标直接读取净多头（O(1)，热路径使用）
    int GetNetLongByIdx(int inst_idx);

private:
    CThostFtdcTraderApi* m_api = nullptr;
    std::string m_broker, m_user, m_pw, m_app, m_auth, m_front_str;
    int m_front_id   = 0; // 前置编号，撤单时需要
    int m_session_id = 0; // 会话编号，撤单时需要

    std::atomic<bool> m_cancel_guard_running{false};
    std::thread       m_cancel_guard; // 超时撤单守护线程

    // 创建 CTP TraderApi 实例并发起连接
    void ConnectApi();

    // 发起资金查询请求
    void QueryAccount();

    // 发起持仓查询请求
    void QueryPosition();

    // 断线重连后重置 OMS 并查询所有挂单以重建状态
    void QueryOpenOrders();

    // 将账户、持仓、订单状态写入共享内存快照（非热路径）
    void UpdateShm();

    // 按合约名查找持仓槽位，不存在时原子递增创建新槽位
    InstPos& GetOrCreateInstPos(const char* name);

    // 按合约名查找持仓槽位，不存在时返回 nullptr
    InstPos* FindInstPos(const char* name);

    // 超时撤单守护线程：每秒扫描 OMS，对超过 3 秒未成交的挂单发起撤单
    void CancelGuardLoop();
};
