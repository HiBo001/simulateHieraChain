# Arbor 仿真系统验收说明

所有命令在项目根目录执行。每一组手动故障测试建议先停止旧集群、启动一轮新实验；停止的副本不要在原运行目录直接重启。

## 1. 一键自动测试

```bash
make
make test
```

`make test` 依次运行 13 项第一阶段回归测试、1 项第二阶段 A 排序证书测试，以及 3 项第二阶段 B 执行测试。测试会选择空闲端口范围，创建独立运行目录，启动真实节点进程，结束时自动停止；不改变普通运行的 `runtime/latest` 指向。第一阶段结果保留在 `test-results/<时间-编号>/`，其中 `summary.json` 应显示：

```json
{"tests": 13, "failures": 0, "errors": 0, "passed": true}
```

13 个测试包括：配置与 NCA、10 种非法配置、负载可重复性、正常共识/检查点/请求去重/伪造签名、备份节点离线、主节点退出恢复、两个节点无法提交、视图恢复必须保留 prepared 值、暂停副本追赶、1000 笔大批次跨第 16 个检查点继续处理、分片对延迟与并发消息、多层 28 节点和协调者计数，以及启停与端口冲突。

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

一项测试构造两笔连续访问同一对 key 的跨片交易，验证远端读值影响本地写值、两个叶子副本状态收敛、协调分片持有三副本提交决定证书，并拒绝签名有效但决定摘要错误的 ACK。其余两项分别验证叶子少一个备份仍可形成三副本法定人数，以及整个叶子分片离线时不会把排序计为完成或暴露暂存写入。

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
