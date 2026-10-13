# Saguaro / AHL 并发规则修复与短测

已修复并验证 Saguaro 的参与分片相交限制与跨 NCA 等待恢复，并将 AHL 的 key 级 2PL 策略显式隔离。Arbor 保持低到高 1:1；SharPer 本轮未改。四种方法继续使用统一 100 ms 跨片组批等待。

## 修改内容

- Saguaro：交集至少两个参与分片的业务批次，协调者等待之前 DECIDE，参与者等待之前 FINISH；交集零或一个仍允许并发。索引由复制状态派生并在恢复时重建，异步最终 ACK 不决定 PBFT 提案有效性。依据 [Saguaro §4.1 / Algorithm 1](https://arxiv.org/pdf/2101.08819)。
- 跨 NCA 反向到达导致等待时，协调者可通过本地 PBFT 形成绑定原始 INIT 的超时 ABORT。未 PREPARE 的参与者也通过 PBFT 保存终止 ACK，防止迟到 INIT/PREPARE 再执行。COMMIT 仍须全部参与方有效 VOTE，三个签名的阶段证明和所有最终 ACK 要求保留。计时器只触发提案；墙钟不进入共识判据。
- 超时与换主计时分开：事务阈值为 view_timeout_ms/2 加协调者偏移，当前 NCA 5/6/7 分别为 1155/1286/1417 ms；view_timeout_ms 保持 2000。这样在本地新 ABORT 工作出现之前，正常远端等待不会先误触发换主。
- AHL：wrapper 明确选择 key_2pl，关闭上述参与集合限制及超时 ABORT。保留唯一 Root 的两层 2PC、原有 key 冲突准入和锁、8 在途工程上限及全部共识阶段。没有加入强制单批串行或人为延时。依据 [AHL §6 的 2PC/2PL](https://arxiv.org/html/1804.00399v4)。
- 新增原生认证内存回归目标 make test-baseline-policies，及分片集合等待次数、累计等待时间、超时 ABORT 和策略标识状态。共享 main 仅增加测试宏保护的 friend；Arbor/SharPer 生产预处理 SHA 与原版完全相同，因此复用原二进制。

## 验证范围与耗时

服务器 10.1.101.71，32 vCPU，隔离验证目录 /root/tanghaibo/baseline-domain-repair-20261012/repo。每次独立生成密钥、端口、run id 和空状态，串行运行；未注入换主或节点故障。

88 项原生认证内存检查通过（Saguaro 46、AHL 34、真实多 NCA 顺序倒置 8），耗时 0.501 秒；94 项配置、成本比较、runner、异常终止与窗口检查通过，耗时 5.760 秒。原生检查包含独立 key 的相同参与对、交集一个分片、释放边界、复制状态重建、真实签名和 quorum、缺票 COMMIT 拒绝、认证 ABORT、终止记录与迟到消息。

四种方法各一次 200 笔正确性，再各一次 1000 笔性能，8 次真实集群运行全部 PASS，耗时 52.193 秒。最终验证驱动耗时 53.354 秒；最终源码强制编译 Saguaro/AHL 另耗 21.249 秒。测试前两次测试夹具问题分别在 9.748 和 22.301 秒停止：网络配置的派生字段未去掉，以及跨 NCA 检查数阈值误写 9（实际 8）。两次都没有启动集群吞吐测试；修正夹具后复用已按最终源码编译的程序。初始编译 21.17 秒，全部已记录服务器编译/验证执行时间累计约 106.57 秒。

整轮驱动预算 300 秒，短测预算 180 秒，单例 60 秒，无完成进展 15 秒，排空 10 秒；失败停止后续，不自动重试性能样本。文件解包时间可能早于先前编译结果，因此最终协议明确重编译，未依据 make 时间戳误用旧程序。

所有完整样本：客户端全部 executed、ordered_only=0、错误=0，无换主或网络错误；四副本状态/链/KV 摘要及计数收敛，队列、锁排空。Arbor 的 24 个叶子副本顺序审计通过，双块合并为零。Saguaro 主副本日志独立重放的准入/释放检查通过；112 对交集至少两个叶子的已提交批次，在共同叶子上的 FINISH 顺序一致。

## 同参数单点性能

发送速率 5000 笔/秒，每请求最多 10 笔，seed=42，跨片组批上限 100。100% 跨叶子，95% 同 Child 子树、5% 跨 Child 子树，90% 双参与片、10% 三参与片。与上轮统一 100 ms 的 unsigned 性能负载 SHA 和四方法实际配置 SHA 完全相同。Arbor/Saguaro/SharPer 为九片三层，AHL 为同六叶子的七片两层；均四副本。

| 方法 | 上轮 TPS | 本轮 TPS | 变化 | 平均延迟 ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|---:|---:|
| Arbor 1:1 | 1279.87 | 1258.73 | -1.65% | 399.00 | 571.63 | 604.13 |
| Saguaro | 746.80 | 552.97 | -25.96% | 722.36 | 1348.29 | 1638.36 |
| SharPer | 429.74 | 429.30 | -0.10% | 941.12 | 1840.21 | 2025.84 |
| AHL | 320.71 | 351.96 | +9.74% | 1351.35 | 2442.97 | 2554.72 |

Arbor 对 Saguaro、SharPer、AHL 的本轮倍率为 2.28 / 2.93 / 3.58。TPS=1000/首次发送至所有客户端执行确认的耗时，包含注入时间及协议等待。本轮每方法只有一个短样本，未开展 10000 笔正式实验。

## 主要成本与解释

- Saguaro：64 个尝试批次中，40 个 COMMIT、24 个普通 key 冲突 ABORT，超时 ABORT 为零；累计尝试 2030 笔、205 个请求尝试，30 个 NO 投票，ABORT 丢弃 700 次参与方准备计算。完成仍只计 1000 笔。恢复参与集合规则后，协调者只能串行处理交集至少两个叶子的批次；允许的交集一个叶子的批次仍可能在同一 key 上冲突。当前主要额外成本是排队以及这些 ABORT、重新准备和退避，而不是故障换主。
- Saguaro：主副本协调者相交阻塞检查共 2389 次；这是调度循环的检查次数，不能当作 2389 笔阻塞交易。主叶子参与集合等待累计 181.762 ms，主要在 Leaf 4（82.200 ms）和 Leaf 9（99.562 ms）；是逐批累计墙钟，不能当作整体耗时占比。
- AHL：零 ABORT，业务批次由上轮 89 变为 78；逻辑 PBFT 槽从 550 变为 482（协调者 156，参与方 326）。成功批平均从 11.24 笔变为 12.82 笔。其 9.74% TPS 上升与本次运行时合批更充分相符；key 2PL 和阶段语义没有优化，单次样本不能归因为协议提速。
- Arbor：主叶子累计墙钟均值为等待 Root 1.804 ms、Child 0、witness 428.849 ms、叶子 PBFT 槽 149.813 ms，调度处理 71.417 ms、事务计算 23.554 ms。等待范围和处理范围可重叠，不能相加作全局比例；剩余主要等待仍是 witness 和叶子 PBFT。

本轮 Saguaro 仍高于 SharPer。上轮审查指出 SharPer 当前存在升序预约、单未执行槽和最终执行确认后再发起的工程串行限制，本轮按请求未改 SharPer。当前自建 C++ 普通 PBFT、显式 access 和四阶段工程路径仍有论文外取舍，不能将这次结果当作 Euro-Par/FISCO BCOS 完整复现，也不应人为调慢 baseline 恢复指定排名。

## 复现与证据

```bash
cd /root/tanghaibo/simulateHieraChain
make -j2 saguaro ahl
make -j2 test-baseline-policies
python3 -B scripts/benchmark_quick.py --skip-build --rate 5000 \
  --correctness-count 200 --count 1000 --batch 10 --seed 42 --budget 180
```

运行目录：/root/tanghaibo/baseline-domain-repair-20261012/repo/test-results/domain-repair-rate5000/。本地 server-results/ 保存原始报告、输入、客户端指标、四副本状态/事件及主副本提交日志，归档不包含签名私钥；journal-audit.json 为独立日志审计。comparison.csv 保存数值。

最终性能二进制 SHA256：

```json
{
  "arbor": "78c9cc06cb3bcdb78ec780ef09238dca12470519d23689c96d7915e24237d106",
  "saguaro": "2275bc058de9e2b3fe8cb75820a7ccb512f3f0ab8350ce108dd3818c874d724b",
  "sharper": "cbeb84f1ade942d896fc9789e5cb93d8dab61c52e20c0614fa3440d606b14363",
  "ahl": "d304aba84e948f2abe61381b72a67d3434e9538c5928f4937a7c417bd1bfebf0"
}
```

原始运行 source_sha256 包含解包产生的 ._ 元数据文件；这些文件未被编译器包含。正式安装只复制明确的协议/测试/文档文件，不复制元数据，并核对所有实际代码文件及二进制 SHA。原始报告保持不变，预处理相同的 Arbor/SharPer 复用证据保存在 validation-driver.json。

