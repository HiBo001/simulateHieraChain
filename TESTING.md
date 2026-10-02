# Arbor 仿真系统验收说明

所有命令在项目根目录执行。每一组手动故障测试建议先停止旧集群、启动一轮新实验；停止的副本不要在原运行目录直接重启。

## 1. 一键自动测试

```bash
make
make test
```

`make test` 依次运行网络专项、隔离清理检查、第一阶段回归、第二阶段 A 排序证书、第二阶段 B 执行、工程专项及性能脚本统计检查。测试会选择空闲端口范围，创建独立运行目录，启动真实节点进程，结束时自动停止；不改变普通运行的 `runtime/latest` 指向。第一阶段结果保留在 `test-results/<时间-编号>/`，其中 `summary.json` 应显示：

```json
{"tests": 14, "failures": 0, "errors": 0, "passed": true}
```

14 个测试包括：配置与 NCA、10 种非法配置、负载可重复性、正常共识/检查点/请求去重/伪造签名、备份节点离线、主节点退出恢复、两个节点无法提交、视图恢复必须保留 prepared 值与完整 commit 证据、暂停副本追赶、1000 笔大批次跨第 16 个检查点继续处理、分片对延迟与并发消息、多层 28 节点和协调者计数，以及启停与端口冲突。

2026-10-02 此前协议简化版本 `make test` 退出码 0，合计 **85 项通过**：网络 5、clean 18、第一阶段 14、第二阶段 A 1、第二阶段 B 5、工程 19、benchmark 23。历史日志保存在 `test-results/benchmark-20261002-205157-2422a358/validation-make-test.log`。本次不整除组批修复后的完整回归为 **86 项通过**，结果见下文工程专项。

故障测试只针对本次创建并记录的进程。测试日志中会出现一次 `completed=0`，这是“只剩两个节点”的预期结果，并不是测试失败。

## 2. 正常处理与一致性

```bash
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py load --shard 1 --count 100 --rate 100 --seed 42
python3 scripts/cluster.py status
```

等待约 1 秒再看状态。预期：分片 1 的四个副本均 `executed=100`，其他分片为 0。`status --json` 中同一分片四个副本的 `state_digest`、`chain_digest` 和 `applied_batches` 相同。客户端报告 100 笔，不是四副本相加的 400 笔。

客户端只等两个一致回复，状态文件又每约 100 ms 更新，所以负载命令刚结束时四个快照可能短暂不同；应在排空后比较。正常运行的 `commits.jsonl` 中可检查每个序号的 `value_digest`，证书里的签名集合可以不同。

## 3. 重复交易不能重复执行

```bash
python3 scripts/cluster.py load --shard 1 --count 40 --rate 100 --seed 7 --id-prefix duplicate-test
python3 scripts/cluster.py load --shard 1 --count 40 --rate 100 --seed 7 --id-prefix duplicate-test
python3 scripts/cluster.py status
```

如果接着上一测试运行，预期四个节点累计 `executed=140`，不是 180。测试参数必须相同；不要用相同前缀发送不同内容。

## 4. 停止一个备份节点

```bash
./stop_all.sh
./start_all.sh --config config/single_shard.json
python3 scripts/cluster.py kill-node --shard 1 --replica 3
python3 scripts/cluster.py load --shard 1 --count 100 --rate 100 --timeout 15
python3 scripts/cluster.py status
```

预期：节点 3 为 `alive=False`，其他三个节点继续完成 100 笔；正常情况下保持 `view=0`、`primary=0`。

## 5. 停止主节点并切换视图

```bash
./stop_all.sh
./start_all.sh --config config/single_shard.json
python3 scripts/cluster.py load --shard 1 --count 40 --rate 100
python3 scripts/cluster.py kill-node --shard 1 --replica 0
python3 scripts/cluster.py load --shard 1 --count 100 --rate 100 --timeout 20
python3 scripts/cluster.py status
```

预期：三个存活节点最终累计 `executed=140`，`view>=1`，主节点变为 1（若环境造成更多超时，可能进入更高视图）。视图切换由待处理交易超时触发，空闲时停止主节点不会立即选举。

`events.jsonl` 中应有 `view_change_started` 和 `new_view_installed`。切换包含 prepared 证据验证，不只是改变主节点编号。

## 6. 只剩两个节点时禁止提交

```bash
./stop_all.sh
./start_all.sh --config config/single_shard.json
python3 scripts/cluster.py kill-node --shard 1 --replica 2
python3 scripts/cluster.py kill-node --shard 1 --replica 3
python3 scripts/cluster.py load --shard 1 --count 20 --rate 100 --timeout 5
python3 scripts/cluster.py status
```

预期：负载命令退出码为 **2**，完成数为 0，存活节点 `executed=0`。两个节点不足以形成 3 个不同副本的 COMMIT 法定人数。

## 7. 检查网络延迟

```bash
./stop_all.sh
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py probe --source 1:0 --to 2:0 --samples 5
python3 scripts/cluster.py probe --source 1:0 --to 5:0 --samples 5
python3 scripts/cluster.py probe --source 2:0 --to 5:0 --samples 5
python3 scripts/cluster.py probe --source 1:0 --to 1:2 --samples 5
```

| 链路 | 配置单向延迟 | 预期 RTT |
|---|---:|---:|
| 1↔2 | 50 ms | 约 100 ms 加实际开销 |
| 1↔5 | 10 ms | 约 20 ms 加实际开销 |
| 2↔5 | 30 ms | 约 60 ms 加实际开销 |
| 分片 1 内节点 0↔2 | 1 ms | 约 2 ms 加实际开销 |

修改 `shard_links` 后需要重新启动。检验默认值时可删除某一对配置，该链路应改用 `default_inter_shard_delay_ms`。将 `network.trace` 设为 `true` 后可检查 `release_ms >= due_ms`；操作系统调度和队列拥塞会使实际发送稍晚。

## 8. 改变分片数量与拓扑

```bash
./stop_all.sh
python3 scripts/cluster.py validate --config config/three_layer.json
python3 scripts/cluster.py topology --config config/three_layer.json
./start_all.sh --config config/three_layer.json
python3 scripts/cluster.py topology
python3 scripts/cluster.py status
python3 scripts/cluster.py load --shard 4 --count 40 --rate 100
python3 scripts/cluster.py load --participants 1,2 --count 20 --rate 100
python3 scripts/cluster.py load --participants 1,3 --count 20 --rate 100
```

预期：`topology --config` 与启动后的 `topology` 打印相同的树：根 7，下接协调者 5、6，叶子 1、2 在 5 下，叶子 3、4 在 6 下；兄弟分片按 ID 升序排列。28 个节点运行；分片 4 四副本执行 40 笔；协调者 5 和 7 各排序 20 笔。协调者显示 `ordered_only=20`、`executed=0`。这些排序样例用来验证 NCA 和协调者 PBFT，不代表跨片执行已完成。

## 9. 一键停止与再次启动

```bash
./stop_all.sh
python3 scripts/cluster.py status
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py status
./stop_all.sh
```

预期：停止后所有节点 `alive=False`；再次启动使用新运行目录和初始状态，端口可复用，旧实验日志保留。

## 遇到问题时保留什么

请保留本次 `runtime/latest` 实际指向的目录，重点是 `config.json`、`manifest.json`、失败节点的 `node.log` / `events.jsonl` / `status.json`，以及对应 `client-*.json`。不需要先删除目录重试。

排查结束后用 `make clean` 清理编译产物、运行日志和全部 `test-results/` 报告。此命令会先停止记录中的活动节点；负载、性能或测试任务仍在运行时会拒绝清理。源码和 `config/` 保留，历史性能基线也属于被删除的结果，需提前另存。清理逻辑检查可单独执行 `python3 -B tests/test_clean.py`；它使用隔离临时目录，不清理真实实验记录。

- `bind/listen failed`：端口占用或 `host` 不是本机地址。
- `prepared` 一直没有 `committed_local`：检查节点存活数量、消息验证错误、片内延迟与超时。
- 客户端超时但稍后节点完成：增加 `--timeout`；超时不会撤销已发送交易。
- `network_failures` 增加：可能是故障测试中的离线目标，也可能是队列上限/帧大小/连接超时；结合节点状态和事件判断。
- 状态暂时不同：等待排空；若持续不同，保留完整日志。

## 第二阶段 A：带证书的二层跨片排序下发

```bash
make
python3 tests/test_stage2a.py
```

该测试启动 12 个真实节点，向叶子 1、2 各需参与的跨片交易发送带访问集的负载，验证协调者排序证书由两个叶子独立验签后进入各自 PBFT 日志。篡改证书中的 COMMIT 签名以及缺少跨片访问集的交易必须被拒绝。当前代码会继续执行并提交这些交易；`leaf_ordered_cst` 仅表示排序，最终完成看 `executed` 和客户端 `completed`。

## 第二阶段 B：跨片执行与最终确认

```bash
make
python3 tests/test_stage2b.py
```

本轮验收按简化协议检查：两个独立客户端请求读写同一对 key 并合入一个有序跨片批次，根只增加一个 ORDER 槽，两叶各增加一个执行槽；不出现 READY/DECISION/FINALIZE/DONE 业务阶段。批内远端读值仍影响写入，两个叶子副本状态收敛；协调片验证双方完成 QC 后直接回复，错误订单或执行摘要的 ACK 必须拒绝。其余场景包括叶子少一个备份仍可形成三副本法定人数、参与叶子缺失时不会把排序计为完成或暴露尚未取得依赖的写入，以及小客户端请求组批后跨越检查点仍可推进。测试通过情况以本轮命令输出为准，旧 6 次 PBFT 路径的通过记录不能代替本轮验收。

手动运行：

```bash
./stop_all.sh
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py load --participants 1,2 --count 40 --rate 100 --batch 8 --timeout 30
sleep 1
python3 scripts/cluster.py status
./stop_all.sh
```

预期客户端报告 `completed=40 ordered_only=0 requests=5/5` 并显示完成 TPS 与秒单位延时。协调分片四副本的 `ordered_only=40`、`completed_cst=40`；两个叶子四副本各自 `leaf_ordered_cst=40`、`executed=40`、`staged_cst=0`。详见 [docs/STAGE2B_DESIGN.md](docs/STAGE2B_DESIGN.md)。

`--batch 8` 仅限制每个客户端请求最多 8 笔；`config/two_layer.json` 的 `cross_shard_batch_size=64` 独立限制协调分片组批，`cross_shard_batch_wait_ms=600` 是额外组批等待阈值，不是逐交易延时上界。达到上限，或下一个有效、交易 ID 无重叠的完整请求放不下，或参与分片集合发生边界时，当前非空批可提前提出；没有这些阻挡的小批仍等待窗口。签名客户端请求不会为了填剩余容量而拆分。相同参与分片集合可合批，即使 key 不同；重复 key 在批内按交易顺序执行。`--timeout` 包含发送时间；例如 4000 笔按 `--rate 100` 的发送过程约需 40 秒，设置 30 秒无法全部完成。长负载请另外预留共识和排空时间。

`--batch 10` 在 64 笔共识上限下会形成六个完整请求共 60 笔；第七个请求放不下时，现在提前发送这 60 笔，不必等待精确凑到 64 笔。最后只有一个请求的尾批仍可等待 `cross_shard_batch_wait_ms`。

大负载对照命令（运行前先 `make` 并启动新的二层集群）：

```bash
python3 scripts/cluster.py load --participants 1,2 --count 4000 --rate 100 --batch 8 --timeout 120
```

完成后查看 `requests=500/500`、`completed=4000`、`completed_tps` 和秒单位延时；再运行 `python3 scripts/cluster.py status`，确认协调分片的 `cst_batches` 明显少于 500，`avg_cst_batch` 大于 8。**旧 6 次 PBFT 版本的历史记录**：引入 TCP 长连接后的某轮隔离实测为 63 个协调批次、平均 63.5 笔/批，4000/4000 完成，耗时 40.73 秒、`completed_tps=98.20`、`avg_latency_s=0.627`、`p95_s=0.911`；12 个节点均无网络失败或视图切换。该数据不是简化协议的性能测量。`--rate 100` 意味着 4000 笔约需 40 秒发送，整轮平均 TPS 因而接近 100。

要测试系统处理能力，可在**新集群**上提高发送速率：

```bash
python3 scripts/cluster.py load --participants 1,2 --count 4000 --rate 1000 --batch 8 --timeout 60
```

同一旧 6 次 PBFT 版本的另一轮隔离实测 4000/4000 完成，耗时 17.36 秒、`completed_tps=230.40`，但 `avg_latency_s=6.534`、`p95_s=15.501`，表明当时高到达率会积压请求。以上仅保留为历史单次记录，不能当作新 3 次 PBFT 协议的结果或同条件多轮基线；新路径性能须重新运行 benchmark 测量。

## 工程专项：forward、去重与网络缓存

```bash
make
python3 -B tests/test_engineering.py
make build/bin/test_network
build/bin/test_network
```

工程测试检查：跨片默认批次及超限请求；相同交易换请求 ID 后不新增共识；进行中的交易别名合并；混合新旧交易保持客户端签名、避免重复 ID 造成无效组批；forward 收齐三个不同副本签名后才跨片转发；forward 在暂存阶段退出后恢复；协调副本跨检查点追赶后通过双叶 ACK 证书恢复完成回复；不足三签名、重复签名者及冲突请求提案被拒绝。

本次针对不整除批大小新增组批回归，可单独运行：

```bash
make
python3 -B tests/test_engineering.py Integration.test_indivisible_requests_fill_batch_without_waiting_for_unreachable_limit
```

该用例发送 70 笔、每请求 10 笔，设置跨片上限 64、额外等待 2500 ms；首批保留六个完整签名请求共 60 笔，在 1.5 秒内完成根排序，不等待 2500 ms；剩余 10 笔正常走尾批等待，最终两批共 70 笔完成且全副本状态一致。**本次用例及完整 `make test` 已通过，退出码 0，合计 86 项：网络 5、clean 18、第一阶段 14、第二阶段 A 1、第二阶段 B 5、工程 20、benchmark 23。** 日志为 `test-results/benchmark-20261002-220743-387996e2/validation-make-test.log`；此前 85 项仍保留为历史记录。

`status` 普通输出新增 `forward=`，初始为 0，视图切换后随本片 primary 变化。`status --json` 提供连接复用数、连接尝试数、待发送字节等工程指标。详情见 [docs/ENGINEERING_REVIEW.md](docs/ENGINEERING_REVIEW.md)。

## 一键性能测试与重复对照

```bash
python3 scripts/benchmark.py
```

脚本自动编译，依次测试片内和跨片的 4000 笔负载，发送速率为 1000、4000；每个用例使用新集群、空闲端口，结束时停止节点。结果保存到 `test-results/benchmark-*/summary.csv` 和 `summary.json`；已有普通集群和 `runtime/latest` 不受影响。参数、重复三轮与基线比较见 [docs/BENCHMARK.md](docs/BENCHMARK.md)。脚本统计回归可单独执行 `python3 -B tests/test_benchmark.py`。

此前协议简化版本（客户端批次 8）的三轮复测命令：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --repeat 3 --seed 42 --cross-batch 8
```

在本机 macOS 15.6、arm64、8 核环境和 `config/two_layer.json` 下，三轮均 4000 笔/500 请求全部确认、全副本收敛、网络失败为 0。TPS 中位数 **625.81**，平均延时的轮次中位数 **1.301094 s**，p95/p99 的轮次中位数 **2.313776/2.396804 s**。原始报告为 `test-results/benchmark-20261002-205157-2422a358/summary.json` 和 `summary.csv`；与旧 258.34 TPS 单轮记录不构成正式固定倍数对照。`make clean` 会删除这些报告和验收日志，需长期保留时先复制到仓库外。

本次组批修复后的批次 10 复测命令：

```bash
python3 scripts/benchmark.py --mode cross --count 4000 --rates 1000 --repeat 3 --seed 42 --cross-batch 10
```

三轮均 4000 笔/400 请求全部完成、全副本收敛、网络失败为 0。TPS 为 **645.82 / 619.01 / 635.82**，中位数 **635.82**；平均延时、p95、p99 的轮次中位数分别为 **1.197158 / 2.178143 / 2.294141 秒**。报告为 `test-results/benchmark-20261002-220743-387996e2/summary.json` 和 `summary.csv`。前后两组报告客户端批次不同，不能直接用作同参数加速倍数对照；`make clean` 同样会删除本次报告与日志。
