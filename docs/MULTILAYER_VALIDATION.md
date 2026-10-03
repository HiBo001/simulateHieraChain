# 多层版本验收记录

日期：2026-10-03。工程目录：`/Users/tanghaibo_office/Desktop/ATLAS/sourceCode/simulateHieraChain`。本轮没有创建分支、提交或推送；验收针对当前未提交工作树。

## 功能与回归

`make test` 退出码 0，112 项检查全部通过：网络 6、增量状态摘要 5、清理 18、第一阶段 15、第二阶段 A 1、第二阶段 B 5、多层 11、客户端 f+1 发送 3、快照摘要 1、工程 21、benchmark 26。

多层集成测试覆盖：多个 NCA 并发、同一签名请求内的不同参与集合、三个及四个参与叶子、非参与订单跳号、不均匀树和四层树、协调及叶子 forward 切换、跨检查点恢复、未来槽自动追赶、轮次缺口补取、非法祖先前沿、合法 QC 签名子集变化，以及将合法证明发送给无关叶子的拒绝检查。独立参考执行器核对本地 KV 的值、摘要和版本。

测试还检查两层直接路径、客户端重复请求去重、forward 三签汇总、ACK 直接完成和不出现 DECISION / FINALIZE / DONE。没有恢复叶子推测流水线；片内/跨片队列公平轮转仍未实现。

## 隔离集群测量

每组均顺序运行三个新集群；客户端请求批量 8、种子 42、timeout 90 秒、drain-timeout 30 秒。机器为 8 核 macOS ARM64。以下 TPS、平均延时和 p95 是三个轮次各自指标的中位数，不是合并所有样本重算的分位数。

| 场景 | 参与叶子 | 每轮交易数 | 发送率 tx/s | 完成 TPS | 平均延时 秒 | p95 秒 |
|---|---|---:|---:|---:|---:|---:|
| 两层，两方 | 1,2 | 4000 | 4000 | 549.37 | 3.288313 | 6.842186 |
| 三层，两方跨子树 | 1,3 | 256 | 100 | 90.81 | 0.339458 | 0.420960 |
| 三层，三方 | 1,2,3 | 256 | 100 | 90.37 | 0.352947 | 0.436156 |
| 四层，不同深度两方 | 1,8 | 128 | 100 | 85.07 | 0.231340 | 0.317681 |

四组共 12 轮均 PASS，所有请求完整确认，所有分片内四副本状态收敛、队列排空，网络失败与视图切换均为 0。TPS 按客户端唯一完成交易数计算，不累计参与片各自的执行数。

这些配置的 Fibonacci 次数、组批等待和链路延时不同，不能从表中计算层数的性能损失或加速倍数。三层和四层采用 100 tx/s 的小负载，是完整执行和统计的验证，并非吞吐上限。两层 4000 tx/s 的发送率也不等于系统完成能力。多层认证封闭需要额外的真实 PBFT 和证书广播；必要祖先的封闭延迟会阻塞叶子。这是保留逐批执行约束后的保守顺序机制，详见 [MULTILAYER_DESIGN.md](MULTILAYER_DESIGN.md)。

## 复测

完整自动检查：

```bash
make test
```

每次 benchmark 自动创建新运行目录并停止其节点，不改变 `runtime/latest`。省略输出目录会自动生成新目录；手动指定 `--output-dir` 时必须使用尚不存在的目录：

```bash
python3 -B scripts/benchmark.py --config config/two_layer.json --mode cross --participants 1,2 --count 4000 --rates 4000 --repeat 3 --seed 42 --cross-batch 8 --timeout 90 --drain-timeout 30
python3 -B scripts/benchmark.py --config config/three_layer.json --mode cross --participants 1,3 --count 256 --rates 100 --repeat 3 --seed 42 --cross-batch 8 --timeout 90 --drain-timeout 30
python3 -B scripts/benchmark.py --config config/three_layer.json --mode cross --participants 1,2,3 --count 256 --rates 100 --repeat 3 --seed 42 --cross-batch 8 --timeout 90 --drain-timeout 30
python3 -B scripts/benchmark.py --config config/four_layer.json --mode cross --participants 1,8 --count 128 --rates 100 --repeat 3 --seed 42 --cross-batch 8 --timeout 90 --drain-timeout 30
```

本轮原始 CSV、JSON、配置、副本状态、客户端结果与日志保存在：

`/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/multilayer-validation-20261003/`

其中 `make-test.log` 为完整回归日志，`validation.json` 保存源码和二进制 SHA-256、配置、机器信息及每组报告路径。各 benchmark 的 `summary.json` 还保存配置指纹、每轮完成数及网络统计。Git 基线为 `217728f02f290f3d0d4cfd2af874ed76d283ac50`，二进制 SHA-256 为 `0bc0c01c0bb707d30dfd89700c7ce1b8d83fc59948ecc6cd86bb6bb009ccdbc5`；该 Git 提交本身不包含尚未提交的多层修改。

修改前源码备份：`/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/backups/pre-multilayer-20261003-161343.zip`。备份和本轮验收产物位于工程外，工程内的 `make clean` 不删除这些外部目录。
