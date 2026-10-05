# AHL：单上层分片的 2PC 基线

AHL 按本项目约定实现：所有叶子分片直接连接到一个上层分片，由这个上层分片协调所有跨片交易，处理协议复用 Saguaro 的传统 2PC。每个分片固定四个副本，真实运行 PBFT；参与叶子真实执行交易的 Fibonacci 计算和读写状态更新。这里的 AHL 是用户指定的对比方法名称，没有额外声称复现某篇同名论文。

与 Arbor 比较时保留 Arbor 的多层拓扑，AHL 使用包含同一批叶子的两层拓扑。示例配置为 `config/ahl_two_layer.json`：

```text
       7
   ┌───┼───┬───┐
   1   2   3   4
```

所有跨片交易的协调者均为 7；片内交易仍只进入相应叶子。启动器和 AHL 二进制都检查两层结构，传入多层树会报错，不会悄悄以多层 Saguaro 运行。

## 构建与单独运行

在项目根目录执行：

```bash
make ahl
python3 -B scripts/cluster.py start --config config/ahl_two_layer.json --method ahl
python3 -B scripts/cluster.py topology
python3 -B scripts/cluster.py load --participants 1,2 --count 4000 --rate 1000 --batch 10 --timeout 120
python3 -B scripts/cluster.py status
python3 -B scripts/cluster.py stop
```

`load` 根据当前运行目录中的方法标识选择 AHL 客户端。片内负载使用相同接口，例如：

```bash
python3 -B scripts/cluster.py load --shard 1 --count 10000 --rate 1000 --batch 10 --timeout 120
```

`--batch` 是每个客户端请求的交易数；`consensus.cross_shard_batch_size` 是一次跨片业务批次的交易上限。它们与本地 PBFT 的 `consensus.batch_size` 分别控制不同的批量。AHL 的每个跨片业务批次仍需要上层 INIT、叶子 PREPARE、上层 DECIDE 和叶子 FINISH 四个有先后关系的阶段，每个阶段分别运行对应分片的 PBFT。

## Arbor 多层与 AHL 两层的同负载比较

固定三个参与叶子：

```bash
python3 -B baseline/compare.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json \
  --mode cross --participants 1,2,3 \
  --count 4000 --rate 1000 --batch 10 --repeat 3 --seed 42 \
  --timeout 120 --drain-timeout 60
```

带访问局部性的 10000 笔全跨片交易，其中 90% 跨两个叶子、10% 跨三个叶子，只有 5% 跨 cluster：

```bash
python3 -B baseline/compare_mixed.py --baseline ahl \
  --config config/three_layer_locality.json \
  --baseline-config config/ahl_two_layer_locality.json \
  --count 10000 --rate 5000 --batch 10 --repeat 3 --seed 42 \
  --cross-cluster-ratio 0.05 --three-shard-ratio 0.10 \
  --timeout 180 --drain-timeout 60
```

新参考树包含 `{1,2,8}`、`{3,4,9}` 两个 cluster，它们分别由直接父分片 5、6 定义。AHL 把六个叶子全部接到唯一上层 7，但负载的 cluster 标签仍来自 Arbor 参考树。不能按照 AHL 拓扑重新把六个叶子看成一个 cluster。此例同 cluster 两方/跨 cluster 两方/同 cluster 三方/跨 cluster 三方分别为 8550/450/950/50 笔，参与叶子和业务输入均与 Arbor 相同。

两个对比入口自动构建、依次启动两个独立集群、发送同一份负载、等待副本收敛，然后停止集群，不需要提前手动 `start`。重复轮次交替先后顺序。`--config` 指定 Arbor 拓扑，`--baseline-config` 指定 AHL 拓扑；不会自动压平 Arbor 的配置。省略 `--baseline-config` 会让双方使用同一份配置，多层配置因此不能用于启动 AHL。

已有负载可以直接复用：

```bash
python3 -B baseline/compare_mixed.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json \
  --workload test-results/mixed-90-10/shared-workload.json \
  --repeat 3 --timeout 180 --drain-timeout 60
```

`--workload` 与生成参数互斥。文件中跨片请求的 `target` 保留 Arbor 原始 NCA；AHL 客户端在签名前改成自己的唯一上层分片。交易 ID、请求 ID、参与叶子、key、value、请求分组、发送速率和顺序不改变，原始文件不被改写。这个路由目标变化是双拓扑比较的必要差异，会在比较报告中说明。

结果保存到 `test-results/compare-*`，包含同一份 workload、两份配置快照及其差异、各轮客户端结果、全部节点状态，以及 `summary.json`、`summary.csv`、`summary.md`。入口要求双方叶子 ID、四副本配置、host、完整共识参数、执行参数、片内延时和 trace 设置一致；共同分片间的有效单向延时也必须相同。父关系、中间分片数量和端口可以不同。拓扑、协调者分布和进程数量是本次架构比较的变量：新局部性配置中 Arbor 有 9 个分片、36 个节点进程，AHL 有 7 个分片、28 个节点进程；原四叶示例仍为 7 片/28 节点与 5 片/20 节点。

同时比较 Arbor、Saguaro、SharPer、AHL 可用 `baseline/compare_all.py --config config/three_layer_locality.json --ahl-config config/ahl_two_layer_locality.json`。它一次生成共享负载，按参考树分类并重放到四种方法，不需要为 AHL 单独生成负载。完整参数、比例和生成命令见 [../../docs/WORKLOAD_LOCALITY.md](../../docs/WORKLOAD_LOCALITY.md)。

只有双方全部交易完整完成、执行计数正确、四副本状态收敛且队列和锁排空的轮次才计算 TPS 比值；任何失败轮次都会让该组比值留空。TPS 以客户端确认的唯一交易计数，平均及 p50/p95/p99 延时单位均为秒。一次短测只用于验证接口与完整性，正式实验请保留三轮以上重复和机器信息。

## 状态与回归测试

节点状态中的 `method` 为 `ahl`，`ahl_coordinator` 显示唯一上层分片，`ahl_topology` 为 `two-layer`。协议复用 Saguaro，状态字段 `sag_*` 和事件名称 `saguaro_*` 继续保留，便于使用同一套排空和故障恢复检查；这些命名不表示运行了另一个方法。

参与叶子统计自己的实际执行数；上层统计跨片排序和最终完成数，不代替叶子执行交易。排空时需要 `sag_active_batches`、`sag_pending_prepares`、`sag_pending_decisions`、`sag_pending_completions`、`sag_held_locks` 以及普通请求和网络等待队列均为空。

```bash
make test-ahl
```

该目标运行 AHL 的工具和真实集群回归；完整项目回归使用 `make test`。具体测试记录由本版本的验证报告给出。协议流程、锁和重复请求的处理见 [DESIGN.md](DESIGN.md)。`make clean` 会停止受管理的节点并清理生成的运行目录、二进制和测试结果；长期保留的 workload 和报告应另行备份。
