# Arbor 业务批次等待：600 ms → 100 ms

本文记录仅 Arbor 使用 100 ms、三个 baseline 使用 600 ms 的历史对照。当前默认性能配置已统一为四种方法 100 ms，公平比较与复现见 [UNIFORM_BATCH_WAIT.md](UNIFORM_BATCH_WAIT.md)。

本次仅覆盖 Arbor 的 cross_shard_batch_wait_ms，保持低到高 1:1、单块叶子提交、空块间隔 50 ms 和业务批量上限 100。三个 baseline 保留原配置的 600 ms，不修改原生协议、不重新编译。

新增 scripts/benchmark_quick.py 参数 --arbor-cross-shard-batch-wait-ms，范围 0–10000。省略参数时沿用输入配置；本历史对照的输入为 600 ms，当前默认输入为 100 ms。覆盖只进入 Arbor 实际配置快照；Saguaro、SharPer、AHL 保持输入参数，比较报告记录实际配置指纹与明确放行的等待时间差异。源码中的 selectArborClientBatch 仍按照每个参与分片组最早请求的入队时间检查等待阈值；满 100 笔或请求容量阻塞时可以提前提交，100 ms 并非固定出块周期。

在 10.1.101.71（32 vCPU）只测发送速率 5000 的一个负载点。每种待验证配置先跑 200 笔，再跑一次 1000 笔；seed=42，请求批量最多 10，交易与请求顺序相同。100% 跨叶子分片，95% 同 Child 子树、5% 跨 Child 子树，10% 为三参与分片。状态、密钥、端口、run id 独立。先测 600 ms 与三个 baseline，再测 100 ms Arbor。

整组耗时 66.61 秒，10 个真实运行全部 PASS；65 项脚本与终止保护检查 PASS。没有换主、网络失败、节点错误、重复或未完成交易；24 个叶子副本执行顺序与最终状态收敛验证通过，测试节点已清理。

| 方法 | 实际业务批次等待 ms | TPS | 平均延迟 ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|---:|
| Arbor 1:1 | 600 | 799.48 | 821.18 | 1060.17 | 1089.71 |
| Arbor 1:1 | 100 | 1290.93 | 387.50 | 560.74 | 593.08 |
| saguaro | 600 | 547.80 | 523.65 | 1077.34 | 1717.90 |
| sharper | 600 | 362.49 | 953.39 | 1798.33 | 2312.35 |
| ahl | 600 | 294.61 | 1332.00 | 2509.84 | 3255.97 |

Arbor TPS 提升 **61.47%**，客户端完成时间从 1.251 秒缩短到 0.775 秒。此轮是对同一程序、同一输入进行参数优化；发送速率两次都为 5000。
100 ms Arbor 相对本轮 Saguaro、SharPer、AHL 的 TPS 分别为 2.36、3.56、4.38 倍。倍率只适用于当前参数与此单次短测，不代表所有负载或稳定容量。

| 每叶子主节点累计时间均值（客户端完成时） | 600 ms | 100 ms |
|---|---:|---:|
| 等待 Root | 472.05 ms | 0.77 ms |
| 等待 Child | 0.00 ms | 0.00 ms |
| 等待 execution witness | 447.03 ms | 422.94 ms |
| 等待叶子 PBFT 槽 | 128.96 ms | 148.18 ms |
| 调度处理 | 70.03 ms | 70.35 ms |
| 事务计算 | 21.29 ms | 21.10 ms |

Root 等待从约 472 ms 降到 0.77 ms，事务计算和调度时间基本不变。客户端确认尾部从 1.052 秒缩短到 0.576 秒，与 Root 更早可用的证据相符。等待类别和调度处理有重叠，不能相加当成全局墙钟时间占比，也不能据此将所有改善归因于单一事件。

缩短等待也产生更多小批次：Child 5 业务块从 6 个增加到 8 个，平均每块从 67.5 笔降到 50.625 笔；Child 6 均为 7 个块；Root 两次都为 5 个业务块、50 笔，批量依次为 10/20/5/10/5。最终六个叶子主节点业务 PBFT 槽合计从 39 增到 44（全副本 156 → 176），增加约 12.82%。客户端结束快照中的 43 个计数尚未排空，最终提交日志以 44 为准。

这一轮提升来自更及时的业务批次提交，虽然共识次数增加，总完成时间仍下降。优化后主要剩余等待是 execution witness（约 423 ms）和叶子 PBFT 槽（约 148 ms）。保留 Arbor 1:1，没有扩大到新策略或 10000 笔正式实验。

复现优化参数与三个原参数 baseline：

```bash
cd /root/tanghaibo/simulateHieraChain
python3 -B scripts/benchmark_quick.py --skip-build --rate 5000 \
  --config config/three_layer_lower_first_wait600.json \
  --ahl-config config/ahl_two_layer_locality_wait600.json \
  --arbor-cross-shard-batch-wait-ms 100 --budget 180
```

选用上述 wait600 配置后，省略覆盖参数，或显式传入 600，可复现原参数。改变该参数后必须重新跑 200 笔正确性；现成 performance-only 入口会拒绝使用 600 ms 的正确性报告验证 100 ms 配置。

历史任务目录保留当时执行的 run_batch_wait_comparison.py；它依赖执行时的默认输入配置。当前复现请显式选用上面的 wait600 配置，分别覆盖 600 和 100。单项 60 秒、无完成进展 15 秒，任一 INVALID 终止后续且不重试。

原始结果在服务器 /root/tanghaibo/simulateHieraChain/test-results/server-batch-wait-100ms-20261012/。本地归档保存原始摘要、实际配置、共享输入、客户端结果、状态和主节点提交日志。
