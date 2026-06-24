#ifndef ROUTER_H
#define ROUTER_H

#include "route_table.h"
#include "neighbor.h"

// 路由器状态
typedef struct {
    char    id[32];          // 路由器 ID
    int     udp_port;        // RIP 协议的 UDP 端口
    int     tcp_port;        // TCP 管理接口端口

    // socket 文件描述符
    int     udp_fd;

    // 路由表和邻居表
    route_table   rt;
    neighborable nt;

    // 周期更新计时器
    time_t  last_periodic;

    // 关闭标志，由信号处理器通过 router_shutdown() 设置
    volatile int shutdown;
} router;

//  生命周期 
void router_init(router *r);
void router_destroy(router *r);

// 创建并绑定 UDP + TCP socket（config 之后调用）
int  router_bind(router *r);

// 主事件循环（阻塞直到收到信号或发生致命错误）
int  router_run(router *r);

// 请求优雅关闭（由信号处理器调用）
void router_shutdown(router *r);

#endif // ROUTER_H
