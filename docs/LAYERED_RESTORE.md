# Arbor 全祖先屏障 m=1 历史记录（2026-10-12）

此版本已被低到高逐层立即提交替代，当前入口和结果见 `LAYER_IMMEDIATE.md`。

## 生效策略

使用 `config/three_layer_layered.json`：`ordering_window_blocks=1`、`ancestor_streaming=false`，空块间隔保持最近比较使用的 50 ms。每个叶子只等待自身祖先链，不等待其他分支的协调分片。收齐同编号区块的有效证明后，Root 到低层逐块提交业务，上一块执行完成后才提交下一层。例如三层 `Root1 → Child1 → Root2 → Child2`；四层 `Root1 → Middle1 → Child1 → Root2`。空块提供编号完整性证明，不计入业务 TPS。每个有业务的祖先块单独进入叶子 PBFT，不合并所有层的交易。

原有 C++ m=1 路径已经满足这个规则，所以本轮不修改生产 C++ 或 baseline 协议。默认快速测试改为 `barrier、saguaro、sharper、ahl`；低层优先和加权实验只保留为显式历史选项。

## baseline 范围

用户确认保留当前 NCA 2PC 和两层 2PC，无需按外部论文重新实现。Saguaro 使用参与叶子的 NCA 协调 INIT/PREPARE/DECIDE/FINISH；AHL 使用唯一上层协调同一 2PC；SharPer 保持参与片直接 ACCEPT/COMMIT 的现有有界实现（升序预约等既有界限见 `SHARPER_PAPER_FAITHFULNESS.md`）。本轮没有给它们加入 Arbor 的祖先屏障、流式或低层比例规则，也没有做 baseline 优化。全部五个 Arbor 专有字段在 baseline 配置副本中被删除，原始 baseline 配置、源码和二进制保持不变。

## 短测试

```bash
python3 -B scripts/benchmark_quick.py --skip-build --budget 300
```

仅一次 200 笔正确性阶段和一次 1000 笔性能阶段，共享同一 unsigned 输入，各方法独立新状态。无故障注入；单 case 60 秒、无交易完成进展 15 秒、整体 300 秒，异常立即记录 INVALID 并停止后续测试，不自动重试或开展万笔实验。Arbor 提交日志必须通过完整祖先、同编号和 Root→低层顺序审计。

## 本轮结果

服务器 `10.1.101.71`，32 核；200 笔正确性及 1000 笔单组性能，四种方法全部 PASS，总耗时 52.91 秒。每个 case 均完整完成、各副本收敛且队列排空，没有异常换主、活动期传输错误或节点错误。Arbor 的 24 个叶子副本日志顺序审计通过，200/1000 笔阶段分别审计到 92/176 个已执行业务块（包含四副本计数）。

| 方法 | 1000 笔 TPS | 平均延迟 s | p95 s | p99 s |
|---|---:|---:|---:|---:|

| Saguaro NCA 2PC | 510.17 | 0.2658 | 0.5771 | 1.3344 |
| SharPer 现有有界实现 | 368.87 | 0.6117 | 1.0798 | 1.6241 |
| AHL 两层 2PC | 302.93 | 0.8858 | 2.2737 | 2.3315 |
| Arbor 1:1 逐层 | 526.99 | 0.8631 | 1.3574 | 1.6024 |

上一轮低层 2:1 的历史 TPS 为 430.76，低层 1:1 为 543.76；本轮没有重跑这两个实验，不能作为同步配对提升结论。此次恢复 m=1 的 526.99 TPS 也接近更早 m=1 样本约 530 TPS。1000 笔、输入速率 1000 TPS 的一次短测用于验证，不代表稳态峰值容量。

## 当前耗时瓶颈

6 个叶子主副本（replica0）的平均观测时间：

| 类别 | 每个叶子主副本平均 s |
|---|---:|
| 等 Root | 0.7785 |
| 等 Child | 0.0000 |
| 等 witness | 0.3683 |
| 等叶子本地 PBFT 槽 | 0.1185 |
| 调度与验证 | 0.0754 |
| 事务执行 | 0.0188 |

本轮主要时间落在 Root 祖先屏障和依赖 witness，业务计算约 0.019 秒，调度与证明验证约 0.075 秒。Root/Child/witness 等待采用事件循环头部阻塞归因；Root 优先归因可能覆盖同时尚未到达的 Child，Child=0 不能解释成 Child 网络耗时为零。调度/执行计时包含在观测区间内，不能与等待时间简单相加。主副本累计 44 个业务 PBFT、0 个流式控制 PBFT、88 份祖先 frontier 证明，共 1,027,517 字节；不再有 2:1 为 Rootₖ 等待 Child₂ₖ 的配额条件。

所有源文件及 baseline 协议/旧配置 SHA 与恢复前一致，四个业务二进制 SHA 也与上一轮一致；仅新编译测试程序。53 项脚本/配置检查通过；原生真实签名/QC/PBFT检查 230 项通过（执行 3.57 秒），其中新增四层 m=1 的 21 项检查：自身祖先同编号完整性、拒绝非祖先替代、Root→Middle→Parent 分别提交、witness 完成后才能提交下一层以及冲突账户执行顺序。未开展 10000 笔实验或重复性能组。

服务器证据：`/root/tanghaibo/simulateHieraChain/test-results/server-layered-20261012/summary.json`。本地结果保存在工作区 `restore-layer-20261012/server-results/summary.json`。
