#ifndef TCP_MGMT_H
#define TCP_MGMT_H

#include "route_table.h"
#include "neighbor.h"

//  管理服务 

// 启动 TCP 管理监听线程
// 在独立线程中运行，rt 和 nt 与主线程共享（通过 rt->lock 保护）
// 返回 0 成功，-1 失败
int tcp_mgmt_start(int port, route_table_t *rt, neighbor_table_t *nt, int udp_fd);

// 优雅关闭管理服务
void tcp_mgmt_stop(void);

#endif // TCP_MGMT_H
