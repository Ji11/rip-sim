#include "ripd.h"
#include <netdb.h>
#include <unistd.h>
#include <libconfig.h>

/*
 * 本文档中使用的 libconfig API 速查（详见 /usr/share/doc/libconfig-doc/html/）
 *
 * ┌──────────────────────────────┬──────────────────────────────────────┐
 * │ API                          │ 作用                                 │
 * ├──────────────────────────────┼──────────────────────────────────────┤
 * │ config_t cfg                 │ 配置对象，所有操作的句柄             │
 * │ config_init(&cfg)            │ 初始化配置对象，用完须 destroy       │
 * │ config_read_file(&cfg, path) │ 从文件读取配置，成功返回 CONFIG_TRUE │
 * │ config_error_file/line/text  │ 读取失败时获取错误文件/行号/描述     │
 * │ config_lookup_string         │ 从根读取字符串标量 (cfg, "key", &s)  │
 * │ config_lookup_int            │ 从根读取整数标量 (cfg, "key", &i)    │
 * │ config_lookup                │ 从根获取子节点 (cfg, "key")          │
 * │                             │ 返回 config_setting_t *，用于访问数组/列表/组 │
 * │ config_setting_length(s)     │ 获取数组/列表的长度                  │
 * │ config_setting_get_elem(s,i) │ 获取数组第 i 个元素（从 0 开始）     │
 * │ config_setting_lookup_string │ 从组节点读取字符串 (elem, "key", &s) │
 * │ config_setting_lookup_int    │ 从组节点读取整数 (elem, "key", &i)   │
 * │ config_destroy(&cfg)         │ 释放配置对象内存                     │
 * └──────────────────────────────┴──────────────────────────────────────┘
 *
 * 对应配置文件结构（router1.cfg 为例）：
 *
 *   id        = "1";                 → config_lookup_string("id")
 *   udp_port  = 5201;               → config_lookup_int("udp_port")
 *   tcp_port  = 8021;               → config_lookup_int("tcp_port")
 *
 *   neighbors = (                    → config_lookup("neighbors")
 *       { id="2"; address="..."; port=5202; },   → 数组元素 (group)
 *   );                               → config_setting_get_elem() 遍历
 *
 *   networks = (                     → config_lookup("networks")
 *       { address="10.0.1.0/24"; },  → 数组元素 (group)
 *   );
 */

// 使用 getaddrinfo 解析地址：先作为 IP 解析，失败则作为主机名（如 router2.local）
static int resolve_addr(const char *hostname, int port,
                        struct sockaddr_storage *sa, socklen_t *sa_len)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;

    int ret = getaddrinfo(hostname, port_str, &hints, &res);
    if (ret != 0) {
        // 数字地址解析失败，去掉 NUMERICHOST 标志尝试作为主机名
        hints.ai_flags = AI_NUMERICSERV;
        ret = getaddrinfo(hostname, port_str, &hints, &res);
        if (ret != 0) {
            log_printf("ERROR: getaddrinfo(%s, %d) failed: %s",
                    hostname, port, gai_strerror(ret));
            return -1;
        }
    }

    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *sa_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

int config_load(const char *path, router *router)
{
    // 1. 初始化配置对象，从文件读取
    config_t cfg;
    config_init(&cfg);

    if (config_read_file(&cfg, path) == CONFIG_FALSE) {
        // 读取失败：打印错误位置和原因
        log_printf("ERROR: %s:%d — %s",
                   config_error_file(&cfg),
                   config_error_line(&cfg),
                   config_error_text(&cfg));
        config_destroy(&cfg);
        return -1;
    }

    // 2. 读取标量值：id（字符串）、udp_port / tcp_port（整数）
    //    config_lookup_string(cfg, "路径", &输出指针)
    //    成功返回 CONFIG_TRUE，键不存在返回 CONFIG_FALSE
    const char *id_str;
    if (config_lookup_string(&cfg, "id", &id_str) == CONFIG_TRUE) {
        strncpy(router->id, id_str, sizeof(router->id) - 1);
        router->id[sizeof(router->id) - 1] = '\0';
        log_printf("config: id=%s", router->id);
    }

    // config_lookup_int(cfg, "路径", &输出整数) — 同样 CONFIG_TRUE/FALSE
    config_lookup_int(&cfg, "udp_port", &router->udp_port);
    config_lookup_int(&cfg, "tcp_port", &router->tcp_port);
    log_printf("config: udp_port=%d, tcp_port=%d",
               router->udp_port, router->tcp_port);

    // 3. 读取数组：neighbors = ( {...}, {...} )
    //    config_lookup 返回 config_setting_t * 节点指针，不存在返回 NULL
    config_setting_t *nbrs = config_lookup(&cfg, "neighbors");
    if (nbrs) {
        // config_setting_length 获取数组长度
        int count = config_setting_length(nbrs);
        for (int i = 0; i < count; i++) {
            // config_setting_get_elem 获取数组第 i 个元素（group 类型）
            config_setting_t *n = config_setting_get_elem(nbrs, i);

            // config_setting_lookup_* 从 group 中按键读取字段
            const char *nid, *addr_str;
            int port;
            config_setting_lookup_string(n, "id", &nid);
            config_setting_lookup_string(n, "address", &addr_str);
            config_setting_lookup_int(n, "port", &port);

            struct sockaddr_storage sa;
            socklen_t sa_len;
            if (resolve_addr(addr_str, port, &sa, &sa_len) < 0) {
                log_printf("ERROR: failed to resolve neighbor %s (%s:%d)",
                        nid, addr_str, port);
                config_destroy(&cfg);
                return -1;
            }

            if (nt_add(&router->nt, nid,
                       (const struct sockaddr *)&sa, sa_len) < 0) {
                log_printf("ERROR: failed to add neighbor %s", nid);
                config_destroy(&cfg);
                return -1;
            }
        }
    }

    // 4. 读取 networks 数组，与 neighbors 结构相同
    config_setting_t *nets = config_lookup(&cfg, "networks");
    if (nets) {
        int count = config_setting_length(nets);
        for (int i = 0; i < count; i++) {
            config_setting_t *net = config_setting_get_elem(nets, i);

            const char *cidr;
            config_setting_lookup_string(net, "address", &cidr);

            struct sockaddr_storage sa;
            int prefix_len, af;
            if (parse_cidr(cidr, &sa, &prefix_len, &af) < 0) {
                log_printf("ERROR: invalid network %s", cidr);
                config_destroy(&cfg);
                return -1;
            }

            // 从 sockaddr 提取二进制地址到 dest[16]
            uint8_t dest[16];
            memset(dest, 0, 16);
            if (af == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
                memcpy(dest, &sin->sin_addr, 4);
            } else {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&sa;
                memcpy(dest, &sin6->sin6_addr, 16);
            }

            // 直连路由：下一跳 0、metric=1、from_neighbor=-1
            uint8_t zero[16];
            memset(zero, 0, 16);
            rt_upsert(&router->rt, af, dest, prefix_len, zero, 1, -1);
            log_printf("config: direct network %s", cidr);
        }
    }

    // 5. 销毁配置对象
    config_destroy(&cfg);

    // 校验必要字段
    if (!router->id[0] || !router->udp_port || !router->tcp_port) {
        log_printf("ERROR: config missing id, udp_port, or tcp_port");
        return -1;
    }

    if (router->nt.count == 0)
        log_printf("WARNING: no neighbors configured");

    log_printf("config loaded: %d neighbors, %d direct routes",
               router->nt.count, router->rt.count);
    return 0;
}
