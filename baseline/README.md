# Saguaro 2PC 对比方法

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

```bash
python3 tests/test_saguaro.py
```

测试包括两层和三层 NCA 路由、两个/三个参与方、相同 key 的连续交易、片内执行、重复交易去重、并发冲突、准备阶段持锁时的协调者/参与方 forward 故障切换、检查点追赶恢复和统一负载比较。`make test` 同时保留原有 Arbor 回归测试。

运行日志和测试结果会像 Arbor 一样归入 `runtime` 与 `test-results`，`make clean` 清理生成文件并停止本项目管理的运行实例；`baseline` 下的源代码和说明会保留。
