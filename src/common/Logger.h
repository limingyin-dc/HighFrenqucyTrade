#pragma once
#include "mpsc.h"
#include <string>
#include <thread>
#include <fstream>
#include <atomic>
#include <ctime>
#include <cstdarg>

// 单条日志条目，固定大小避免堆分配
struct LogEntry {
    time_t  wall_sec;   // 秒级时间戳
    int     wall_ms;    // 毫秒部分
    char    level[8];   // 日志级别："INFO"/"WARN"/"ERROR"/"DEBUG"
    char    msg[512];   // 格式化后的消息体
};

// 日志队列：MPSC，多个业务线程写，flush 线程单独消费
using LogQueue = MPSCQueue<LogEntry, 8192>;

// 异步无锁日志器（单例）
// 业务线程只做格式化 + Push（无锁，无 I/O），独立 flush 线程负责写文件
class LockFreeLogger {
public:
    // 获取全局单例（Magic Static，线程安全）
    static LockFreeLogger& getInstance();

    // 初始化日志目录，启动 flush 线程，预热队列内存
    void init(const std::string& log_dir = "./logs");

    // 格式化日志并推入 MPSC 队列（热路径，无 I/O，无锁）
    void log(const char* level, const char* fmt, ...);

    // 优雅退出：停止 flush 线程，排空队列，关闭文件
    void Flush();

    ~LockFreeLogger();

private:
    LockFreeLogger();

    // 按日期打开新日志文件（首次启动或跨天时调用）
    void openTodayFile();

    // 检查是否需要按天切换日志文件
    void checkRollover(time_t wall_sec);

    // 将单条 LogEntry 格式化写入文件流
    void writeEntry(const LogEntry& entry);

    // flush 线程主循环：持续 Pop 并写文件，每 100 条 flush 一次
    void flushLoop();

    LogQueue          m_queue;
    std::ofstream     m_outFile;
    std::atomic<bool> m_running;
    std::thread       m_flushThread;
    std::string       m_log_dir;
    std::string       m_current_day; // 当前日志文件对应的日期字符串
};

#define LOG_INFO(fmt, ...)  LockFreeLogger::getInstance().log("INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LockFreeLogger::getInstance().log("WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LockFreeLogger::getInstance().log("ERROR", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LockFreeLogger::getInstance().log("DEBUG", fmt, ##__VA_ARGS__)
