#include "MdEngine.h"
#include "TdEngine.h"
#include "Strategy.h"
#include "ConfigLoader.h"
#include "Logger.h"
#include "Tsc.h"
#include <csignal>
#include <atomic>
#include <thread>
#include <unistd.h>

static std::atomic<bool> g_running{true};

// 信号处理：收到 SIGINT/SIGTERM 时置标志位，主线程退出循环
static void OnSignal(int sig) {
    LOG_WARN("[System] 收到信号 %d，准备退出...", sig);
    g_running = false;
}

int main() {
    // 注册信号，保证 Ctrl+C 能优雅退出并刷新日志
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 加载配置，user_id 为空表示解析失败
    Config conf = Config::Load("config.xml");
    if (conf.user_id.empty()) return -1;

    // 初始化日志系统，启动 flush 线程
    LockFreeLogger::getInstance().init("./logs");

    // 校准 CPU 频率，rdtsc 差值转纳秒依赖此值（耗时约 100ms）
    Tsc::Init();

    // 清理并重建 CTP flow 目录（存放 CTP 会话文件）
    if (system("rm -rf flow/* && mkdir -p flow/md flow/td") != 0) {}

    // ---- 内存预热 ----
    // 在系统启动阶段遍历所有关键缓冲区，触发物理页分配和 TLB 加载
    // 避免开盘瞬间触发 Page Fault 导致几毫秒的延迟抖动
    LOG_INFO("[System] 开始内存预热...");
    g_tick_pool.WarmUp(); // TickPool 1024 槽（约 600KB）
    {
        TickIndexQueue warmup_q;
        warmup_q.WarmUp(); // tick 下标队列缓冲区
    }
    LOG_INFO("[System] 内存预热完成");

    // 行情队列：只传递 tick 下标（4 字节），而非整个结构体（~600 字节）
    TickIndexQueue tickQueue;

    TdEngine td;
    MdEngine md;
    Strategy strategy(td, conf.threshold_price, conf.instruments);

    // 策略线程绑到 core 2，与行情/交易线程隔离
    strategy.Start(2);

    // 交易线程：Init 内部调用 CTP API，阻塞直到断线
    std::thread td_thread([&]() {
        LOG_INFO("[System] 交易线程启动");
        td.Init(conf.trade_front.c_str(),
                conf.broker_id.c_str(),
                conf.user_id.c_str(),
                conf.password.c_str(),
                conf.app_id.c_str(),
                conf.auth_code.c_str());
    });
    td_thread.detach();

    // 行情线程：Init 内部调用 Join() 阻塞
    std::thread md_thread([&]() {
        LOG_INFO("[System] 行情线程启动");
        md.Init(conf.market_front.c_str(),
                conf.broker_id.c_str(),
                conf.user_id.c_str(),
                conf.password.c_str(),
                conf.instruments);
    });
    md_thread.detach();

    // 主线程：每 10 秒打印心跳，等待退出信号
    while (g_running) {
        LOG_INFO("[Heartbeat] 系统运行中...");
        sleep(10);
    }

    // 优雅退出：等待 flush 线程排空日志队列后关闭文件
    LOG_INFO("[System] 正在退出，刷新日志...");
    LockFreeLogger::getInstance().Flush();
    return 0;
}
