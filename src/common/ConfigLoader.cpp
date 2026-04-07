#include "ConfigLoader.h"
#include "tinyxml2.h"
#include <iostream>

// 解析 config.xml，填充并返回 Config 结构体
// XML 结构：AppConfig > Network / Account / Strategy
Config Config::Load(const std::string& filename) {
    Config conf;
    tinyxml2::XMLDocument doc;

    if (doc.LoadFile(filename.c_str()) != tinyxml2::XML_SUCCESS) {
        std::cerr << "无法加载 XML 文件: " << filename << std::endl;
        return conf;
    }

    tinyxml2::XMLElement* root = doc.FirstChildElement("AppConfig");
    if (!root) return conf;

    // 解析交易/行情前置地址
    auto* net = root->FirstChildElement("Network");
    if (net) {
        conf.trade_front  = net->FirstChildElement("TradeFront")->GetText();
        conf.market_front = net->FirstChildElement("MarketFront")->GetText();
    }

    // 解析账户信息
    auto* acc = root->FirstChildElement("Account");
    if (acc) {
        conf.broker_id  = acc->FirstChildElement("BrokerID")->GetText();
        conf.user_id    = acc->FirstChildElement("UserID")->GetText();
        conf.password   = acc->FirstChildElement("Password")->GetText();
        conf.app_id     = acc->FirstChildElement("AppID")->GetText();
        conf.auth_code  = acc->FirstChildElement("AuthCode")->GetText();
    }

    // 解析策略参数
    auto* strat = root->FirstChildElement("Strategy");
    if (strat) {
        auto* sp = strat->FirstChildElement("Spread");
        conf.spread_ticks = sp ? sp->IntText(2) : 2;
        auto* mp = strat->FirstChildElement("MaxNetPos");
        conf.max_net_pos  = mp ? mp->IntText(3) : 3;
        auto* insts = strat->FirstChildElement("Instruments");
        if (insts) {
            for (auto* e = insts->FirstChildElement("Instrument");
                 e != nullptr; e = e->NextSiblingElement("Instrument"))
                conf.instruments.push_back(e->GetText());
        }
    }

    return conf;
}
