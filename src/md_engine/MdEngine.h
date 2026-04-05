#pragma once
#include "ThostFtdcMdApi.h"
#include "TickPool.h"
#include "Logger.h"
#include <vector>
#include <string>
#include <array>
#include <cstring>

#ifndef LIKELY
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

class MdEngine : public CThostFtdcMdSpi {
public:
    MdEngine();

    void Init(const std::string& front, const std::string& broker,
              const std::string& user, const std::string& pw,
              const std::vector<std::string>& instruments);

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*,
                        int, bool) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) override;

private:
    bool IsValid(const CThostFtdcDepthMarketDataField* p) const;
    // 返回合约下标，-1 表示未订阅
    int  FindInstIdx(const char* inst_id) const;

    CThostFtdcMdApi* m_api;
    std::vector<std::string> m_instruments;
    std::string m_broker, m_user, m_pw;

    static constexpr int MAX_INST = 16;
    int  m_inst_count = 0;

    // 去重缓存：按合约下标索引，数组访问 O(1)，无哈希开销
    struct LastTick {
        char   inst_id[32]{};
        double last_price = 0.0;
        int    volume     = 0;
    };
    std::array<LastTick, MAX_INST> m_last_tick{};
};
