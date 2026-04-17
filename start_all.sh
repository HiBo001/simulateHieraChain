#!/bin/bash

# start_all.sh - 启动所有分片的 hierachain 节点（不拷贝可执行文件）

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/bin"
HIERACHAIN="$BUILD_DIR/hierachain"

# 颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  启动 Hierachain 所有分片节点${NC}"
echo -e "${GREEN}========================================${NC}"

# 检查可执行文件是否存在
if [ ! -f "$HIERACHAIN" ]; then
    echo -e "${RED}错误: 未找到 hierachain 可执行文件${NC}"
    echo -e "${RED}请先运行 make 编译项目${NC}"
    exit 1
fi

# 查找所有分片目录
shard_dirs=$(find "$PROJECT_ROOT" -maxdepth 1 -type d -name "shard*" | sort)

if [ -z "$shard_dirs" ]; then
    echo -e "${RED}错误: 未找到任何 shard 目录${NC}"
    exit 1
fi

# 启动每个分片节点
for shard_dir in $shard_dirs; do
    shard_name=$(basename "$shard_dir")
    
    # 检查分片目录下是否有 shardId 文件
    if [ ! -f "$shard_dir/shardId" ]; then
        echo -e "${YELLOW}警告: $shard_name 缺少 shardId 文件，跳过${NC}"
        continue
    fi
    
    echo -e "${GREEN}启动 $shard_name ...${NC}"
    
    # 切换到分片目录，但执行 build/bin 下的可执行文件
    # 这样程序运行时，当前工作目录是分片目录，可以正确读取 shardId 等配置文件
    (cd "$shard_dir" && "$HIERACHAIN" > "node.log" 2>&1 &)
    
    # 记录 PID（需要稍微延迟一下让进程启动）
    sleep 0.1
    pid=$(pgrep -f "$HIERACHAIN.*$shard_name" | head -1)
    if [ -n "$pid" ]; then
        echo $pid > "$shard_dir/node.pid"
        echo -e "${GREEN}  PID: $pid${NC}"
    fi
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}所有分片节点已启动${NC}"
echo -e "${GREEN}日志文件: shard*/node.log${NC}"
echo -e "${GREEN}PID 文件: shard*/node.pid${NC}"
echo -e "${GREEN}========================================${NC}"
