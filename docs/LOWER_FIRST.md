# 低层协调者优先的祖先流式顺序

本页记录1:1低层优先的已通过结果。当前脚本默认比较1:1与2:1，结果见[LOWER_2.md](LOWER_2.md)；10:1的未完成诊断见[LOWER_10.md](LOWER_10.md)。复现本页旧六配置时，显式传入 `--cases barrier streaming lower_first saguaro sharper ahl`。

多层 Arbor 可使用 `consensus.ancestor_order: "lower_first"`。需要 `ancestor_streaming: true` 与 `ordering_window_blocks>=1`。省略 ancestor_order 时保留原来的 root_first；baseline 会去除该 Arbor 专用字段。

三层执行序列为 `Child1 → Root1 → Child2 → Root2`；四层为 `最深协调者1 → 中间协调者1 → Root1 → 最深协调者2 → ...`。每个叶子仅处理其自身祖先。整个树使用 `(协调者逻辑区块编号, -全局深度, 协调者ID)` 固定总序，每个叶子的路径是该序的投影。共同参与一笔交易的叶子，对共同协调者的相对顺序相同，包含不平衡树。

Child1 已收到且认证通过就能执行，无需等 Root1。Root1 未收到时，Child2 不能绕过它。Root 在每个编号内都有固定位置，避免长期饥饿；这个最小修改仍可能受 Root 空块速度约束。

选择、认证 frontier、skip 证明、query 与持久化 cursor 统一使用同一祖先序；保持 32-proof 前缀上限和认证 PBFT 游标推进。每次运行使用全新 run id 和状态，不能给运行中的集群切换该字段；恢复同一运行时也必须保留原配置。

示例配置：`config/three_layer_lower_first.json`。验证采用默认受限脚本：

```bash
cd /root/tanghaibo/simulateHieraChain
make -j5 all build/bin/test_ordering_windows
python3 -B scripts/benchmark_quick.py --skip-build --budget 300
```

先 200 笔正确性，再单组 1000 笔共享 unsigned 输入，串行比较旧窗口、Root 优先流式、低层优先流式与三种 baseline；不同用例完全隔离状态。异常换主、错误或 15 秒无完成进展立即停止，单例 60 秒、整轮 300 秒，无自动重试和万笔实验。

## 2026-10-12 服务器结果

服务器 `10.1.101.71`，32 核，代码位于 `/root/tanghaibo/simulateHieraChain`，同一份 O2 Arbor 二进制切换配置对比。参数与上一轮保持一致：100% 跨片，95% 同子树、5% 跨子树，90% 两参与片、10% 三参与片，seed42、输入 1000 tx/s、batch10、m1、empty50ms、view_timeout2000ms。AHL 保持两层同六叶拓扑，其余方法使用三层六叶。

161 项真实签名/QC、顺序、游标恢复与证明边界断言通过，保留原109项并新增低层优先检查。三层及四层各120笔冲突账户验证通过，读取真实执行见证，核对账户版本和全部副本顺序；没有注入故障。随后200笔×6配置正确性及同一1000笔×6配置单组性能全部PASS，合计85.47秒（包含启动、排空、停止及日志审计，不含编译与前置120笔）。没有重复参数或seed扫网格。

| 配置 | TPS | 平均延迟 s | p95 s |
|---|---:|---:|---:|
| Arbor旧窗口（m1） | 530.02 | 0.8603 | 1.3581 |
| Arbor Root优先流式 | 536.41 | 0.9226 | 1.5182 |
| Arbor低层优先流式 | 543.28 | 0.7147 | 1.1780 |
| Saguaro | 491.51 | 0.2769 | 0.6258 |
| SharPer | 369.96 | 0.6128 | 1.0700 |
| AHL | 294.82 | 0.9601 | 2.3569 |

相对Root优先，低层优先TPS +1.28%，平均延迟 -22.53%，p95 -22.41%；相对旧窗口TPS +2.50%，平均延迟 -16.92%。这是一次有限负载对比，尚不能确认稳定吞吐提升或峰值容量。没有继续开展10000笔测试。

### 性能瓶颈

下表使用六个叶子primary（replica0，零换主）从client开始前到完成时status的计数差值，取每叶平均秒数。status按周期更新，所以计时及切点计数为近似值；详细raw before/at-client-finish/after状态均保留。等待为互斥head状态wall分类，调度为选择/祖先证明处理作用域elapsed调用时间，二者可能重叠，不能相加，也不能当作纯CPU。

| 类别 | 旧窗口 | Root优先 | 低层优先 |
|---|---:|---:|---:|
| 等待Root区块 | 0.7826 | 0.8381 | 0.6130 |
| 等待Child区块 | 0 | 0 | 0 |
| 等待跨叶执行见证 | 0.3612 | 0.2893 | 0.3407 |
| 本地叶PBFT head | 0.1151 | 0.1215 | 0.1340 |
| 调度选择与祖先证明校验 | 0.0736 | 0.0716 | 0.0665 |
| 事务执行（生成读写记录及应用） | 0.0187 | 0.0193 | 0.0183 |

低层优先减少了Root head等待，因而这一组交易延迟下降；见证等待从0.2893增至0.3407秒。Root仍占主要等待，且固定顺序要求每个Child编号之后经过Root，Root空块速率仍会限制连续Child业务。Child=0仅表示它没有成为采样中首个缺失的祖先，并不意味着Child始终及时到达。本地执行和调度耗时较小，当前不能把瓶颈归因于事务计算。

三个Arbor样本均无额外control PBFT，证明处理已使用缓存。既有长负载的control开销尚未由这次1000笔样本覆盖，因此不据此断言已解决长负载退化。

### 启动计数保护修正与证据

第一轮三层120笔被保护脚本误报终止：co5/co6/co7主节点分别已有3/4/2次连接错误，before与after计数完全相同，发生于节点启动监听阶段，client期间没有新增。该样本仍保留INVALID，只有116/120完成，TPS不参与比较。此前四层120笔PASS结果保留，不重复执行。

已修正保护脚本：在begin_client固定保存逐节点status-before传输错误基线，压测期任一增量立即INVALID；计数回退也立即INVALID。启动连接错误另记startup_transport_errors；换主、节点退出/error、解析/队列/丢消息始终按绝对计数检查。增加三项回归检查，异常保护共9项PASS。修复后仅重做三层120笔，通过才运行后续唯一1000笔组。正常性能组client活跃期零新增传输错误、零换主、零事务错误，完整收敛后停止全部节点。

证据目录：

- 性能与200笔：`/root/tanghaibo/simulateHieraChain/test-results/server-lower-first-20261012/summary.json`
- 首轮三层INVALID：`/root/tanghaibo/simulateHieraChain/test-results/lower-first-live-b71fbbefef/result/summary.json`
- 四层120笔PASS：`/root/tanghaibo/simulateHieraChain/test-results/lower-first-live-6f73e16b2f/result/summary.json`
- 修复后三层120笔PASS：`/root/tanghaibo/simulateHieraChain/test-results/lower-first-live-dec357369d/result/summary.json`
- 161项认证检查：`/root/tanghaibo/simulateHieraChain/test-results/ordering-windows-unit-3e989b6e6f.json`

每个性能case保留相同unsigned输入SHA、实际配置fingerprint、源码与四方法二进制SHA、前后status及真实提交顺序审计。只使用顶层PASS指标，不把INVALID的原始partial值纳入TPS。
