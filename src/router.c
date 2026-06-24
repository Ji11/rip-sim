#include "router.h"
#include "rip.h"
#include "tcp_mgmt.h"
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

void router_shutdown(router *r)
{
    r->shutdown = 1;
}

void router_init(router *r)
{
    memset(r, 0, sizeof(*r));
    rt_init(&r->rt);
    nt_init(&r->nt);
    r->udp_fd = -1;
}

void router_destroy(router *r)
{
    tcp_mgmt_stop();
    if (r->udp_fd >= 0) close(r->udp_fd);
    rt_destroy(&r->rt);
}

int router_bind(router *r)
{
    int use_v6 = 0;
    // 根据邻居地址族决定 socket 类型：有 IPv6 邻居则用双栈，否则纯 IPv4
    // 遍历邻居表检查是否有 IPv6 邻居，如果有则使用 AF_INET6 创建双栈 socket，否则使用 AF_INET 创建 IPv4 socket
    for (int i = 0; i < r->nt.count; i++) {
        if (r->nt.neighbors[i].addr.ss_family == AF_INET6) {
            use_v6 = 1;
            break;
        }
    }

    int af = use_v6 ? AF_INET6 : AF_INET;
    r->udp_fd = socket(af, SOCK_DGRAM, 0);
    if (r->udp_fd < 0) {
        log_printf("ERROR: udp socket failed: %s", strerror(errno));
        return -1;
    }

    // SO_REUSEADDR 允许地址端口重用，避免重启时 bind 失败
    // &opt 类型为 int *，可以隐式转换成 void *
    int opt = 1;
    setsockopt(r->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 

    // 如果使用 IPv6 socket，设置 IPV6_V6ONLY=0 以支持双栈，绑定到 :: 接收所有本地 IPv4/IPv6 流量
    if (af == AF_INET6) {
        setsockopt(r->udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){0}, sizeof(int));

        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(r->udp_port);

        if (bind(r->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_printf("ERROR: udp bind failed: %s (port=%d)",
                    strerror(errno), r->udp_port);
            close(r->udp_fd);
            r->udp_fd = -1;
            return -1;
        }
    } else { // 否则 使用 IPv4 socket
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(r->udp_port);

        if (bind(r->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_printf("ERROR: udp bind failed: %s (port=%d)",
                    strerror(errno), r->udp_port);
            close(r->udp_fd);
            r->udp_fd = -1;
            return -1;
        }
    }

    log_printf("router %s: UDP socket bound on port %d (%s)",
            r->id, r->udp_port, af == AF_INET6 ? "IPv6 dual-stack" : "IPv4");

    if (tcp_mgmt_start(r->tcp_port, &r->rt, &r->nt, r->udp_fd) < 0) {
        log_printf("ERROR: failed to start TCP management");
        close(r->udp_fd);
        r->udp_fd = -1;
        return -1;
    }

    r->last_periodic = time(NULL);
    return 0;
}

// 每 30 秒周期性更新，在 router_run 使用 select 的超时机制实现
static void periodic_update(router *r)
{
    time_t now = time(NULL);
    // 如果距离上次周期更新不足 30 秒，直接返回
    if (now - r->last_periodic < RIP_PERIODIC_SEC) return;

    r->last_periodic = now;
    log_printf("periodic update, %d neighbors", r->nt.count);

    for (int i = 0; i < r->nt.count; i++) {
        if (!r->nt.neighbors[i].active) continue;
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.neighbors[i].addr,
                         r->nt.neighbors[i].addr_len, &r->rt, i);
    }
}

// 触发更新 路由表变化时立即发送
static void triggered_update(router *r)
{
    log_printf("triggered update (route table changed)");

    // 遍历邻居表，向所有活跃邻居发送 RIP 响应报文
    for (int i = 0; i < r->nt.count; i++) {
        if (!r->nt.neighbors[i].active) continue;
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.neighbors[i].addr,
                         r->nt.neighbors[i].addr_len, &r->rt, i);
    }

    // 触发更新后清除 changed 标志，避免重复发送
    pthread_mutex_lock(&r->rt.lock);
    r->rt.changed = 0;
    pthread_mutex_unlock(&r->rt.lock);
}

// 邻居超时检测：遍历邻居表，将超时的标记 DOWN 并毒化路由
static void check_neighbor_timeouts(router *r)
{
    time_t now = time(NULL);

    // 遍历邻居表，检查每个邻居的 last_recv，如果超过 180 秒没有收到更新，则该邻居超时
    // 然后将其标记为 DOWN，并调用 rt_poison_from_neighbor 毒化从该邻居学到的路由
    for (int i = 0; i < r->nt.count; i++) {
        if (!r->nt.neighbors[i].active) continue;
        if ((now - r->nt.neighbors[i].last_recv) <= RIP_NEIGHBOR_SEC) continue; // 180 秒内有收到更新，说明邻居活跃

        log_printf("neighbor %s timed out (180s no update)", r->nt.neighbors[i].id);
        nt_set_active(&r->nt, i, 0); // 标记邻居为 DOWN

#if POISON_REVERSE
        int poisoned = rt_poison_from_neighbor(&r->rt, i);
#else
        int poisoned = 0;
#endif
        log_printf("%d routes poisoned from timed-out neighbor %s", poisoned, r->nt.neighbors[i].id);
    }
}

// 主事件循环 select
int router_run(router *r)
{
    log_printf("router %s starting main loop", r->id);

    // 启动时发送初始路由表给所有邻居
    for (int i = 0; i < r->nt.count; i++) {
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.neighbors[i].addr,
                         r->nt.neighbors[i].addr_len, &r->rt, -1);
    }

    // 同时发送 RIP 请求以主动拉取邻居的路由表
    for (int i = 0; i < r->nt.count; i++) {
        rip_send_request(r->udp_fd,
                        (const struct sockaddr *)&r->nt.neighbors[i].addr,
                        r->nt.neighbors[i].addr_len);
    }

    while (!r->shutdown) {
        fd_set readfds;
        FD_ZERO(&readfds);

        int max_fd = r->udp_fd;
        FD_SET(r->udp_fd, &readfds);

        // select 超时 = 距下次周期发送的剩余秒数，最少 1s
        // 没包时直接睡到该发的时刻，收包提前返回后自动重新计算剩余时间
        time_t elapsed = time(NULL) - r->last_periodic;
        time_t remaining = (elapsed < RIP_PERIODIC_SEC)
                         ? (RIP_PERIODIC_SEC - elapsed) : 0;

        struct timeval tv;
        tv.tv_sec = (remaining > 1) ? remaining : 1; // 防止 remaining 是 0，select 无限唤醒返回 忙轮询
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_printf("ERROR: select failed: %s", strerror(errno));
            break;
        }

        // 如果 UDP socket FD ISSET，说明有 RIP 报文到达，调用 rip_recv() 处理
        if (FD_ISSET(r->udp_fd, &readfds)) {
            rip_recv(r->udp_fd, &r->rt, &r->nt);

            // changed 需要加锁，因为：
            // 主线程 rt_upsert() 可能修改路由表，把 rt->changed 置 1
            // TCP 管理线程也会通过 link down → rt_poison_from_neighbor 间接写 rt->changed
            pthread_mutex_lock(&r->rt.lock);
            int changed = r->rt.changed;
            pthread_mutex_unlock(&r->rt.lock);

            // 如果路由表发生变化，立即触发更新
            if (changed) {
                triggered_update(r);
            }
        }

        // 周期性更新和邻居超时检测
        periodic_update(r);
        check_neighbor_timeouts(r);

        // 垃圾回收过期路由
        int collected = rt_garbage_collect(&r->rt);
        if (collected > 0) {
            log_printf("garbage collected %d routes", collected);
        }
    }

    log_printf("main loop exited (r->shutdown=%d), shutting down", r->shutdown);
    log_printf("router %s shutting down", r->id);
    return 0;
}
