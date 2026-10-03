# 一键性能测试

在仓库目录执行。脚本只使用 Python 标准库，默认自动运行 `make`：

```bash
cd /Users/tanghaibo_office/Desktop/ATLAS/sourceCode/simulateHieraChain
python3 scripts/benchmark.py
```

默认使用 `config/two_layer.json`，分别测试片内和跨片交易；每轮 4000 笔，发送速率为 1000、4000 笔/秒，各运行一次。片内默认客户端批次取配置的 `batch_size`，跨片默认每个客户端请求包含 8 笔。需要直接使用已编译二进制时加 `--skip-build`。

每个用例都会在空闲端口启动一个新集群，结束或按 Ctrl+C 后停止本轮节点，不修改 `runtime/latest`，不停止已有集群。每个分片仍有 4 个真实运行的 PBFT 副本。片内目标须是叶子；跨片 `--participants` 须指定至少两个不重复叶子，目标自动取最近公共祖先 NCA。三层、四层、非均匀深度树和超过两个参与叶子的完整执行均可测试。默认片内目标为分片 1，跨片参与分片为 1、2，可用 `--shard`、`--participants` 调整。

当前版本已移除流水线。自定义配置若含 `consensus.pipeline_window`（包括 0），请删除后再运行。

## 常用指令

快速检查片内与跨片路径：

```bash
python3 scripts/benchmark.py --count 256 --rates 1000 --intra-batch 32
```

复测之前的跨片负载，保持 4000 笔、1000 笔/秒、客户端批次 8：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --cross-batch 8
```

检查分片 1 的片内吞吐，逐级提高发送速率：

```bash
python3 scripts/benchmark.py --mode intra --shard 1 --count 10000 --rates 1000,4000,8000 --intra-batch 1000
```

这条指令要求配置中的 `batch_size` 至少为 1000。`--intra-batch`、`--cross-batch` 分别控制一个客户端请求包含多少笔交易，不能超过对应的共识批次上限；跨片多个小请求仍可被协调片合并到一个共识批次。

研究实验建议至少重复三轮，报告中位数：

```bash
python3 scripts/benchmark.py --mode all --count 10000 --rates 1000,4000 --repeat 3
```

复测此前两层三分片、仅双方参与的负载：

```bash
python3 scripts/benchmark.py --config config/two_layer.json --mode cross --participants 1,2 --count 4000 --rates 4000 --cross-batch 8 --repeat 3 --seed 42 --timeout 90
python3 scripts/benchmark.py --config config/two_layer.json --mode cross --participants 1,2 --count 10000 --rates 4000 --cross-batch 8 --repeat 3 --seed 42 --timeout 90
```

客户端每轮默认超时为 `count / rate + 60` 秒，包含负载发送和等待确认；之后最多等待 30 秒，让其余副本排空并收敛。需要更长时间时指定：

```bash
python3 scripts/benchmark.py --mode cross --count 10000 --rates 1000 --timeout 180 --drain-timeout 60
```

## 多层性能测试

```bash
python3 scripts/benchmark.py --config config/three_layer.json --mode cross --participants 1,2 --count 256 --rates 100 --cross-batch 8 --repeat 3 --timeout 180 --drain-timeout 60
python3 scripts/benchmark.py --config config/three_layer.json --mode cross --participants 1,3 --count 256 --rates 100 --cross-batch 8 --repeat 3 --timeout 180 --drain-timeout 60
python3 scripts/benchmark.py --config config/three_layer.json --mode cross --participants 1,2,3 --count 256 --rates 100 --cross-batch 8 --repeat 3 --timeout 180 --drain-timeout 60
python3 scripts/benchmark.py --config config/four_layer.json --mode cross --participants 1,8 --count 128 --rates 100 --cross-batch 8 --repeat 3 --timeout 240 --drain-timeout 60
```

多协调者拓扑有按需轮次和 PBFT 封闭控制槽，每个协调者即使本轮没有业务请求也必须证明该轮已封闭。只有实际 NCA 的排序和完成业务计数应为 count，只有参与叶子执行 count 笔；无关协调者允许出现空封闭槽，但业务计数应为 0，其他叶子也不应执行本轮交易。benchmark 检查这些条件，并要求每个分片内部四副本应用槽数及摘要一致。所有进程共享 CPU，因此跨层数量与节点数量都会影响本机结果；控制槽的网络与共识成本计入测量。

## 结果与统计口径

脚本打印结果目录，默认保存在 `test-results/benchmark-时间-随机标识/`。可用 `--output-dir` 指定一个尚不存在的目录。

`make clean` 会删除整个 `test-results/`，包括历史性能报告和基线；需要长期保留的结果先复制到仓库外。仓库外显式指定的输出目录不自动清理。

- `summary.csv`：每轮的 TPS、平均延时、p50/p95/p99 延时、网络流量、TCP 连接尝试和复用次数、失败原因。延时单位均为秒。
- `summary.json`：逐轮结果、配置快照、二进制摘要、版本与机器信息；`aggregates` 给出同配置、同负载各指标的轮次中位数。只要组内有失败轮次，中位数就留空。
- `case-*/client.json`、`workload.json`、`client.log`：客户端逐笔确认时间、输入负载和输出。
- `case-*/status-before.json`、`status-after.json` 与 `case-*/run/`：负载前后节点状态和各节点日志。

TPS 以客户端确认的唯一交易数除以客户端负载时间计算。它包含发送过程、等待两个一致的签名回复以及协议提交完成所需的时间；不包含集群启动时间和客户端结束后等待其他副本排空的时间。4 个副本执行同一笔交易只计一笔，多个叶子执行同一笔跨片交易也只计一笔。

`PASS` 要求请求全部确认、执行笔数正确，以及各分片四副本状态摘要一致，待处理交易和网络发送缓存排空。超时或未完成的轮次标为 `FAIL`，TPS 和延时汇总留空，原始客户端结果仍保留；不能把局部完成的 TPS 用作性能对照。

发送按客户端请求成批进行。交易数量很小或客户端批次很大时，负载呈明显突发，最后一个批次无需等待它自身对应的发送间隔，因此测得 TPS 可能高于设置的发送速率。测试持续吞吐建议 `count >= 10 × 客户端批次大小`；如果发送速率低于系统处理能力，TPS 主要反映限速值，升压测试才能观察处理上限。

`network_failures` 是独立诊断项：客户端收到两个一致回复后关闭，其他副本晚到的回复可能触发连接错误，不能仅据此判断交易失败。`network_connect_attempts` 包含新建连接和重连尝试，`network_connections_reused` 记录长连接复用。网络字节是整个集群的传输开销，包含 PBFT 副本间通信。网络跟踪沿用配置中的 `trace`，性能测试通常保持 `false`，避免逐消息日志影响结果。

## 与基线比较

保留旧结果的 `summary.json`，在新版本使用相同参数，并通过 `--baseline` 对照。例如先保存一组基线：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --cross-batch 8 --repeat 3 --output-dir test-results/perf-before
```

在待比较的版本运行：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --cross-batch 8 --repeat 3 --baseline test-results/perf-before/summary.json --output-dir test-results/perf-after
```

脚本在启动集群前验证基线结构。只有配置、负载参数、随机种子和机器/系统环境匹配，且相关轮次全部成功，才计算 TPS 中位数倍数，并保存到 `summary.json` 的 `comparisons`。单轮结果适合观察趋势；正式比较建议至少三轮，同时保持机器上的其他负载相近。减少重复共识和通信开销预期有助于高负载表现，具体提升幅度以同条件实测为准。

## 旧 6 次 PBFT 版本的验证记录（2026-10-02）

脚本的 21 项统计单元检查通过；小规模片内/跨片负载、基线比较、客户端超时及 SIGTERM 中断清理均验证通过。中断后的节点清理只操作本次运行的进程。

使用本机 `config/two_layer.json`，跨片 4000 笔、速率 1000、客户端批次 8、随机种子 42，旧路径某单轮全部成功：`completed_tps=258.34`、`avg_latency_s=5.431696`、`p95_s=10.361180`、`p99_s=14.004256`，网络失败差值为 0。历史报告位置为 `test-results/benchmark-20261002-154253-7a84612f/summary.json`，若已执行 `make clean`，该记录可能已被删除。**这不是新 3 次 PBFT 协议的实测值。** 之前的 230.40 TPS 也不是经脚本验证的同负载、多轮基线。

下面保留的是两层历史路径：根一次 ORDER PBFT、两叶各一次 PBFT；叶子在同一槽内等待依赖并完成执行，上层收齐两叶完成 QC 后直接确认。以下是该新路径的最终复测，不能把旧单轮 258.34 TPS 当作正式多轮基线来宣称固定倍数。

## 新 3 次 PBFT 版本的最终验证（2026-10-02）

最终源码 `make test` 退出码 0，85 项通过：网络 5、clean 18、第一阶段 14、第二阶段 A 1、第二阶段 B 5、工程 19、benchmark 23。完整日志保存在 `test-results/benchmark-20261002-205157-2422a358/validation-make-test.log`。

三轮独立集群复测：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --repeat 3 --seed 42 --cross-batch 8
```

本机环境为 macOS 15.6、arm64、8 核，配置 `config/two_layer.json`。三轮各 4000 笔/500 请求全部完成，全副本状态收敛，网络失败为 0。

| 轮次 | TPS | 平均延时 / s | p95 / s | p99 / s |
|---|---:|---:|---:|---:|
| 1 | 619.41 | 1.413786 | 2.383594 | 2.448865 |
| 2 | 625.81 | 1.301094 | 2.313776 | 2.396804 |
| 3 | 632.35 | 1.253473 | 2.246244 | 2.316098 |
| 轮次中位数 | **625.81** | **1.301094** | **2.313776** | **2.396804** |

原始报告在 `test-results/benchmark-20261002-205157-2422a358/summary.json` 和 `summary.csv`。平均延时、p95/p99 中位数分别对各轮相应指标取中位数，没有混合三轮交易重算分位数。这是所给配置和到达率的有限负载结果，不能当作稳态处理上限。`make clean` 会删除报告及验收日志，需长期保留时先复制到仓库外。
