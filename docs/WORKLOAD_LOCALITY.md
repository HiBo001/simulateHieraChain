# 跨片负载的访问局部性

## 实验口径

本版本按用户确认的实验设计生成跨片负载：具有相同直接父分片的叶子构成一个 cluster；所有跨片交易中，只有 5% 的参与叶子跨越两个以上 cluster。同时保留之前的 90% 两方、10% 三方比例。这里的 5% 以跨片业务交易为分母，不是所有消息、客户端请求或者全部含片内交易的工作负载。本页命令生成的是 100% 跨片交易，因此总交易数也就是跨片交易数。

此口径对应提供的 `HieraChain_submit_Euro_Par2024.pdf` 第 7.2 节实验设置。论文中的 cluster 由三个 sibling leaf shards 构成；新配置采用两个三叶 cluster，解决原来每组只有两个叶子、三方交易必然跨 cluster 的限制。本修改针对测试负载和比较入口，不修改共识或执行协议。

## 拓扑

Arbor 与 Saguaro 使用 `config/three_layer_locality.json`：

```text
                   7
             ┌─────┴─────┐
             5           6
          ┌──┼──┐     ┌──┼──┐
          1  2  8     3  4  9
         cluster 5    cluster 6
```

共有 9 个分片、36 个四节点 PBFT 副本进程。原有叶子与父关系保持，新增 `8→5` 与 `9→6`。叶子集合为 `{1,2,3,4,8,9}`。Arbor/Saguaro 的同 cluster 交易由 NCA 5 或 6 协调，跨 cluster 交易由根 7 协调。

AHL 使用 `config/ahl_two_layer_locality.json`，同一批六个叶子直接连接唯一上层 7，共有 7 个分片、28 个副本进程。cluster 标签仍从 Arbor 参考树取得；不能因为 AHL 的父节点相同，就将它们重定义为一个 cluster。SharPer 使用相同叶子、网络延时和业务输入，祖先分片不处理其跨片协议。

两份配置的共识、执行、片内延时和共同分片对的实际网络延时一致。AHL 删除中间片 5、6，保留所有共同分片之间的链路设置。拓扑和节点数差异是此架构比较的一部分，不能把结果称为相同进程数量下的纯协议对比。

## 交易数量

以默认 10000 笔为例：

| 类别 | 数量 | 占全部跨片交易 |
|---|---:|---:|
| 同 cluster、两方 | 8550 | 85.5% |
| 跨 cluster、两方 | 450 | 4.5% |
| 同 cluster、三方 | 950 | 9.5% |
| 跨 cluster、三方 | 50 | 0.5% |
| 合计 | 10000 | 100% |

两方合计 9000，三方合计 1000；跨 cluster 合计 500。默认计数下两方和三方各有 5% 跨 cluster。生成器按四个类别先分成客户端请求，再为每个请求从对应类别的合法叶子组合中均匀抽样；请求内交易具有相同参与集合，客户端请求顺序由固定种子打散。不同参与组合的交易数是种子固定的抽样结果，不保证每个组合或每个叶子的访问量完全相等。`--batch` 是每个请求的交易上限，类别尾部不足一批时保留短请求，不能为了凑满请求而改变交易比例。

总数或比例不能产生整数时，先以十进制四舍五入分别计算三方总数和跨 cluster 总数，再按两者比例计算跨 cluster 三方交集，并限制在数学上可行的范围；其余三个类别由差值确定。这样同时保持两种比例的取整边际，实际数量和比例保存于 workload 的 `locality` 中。需要某类同 cluster 三方交易时，参考拓扑必须有能容纳三个叶子的 cluster；需要跨 cluster 交易时，参考拓扑必须有至少两个 cluster。无法满足的配置会报错，不会暗中将同 cluster 交易改成跨 cluster 交易。

## 生成一份共享负载

在项目根目录执行：

```bash
python3 -B scripts/generate_mixed_workload.py \
  --config config/three_layer_locality.json \
  --count 10000 --rate 5000 --batch 10 --seed 42 \
  --cross-cluster-ratio 0.05 --three-shard-ratio 0.10 \
  --timeout 180 \
  --output test-results/locality/shared-workload.json
```

这一步只生成输入，不启动节点。负载包含全部交易与请求；跨片请求的 `target` 使用 Arbor 参考树的 NCA。客户端仍真实运行所选方法的共识与执行。

为了保持严格可比性，四种方法读取同一份 workload。交易 ID、请求 ID、参与叶子、访问 key/value、请求分组、顺序和速率均保持一致。SharPer 客户端在签名前把路由目标改为其发起叶子，AHL 改为唯一上层 7；原始输入文件保持不变。新运行 ID、密钥、回复端口和签名属于每轮认证信封，并非不同业务负载。

## 一键比较四种方法

直接生成并比较：

```bash
python3 -B baseline/compare_all.py \
  --config config/three_layer_locality.json \
  --ahl-config config/ahl_two_layer_locality.json \
  --count 10000 --rate 5000 --batch 10 --seed 42 \
  --cross-cluster-ratio 0.05 --three-shard-ratio 0.10 \
  --repeat 3 --timeout 180 --drain-timeout 60 \
  --output-dir test-results/locality-comparison
```

重放上一步已生成的文件：

```bash
python3 -B baseline/compare_all.py \
  --config config/three_layer_locality.json \
  --ahl-config config/ahl_two_layer_locality.json \
  --workload test-results/locality/shared-workload.json \
  --repeat 3 --timeout 180 --drain-timeout 60 \
  --output-dir test-results/locality-replay
```

`--workload` 与显式负载生成参数互斥，`--timeout` 可统一覆盖重放的客户端超时。比较入口依次启动独立集群、发送负载、检查副本收敛与协议队列排空，再停止节点；无需手动 `start`，也不要同时运行其他压测。`--skip-build` 可使用现有二进制，首次运行建议保留默认构建。

四种方法共享一次生成的输入文件，报告保留配置、负载 SHA256、各轮原始结果和统计。只有完整业务完成并通过副本及队列检查的轮次才报告成功 TPS；失败轮次保留已完成数和原因。任何方法有失败轮次，都不能把其成功轮次挑出后宣称完整实验的性能倍率。TPS 使用客户端完整确认的唯一交易数，延时为客户端发送到收到有效完成确认，平均和 p50/p95/p99 均以秒计。

## 成对比较或单方法重放

比较 Arbor 与 Saguaro：

```bash
python3 -B baseline/compare_mixed.py --baseline saguaro \
  --config config/three_layer_locality.json \
  --count 10000 --rate 5000 --batch 10 --seed 42 \
  --cross-cluster-ratio 0.05 --three-shard-ratio 0.10 \
  --repeat 3 --timeout 180 --drain-timeout 60
```

将 `--baseline saguaro` 改成 `--baseline sharper` 可比较 SharPer；选择 AHL 时使用 `--baseline ahl --baseline-config config/ahl_two_layer_locality.json`。若希望不同成对测试也复用完全相同输入，使用 `--workload test-results/locality/shared-workload.json` 替代生成参数。

单方法重放仍使用统一 benchmark：

```bash
python3 -B scripts/benchmark_mixed.py \
  --config config/three_layer_locality.json \
  --workload test-results/locality/shared-workload.json \
  --method arbor --drain-timeout 60
```

Saguaro、SharPer 使用相同配置，仅更改 `--method`。AHL 使用 `--method ahl --config config/ahl_two_layer_locality.json`，重放同一负载文件。

## 旧负载与其他局部性

旧的 `three_layer_cross100.json`、`ahl_two_layer.json` 及已存在 workload 保留，仍可用 `--workload` 复现以前实验。需要重新生成并测试旧前三叶均匀模式时，在比较入口显式加 `--uniform`：

```bash
python3 -B baseline/compare_mixed.py --baseline saguaro \
  --config config/three_layer_cross100.json --uniform \
  --count 10000 --rate 5000 --batch 10 --seed 42 --timeout 180 \
  --repeat 3 --drain-timeout 60 \
  --output-dir test-results/legacy-uniform-comparison
```

默认 10% 三方时，旧模式仍为 `[1,2]`、`[1,3]`、`[2,3]` 各 3000 笔及 `[1,2,3]` 1000 笔。它不保证 5% 跨 cluster；请与新局部性实验分开命名和报告。

`--cross-cluster-ratio 0` 表示全部交易在 cluster 内；`--cross-cluster-ratio 1` 表示全部交易跨 cluster。后者不等于关闭局部性或恢复旧均匀模式。比较入口的 `--uniform` 与 `--cross-cluster-ratio` 互斥；`scripts/generate_mixed_workload.py` 专门生成局部性负载，不提供旧均匀模式。`--three-shard-ratio` 可以独立修改两方/三方比例，默认仍为 0.10。

`make clean` 会删除 `test-results` 中生成的负载与报告。需要长期复现的输入和结果请在清理前备份。
