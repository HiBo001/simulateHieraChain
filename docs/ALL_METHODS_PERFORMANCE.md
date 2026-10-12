# Arbor 和全部 baseline 的性能测试

项目实际支持四种方法：Arbor、Saguaro、SharPer 和 AHL。`scripts/benchmark_all_methods.py` 复用现有集群启动、负载生成、完整执行和副本收敛检查，统一运行四种方法并汇总结果；只依赖 Python 标准库。

默认测试新增窗口机制下的 Arbor：`ordering_window_blocks=1`、`empty_block_interval_ms=200`。这是明确的实验参数，不代表最优参数。`--window-blocks 0` 测试关闭窗口的 Arbor。三个 baseline 始终使用原配置，不包含这两个 Arbor 参数。

## 直接运行

```bash
cd /Users/tanghaibo_office/Desktop/ATLAS/sourceCode/simulateHieraChain

# 先查看实验矩阵；不会编译、启动节点或写结果
python3 -B scripts/benchmark_all_methods.py --profile rate --dry-run

# 快速验证：四方法各 200 笔、100 tx/s、1 轮
python3 -B scripts/benchmark_all_methods.py --profile smoke

# 发送速率扫描：四方法各 10000 笔，100/500/1000/2000/5000 tx/s，各 3 轮
python3 -B scripts/benchmark_all_methods.py --profile rate

# 跨 cluster 比例扫描：0/5/25/50/100%，1000 tx/s，各 3 轮
python3 -B scripts/benchmark_all_methods.py --profile locality

# 两个维度的完整矩阵，共 300 个独立集群用例
python3 -B scripts/benchmark_all_methods.py --profile full

# 关闭窗口的 Arbor 与同一批 baseline 比较
python3 -B scripts/benchmark_all_methods.py --profile rate --window-blocks 0
```

不需要手动 start。默认先调用 `make` 编译四个节点程序；每个用例选择空闲端口、启动独立集群、发送负载、检查完成与收敛并停止本轮节点。测试顺序按矩阵位置和重复轮次轮换，不同时启动多个方法。

构建依赖 C++17、Python 3.9+、OpenSSL 开发库和 zlib。macOS 可先 `brew install openssl@3`；Debian/Ubuntu 可先 `sudo apt-get install build-essential python3 libssl-dev openssl zlib1g-dev`。非标准 OpenSSL 路径可先 `make OPENSSL_PREFIX=/path/to/openssl`，再给测试脚本加 `--skip-build`。

## 自定义实验

```bash
python3 -B scripts/benchmark_all_methods.py \
  --profile rate \
  --count 10000 --rates 500,1000,2000,5000 \
  --cross-cluster-ratios 0.05,0.5 \
  --three-shard-ratio 0.10 \
  --batch 10 --repeat 3 --seed 42 \
  --window-blocks 1 --empty-ms 200 \
  --timeout 300 --drain-timeout 60 \
  --output-dir test-results/my-all-methods-run
```

| 参数 | 含义 |
|---|---|
| `--count` | 每个方法、每轮的业务交易总数 |
| `--rates` | 全局目标发送速率列表，单位 tx/s；并非系统完成 TPS |
| `--cross-cluster-ratios` | 全部跨片交易中，访问不同 cluster 的比例 |
| `--three-shard-ratio` | 三参与叶子交易占比，其余为双参与叶子；默认 10% |
| `--batch` | 每个客户端请求的交易数上限，默认 10 |
| `--repeat` | 每个矩阵点的重复轮数；种子为 seed、seed+1、seed+2 等 |
| `--window-blocks` | Arbor 排序窗口 m；默认 1，0 表示关闭 |
| `--empty-ms` | Arbor 空块间隔，默认 200 ms；m=0 时不写入实际配置 |
| `--timeout` | 每轮客户端总超时，包含发送和等待确认；默认 count/min(rates)+180 秒 |
| `--drain-timeout` | 客户端结束后副本排空、收敛检查的最长等待；默认 60 秒 |
| `--output-dir` | 尚不存在的结果目录；不覆盖历史实验 |
| `--skip-build` | 显式使用现有二进制，适合已经完成构建的实验机器 |

`smoke/rate/locality/full` 分别默认为 4/60/60/300 个独立集群用例。覆盖参数后实际规模会在启动前打印。

## 负载和比较条件

默认参考树使用 `config/three_layer_locality.json`：cluster 5 为 `{1,2,8}`，cluster 6 为 `{3,4,9}`。Arbor/Saguaro/SharPer 共 9 片、36 个副本进程。AHL 使用 `config/ahl_two_layer_locality.json`，将同样六个叶子接到根 7，共 7 片、28 个进程。

所有生成的交易都是跨片交易。默认其中 90% 涉及两个叶子，10% 涉及三个叶子，5% 跨 cluster；另外 95% 虽然在同 cluster，仍然是跨片交易。10000 笔的四类交易数依次为：同 cluster 双方 8550、跨 cluster 双方 450、同 cluster 三方 950、跨 cluster 三方 50。

每个比例/速率/轮次仅生成一次 unsigned workload；四方法读取相同文件、相同 ID、读写输入、参与方、请求批次及顺序。AHL 和 SharPer 在签名前使用各自路由规则。AHL 继续沿用 Arbor 的 cluster 分类，不按展平后的树重新分类。

脚本在运行前验证共同叶子、PBFT/执行设置和共同分片对的实际延迟。只允许 Arbor 的两个窗口字段以及显式 AHL 拓扑产生协议配置差异。原配置保持不变，实验使用结果目录内的配置快照；记录源码、二进制、配置和负载 SHA256。

当前默认配置的 `fib_iterations=1`，主要测量协议和通信成本。若要测量较重的交易执行，制作 Arbor 和 AHL 两份配置副本，同时调整相同的 execution 参数，再使用 `--config` 与 `--ahl-config` 指定。

## 结果怎么看

脚本打印结果目录，默认在 `test-results/all-methods-时间-随机标识/`。

| 文件 | 内容 |
|---|---|
| `summary.md` | 按比例、速率、方法排列的 TPS 和延时中位数及配对 TPS 比值 |
| `summary.csv` | 汇总行，便于绘制发送速率—吞吐量、发送速率—p95 曲线 |
| `cases.csv` | 逐轮指标、完成数量、网络字节/交易、失败原因与运行路径 |
| `summary.json` | 所有分组、配置与环境、源码和二进制哈希、负载路径、逐轮网络计数 |
| `cross-cluster-*/rate-*/repeat-*/shared-workload.json` | 该轮四方法共用的原始负载 |
| `.../<method>/client.json` | 完整客户端结果和逐笔延时 |
| `.../<method>/status-after.json`、`run/` | 副本状态、密钥、配置和节点日志 |

TPS 按客户端完整确认的唯一交易数除以 `elapsed_s` 计算，包含发送和等待确认；同一交易在多个副本、多个叶子执行仍只算一笔。延时从客户端请求首次发送计到得到足够一致回复，每个请求内的交易继承该请求的确认延时。单位全部为秒。集群启动和客户端结束后的副本收敛等待不进入客户端 TPS/延时。

`PASS` 要求全部请求确认、执行数量正确、所有副本摘要收敛，并且待处理交易、协议状态、锁和发送缓存排空。启用窗口的 Arbor 在客户端计时结束后暂停后台空块，再做严格收敛检查。失败轮次保留客户端和节点证据，整个方法分组的中位数留空；Arbor 与某 baseline 的完整配对组有失败或缺轮时，该比值也留空。

每个指标的中位数是“各轮指标”的中位数；p95/p99 不将多个轮次的逐笔延时混合重算。配对比值是各轮 Arbor TPS / baseline TPS 的中位数。

先看低负载下的延时，再逐步提高 rate 观察 TPS 是否趋于平台、p95 是否增加。默认 10000 笔在 5000 tx/s 下只发送约 2 秒；需要持续负载时增大 count，例如 `--count 100000 --rates 1000,2000,5000`，同时让 timeout 足够覆盖最慢 baseline 的完成时间。当前工具不排除预热，报告属于有限负载结果，稳态能力需结合更长实验判断。

AHL 进程数量与其他方法不同；所有进程运行在同一机器，共享 CPU。SharPer 使用当前升序预约、单未执行跨片 slot 的工程实现；AHL 使用当前两层 PBFT+2PC 实现。结果应标明这些实现范围和拓扑差异。

网络字节统计来自负载前后全部节点计数差，包含副本检查排空及窗口暂停阶段的开销，适合报告本轮总通信成本；它不是客户端计时区间内精确采样的瞬时带宽。当前脚本不采样 CPU/RSS；资源占用需由独立系统监测工具记录。

`make clean` 会删除项目内的 `test-results/`；长期实验可将 `--output-dir` 指向项目外的新目录。Ctrl+C 或 SIGTERM 会走现有清理逻辑并保留已完成数据。退出码 0 表示全部 PASS，2 表示存在失败用例，1 表示配置/构建/运行异常，130 表示中断。

## 已有入口

- `baseline/compare_all.py`：原配置、四方法同负载比较，默认不启用 Arbor 新窗口。
- `scripts/benchmark_ordering_windows.py`：扫描多个 Arbor `m:empty_ms` 参数对，并与原 baseline 比较；其负载固定为 5% 跨 cluster、10% 三方。
- `baseline/compare.py`：Arbor 与单个 baseline、固定参与叶子集合比较。
- `scripts/benchmark.py`：片内和固定参与组跨片测试，支持 `--method`。

例如扫描窗口参数时，可继续使用已有专用入口：

```bash
python3 -B scripts/benchmark_ordering_windows.py \
  --parameter-pairs 1:200,2:200,4:200 \
  --rates 1000,5000 --count 10000 --batch 10 --repeat 3 \
  --timeout 300 --drain-timeout 60 \
  --baseline-methods saguaro,sharper,ahl
```

新脚本的工具回归：`python3 -B tests/test_all_methods_benchmark.py`。

## 本次验证记录（2026-10-12）

在当前源码的独立副本中重新编译了四个节点程序。新脚本 6 项工具检查、已有窗口脚本 12 项检查、baseline 兼容性 7 项检查均通过。窗口开启 `m=1/200 ms` 和关闭 `m=0` 各运行四方法，每方法 200 笔、100 tx/s，8 个真实集群用例全部 PASS，结束后停止本轮节点。

这次小规模测试只验证脚本完整执行、输入一致和副本收敛，没有执行完整速率扫描。原始验证文件位于 `/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/perf-suite-20261012/` 下的 `test-results/smoke-window-verified/` 与 `test-results/smoke-legacy-verified/`。
