#ifndef NEIGHBOR_H
#define NEIGHBOR_H

#include <sys/socket.h>
#include <netinet/in.h>
#include "utils.h"

#define MAX_NEIGHBORS 16

// 邻居条目
typedef struct {
    char        id[32];               // 邻居 ID 字符串
    struct sockaddr_storage addr;     // 解析后的 UDP 地址
    socklen_t   addr_len;
    int         active;               // 1 表示链路 UP，0 表示 DOWN
    time_t      last_recv;            // 最近一次收到更新的时间
} neighbor;

// 邻居表
typedef struct {
    neighbor neighbors[MAX_NEIGHBORS];
    int        count;
} neighborable;

//  生命周期 
void nt_init(neighborable *nt);

//  CRUD 操作 
// 添加邻居，返回索引
int  nt_add(neighborable *nt, const char *id, const struct sockaddr *addr, socklen_t addr_len);

// 按 ID 查找，返回索引或 -1
int  nt_find_by_id(const neighborable *nt, const char *id);

// 按 sockaddr 查找，返回索引或 -1
int  nt_find_by_addr(const neighborable *nt, const struct sockaddr *addr);

// 设置链路状态
void nt_set_active(neighborable *nt, int idx, int active);

// 更新最近收包时间戳
void nt_update_last_recv(neighborable *nt, int idx);

//  超时检测 
// 返回超过 180 秒未收到更新的邻居索引数组（调用者负责 free）
int *nt_check_timeouts(const neighborable *nt, int *count_out);

// 显示邻居表（直接写入文件描述符）
void nt_show(const neighborable *nt, int fd);

#endif // NEIGHBOR_H
