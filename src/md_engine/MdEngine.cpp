#include "MdEngine.h"
#include "Logger.h"
#include "Tsc.h"
#include <cstring>

MdEngine::MdEngine() : m_api(nullptr) {}

void MdEngine::Init(const std::string& front, const std::string& broker,
                    const std::string& user, const std::string& pw,
                    const std::vector<std::string>& instruments) {
    m_instruments = instruments;

    // 预填充合约表，同时初始化去重缓存
    m_inst_count = (int)instruments.size();
    if (m_inst_count > MAX_INST) m_inst_count = MAX_INST;
    for (int i = 0; i < m_inst_count; ++i) {
        strncpy(m_last_tick[i].inst_id, instruments[i].c_str(), 31);
        m_last_tick[i].inst_id[31] = '\0';
    }

    g_tick_pool.WarmUp();
    m_api = CThostFtdcMdApi::CreateFtdcMdApi("./flow/md/");
    m_api->RegisterSpi(this);
    m_api->RegisterFront((char*)front.c_str());
    m_broker = broker; m_user = user; m_pw = pw;
    m_api->Init();
    m_api->Join();
}

void MdEngine::OnFrontConnected() {
    CThostFtdcReqUserLoginField req = {};
    strcpy(req.BrokerID, m_broker.c_str());
    strcpy(req.UserID,   m_user.c_str());
    strcpy(req.Password, m_pw.c_str());
    m_api->ReqUserLogin(&req, 1);
}

void MdEngine::OnFrontDisconnected(int nReason) {
    LOG_WARN("[Md] 行情断线，原因=%d，等待重连...", nReason);
    // 重连后交易所会重推行情，volume 从0开始，时间戳也会重置
    // 清空 update_key 缓存，避免重连后第一批 tick 被误判为重复
    for (int i = 0; i < m_inst_count; ++i)
        m_last_tick[i].update_key = 0;
}

void MdEngine::OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*,
                               int, bool) {
    LOG_INFO("[Md] 登录成功，订阅 %zu 个合约", m_instruments.size());
    std::vector<char*> ids;
    ids.reserve(m_instruments.size());
    for (auto& s : m_instruments) ids.push_back((char*)s.c_str());
    m_api->SubscribeMarketData(ids.data(), (int)ids.size());
}

void MdEngine::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) {
    const uint64_t t1_tsc = Tsc::Now();

    if (!p) return;
    if (!IsValid(p)) return;

    int idx = FindInstIdx(p->InstrumentID);
    if (UNLIKELY(idx < 0)) return;

    LastTick& last = m_last_tick[idx];

    // 用交易所时间戳做去重和乱序检测
    // UpdateTime 格式 "HH:MM:SS"，转为 HHMMSS * 1000 + UpdateMillisec 的整数 key
    // 比单纯用 volume 更可靠：
    //   1. 重连后 volume 归零，旧 tick 不会被误当新 tick 放行
    //   2. 乱序到达的旧 tick（key <= last.update_key）直接丢弃
    const char* t = p->UpdateTime;
    int update_key = ((t[0]-'0')*10 + (t[1]-'0')) * 10000000
                   + ((t[3]-'0')*10 + (t[4]-'0')) * 100000
                   + ((t[6]-'0')*10 + (t[7]-'0')) * 1000
                   + p->UpdateMillisec;

    if (LIKELY(update_key != 0 && update_key <= last.update_key))
        return; // 乱序或重复，丢弃

    last.update_key = update_key;
    last.volume     = p->Volume;
    last.last_price = p->LastPrice;

    g_tick_pool.Write(*p, t1_tsc);
}

bool MdEngine::IsValid(const CThostFtdcDepthMarketDataField* p) const {
    if (p->LastPrice <= 0.0) return false;
    if (p->UpperLimitPrice > 0.0 && p->LastPrice > p->UpperLimitPrice) return false;
    if (p->LowerLimitPrice > 0.0 && p->LastPrice < p->LowerLimitPrice) return false;
    return true;
}

int MdEngine::FindInstIdx(const char* inst_id) const {
    for (int i = 0; i < m_inst_count; ++i)
        if (strncmp(m_last_tick[i].inst_id, inst_id, 31) == 0) return i;
    return -1;
}
