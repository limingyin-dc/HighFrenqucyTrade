#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sys/stat.h>

LockFreeLogger::LockFreeLogger() : m_running(false) {}

// 返回全局单例，局部静态变量保证线程安全初始化（C++11 Magic Static）
LockFreeLogger& LockFreeLogger::getInstance() {
    static LockFreeLogger instance;
    return instance;
}

// 初始化日志系统：打开今日日志文件，预热队列，启动 flush 线程
void LockFreeLogger::init(const std::string& log_dir) {
    m_log_dir = log_dir;
    m_running = true;
    openTodayFile();
    m_queue.WarmUp();
    m_flushThread = std::thread(&LockFreeLogger::flushLoop, this);
}

// 格式化日志条目并推入 MPSC 队列
// 热路径：只做内存操作，无系统调用，无锁
void LockFreeLogger::log(const char* level, const char* fmt, ...) {
    LogEntry entry;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    entry.wall_sec = ts.tv_sec;
    entry.wall_ms  = (int)(ts.tv_nsec / 1000000);
    strncpy(entry.level, level, 7);
    entry.level[7] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.msg, sizeof(entry.msg), fmt, args);
    va_end(args);

    m_queue.Push(entry);
}

// 停止 flush 线程并等待其排空队列，确保进程退出前所有日志落盘
void LockFreeLogger::Flush() {
    m_running = false;
    if (m_flushThread.joinable()) m_flushThread.join();
    if (m_outFile.is_open()) { m_outFile.flush(); m_outFile.close(); }
}

LockFreeLogger::~LockFreeLogger() { Flush(); }

// 按日期创建日志文件，路径格式：{log_dir}/trading_YYYYMMDD.log
void LockFreeLogger::openTodayFile() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y%m%d", &t);
    m_current_day = date_str;

    mkdir(m_log_dir.c_str(), 0755);
    std::string path = m_log_dir + "/trading_" + date_str + ".log";
    if (m_outFile.is_open()) { m_outFile.flush(); m_outFile.close(); }
    m_outFile.open(path, std::ios::app);
    printf("[Logger] 日志文件: %s\n", path.c_str());
}

// 检查日期是否变化，跨天时自动切换日志文件
void LockFreeLogger::checkRollover(time_t wall_sec) {
    struct tm t;
    localtime_r(&wall_sec, &t);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y%m%d", &t);
    if (m_current_day != date_str) openTodayFile();
}

// 将单条 LogEntry 格式化为 "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] msg" 写入文件
void LockFreeLogger::writeEntry(const LogEntry& entry) {
    struct tm t;
    localtime_r(&entry.wall_sec, &t);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &t);
    m_outFile << time_buf
              << "." << std::setw(3) << std::setfill('0') << entry.wall_ms
              << " [" << entry.level << "] "
              << entry.msg << "\n";
}

// flush 线程主循环
// 有数据时持续写入，每 100 条 flush 一次；空闲时 sleep 1ms 让出 CPU
void LockFreeLogger::flushLoop() {
    LogEntry entry;
    int unflushed = 0;
    while (m_running) {
        if (m_queue.Pop(entry)) {
            checkRollover(entry.wall_sec);
            writeEntry(entry);
            if (++unflushed >= 100) { m_outFile.flush(); unflushed = 0; }
        } else {
            if (unflushed > 0) { m_outFile.flush(); unflushed = 0; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // 退出前排空队列，保证所有日志落盘
    while (m_queue.Pop(entry)) writeEntry(entry);
    m_outFile.flush();
}
