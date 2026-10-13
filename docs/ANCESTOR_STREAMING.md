# 按固定祖先顺序流式执行

基于 `75d32d5` 的窗口版本，新增 `consensus.ancestor_streaming=true`。执行次序按 `(协调片逻辑区块编号, 协调片距根深度, 协调片ID)` 升序，只包含当前叶子的祖先。两祖先例子为 `R1,C1,R2,C2,...`；三祖先为 `R1,M1,C1,R2,M2,C2,...`。

当 C4 已处理，R5 到达即可进入叶子 PBFT 并交换真实读写依赖、执行；不等待 C5。R5 执行完成后才能处理 C5。C5 未到时，缓存 R6，严格停在 C5。已认证的空块或不涉及本叶子的块可顺序跳过。祖先证书不足时不会猜测其为空，也不会等待兄弟协调片。

## 配置与运行

```bash
make
python3 -B scripts/cluster.py start --config config/three_layer_streaming.json --method arbor
```

新配置保留原九片/36节点参考树与原批量、网络、执行参数，设置 m=1、空块等待50ms、ancestor_streaming=true。旧配置未填写该字段时保持原窗口屏障策略；显式 false 也使用旧策略。该字段只允许布尔值，true 要求多协调片拓扑和 ordering_window_blocks>=1。baseline 启动器会剔除这个 Arbor 专有字段。

启用流式后，m仍定义协调片闭窗清单和实验暂停边界，叶子按每个区块顺序推进，不等待窗口封闭，也不按旧策略先处理完整根窗口再处理子窗口。

## 验证与可恢复执行位置

每个叶子维护复制状态 `cst_stream_cursor`，表示固定祖先序列已经处理的位置。主节点把从旧位置到下一业务块的连续证书前缀放入 `cst_frontier`。备份独立验证每一项的祖先身份、准确块号、PBFT quorum、叶子累计projection，以及跳过项确实不涉及本叶子；前缀最后一项必须与所提业务块一致。

业务块提交后仍等待原有读写依赖证明和实际执行，完成后才更新cursor、状态摘要和完成ACK。窗口证书到达、收到后续块或单纯PBFT提交都不能提前推进业务cursor。换主与检查点同步复用原有认证机制；cursor进入状态摘要和认证快照。

一个前缀最多32张证书。长空块前缀使用真实叶子PBFT的 `cst_stream_progress` 控制槽，复制消费这个完整前缀，再寻找后续业务；不产生交易或TPS计数。控制槽减少单次提案的证书数量，额外共识成本进入测量。叶子没有待处理业务时不会为后台空块不断生成控制槽。

## 同负载吞吐比较

```bash
# 默认：2000笔、1000 tx/s、50/200ms、每种策略各三轮
python3 -B scripts/benchmark_streaming.py

# 大负载诊断：共同使用10秒共识超时，各策略单轮
python3 -B scripts/benchmark_streaming.py \
  --config config/three_layer_streaming_stress.json \
  --count 10000 --rates 5000 --empty-ms-values 50 \
  --repeat 1 --seed 1042 --timeout 300 --drain-timeout 60

# 新流式 Arbor 与原 Saguaro/SharPer/AHL 同负载比较
python3 -B scripts/benchmark_all_methods.py \
  --profile rate --count 2000 --rates 100,1000 \
  --ancestor-streaming --window-blocks 1 --empty-ms 50
```

每个间隔/发送率/轮次只生成一份 workload，同一二进制分别运行 false/true 两种策略。共识、执行、网络、空块间隔、参与集合、交易ID和批次输入一致，配对只改变 ancestor_streaming；轮次交替先后顺序，seed逐轮加一。100%跨片，90%双方/10%三方，5%跨cluster。逐轮严格检查实际配置、二进制、负载哈希、全部交易完成和副本收敛。失败或缺轮时对应组的中位数和配对比值为空。

脚本输出 `summary.md/csv/json`，客户端完整逐笔延时和节点日志留在每轮策略目录。TPS和延时沿用现有客户端口径，包含发送及等待实际执行确认，没有预热剔除，属于有限负载结果。

```bash
make build/bin/arbor_node build/bin/test_ordering_windows
python3 -B tests/test_ordering_windows.py OrderingWindowLogic
python3 -B tests/test_ancestor_streaming.py
python3 -B tests/test_streaming_benchmark.py
```

确定性认证测试覆盖根头先执行、缺失父块阻塞未来根块、伪造/乱序/兄弟证书、执行依赖、复制摘要、换主与检查点cursor、长空前缀及重放。真实集群测试覆盖三层、四层、冲突账户、多个NCA和所有副本一致顺序。

真实故障测试会暂停本轮根协调片的primary，触发其他副本换主，再恢复原进程，要求所有交易完成和四副本检查点/状态追赶收敛。

本次性能结果见 [ANCESTOR_STREAMING_RESULTS.md](ANCESTOR_STREAMING_RESULTS.md)。原2秒共识超时下，大负载出现反复换主与停滞；stress配置只为诊断明确使用10秒，原配置和baseline配置保持原值。此次大负载流式性能下降，当前作为实验策略保留。
