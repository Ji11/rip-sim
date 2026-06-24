#ifndef RIP_H
#define RIP_H

#include <stdint.h>
#include <sys/socket.h>
#include "route_table.h"
#include "neighbor.h"

//  RIP 报文格式 支持 IPv4 & IPv6

#define RIP_VERSION        1
#define RIP_CMD_REQUEST     1   // 请求报文
#define RIP_CMD_RESPONSE    2   // 响应报文
#define RIP_MAX_ENTRIES     25  // 每个 UDP 数据报最多携带的路由条目数

// 以下两个结构体是用 reserved 字段对齐的，否则还需要 __attribute__((packed))
// rip_header: command(1) + version(1) + reserved(2) = 4 bytes
// rip_route:  af(2) + addr(16) + prefix_len(1) + reserved(1) + metric(2) = 22 bytes

// RIP 报文头
typedef struct {
    uint8_t  command;       // 命令类型：1=请求, 2=响应
    uint8_t  version;       // 协议版本
    uint16_t reserved;      // 保留字段
} rip_header;

// RIP 路由条目（22 bytes）
typedef struct {
    uint16_t af;            // 地址族：AF_INET=2, AF_INET6=10 跟 <sys/socket.h> 常量相同
    uint8_t  addr[16];      // 目标地址（IPv4 用前 4 bytes）
    uint8_t  prefix_len;    // 前缀长度
    uint8_t  reserved;      // 保留
    uint16_t metric;        // 度量 1-16，网络字节序
} rip_route;

// 完整 RIP 报文 = rip_header + N * rip_route

// 构造并发送 RIP 响应到指定邻居
// poison_reverse_idx >= 0 时：将从该邻居学到的路由毒性化（metric=16）
int rip_send_response(int udp_fd, const struct sockaddr *dst,
                      socklen_t dst_len, route_table *rt,
                      int poison_reverse_idx);

// 发送 RIP 请求，用于主动拉取邻居路由表
int rip_send_request(int udp_fd, const struct sockaddr *dst, socklen_t dst_len);

// 从 UDP socket 接收 RIP 报文
// 收到 RESPONSE：逐条调用 rt_upsert() 更新路由表
// 收到 REQUEST：立即向发送者回复路由表
// 返回处理的条目数，-1 表示出错
// *from_nbr_idx 被设为邻居索引（未知则为 -1）
int rip_recv(int udp_fd, route_table *rt, neighborable *nt,
             int *from_nbr_idx);

#endif // RIP_H
