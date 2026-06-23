#include "config.h"
#include <netdb.h>
#include <unistd.h>

// 去除首尾空白字符
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
    return s;
}

// 使用 getaddrinfo 解析地址
// 先尝试作为 IP 地址解析，失败则作为主机名 router2.local
// hostname：主机名或者IP字符串
// port：端口号字符串
static int resolve_addr(const char *hostname, const char *port,
                        struct sockaddr_storage *sa, socklen_t *sa_len)
{
    struct addrinfo hints, *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // 同时支持 IPv4 和 IPv6
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

    int ifzero = getaddrinfo(hostname, port, &hints, &res); // 若成功，res 指向解析结果 addrinfo 链表
    if (ifzero != 0) {
        // 数字地址解析失败，尝试作为主机名解析（去掉 NUMERICHOST 标志）
        hints.ai_flags = AI_NUMERICSERV;
        ifzero = getaddrinfo(hostname, port, &hints, &res); // 成功返回 0
        if (ifzero != 0) {
            log_printf("ERROR: getaddrinfo(%s, %s) failed: %s",
                    hostname, port, gai_strerror(ifzero));
            return -1;
        }
    }

    memcpy(sa, res->ai_addr, res->ai_addrlen); // 把 res 传给 sa
    *sa_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

// 解析配置文件
int config_load(const char *path, router_t *router)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_printf("ERROR: cannot open config file: %s (errno=%d: %s)",
                path, errno, strerror(errno));
        return -1;
    }

    char line[512];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *s = trim(line);

        // 跳过注释行和空行
        if (*s == '#' || *s == '\0') continue;

        // 解析config的键值对，key 分为 id, udp_port, tcp_port, neighbor, network
        // id <router_id>
        // udp_port <port>
        // tcp_port <port>
        // neighbor <neighbor_id> <address> <port>
        // network <cidr>
        char key[64], val[256], nb_addr[256], nb_port[256];

        sscanf(s, "%63s %255s %255s %255s", key, val, nb_addr, nb_port);

        if (strcmp(key, "id") == 0) {
            strncpy(router->id, val, sizeof(router->id) - 1);
            router->id[sizeof(router->id) - 1] = '\0';
            log_printf("config: id=%s", router->id);

        } else if (strcmp(key, "udp_port") == 0) {
            router->udp_port = atoi(val);
            log_printf("config: udp_port=%d", router->udp_port);

        } else if (strcmp(key, "tcp_port") == 0) {
            router->tcp_port = atoi(val);
            log_printf("config: tcp_port=%d", router->tcp_port);

        } else if (strcmp(key, "neighbor") == 0) {
            // neighbor id=<id> nb_addr=<address> nb_port=<port>
            struct sockaddr_storage sa;
            socklen_t sa_len;
            if (resolve_addr(nb_addr, nb_port, &sa, &sa_len) < 0) {
                log_printf("ERROR: line %d: failed to resolve neighbor %s:%s",
                        line_num, nb_addr, nb_port);

                fclose(fp);
                return -1;
            }

            // 解析出邻居后添加到邻居表中
            if (nt_add(&router->nt, val, (const struct sockaddr *)&sa, sa_len) < 0) {
                log_printf("ERROR: line %d: failed to add neighbor", line_num);

                fclose(fp);
                return -1;
            }

        } else if (strcmp(key, "network") == 0) {
            // 格式：network <cidr>
            struct sockaddr_storage sa;
            int prefix_len, af;
            if (parse_cidr(val, &sa, &prefix_len, &af) < 0) { // 解析 CIDR 字符串，得到 sa, prefix_len, af
                log_printf("ERROR: line %d: invalid network %s", line_num, val);

                fclose(fp);
                return -1;
            }

            // 把解析出来的网络添加到路由表中，作为直连路由
            uint8_t dest[16];
            memset(dest, 0, 16);
            if (af == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
                memcpy(dest, &sin->sin_addr, 4);
            } else {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&sa;
                memcpy(dest, &sin6->sin6_addr, 16);
            }

            // 直连路由：下一跳占位为全0，用不到；度量 = 1，from_neighbor = -1
            uint8_t zero[16];
            memset(zero, 0, 16);
            rt_upsert(&router->rt, af, dest, prefix_len, zero, 1, -1);
            log_printf("config: direct network %s", val);

        } else {
            log_printf("line %d: unknown key '%s', ignoring", line_num, key);
        }
    }

    fclose(fp);

    if (!router->id[0] || !router->udp_port || !router->tcp_port) {
        log_printf("ERROR: config missing id, udp_port, tcp_port");
        return -1;
    }

    if (router->nt.count == 0) {
        log_printf("WARNING: no neighbors configured");

    }

    log_printf("config loaded: %d neighbors, %d direct routes",
            router->nt.count, router->rt.count);
        return 0;
}
