#pragma once
#include <string>
#include <vector>

// 系统配置结构体，由 config.xml 解析填充
struct Config {
    std::string trade_front;
    std::string market_front;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;

    int    spread_ticks;                   // 做市挂单价差（tick 数）
    int    max_net_pos;                    // 单合约最大净持仓
    std::vector<std::string> instruments;

    static Config Load(const std::string& filename);
};
