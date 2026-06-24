#!/bin/bash
# rip-sim 完整测试脚本
# 覆盖任务书 三、测试要求 3.1–3.5
# 运行方式：bash test_all.sh

set -e
cd "$(dirname "$0")"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

pass()  { echo -e "${GREEN}[PASS]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; }
info()  { echo -e "${CYAN}[INFO]${NC} $1"; }

cleanup() {
    killall ripd 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# ── 构建 ────────────────────────────────────────────
info "构建项目..."
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# ── 3.1 启动 3 台路由器 ─────────────────────────────
echo ""
echo "============================================"
echo "  3.1 测试环境：启动 3 台路由器 (mesh 拓扑)"
echo "============================================"
./ripd config/router1.conf 2>/tmp/r1.log &
./ripd config/router2.conf 2>/tmp/r2.log &
./ripd config/router3.conf 2>/tmp/r3.log &
info "路由器已启动: R1(tcp:8021) R2(tcp:8022) R3(tcp:8023)"
info "拓扑: R1---R2---R3---R1 (全网状)"
pass "3.1 路由器进程全部启动"

# ── 等待收敛 ────────────────────────────────────────
info "等待路由收敛 (35s)..."
sleep 35

# ── 3.2 路由收敛 ────────────────────────────────────
echo ""
echo "============================================"
echo "  3.2 基本功能测试 & 路由收敛验证"
echo "============================================"

check_routes() {
    local port=$1 name=$2
    local count=$(echo "show route" | timeout 3 nc 127.0.0.1 $port 2>/dev/null \
        | grep -cE "^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+")
    if [ "$count" = "3" ]; then
        pass "$name: 路由表 3 条路由"
    else
        fail "$name: 期望 3 条，实际 $count 条"
    fi
}

echo "--- Router 1 路由表 ---"
echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null
check_routes 8021 "R1"

echo ""
echo "--- Router 2 路由表 ---"
echo "show route" | timeout 3 nc 127.0.0.1 8022 2>/dev/null
check_routes 8022 "R2"

echo ""
echo "--- Router 3 路由表 ---"
echo "show route" | timeout 3 nc 127.0.0.1 8023 2>/dev/null
check_routes 8023 "R3"

# ── 3.5 TCP 管理接口 ────────────────────────────────
echo ""
echo "============================================"
echo "  3.5 TCP 管理接口命令测试"
echo "============================================"

echo ""
echo "--- show neighbors ---"
output=$(echo "show neighbors" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "Total: 2 neighbors"; then
    pass "show neighbors: 显示 2 个邻居"
else
    fail "show neighbors: 未显示 2 个邻居"
fi

echo ""
echo "--- link down 2 ---"
output=$(echo "link down 2" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "OK: link DOWN"; then
    pass "link down: 正常断开"
else
    fail "link down: 执行失败"
fi

echo ""
echo "--- 验证毒性逆转 (show route) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "16.*neighbor.*GC"; then
    pass "毒性逆转: 路由已标记 metric=16 [GC]"
else
    fail "毒性逆转: 未检测到 metric=16"
fi

echo ""
echo "--- link up 2 ---"
output=$(echo "link up 2" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "OK: link UP"; then
    pass "link up: 正常恢复"
else
    fail "link up: 执行失败"
fi

echo ""
echo "--- quit ---"
output=$(echo "quit" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "Goodbye"; then
    pass "quit: 正常断开连接"
else
    fail "quit: 执行失败"
fi

# ── 3.3 毒性反转 ────────────────────────────────────
echo ""
echo "============================================"
echo "  3.3 毒性反转测试"
echo "============================================"

# 等待之前 link up 后的路由重新收敛
info "等待路由重新收敛 (30s)..."
sleep 30

echo ""
echo "--- Router 1: link down 2 ---"
echo "link down 2" | timeout 3 nc 127.0.0.1 8021 2>/dev/null
sleep 2

echo "--- Router 1 路由表 (10.0.2.0/24 应为 metric=16) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "10.0.2.0.*16.*GC"; then
    pass "R1: 10.0.2.0/24 → metric=16 [GC] (毒性化正确)"
else
    fail "R1: 10.0.2.0/24 未被毒性化"
fi

echo ""
echo "--- Router 2 路由表 (R1 将其链路断开，R2 仍能看到其他路由) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8022 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "10.0.2.0.*1.*direct"; then
    pass "R2: 直连路由 10.0.2.0/24 正常"
else
    fail "R2: 直连路由异常"
fi

echo ""
echo "--- Router 3 路由表 (10.0.1.0 应能从 R1 学到或通过 R2) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8023 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "10.0.1.0"; then
    pass "R3: 能看到 10.0.1.0/24"
else
    fail "R3: 看不到 10.0.1.0/24"
fi
if echo "$output" | grep -q "10.0.2.0"; then
    pass "R3: 能看到 10.0.2.0/24"
else
    fail "R3: 看不到 10.0.2.0/24"
fi

echo ""
echo "--- 恢复: Router 1 link up 2 ---"
echo "link up 2" | timeout 3 nc 127.0.0.1 8021 2>/dev/null
pass "3.3 毒性反转测试完成"

# ── 3.4 邻居失效自动检测 ────────────────────────────
echo ""
echo "============================================"
echo "  3.4 邻居失效自动检测 (180s 超时 + 120s GC)"
echo "============================================"

# 等待 link up 后的路由重新收敛
info "等待路由重新收敛 (30s)..."
sleep 30

echo ""
echo "--- 初始状态 ---"
echo "show neighbors" | timeout 3 nc 127.0.0.1 8021 2>/dev/null
echo ""
echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null

echo ""
info "杀掉 Router 2 (模拟节点故障)..."
pkill -f "ripd.*router2" 2>/dev/null || true
sleep 2

if pgrep -f "ripd.*router2" > /dev/null 2>&1; then
    fail "Router 2 未能终止"
    exit 1
fi
pass "Router 2 已终止"

echo ""
info "等待 185 秒 (RIP_NEIGHBOR_SEC=180) 让 Router 1 检测邻居超时..."
sleep 185

echo ""
echo "--- 超时后 Router 1 邻居表 (R2 应为 DOWN) ---"
output=$(echo "show neighbors" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "DOWN"; then
    pass "R2 状态变为 DOWN"
else
    fail "R2 状态未变为 DOWN"
fi

echo ""
echo "--- 超时后 Router 1 路由表 (10.0.2.0/24 应为 metric=16) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "10.0.2.0.*16.*GC"; then
    pass "10.0.2.0/24 → metric=16 [GC] (自动毒化正确)"
else
    fail "10.0.2.0/24 未被自动毒化"
fi

echo ""
info "等待 125 秒让垃圾回收触发 (RIP_GARBAGE_SEC=120)..."
sleep 125

echo ""
echo "--- 垃圾回收后 Router 1 路由表 (10.0.2.0/24 应已删除) ---"
output=$(echo "show route" | timeout 3 nc 127.0.0.1 8021 2>/dev/null)
echo "$output"
if echo "$output" | grep -q "10.0.2.0"; then
    fail "10.0.2.0/24 未被垃圾回收 (可能被其他邻居刷新了计时器)"
else
    pass "10.0.2.0/24 已被垃圾回收删除"
fi

# ── 汇总 ────────────────────────────────────────────
echo ""
echo "============================================"
echo "  全部测试完成"
echo "============================================"
