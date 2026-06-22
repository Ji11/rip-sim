#include "router.h"
#include "rip.h"
#include "tcp_mgmt.h"
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

void router_shutdown(router_t *r)
{
    r->shutdown = 1;
}

void router_init(router_t *r)
{
    memset(r, 0, sizeof(*r));
    rt_init(&r->rt);
    nt_init(&r->nt);
    r->udp_fd = -1;
}

void router_destroy(router_t *r)
{
    tcp_mgmt_stop();
    if (r->udp_fd >= 0) close(r->udp_fd);
    rt_destroy(&r->rt);
}

int router_bind(router_t *r)
{
    int use_v6 = 0;
    // 根据邻居地址族决定 socket 类型：有 IPv6 邻居则用双栈，否则纯 IPv4
    // 遍历邻居表检查是否有 IPv6 邻居，如果有则使用 AF_INET6 创建双栈 socket，否则使用 AF_INET 创建 IPv4 socket
    for (int i = 0; i < r->nt.count; i++) {
        if (r->nt.entries[i].addr.ss_family == AF_INET6) {
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
    } else {
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

// 周期性更新（每 30 秒），通过 select 超时实现，不使用 sleep 或 alarm
static void periodic_update(router_t *r)
{
    time_t now = time(NULL);
    if (now - r->last_periodic < RIP_PERIODIC_SEC) return;

    r->last_periodic = now;
    log_printf("periodic update, %d neighbors", r->nt.count);

    for (int i = 0; i < r->nt.count; i++) {
        if (!r->nt.entries[i].active) continue;
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.entries[i].addr,
                         r->nt.entries[i].addr_len, &r->rt, i);
    }
}

// 触发更新（路由表变化时立即发送）
static void triggered_update(router_t *r)
{
    log_printf("triggered update (route table changed)");

    for (int i = 0; i < r->nt.count; i++) {
        if (!r->nt.entries[i].active) continue;
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.entries[i].addr,
                         r->nt.entries[i].addr_len, &r->rt, i);
    }

    pthread_mutex_lock(&r->rt.lock);
    r->rt.changed = 0;
    pthread_mutex_unlock(&r->rt.lock);
}

// 邻居超时检测
static void check_neighbor_timeouts(router_t *r)
{
    int count = 0;
    int *indices = nt_check_timeouts(&r->nt, &count);

    for (int i = 0; i < count; i++) {
        int idx = indices[i];
        log_printf("neighbor %s timed out (180s no update)",
                r->nt.entries[idx].id);

        nt_set_active(&r->nt, idx, 0);

        int poisoned = rt_poison_from_neighbor(&r->rt, idx);
        log_printf("%d routes poisoned from timed-out neighbor %s",
                poisoned, r->nt.entries[idx].id);
    }

    free(indices);
}

// 主事件循环（select 驱动）
int router_run(router_t *r)
{
    log_printf("router %s starting main loop", r->id);

    // 启动时发送初始路由表给所有邻居
    for (int i = 0; i < r->nt.count; i++) {
        rip_send_response(r->udp_fd,
                         (const struct sockaddr *)&r->nt.entries[i].addr,
                         r->nt.entries[i].addr_len, &r->rt, -1);
    }

    // 同时发送 RIP 请求以主动拉取邻居的路由表
    for (int i = 0; i < r->nt.count; i++) {
        rip_send_request(r->udp_fd,
                        (const struct sockaddr *)&r->nt.entries[i].addr,
                        r->nt.entries[i].addr_len);
    }

    while (!r->shutdown) {
        fd_set readfds;
        FD_ZERO(&readfds);

        int max_fd = r->udp_fd;
        FD_SET(r->udp_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            log_printf("ERROR: select failed: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(r->udp_fd, &readfds)) {
            int from_nbr = -1;
            rip_recv(r->udp_fd, &r->rt, &r->nt, &from_nbr);

            pthread_mutex_lock(&r->rt.lock);
            int changed = r->rt.changed;
            pthread_mutex_unlock(&r->rt.lock);

            if (changed) {
                triggered_update(r);
            }
        }

        periodic_update(r);
        check_neighbor_timeouts(r);

        int collected = rt_garbage_collect(&r->rt);
        if (collected > 0) {
            log_printf("garbage collected %d routes", collected);
        }
    }

    log_printf("main loop exited (r->shutdown=%d), shutting down", r->shutdown);
    log_printf("router %s shutting down", r->id);
    return 0;
}
