# 多层窗口参数建议与复现

> 2026-10-12：按用户要求，baseline 恢复到本轮 Arbor 窗口改动前的运行方式：原配置、原组批方式和客户端请求大小。m 与空块定时参数只进入 Arbor；每请求 100 笔的整批实验仅作为历史记录，已撤回其推荐运行指令。当前恢复说明与指令见 [BASELINE_RUNTIME_RESTORE.md](BASELINE_RUNTIME_RESTORE.md)。

## 此前扫描的建议

当前三层、六叶子拓扑的高负载实验，建议先采用 **m=2，空区块空闲等待 200 ms（0.2 秒）**，配置文件为 `config/three_layer_ordering_windows.json`。这是一组通过独立重复实验的实用参数，不是全局最优结论。

新版 `(4).pdf` 的 B. Multi-Layer Cross-Shard Transaction Processing 已取消额外 watermark；此次没有 k 参数。m 是协调片连续排序区块的数量，包含真正运行 PBFT 的空区块，不是交易数、客户端 `--batch` 或 PBFT 区块交易上限。

## 高负载结果

Apple M2、8 核，本机独立节点进程与可配置网络延迟；每分片 4 个 PBFT 节点。参考树：根 7，下层协调片 5/6，叶子分别 1/2/8 和 3/4/9。AHL 用同六个叶子的两层拓扑。100% 跨片交易，90% 双参与、10% 三参与，跨 cluster 占 5%。每个客户端请求 10 笔，跨片业务批上限 100 笔、组批等待 600 ms，执行 `fib_iterations=1`。

先筛选 m=1/2/4/8 × 空块等待 5/10/20/50/100/200 ms，共 24 组，每组 6000 笔；再用独立种子 1042–1044，每轮 10000 笔、提交率 5000 tx/s，与原始 baseline 重放相同输入。下表为三轮指标的中位数，延时单位为秒。

| 方法 | 完成 TPS | 平均延时 s | p95 s |
|---|---:|---:|---:|
| Arbor，m=2 / 200 ms | 394.82 | 13.38 | 22.94 |
| Saguaro | 176.99 | 27.42 | 53.43 |
| SharPer | 219.88 | 18.48 | 39.93 |
| AHL | 117.78 | 38.89 | 80.14 |

Arbor 推荐组合三轮 TPS 为 412.69、393.81、394.82，全部交易完成、全部副本收敛、无换主。相对 SharPer，TPS 中位数约为 1.80 倍，平均延时约低 27.6%，p95 约低 42.6%。这是包含发送、排队及排空的有限负载完成 TPS，未进行预热剔除，不能称为稳态峰值容量。

## 低负载限制

另外使用独立种子 2042–2044，每轮 2000 笔、100 tx/s。推荐组合的平均延时中位数约 1.06 秒，p95 约 1.94 秒；Saguaro 为 0.40/1.30 秒，SharPer 为 0.47/0.71 秒，AHL 为 0.31/0.91 秒。**当前参数不能使 Arbor 在低负载下也获得延时优势。** 低提交率下的完成 TPS 受发送速率和最后一笔的排空时间影响，不用于声称系统最大处理能力。

m=1 / 50 ms 和 m=8 / 20 ms 的三轮低负载延时更差。进一步按同一 seed=2042、同一输入补测 m=1 / 300、400、600 ms，平均延时分别为 1.44、4.50、7.75 秒，未推广为默认参数。这三项是单轮补测，不与三轮确认结果混合汇总。

## 原因与修复

窗口需要所有祖先协调片关闭相同编号的窗口。频繁空块让空闲根片的逻辑区块号快速前进，繁忙下层协调片可能落后。在 m=8 / 20 ms 的第三轮高负载中，最后 30 笔等待分片 5 的封窗证明，较根片晚 76.42 秒；叶子全程 view 0。该轮最终全部完成，但整体耗时 150.68 秒。三个协调片累计生成 4665 个空块；同一输入的 m=2 / 200 ms 为 146 个。不能简单认为空块越快、窗口越大，性能就越好。

首次扫描还发现了一项计时实现问题：合法等待封窗期间沿用旧业务的 PBFT 超时，窗口刚就绪就换主。已改为完整认证的执行头首次就绪时启动一次期限；重复证明及已在途 PBFT 不延长期限。原来卡住的同一份 10000 笔输入重放后全部完成，副本收敛、换主次数为 0。修复前结果单独保留，不用于推荐。

第一次低负载测试期间，系统因低电量休眠约 6698 秒，AHL 一轮因此被中断。该整组结果保留为异常证据，最终低负载表采用接电、防空闲休眠后的整组重跑数据，18 次均通过；没有修改 baseline 来解决这个环境中断。

## 测试与复现

新增窗口协议的 59 项 C++ 检查、三层 m=1/2/4 的真实 PBFT 冲突交易测试、四层 root9/中层7/下层5 的冲突顺序与账户版本测试均通过。原有组批单元测试与多层集成回归测试也通过。三个 baseline 使用从原提交构建的二进制，被测副本中对应的预处理有效代码与原提交一致；各方法和参数的输入 SHA256、配置、源码、二进制与逐交易时间均留档。

安装该改动后，在项目目录运行：

```bash
make test-ordering-windows
python3 scripts/cluster.py start --config config/three_layer_ordering_windows.json
python3 scripts/cluster.py topology
python3 scripts/cluster.py stop
```

重新执行高负载参数扫描及三轮确认：

```bash
python3 -B scripts/benchmark_ordering_windows.py \
  --m-values 1,2,4,8 --empty-ms-values 5,10,20,50,100,200 \
  --count 6000 --rates 5000 --confirm-top 2 --confirm-count 10000 \
  --repeat 3 --timeout 180 --drain-timeout 30
```

低负载复测：

```bash
python3 -B scripts/benchmark_ordering_windows.py \
  --parameter-pairs 2:200,8:20,1:50 \
  --rates 100 --count 2000 --repeat 3 --seed 2042 \
  --timeout 180 --drain-timeout 30
```

macOS 可在上述 Python 指令前加 `caffeinate -i`，测试时接电，避免此次实际出现的休眠中断。脚本串行启动全新集群，自动停止自身节点，并保留失败结果；不要与其他压测同时运行。新的默认配置不自动修改既有配置；`ordering_window_blocks=0` 仍可复测旧实现。

## 数据位置

完整 24 组矩阵、逐种子比较、原始日志、配置与哈希位于本次任务的 `hyperhiera-parameters-20261011/`：`PARAMETER_RESULTS.md`、`parameter_metrics.json`、`parameter_aggregates.csv`、`parameter_pairs.csv`，以及 `high-load-fixed/`、`low-load-awake/`、`low-load-refine-screen/`。`high-load/` 是修复前诊断运行；`low-load-exact/` 是遭遇低电量休眠的运行。

拓扑、网络、业务组批等待、执行强度及硬件改变后，需要重新测量，不能把当前值直接当作所有多层系统的最优参数。
