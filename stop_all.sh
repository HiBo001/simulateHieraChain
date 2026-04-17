#!/bin/bash

# stop_all.sh - 停止所有分片节点（改进版）

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HIERACHAIN_BIN="$PROJECT_ROOT/build/bin/hierachain"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}停止所有 Hierachain 分片节点...${NC}"

# 方法1：通过 PID 文件停止
shard_dirs=$(find "$PROJECT_ROOT" -maxdepth 1 -type d -name "shard*" | sort)

for shard_dir in $shard_dirs; do
    shard_name=$(basename "$shard_dir")
    pid_file="$shard_dir/node.pid"
    
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${GREEN}停止 $shard_name (PID: $pid)${NC}"
            kill "$pid"
            rm "$pid_file"
        else
            echo -e "${YELLOW}$shard_name PID 文件存在但进程已不存在 (PID: $pid)${NC}"
            rm "$pid_file"
        fi
    fi
done

# 等待进程优雅退出
sleep 2

# 方法2：强制清理所有 hierachain 进程（确保没有遗漏）
echo -e "${GREEN}检查残留进程...${NC}"
remaining_pids=$(pgrep -f "$HIERACHAIN_BIN" 2>/dev/null)

if [ -n "$remaining_pids" ]; then
    echo -e "${YELLOW}发现残留进程，强制终止:${NC}"
    for pid in $remaining_pids; do
        # 获取进程的工作目录
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS
            proc_cwd=$(lsof -p $pid -d cwd -Fn | head -1 | sed 's/^n//')
        else
            # Linux
            proc_cwd=$(readlink -f /proc/$pid/cwd 2>/dev/null)
        fi
        
        echo -e "${YELLOW}  PID: $pid, 工作目录: $proc_cwd${NC}"
        kill -9 "$pid" 2>/dev/null
    done
else
    echo -e "${GREEN}没有残留进程${NC}"
fi

# 清理孤立的 PID 文件（可选）
echo -e "${GREEN}清理孤立的 PID 文件...${NC}"
for shard_dir in $shard_dirs; do
    pid_file="$shard_dir/node.pid"
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        if ! kill -0 "$pid" 2>/dev/null; then
            rm "$pid_file"
            echo -e "${YELLOW}  清理 $shard_dir/node.pid${NC}"
        fi
    fi
done

echo -e "${GREEN}完成${NC}"
