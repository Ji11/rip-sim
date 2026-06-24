#include "ripd.h"
#include <netdb.h>
#include <unistd.h>
#include <libconfig.h>

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
    config_t cfg;
    config_init(&cfg);

    if (config_read_file(&cfg, path) == CONFIG_FALSE) {
        log_printf("ERROR: %s:%d — %s", config_error_file(&cfg),
                   config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return -1;
    }

    // id
    const char *id_str;
    if (config_lookup_string(&cfg, "id", &id_str) == CONFIG_TRUE) {
        strncpy(router->id, id_str, sizeof(router->id) - 1);
        router->id[sizeof(router->id) - 1] = '\0';
        log_printf("config: id=%s", router->id);
    }

    // 端口
    config_lookup_int(&cfg, "udp_port", &router->udp_port);
    config_lookup_int(&cfg, "tcp_port", &router->tcp_port);
    log_printf("config: udp_port=%d, tcp_port=%d",
               router->udp_port, router->tcp_port);

    // 邻居
    config_setting_t *nbrs = config_lookup(&cfg, "neighbors");
    if (nbrs) {
        int count = config_setting_length(nbrs);
        for (int i = 0; i < count; i++) {
            config_setting_t *n = config_setting_get_elem(nbrs, i);

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

    // 直连网络
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

            uint8_t dest[16];
            memset(dest, 0, 16);
            if (af == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
                memcpy(dest, &sin->sin_addr, 4);
            } else {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&sa;
                memcpy(dest, &sin6->sin6_addr, 16);
            }

            uint8_t zero[16];
            memset(zero, 0, 16);
            rt_upsert(&router->rt, af, dest, prefix_len, zero, 1, -1);
            log_printf("config: direct network %s", cidr);
        }
    }

    config_destroy(&cfg);

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
