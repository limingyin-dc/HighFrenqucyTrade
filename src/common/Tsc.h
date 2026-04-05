#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>

// ============================================================
// TSC (Time Stamp Counter) 工具
//
// rdtsc 直接读取 CPU 周期计数器，开销约 5~10ns，
// 远低于 clock_gettime (~25ns) 和 chrono (~30ns)。
// 适合热路径打时间戳，不适合跨核或跨 NUMA 节点比较。
//
// 使用前提：
//   1. 绑核（Strategy 线程已绑 core 2，行情线程由 CTP 管理）
//   2. CPU 支持 constant_tsc（现代 x86 均支持，可用 rdtscp 验证）
//   3. 启动时调用 Tsc::Init() 校准 CPU 频率
// ============================================================
namespace Tsc {

// CPU 频率（Hz），Init() 校准后写入，只读不写
inline uint64_t g_hz = 0;

// 读取当前 TSC 值（约 5~10ns 开销）
static inline uint64_t Now() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// 序列化版本：等待之前所有指令完成后再读 TSC，测量起点更精确
// 用于 T1 打点（OnRtnDepthMarketData 入口），避免乱序执行导致时间戳提前
static inline uint64_t NowSerialized() {
    uint32_t lo, hi, aux;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

// 将 TSC 差值转换为纳秒
// 调用前必须先 Init()
static inline int64_t ToNs(uint64_t tsc_delta) {
    if (__builtin_expect(g_hz == 0, 0)) return 0;
    // 先乘 1e9 再除频率，避免整数截断误差
    return (int64_t)(tsc_delta * 1000000000ULL / g_hz);
}

// 启动时校准 CPU 频率
// 用忙等而非 sleep，避免节能降频导致 TSC 频率测量偏低
static inline void Init() {
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t tsc1 = Now();

    // 忙等 200ms，期间 CPU 保持全速，避免 C-state 降频
    uint64_t spin = 0;
    do {
        ++spin;
        clock_gettime(CLOCK_MONOTONIC, &t2);
    } while ((t2.tv_sec - t1.tv_sec) * 1000000000ULL +
             (t2.tv_nsec - t1.tv_nsec) < 200000000ULL);

    uint64_t tsc2 = Now();

    uint64_t ns_elapsed = (t2.tv_sec - t1.tv_sec) * 1000000000ULL
                        + (t2.tv_nsec - t1.tv_nsec);
    uint64_t tsc_delta  = tsc2 - tsc1;

    g_hz = tsc_delta * 1000000000ULL / ns_elapsed;
    printf("[Tsc] CPU 频率校准完成: %.3f GHz  (spin=%llu)\n",
           g_hz / 1e9, (unsigned long long)spin);
}

} // namespace Tsc
