#ifndef CONFIG_H
#define CONFIG_H

#include "router.h"

// 从配置文件加载路由器配置
// 填充 router 结构体：id、udp/tcp 端口、邻居、直连网络
// 返回 0 成功，-1 失败
int config_load(const char *path, router *router);

#endif // CONFIG_H
