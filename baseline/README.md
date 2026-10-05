# Arbor 的对比方法

`baseline/saguaro` 提供 NCA 协调的传统 2PC；`baseline/sharper` 根据项目提供的 `SharPer.pdf` 实现参与叶子直接通信的 Byzantine flattened 跨片协议。SharPer 的实现范围、状态字段和命令见 [sharper/README.md](sharper/README.md)。

## Saguaro 2PC

`baseline/saguaro` 实现本项目的 Saguaro 对比方法：参与分片的最近公共祖先（NCA）担任传统两阶段提交（2PC）的协调者。它与 Arbor 使用同一套交易、四节点 PBFT、网络延时、执行计算、客户端确认和性能统计代码。

这里的 Saguaro 是本项目给这个 2PC 基线起的名字；本实现没有宣称复现某篇同名论文的全部协议。详细流程和取舍见 [saguaro/DESIGN.md](saguaro/DESIGN.md)。

## 构建和单独运行

在项目根目录执行：

```bash
make -j4
python3 scripts/cluster.py start --config config/two_layer.json --method saguaro
python3 scripts/cluster.py topology
python3 scripts/cluster.py load --participants 1,2 --count 4000 --rate 1000 --batch 10 --timeout 120
python3 scripts/cluster.py status
python3 scripts/cluster.py stop
```

`start` 指定算法，`load` 根据当前运行目录的 manifest 选择对应的客户端程序。启动 Arbor 时用 `--method arbor`，省略 `--method` 仍为 Arbor。每次切换算法请先停止上一轮，再启动新的运行目录。

片内交易接口与 Arbor 相同：

```bash
python3 scripts/cluster.py load --shard 1 --count 10000 --rate 1000 --batch 10 --timeout 120
```

片内交易只执行本分片 PBFT 与计算；跨片交易执行协调者 INIT/DECIDE 和各参与方 PREPARE/FINISH 共识。Saguaro 完成日志后也会显示 TPS，以及平均、p50、p95、p99 交易延时，延时单位为秒。

## 同一负载比较 Arbor 与 Saguaro

推荐用统一对比入口，避免手动启动两次时生成了不同交易：

```bash
python3 baseline/compare.py \
  --config config/two_layer.json \
  --participants 1,2 \
  --count 4000 --rate 1000 --batch 10 \
  --repeat 3 --seed 42 \
  --timeout 120 --drain-timeout 60
```

三层拓扑、三个参与方的示例：

```bash
python3 baseline/compare.py \
  --config config/three_layer_cross100.json \
  --participants 1,2,3 \
  --count 4000 --rate 1000 --batch 10 \
  --repeat 3 --seed 42 \
  --timeout 120 --drain-timeout 60
```

多个发送速率用 `--rates 500,1000,2000`。指定输出路径用 `--output-dir test-results/saguaro-comparison`。工具会自动启动、测试、等待副本收敛并停止两个方法；每个重复轮次交替先后顺序。

每个配对用例只生成一次未签名 workload，两种方法使用完全一致的交易 ID、参与方、key、value、请求分组和发送速率。每轮使用新的运行 ID 和密钥，因此签名信封的运行 ID、回复端口、签名可以不同。拓扑、PBFT 批量参数、执行迭代次数和网络延时保持相同。

结果包含原始 workload、客户端结果、各节点状态和汇总 JSON/CSV/Markdown。只有两种方法都完成全部交易、所有副本计数正确、状态收敛且待处理队列排空的配对轮次，才计算 TPS 比值。失败和超时用例保留记录，不算成成功吞吐。

`--batch` 控制每个客户端请求包含的交易数。`consensus.cross_shard_batch_size` 控制协调者一次业务批次最多收集多少笔跨片交易；两者不是同一个参数。Saguaro 各阶段各自运行真实 PBFT，并不把跨片网络往返直接换算成固定耗时。

## 验证

已有混合负载可以直接复用，例如 10000 笔、90% 跨两个分片和 10% 跨三个分片：

```bash
make
python3 scripts/benchmark_mixed.py \
  --config config/three_layer_cross100.json \
  --workload test-results/mixed-90-10/shared-workload.json \
  --method saguaro
```

此命令自动启动独立集群、重放已有文件、核对全部请求完成及各分片计数/状态/锁收敛，然后停止集群；不需要先手动 start。输出目录会保留 `client.log`、`client.json`、`status-after.json` 和 `summary.json`。使用同一条命令把 `--method` 改成 `arbor` 即可重放完全相同的业务负载。默认沿用负载文件内的速率和超时；`--timeout` 可显式覆盖超时。负载文件必须已存在，且交易请求目标为其参与方的 NCA。`make clean` 会删除 test-results 内的负载和报告，需要长期保留的文件请另行备份。

客户端每 5 秒输出已提交/完成交易数、已确认请求数和运行时间；最终 TPS 仍只在完整确认后输出。修复说明见 [../docs/SAGUARO_STALL_FIX.md](../docs/SAGUARO_STALL_FIX.md)。

```bash
python3 tests/test_saguaro.py
```

测试包括两层和三层 NCA 路由、两个/三个参与方、相同 key 的连续交易、片内执行、重复交易去重、并发冲突、准备阶段持锁时的协调者/参与方 forward 故障切换、检查点追赶恢复和统一负载比较。`make test` 同时保留原有 Arbor 回归测试。

运行日志和测试结果会像 Arbor 一样归入 `runtime` 与 `test-results`，`make clean` 清理生成文件并停止本项目管理的运行实例；`baseline` 下的源代码和说明会保留。

## SharPer 与 Arbor 的同负载比较

SharPer 每片四个副本、`f=1`；`SUPER_PROPOSE` 直接发送给所有参与片的全部副本，ACCEPT/COMMIT 在参与副本间直接交换，跨片提交需要每个参与片的三票 ACCEPT 和三票 COMMIT。当前按参与片 ID 升序预约，每片见到全部较小参与片各三个匹配 ACCEPT 后才分配本地序号，每个叶子一个未执行跨片 slot。祖先片不进行排序或 2PC；发起叶子等待每个参与片三个实际执行通知后才回复客户端。

升序预约是工程调度限制，会增加等待、降低相对于论文默认并行模式的并行度。当前不声称完整 Algorithm 4 或完整 Byzantine 活性保障。历史批次和投票记录会持续增长，长期实验需要评估内存及恢复消息大小。

固定参与分片的经典比较入口：

```bash
python3 -B baseline/compare.py --baseline sharper \
  --config config/two_layer.json --participants 1,2 \
  --count 4000 --rate 1000 --batch 10 --repeat 3 --seed 42 \
  --timeout 120 --drain-timeout 60
```

生成并比较 10000 笔、90% 双片 / 10% 三片的混合负载：

```bash
python3 -B baseline/compare_mixed.py --baseline sharper \
  --config config/three_layer_cross100.json \
  --count 10000 --rate 5000 --batch 10 --repeat 3 --seed 42 \
  --timeout 180 --drain-timeout 60
```

复用已经生成的混合 JSON 用 `--workload 路径` 替代 `--count/--rate/--batch/--seed`。原始 unsigned workload 的跨片 `target` 仍为 NCA，SharPer 客户端在签名前将路由目标改为最小参与叶子，输入文件保留。将 `--baseline` 改为 `saguaro` 可执行相同的混合对比；经典入口省略该参数仍保持原有 Saguaro 行为。

两个入口自动依次运行新集群，重复轮次交替顺序；核对同一配置、同一实际负载 SHA256、唯一交易完成、四副本收敛及协议队列排空后，才报告 `arbor_over_sharper_tps`。失败轮次不参与成功比值，整组比值留空。SharPer 所有节点的 NCA 排序/完成计数为零，叶子执行数按各自参与的交易计算。

执行 `make sharper` 单独构建，执行 `make test-sharper` 运行其工具与集群回归。具体协议、预约调度与当前故障支持范围见 [sharper/README.md](sharper/README.md)。

## AHL 与 Arbor 的同负载比较

`baseline/ahl/` 复用 Saguaro 2PC，强制所有叶子只连接一个上层分片，统一由该上层协调所有跨片交易。每片仍有四个真实 PBFT 副本；协调片 INIT/DECIDE、参与片 PREPARE/FINISH 都实际共识。片内交易直接在叶子执行。实现和测试说明见 [ahl/README.md](ahl/README.md)。

Arbor 保留多层，AHL 用同叶子的两层配置：

```bash
python3 -B baseline/compare_mixed.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json \
  --count 10000 --rate 5000 --batch 10 --repeat 3 --seed 42 \
  --timeout 180 --drain-timeout 60
```

固定两方参与的负载：

```bash
python3 -B baseline/compare.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json --participants 1,2 \
  --count 4000 --rate 1000 --batch 10 --repeat 3 --seed 42 \
  --timeout 180 --drain-timeout 60
```

这两个入口共享实际 unsigned 负载、交易 ID、读写集、请求分组、速率和超时。AHL 客户端签名前把跨片请求的旧 NCA 路由提示改成唯一上层；交易内容保持一致。对比报告记录各自拓扑、节点数、配置 SHA256，不将不同拓扑标成相同配置。共同叶子、共识/执行设置、片内和共同链路实际延时必须一致。完整客户端确认、全部副本状态收敛、2PC 和锁排空后才计算 `arbor_over_ahl_tps`。

省略 `--baseline-config` 可以在同一两层拓扑比较两种方法。Arbor/Saguaro/SharPer 的原有同配置严格配对规则不变；AHL 的不同配置必须明确指定。已有混合 JSON 用 `--workload 路径` 替代生成参数。

```bash
make ahl
make test-ahl
```
