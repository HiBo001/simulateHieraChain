# 服务器短周期验证与诊断

此文档记录新增低层优先之前的五配置对比。1:1低层优先设计与结果见 [LOWER_FIRST.md](LOWER_FIRST.md)，当前10:1短周期诊断见 [LOWER_10.md](LOWER_10.md)。

代码已上传至 `root@10.1.101.71:/root/tanghaibo/simulateHieraChain`。服务器 32 核、125 GiB 内存，Ubuntu g++ 11.4.0 / Python 3.10.12；全部四方法以默认 `-O2` 编译。没有安装软件，没有注入故障，没有开展新的万笔测试。

## 本轮修改

- 当前流式/旧窗口策略、32个proof上限、50ms空块间隔、2000ms view timeout均保持；未扩展新策略。
- 增加叶子 `leaf_profile` 计数：Root/Child/head PBFT/witness等待，调度/证明校验、事务执行和状态应用，证明数量/字节，QC缓存命中与完整校验。
- 新增 `scripts/benchmark_quick.py`，默认200笔正确性与单组1000笔性能，包含barrier、streaming、Saguaro、SharPer、AHL。每组各方法复用完全相同unsigned输入，状态、密钥、run id、端口相互隔离，串行运行。
- `benchmark_guard.py` 在启动、client、排空期间监控；任一换主、节点退出/error、解析/队列/丢消息立即INVALID并停集群，停止后续case，不重试。client活跃阶段也监控transport错误；完成后迟到回复到关闭client监听器的连接失败单列说明。
- 默认case预算60秒、完成进展超时15秒、整轮300秒，并预留10秒清理。无进展指client completed请求数未增长，空区块增长不会延长它。所有INVALID/中断TPS留空，原始证据保存。

## 测试结果

本轮包含200笔×5方法的正确性，再进行同一1000笔×5方法的单组性能。实际组总耗时 69.05 秒，109项真实QC/顺序/计数单元断言、12项短周期runner单元检查和6项异常保护单元检查均PASS。10个用例全部PASS，全部完成、无重复、无队列/锁残留、副本摘要收敛、零换主。两个Arbor策略每个case审计24个叶子副本的真实提交日志，确认仅自身祖先参与，窗口完整或流式连续前缀/游标一致、业务不跳过。AHL按其要求保留两层同六叶拓扑。

100%跨片交易，95%同子树、5%跨子树，90%两参与片、10%三参与片；输入rate1000 tx/s，client batch10，seed42，m=1，empty=50ms。下面TPS包含该有限负载的完整client等待与完成时间，不等于稳态峰值容量；case耗时还含启动、排空、停止和日志审计。

| 方法/策略 | TPS | client耗时 s | case耗时 s | 结果 |
|---|---:|---:|---:|---|
| saguaro | 477.10 | 2.096 | 6.82 | PASS |
| sharper | 368.40 | 2.714 | 7.48 | PASS |
| ahl | 320.04 | 3.125 | 6.88 | PASS |
| barrier | 525.88 | 1.902 | 8.29 | PASS |
| streaming | 530.17 | 1.886 | 8.29 | PASS |

流式相对旧策略变化为 **+0.82%**。单次小样本不能确认改善，本轮没有复现此前明显下降。因此停止于1000笔，不启动10000笔测试。

## 时间分解和瓶颈

每个叶子primary（零换主，replica0）在client开始前到结束时status的差值，六叶平均，单位秒；不是四副本相加。status是周期采样，Root/Child表示叶子已有待处理CST时、下一个固定前缀或最早window候选缺少的首个自身祖先。Root优先时Child=0不能解释为Child从未晚到。staged head优先归witness。head等待时间是互斥event-loop wall分类，包含该阶段运行的本地处理；调度是作用域调用elapsed时间，不是纯CPU，不覆盖所有网络/handle和完整witness校验。它与wall类别会重叠，不能相加。selection/frontier/QC为调度子项，也不能互相相加。

| 类别（六叶均值） | 旧策略 s | 流式 s |
|---|---:|---:|
| 等待 Root | 0.7778 | 0.7760 |
| 等待 Child | 0.0000 | 0.0000 |
| 等待其他参与叶子的执行见证 | 0.3706 | 0.3478 |
| 本地叶 PBFT | 0.1168 | 0.1234 |
| 调度选择与祖先证明校验 | 0.0720 | 0.0718 |
| 事务生成读写记录和执行 | 0.0182 | 0.0177 |
| 状态应用（包括事务、hash、digest） | 0.0297 | 0.0292 |

1. **这组主要阻塞是Root前缀和跨叶witness**，本地事务计算均值仅约0.018秒，调度约0.072秒。流式仍必须等待固定顺序中缺失Root，未明显减少这一等待；Child并非本轮首要head阻塞。Root产块和分组等待（cross_shard_batch_wait_ms=600）值得先定位，暂不修改这些参数。
2. 本轮流式无额外control槽，primary的business槽均44；但frontier证明总数88→236（2.68倍），序列化字节1,027,517→1,412,161（1.37倍）。QC调用658→1092；完整校验293→300近似不变，cache hits365→792，新增调用多数命中缓存，不能归咎于重复Ed25519验签。调度时间两者约0.43秒/六叶合计，没有因证明数量增加而在这组显著增长。
3. 复用之前已有的长负载日志，流式出现105个额外control PBFT，4410个frontier证明中3360个（76.2%）由control槽带来，selection rebuild5354→9146（+70.8%）。提案等待与witness阶段也增长。**额外游标推进共识、前缀传输/序列化，以及跨叶同步等待是先前退化的证据支持原因；各因素因果贡献尚未独立测定。** 这些数据均来自旧日志，本轮没有重跑万笔。

详细性能计时主要归因于Arbor；baseline专有执行函数只复用原有状态应用时间，不把其细项零值当作未执行事务。

## 再次运行

服务器已有编译程序，下次先复用构建；`make -j4 all`仅重编译实际变更目标。默认运行如下：

```bash
ssh root@10.1.101.71
cd /root/tanghaibo/simulateHieraChain
make -j4 all
python3 -B scripts/benchmark_quick.py --skip-build --budget 300
```

每次生成新的结果目录。脚本限制correctness 100–300、performance 1000–2000、case<=60秒，不含重复参数网格和故障注入。需要操作系统硬时限时可用：

```bash
timeout --signal=TERM --kill-after=10s 310s python3 -B scripts/benchmark_quick.py --skip-build --budget 300
```

只有源码/Makefile、四二进制、五配置和原200笔负载哈希全部匹配、五方法正确性均PASS且顺序审计PASS时，才能只运行性能阶段，仍创建全新集群：

```bash
python3 -B scripts/benchmark_quick.py --phase performance --correctness-report /path/to/previous/summary.json --skip-build --budget 300
```

本轮服务器报告保存在 `/root/tanghaibo/simulateHieraChain/test-results/server-quick-20261012/summary.json`，包含全部source/binary/config/workload SHA。性能比较应只读顶层PASS字段；INVALID原始partial值不能参与TPS比较。故障恢复另用专门测试，不能和本脚本的性能结果混合。
