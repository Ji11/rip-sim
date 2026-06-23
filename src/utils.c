#include "utils.h"

//  日志

void log_printf(const char *fmt, ...)
{
    char timebuf[32];
        time_str(time(NULL), timebuf, sizeof(timebuf));
    fprintf(stderr, "[%s] ", timebuf);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    fflush(stderr);
}

//  地址工具 

// 将 sockaddr 转换为字符串形式，存储在 buf 中，返回 buf
const char *sockaddr_str(const struct sockaddr *sa, char *buf, size_t len)
{
    if (!sa) {
        snprintf(buf, len, "(null)");
        return buf;
    }

    void *addr; // void * 指针，存IP
    uint16_t port; // 用 uint16 存端口号

    // 根据 IPv4 或 IPv6，将 tcp_mgmt 传来的 sockaddr 类型 sa 转为 sockaddr_in 类型 然后拿到 addr 和 port
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        addr = (void *)&sin->sin_addr;
        port = ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        addr = (void *)&sin6->sin6_addr;
        port = ntohs(sin6->sin6_port);
    } else {
        snprintf(buf, len, "(unknown af=%d)", sa->sa_family);
        return buf;
    }

    char ip[INET6_ADDRSTRLEN];
    inet_ntop(sa->sa_family, addr, ip, sizeof(ip));
    snprintf(buf, len, "%s:%u", ip, port);
    return buf;
}

int sockaddr_eq(const struct sockaddr *a, const struct sockaddr *b)
{
    if (!a || !b) return 0;
    if (a->sa_family != b->sa_family) return 0;

    if (a->sa_family == AF_INET) {
        const struct sockaddr_in *ai = (const struct sockaddr_in *)a;
        const struct sockaddr_in *bi = (const struct sockaddr_in *)b;
        return (ai->sin_addr.s_addr == bi->sin_addr.s_addr &&
                ai->sin_port == bi->sin_port);
    } else if (a->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ai = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *bi = (const struct sockaddr_in6 *)b;
        return (memcmp(&ai->sin6_addr, &bi->sin6_addr, 16) == 0 &&
                ai->sin6_port == bi->sin6_port);
    }
    return 0;
}

int parse_cidr(const char *cidr, struct sockaddr_storage *sa, int *prefix_len, int *af)
{
    memset(sa, 0, sizeof(*sa));

    // 查找 '/' 分隔符
    const char *slash = strchr(cidr, '/'); // slash为一个指向 '/' 的字符串指针 eg. slash = "/24" 或者 NULL
    char addr_str[INET6_ADDRSTRLEN];
    int pre_len = -1; // 默认前缀长度为 -1，表示未指定

    if (slash) {
        // 提取地址部分和前缀长度
        size_t addr_len = slash - cidr;
        if (addr_len >= sizeof(addr_str)) return -1;
        memcpy(addr_str, cidr, addr_len);
        addr_str[addr_len] = '\0';
        pre_len = atoi(slash + 1);
    } else {
        strncpy(addr_str, cidr, sizeof(addr_str) - 1);
        addr_str[sizeof(addr_str) - 1] = '\0';
    }

    // 先尝试 IPv4
    struct sockaddr_in *sin = (struct sockaddr_in *)sa;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) == 1) { // 成功解析为 IPv4 地址，则设置地址族和前缀长度
        sin->sin_family = AF_INET;
        *af = AF_INET;
        *prefix_len = (pre_len >= 0) ? pre_len : 32;
        return 0;
    }

    // 再尝试 IPv6
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
    if (inet_pton(AF_INET6, addr_str, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        *af = AF_INET6;
        *prefix_len = (pre_len >= 0) ? pre_len : 128;
        return 0;
    }

    return -1;
}

void addr_copy(int af, const uint8_t *src, uint8_t *dst)
{
    int len = (af == AF_INET) ? 4 : 16;
    memset(dst, 0, 16); // 把 dst 后 12 位清零
    memcpy(dst, src, len);
}

const char *addr_str(int af, const uint8_t *addr, char *buf, size_t len)
{
    if (af == AF_INET) {
        inet_ntop(AF_INET, addr, buf, len);
    } else {
        inet_ntop(AF_INET6, addr, buf, len);
    }
    return buf;
}

// 路由表键比较，相同返回 1，不同返回 0
int route_key_eq(int af1, const void *addr1, int pl1,
                 int af2, const void *addr2, int pl2)
{
    if (af1 != af2) return 0;
    if (pl1 != pl2) return 0;

    int addr_bytes = (af1 == AF_INET) ? 4 : 16;
    return (memcmp(addr1, addr2, addr_bytes) == 0);
}

//  时间工具 

const char *time_str(time_t t, char *buf, size_t len)
{
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%H:%M:%S", &tm_info);
    return buf;
}
