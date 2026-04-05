#pragma once
#include <string>
#include <vector>

// 系统配置结构体，由 config.xml 解析填充
struct Config {
    std::string trade_front;   // 交易前置地址
    std::string market_front;  // 行情前置地址
    std::string broker_id;     // 经纪商代码
    std::string user_id;       // 投资者账号
    std::string password;      // 登录密码
    std::string app_id;        // 客户端 AppID（认证用）
    std::string auth_code;     // 客户端认证码

    double threshold_price;                // 策略价格阈值
    std::vector<std::string> instruments;  // 订阅的合约列表

    // 从 XML 文件加载配置，解析失败时返回空 Config（user_id 为空）
    static Config Load(const std::string& filename);
};
