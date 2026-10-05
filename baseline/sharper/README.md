# SharPer 无协调者跨片基线

本目录根据项目提供的 `SharPer.pdf` 实现 SharPer Byzantine flattened 正常投票路径的有界基线，采用论文高负载时由分片 primary 统一分配本地序号的 `SUPER_PROPOSE` 变体。每个分片固定四个副本，故障模型为 `f=1`。参与叶子直接交换消息，祖先分片不处理跨片交易；当前实现不宣称覆盖原论文全部冲突与故障恢复场景。

`main.cpp` 通过 `ARBOR_SHARPER` 接入公共运行框架，协议逻辑放在 `protocol.inc`。它与 Arbor、Saguaro 使用相同的认证 TCP、交易输入、网络延时、执行计算、状态摘要和客户端统计。片内交易继续执行本片 PBFT；跨片交易由每个参与片的至少三个 `SH_ACCEPT` 和至少三个 `SH_COMMIT` 组成提交证明，这两类消息本身就是跨片共识投票，未在外层再嵌套一次 PBFT。

## 协议与实现范围

1. 客户端在签名前把跨片请求发送到最小参与叶子；该叶子的 primary 向所有参与片的全部副本发送 `SH_SUPER_PROPOSE`。共享的未签名 workload 继续采用 NCA `target`，方便 Arbor、Saguaro、SharPer 精确复用同一文件。
2. 按参与片 ID 升序预约本地 slot。最小参与片先分配；后续参与片看到全部较小参与片各三个匹配的 `SH_ACCEPT` 后，才由本片 primary 分配本地序号，并在片内传播该分配。四个副本各自向所有参与片发送 `SH_ACCEPT`，绑定该交易批次和本片序号。
3. 收齐所有参与片的三票 `SH_ACCEPT` 后，副本发送 `SH_COMMIT`，携带一致的参与片序号向量和本片读取记录。只有所有参与片的三票 `SH_COMMIT` 完整匹配才提交，并按本地序号实际执行。
4. 执行完成后发送 `SH_EXECUTED`。发起叶子收齐每个参与片三个匹配的执行通知，再向客户端回复执行结果；客户端接受两个不同副本的匹配回复。`SH_EXECUTED` 只确认实际执行完成，未增加跨片排序或 2PC 决定阶段。

每个叶子只处理一个尚未执行的跨片 slot。本地序号、读状态与提交证明绑定，当前批次执行后才分配下一批。参与片按 ID 升序预约是当前实现的工程调度限制：增加了预约等待，并行度低于论文默认的并行分配模式。ACCEPT 和 COMMIT 仍在所有参与副本之间直接交换，使用相同的每片三票门限；祖先片不承担额外共识。

当前复现范围覆盖正常 Byzantine flattened 提议、接受、提交和实际执行通知，并采用上述预约调度；未声称完整复现论文 Algorithm 4 的并行冲突处理或完整 Byzantine 活性保障。

实现包含消息重传、分片内视图切换证据和认证检查点追赶，并沿用公共框架禁止在同一运行目录直接重启副本的限制。复现范围为上述本地四副本协议及回归用例，未宣称完成论文全部部署功能或所有故障组合的保障。

历史请求、跨片批次及投票记录会随有效交易增长，目前适合有限负载实验；长期运行时需要额外评估内存、检查点和恢复消息体积。三个排空队列衡量尚未结束的业务，不表示历史记录已经回收。

2026-10-06 版本已去掉扫描整条队列再按参与集合聚类、以及按请求 ID 决定新批次顺序的行为。新客户端请求按本副本首次接纳次序，只合并连续同参与集合的完整签名请求；遇不同参与集合、容量或不可执行边界即停止，不跳到后面的同组请求。已遇边界的非空前缀仍满足原普通组批等待后提议，开放且不足额的队尾保留原跨片组批等待。

论文明确允许组块，并给出 `SUPER_PROPOSE` 高负载变体；这些机制和已有认证缓存、持久 TCP、去重、共享 PBFT 恢复修复均保留。当前比较结果仍对应有界工程实现，因为升序预约、最小参与片发起和单 slot 限制尚未替换为完整 Algorithm 4。2026-10-05 审查作为历史记录保留在 [DESIGN.md](DESIGN.md)；本轮说明见 [../../docs/SHARPER_PAPER_FAITHFULNESS.md](../../docs/SHARPER_PAPER_FAITHFULNESS.md)。不能把当前 TPS 直接标为完整原论文复现成绩，也不会为改变吞吐排名增加人为等待。

## 构建与单独运行

在项目根目录执行：

```bash
make sharper
python3 -B scripts/cluster.py start --config config/two_layer.json --method sharper
python3 -B scripts/cluster.py load --participants 1,2 --count 100 --rate 100 --batch 8 --timeout 60
python3 -B scripts/cluster.py status
python3 -B scripts/cluster.py stop
```

片内负载使用相同接口，例如 `python3 -B scripts/cluster.py load --shard 1 --count 100 --rate 100 --batch 8`。`load` 从当前运行 manifest 选择 `build/bin/sharper_node`，跨片签名路由由该客户端完成。

同一请求 ID 的网络重试会重新发送其原始执行结果；使用不同请求 ID 重复提交相同交易时，结果标记为 `duplicate`，客户端以唯一交易计数。

## 与 Arbor 使用同一负载比较

固定两个参与片的比较：

```bash
python3 -B baseline/compare.py --baseline sharper \
  --config config/two_layer.json --mode cross --participants 1,2 \
  --count 4000 --rate 1000 --batch 10 --repeat 3 --seed 42 \
  --timeout 120 --drain-timeout 60
```

一条命令生成并比较 10000 笔带访问局部性的混合交易：90% 跨两个分片、10% 跨三个分片，只有 5% 的跨片交易跨越 cluster。cluster 按 Arbor 参考树的叶子直接父节点划分；SharPer 协议仍不使用协调者。

```bash
python3 -B baseline/compare_mixed.py --baseline sharper \
  --config config/three_layer_locality.json \
  --count 10000 --rate 5000 --batch 10 --repeat 3 --seed 42 \
  --cross-cluster-ratio 0.05 --three-shard-ratio 0.10 \
  --timeout 180 --drain-timeout 60
```

新配置中两个 cluster 为 `{1,2,8}` 和 `{3,4,9}`。同 cluster 两方/跨 cluster 两方/同 cluster 三方/跨 cluster 三方分别为 8550/450/950/50 笔。生成器使用参考树的全部叶子，SharPer 与 Arbor 共享相同的交易参与方、读写输入、请求分组和发送顺序。对比自动构建所选二进制，在独立目录顺序启动和停止两个方法，并在重复轮次交替运行顺序。

也可以直接重放原有 unsigned JSON，保持文件中的速率、请求分组、交易 ID、参与方和 key/value：

```bash
python3 -B baseline/compare_mixed.py --baseline sharper \
  --config config/three_layer_cross100.json \
  --workload test-results/mixed-90-10/shared-workload.json \
  --repeat 3 --drain-timeout 60
```

`--workload` 与生成参数互斥；`--timeout` 可为两个方法同时覆盖客户端超时。此入口支持任意合法的全跨片混合负载，原文件不会被修改。将 `--baseline` 改为 `saguaro` 可以执行同样的 Arbor/Saguaro 配对比较；经典 `compare.py` 省略该参数仍选择 Saguaro。

同时比较四种方法可用 `baseline/compare_all.py --config config/three_layer_locality.json --ahl-config config/ahl_two_layer_locality.json`。cluster 分类只影响生成的业务分布，不会为 SharPer 增加树状共识或上层路由。旧前三叶均匀 90/10 生成模式用旧配置与 `--uniform`；该选项与 `--cross-cluster-ratio` 互斥。完整生成和重放说明见 [../../docs/WORKLOAD_LOCALITY.md](../../docs/WORKLOAD_LOCALITY.md)。

结果保存到 `test-results/compare-*`，包含共享 workload、每轮客户端日志与 JSON、全部副本状态、`summary.json`、`summary.csv` 和 `summary.md`。只有双方均完整完成、配置指纹和实际负载 SHA256 相同、各副本收敛且队列排空，才计算 `arbor_over_sharper_tps`。某轮失败会使整组比值留空。TPS 为客户端确认的唯一执行交易数，平均和 p50/p95/p99 延时单位均为秒。

## 状态统计与验证

SharPer 的叶子 `executed_transactions` 等于该叶子参与的交易数。`leaf_ordered_cst_transactions` 记录本片参与的跨片交易；原有 `ordered_cst_transactions` 和 `completed_cst_transactions` 是 NCA 工作统计，所有 SharPer 节点均为零。非叶子保持零业务批次和零交易执行。

统一启动器仍按相同配置启动全部拓扑节点，非叶子的通用 `role` 标签继续显示 `coordinator`，此标签表示其在配置树中的位置；SharPer 的跨片消息、序号分配、投票和执行均发生在参与叶子。

排空需要三个 SharPer 队列均为零：`sharper_active_batches`、`sharper_pending_batches`、`sharper_waiting_execution`，并核对普通请求、去重等待和网络队列。`sharper_initiated_transactions`、`sharper_committed_batches` 供观察发起和提交数量。测试不会要求 Arbor 的封闭轮次或 Saguaro 的锁统计。

```bash
make test-sharper
```

该目标运行 SharPer 工具检查与真实集群协议回归；`make test` 继续执行 Arbor、Saguaro 和 SharPer 的完整项目测试。`make clean` 会停止本项目管理的节点并删除 `runtime`、`build` 和 `test-results`，活动负载或比较脚本存在时拒绝清理。
