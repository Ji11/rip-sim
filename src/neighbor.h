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
} neighbor_t;

// 邻居表
typedef struct {
    neighbor_t entries[MAX_NEIGHBORS];
    int        count;
} neighbor_table_t;

//  生命周期 
void nt_init(neighbor_table_t *nt);

//  CRUD 操作 
// 添加邻居，返回索引
int  nt_add(neighbor_table_t *nt, const char *id, const struct sockaddr *addr, socklen_t addr_len);

// 按 ID 查找，返回索引或 -1
int  nt_find_by_id(const neighbor_table_t *nt, const char *id);

// 按 sockaddr 查找，返回索引或 -1
int  nt_find_by_addr(const neighbor_table_t *nt, const struct sockaddr *addr);

// 设置链路状态
void nt_set_active(neighbor_table_t *nt, int idx, int active);

// 更新最近收包时间戳
void nt_touch(neighbor_table_t *nt, int idx);

//  超时检测 
// 返回超过 180 秒未收到更新的邻居索引数组（调用者负责 free）
int *nt_check_timeouts(const neighbor_table_t *nt, int *count_out);

//  显示 
void nt_dump(const neighbor_table_t *nt, FILE *fp);

#endif // NEIGHBOR_H
