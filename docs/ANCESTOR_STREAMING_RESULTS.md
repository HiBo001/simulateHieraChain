# 固定祖先流式策略：实现与吞吐验证（2026-10-12）

基准提交为 `75d32d5`。新旧策略在同一重新编译的二进制内，通过 `ancestor_streaming=false/true` 切换，每个配对使用同一份 unsigned workload。

图中的严格顺序已实现并通过认证、执行、恢复验证。性能依负载而变：2000笔的部分配对有收益，10000笔高压诊断单轮下降，暂不能作为稳定的吞吐优化。

## 2000笔，原2秒共识超时

m=1，100%跨片，90%双方/10%三方，5%跨cluster，rate=1000 tx/s，客户端batch=10。种子42、43、44，每轮交替先后顺序。12/12独立集群用例全部PASS。

| 空块等待 ms | 策略 | TPS中位数 | 平均延时中位数 s | p95中位数 s |
|---:|---|---:|---:|---:|
| 50 | barrier | 121.03 | 8.36 | 14.21 |
| 50 | streaming | 156.40 | 6.30 | 10.92 |
| 200 | barrier | 124.32 | 8.82 | 14.34 |
| 200 | streaming | 177.35 | 6.16 | 9.42 |

逐种子 TPS 比值的中位数：50ms 为 1.0285（+2.85%），200ms 为 1.2704（+27.04%）。这与两个方法的TPS中位数相除不同。50ms第三轮流式从99.23降至84.11 TPS；不能声称该档稳定改善。200ms这组三轮均提高，结论仅限这组输入和主机。

## 10000笔，原2秒共识超时失败

m=1，50ms，rate=5000 tx/s，batch=10，seed1042，客户端超时300秒。旧策略完成4500/10000笔后停滞，多个协调片反复换主，最终记FAIL。随后流式轮次也开始停滞，主动中断剩余扫描并清理节点。流式的未完成轮次记中断，不计算这组TPS或吞吐比值，原始证据保留。

## 10000笔，共同10秒共识超时诊断

两种策略均使用 `view_timeout_ms=10000`，其他PBFT、执行、网络和业务成本相同。m=1，50ms，5000 tx/s，seed1042，各运行一次，2/2 PASS，换主计数均为0。此结果不能混同原2秒参数下的结果。

| 策略 | 完成 TPS | 平均延时 s | p95 s |
|---|---:|---:|---:|
| barrier | 100.21 | 27.12 | 55.94 |
| streaming | 69.74 | 49.59 | 86.59 |

本轮流式 TPS 约下降30.4%，且延时更高；单轮诊断不足以判断普遍性能。

## 已观察到的额外成本

从六个叶子的node0提交日志统计：

| 策略 | 叶子共识槽总数 | 跳过前缀控制槽 | frontier证书数 | frontier序列化总字节 | 集群发送总字节 |
|---|---:|---:|---:|---:|---:|
| barrier | 257 | 0 | 514 | 8019617 | 1023076543 |
| streaming | 362 | 105 | 4410 | 19498475 | 1399786180 |

流式的逐项认证前缀额外产生105个叶子控制槽，证书数514→4410，总发送字节也更多。这是成本观察，未通过独立消融证明某一项是全部性能差异的原因。下一步值得减少前缀证明和空块跳过的复制成本，同时保持图示顺序和认证检查。

## 验证范围

- 96项真实签名/PBFT证书的确定性检查通过，包括根头先执行、缺失父块阻塞后续根块、伪造/乱序/兄弟证书、执行依赖、复制摘要和检查点cursor。
- 三层m=1、四层m=4的冲突账户、多个NCA、真实共识和四副本一致顺序通过。
- 暂停根协调片primary、其他副本换主、原副本恢复追赶通过。
- 原窗口屏障的三层m=1/2/4、四层回归通过。
- 流式Arbor与Saguaro/SharPer/AHL的四方法小规模运行通过，baseline目录文件保持字节一致。
- StateDigest、Arbor组批选择、基线兼容性，以及脚本的失败/中断汇总检查通过。

旧配置默认保持屏障策略；新增 `three_layer_streaming.json` 开启流式，`three_layer_streaming_stress.json` 明确使用10秒超时做高压诊断。没有自动提交、推送或改动Git标签。

## 原始记录

- [2000笔三轮完整结果](</Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/streaming-ancestors-20261012/test-results/streaming-compare-2000/summary.json>)
- [原2秒超时大负载失败及中断记录](</Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/streaming-ancestors-20261012/test-results/streaming-compare-10000/summary.json>)
- [共同10秒超时大负载结果](</Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/streaming-ancestors-20261012/test-results/streaming-10000-timeout10s/summary.json>)
- [前缀开销统计](</Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/streaming-ancestors-20261012/test-results/streaming-10000-timeout10s/prefix-overhead.json>)

TPS按去重后的完整执行确认交易数除客户端耗时计算，没有预热剔除；失败和中断不提供成功吞吐估计。每轮配置快照、原始负载、客户端逐笔延时和节点日志都保存在对应结果目录中。
