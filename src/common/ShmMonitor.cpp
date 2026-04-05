#include "ShmMonitor.h"
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 创建 POSIX 共享内存段并映射为可读写
// 初始化快照内容为全零，状态置为 "INIT"
ShmSnapshot* ShmMonitor::Create() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, sizeof(ShmSnapshot)) != 0) { close(fd); return nullptr; }
    void* ptr = mmap(nullptr, sizeof(ShmSnapshot),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return nullptr;
    auto* snap = static_cast<ShmSnapshot*>(ptr);
    memset(snap, 0, sizeof(ShmSnapshot));
    strncpy(snap->status, "INIT", 31);
    return snap;
}

// 以只读方式映射已有共享内存段，供外部监控进程调用
// 不影响主进程的写操作
const ShmSnapshot* ShmMonitor::Attach() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) return nullptr;
    void* ptr = mmap(nullptr, sizeof(ShmSnapshot),
                     PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return (ptr == MAP_FAILED) ? nullptr : static_cast<const ShmSnapshot*>(ptr);
}

// 删除共享内存段，进程退出时由 TdEngine 析构函数调用
void ShmMonitor::Destroy() {
    shm_unlink(SHM_NAME);
}
