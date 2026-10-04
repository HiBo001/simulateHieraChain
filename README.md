# Arbor 仿真系统：PBFT 与跨片执行

每个分片固定 4 个独立 C++ 进程，通过 TCP 和真实签名运行 PBFT。跨片交易支持多层树和两个以上参与叶子：最近公共祖先（NCA）通过 PBFT 排序，将认证订单直接发给参与叶子；每个叶子通过一次 PBFT 纳入本片顺序，交换依赖证明后执行并写入。叶子 forward 汇总完成 ACK QC，NCA 收齐所有参与叶子的证明后直接回复客户端。交易执行实际计算 Fibonacci。

叶子按批次顺序处理，等待当前批次依赖并完成执行后再处理下一批。多协调者拓扑使用按需开启的认证轮次：各协调者通过 PBFT 封闭本轮订单，叶子收齐其所有祖先的封闭证书后，按 `(round, coordinator_id)` 顺序处理适用订单，避免不同层级订单形成相互等待。当前没有逐交易推测执行、多版本历史或重做路径。详细说明见 [docs/MULTILAYER_DESIGN.md](docs/MULTILAYER_DESIGN.md) 和 [当前实现设计](docs/CURRENT_IMPLEMENTATION_DESIGN.md)。

支持 macOS / Linux 本机多进程运行。构建依赖 C++17 编译器、Python 3.9+ 和 OpenSSL 3 开发库；不依赖 FISCO、Python 第三方包或额外 JSON 包安装。nlohmann/json 3.11.3 单头文件及 MIT 许可证已放入 `third_party/nlohmann/`。

## 快速开始

在项目根目录运行：

```bash
# macOS 缺少依赖时执行：
brew install openssl@3

# Debian / Ubuntu 缺少依赖时执行：
# sudo apt-get install build-essential python3 libssl-dev openssl

make
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py topology
python3 scripts/cluster.py status
python3 scripts/cluster.py load --shard 1 --count 100 --rate 100 --seed 42
python3 scripts/cluster.py probe --source 1:0 --to 2:0 --samples 5
./stop_all.sh
```

`two_layer.json` 定义叶子 1、2 和协调者 5，总共启动 **12 个节点进程**。节点编号为 0、1、2、3，初始主节点为 0。单分片示例在 `config/single_shard.json`；三层 7 分片、28 节点示例在 `config/three_layer.json`；四层 9 分片、36 节点示例在 `config/four_layer.json`。

`topology` 默认读取 `runtime/latest/config.json`，打印最近一次运行实际保存的分片树；指定运行目录可用 `--run-dir 路径`。启动前可用 `python3 scripts/cluster.py topology --config config/three_layer.json` 预览并校验配置。启动成功时也会打印同一棵树。两层示例输出：

```text
分片拓扑（3 个分片，每片 4 个 PBFT 副本）
分片 5（根/协调者）
├── 分片 1（叶子）
└── 分片 2（叶子）
```

如果 OpenSSL 安装在其他位置：

```bash
make OPENSSL_PREFIX=/your/openssl/prefix
OPENSSL_BIN=/your/openssl/prefix/bin/openssl ./start_all.sh
```

自动测试：

```bash
make test
```

完整设计和待审阅边界见 [docs/CURRENT_IMPLEMENTATION_DESIGN.md](docs/CURRENT_IMPLEMENTATION_DESIGN.md)，覆盖 PBFT、跨片时序、forward 聚合、去重、批处理、网络延迟、故障恢复与性能统计。详细手动测试见 [TESTING.md](TESTING.md)，阶段演进说明见 [docs/STAGE1_DESIGN.md](docs/STAGE1_DESIGN.md) 和 [docs/STAGE2B_DESIGN.md](docs/STAGE2B_DESIGN.md)。最近工程修改记录见 [docs/ENGINEERING_REVIEW.md](docs/ENGINEERING_REVIEW.md)。

协议审阅更正后的目标流程见 [docs/CROSS_SHARD_PROTOCOL_REVISION.md](docs/CROSS_SHARD_PROTOCOL_REVISION.md)：上层排序一次，各叶子排序并执行一次，上层收齐完成证明后直接回复，无第二轮上层共识。当前代码已实现上述流程。

本轮工程优化说明、验收及复测命令见 [docs/PERFORMANCE_OPTIMIZATION.md](docs/PERFORMANCE_OPTIMIZATION.md)。编译后请停止旧集群并启动新集群，全部节点使用同一版本。

一键性能测试使用 `python3 scripts/benchmark.py`（或 `make benchmark`）：自动编译，逐轮启动独立集群，运行片内/跨片负载，停止节点，保存 TPS、秒延时及网络指标。默认每类各测试 4000 笔、速率 1000 和 4000；详见 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

## Saguaro 2PC 对比方法

`baseline/saguaro/` 提供由参与分片最近公共祖先协调的传统 2PC。它复用 Arbor 的四节点 PBFT、网络、执行与客户端；通过 `start --method saguaro` 选择，默认启动方式仍为 Arbor。

```bash
python3 baseline/compare.py --config config/two_layer.json \
  --participants 1,2 --count 4000 --rate 1000 --batch 10 \
  --repeat 3 --seed 42 --timeout 120 --drain-timeout 60
```

对比工具把同一份负载分别交给两个方法，自动启动和停止集群，保存完成 TPS、秒制延时与副本收敛检查。使用和测试见 [baseline/README.md](baseline/README.md)，协议设计见 [baseline/saguaro/DESIGN.md](baseline/saguaro/DESIGN.md)。

## 配置分片与拓扑

当前系统的配置统一存放在 `config/`：JSON 文件供当前启动器使用；`accessControlList`、`networkConfig`、`shardsTopology`、`workloadProfile` 和 `topShardId` 是旧格式参考文件，不能直接传给新启动器。当前可运行的配置采用 JSON。分片数量由 `shards` 数组长度决定，不重复配置数量。每个分片有唯一的正整数 `id` 和一个 `parent`；根的 `parent` 为 `null`。

```json
{
  "replicas_per_shard": 4,
  "host": "127.0.0.1",
  "base_port": 19000,
  "shards": [
    {"id": 1, "parent": 5},
    {"id": 2, "parent": 5},
    {"id": 5, "parent": null}
  ],
  "network": {
    "intra_shard_delay_ms": 1,
    "default_inter_shard_delay_ms": 20,
    "shard_links": [
      {"shards": [1, 2], "delay_ms": 50},
      {"shards": [1, 5], "delay_ms": 10},
      {"shards": [2, 5], "delay_ms": 30}
    ],
    "trace": false
  }
}
```

配置修改后，先检查，再停止旧集群并启动新集群：

```bash
python3 scripts/cluster.py validate --config config/two_layer.json
./stop_all.sh
./start_all.sh --config config/two_layer.json
```

- 自动识别根、叶子和协调者，自动计算 NCA；允许非连续 ID。
- 检查重复 ID、缺失父节点、环路、多个根、重复链路、非法延迟和端口。
- 按分片 ID 排序，每个分片的 4 个节点依次使用 `base_port` 开始的连续端口。
- 配置在一次运行内固定，运行中修改源 JSON 不会生效。
- 当前启动器在一台机器上启动所有节点。`host` 必须是本机可绑定的 IPv4 地址；不提供远程 SSH 部署。

## 延迟配置

`delay_ms` 是**单向附加消息延迟**，分片对默认双向对称。未列出的分片对使用 `default_inter_shard_delay_ms`。片内不同副本之间使用 `intra_shard_delay_ms`；副本投给自身的本地消息不附加网络延迟。

例如 1↔2 配置 50 ms，该分片对的任意副本之间均使用 50 ms；一次往返约为 100 ms 加真实网络和处理开销。客户端请求、客户端回复不使用分片对延迟。

```bash
python3 scripts/cluster.py probe --source 1:0 --to 2:3 --samples 5
python3 scripts/cluster.py probe --source 1:0 --to 5:0 --samples 5
```

探测请求由指定源副本发出，PONG 从目标副本返回，RTT 使用源副本的单调时钟计算。探测结果还会写入运行目录中的 `probe-*.json`。开启 `network.trace` 后，各节点额外输出 `network.jsonl`，记录 `enqueue_ms`、`due_ms`、`release_ms`、`delay_ms` 和目标端口。`release_ms` 是消息交给目标长连接发送队列的时间，不是对端收到完整消息的时间。运行状态中的 `network_failures` 及分类字段可用于诊断连接、写入或消息解析错误。

## 共识与执行参数

```json
"consensus": {
  "batch_size": 32,
  "batch_wait_ms": 10,
  "cross_shard_batch_size": 32,
  "cross_shard_batch_wait_ms": 200,
  "view_timeout_ms": 2000,
  "checkpoint_batches": 16
},
"execution": {
  "fib_iterations": 10000
}
```

| 参数 | 含义 |
|---|---|
| `batch_size` | 一个 PBFT 批次最多容纳的交易数量，默认 32 |
| `batch_wait_ms` | 主节点组批等待时间，默认 10 ms |
| `cross_shard_batch_size` | 协调分片一个跨片批次最多容纳的交易数量，默认 `min(64, batch_size)`；不得大于 `batch_size` |
| `cross_shard_batch_wait_ms` | 协调分片等待跨片批次凑满的最长时间，默认 200 ms；示例二层配置为 600 ms |
| `view_timeout_ms` | 等待交易取得进展的基础超时，默认 2000 ms；应明显大于正常共识耗时 |
| `checkpoint_batches` | 每多少个已应用 PBFT 批次生成稳定检查点，默认 16，允许 1–32 |
| `fib_iterations` | 每笔叶子交易实际执行的迭代斐波那契循环数，默认 10000 |

上面的参数块是说明示例；实际值以选用的 JSON 为准。当前 `config/two_layer.json` 的 `batch_size=1000`、`cross_shard_batch_size=64`、`cross_shard_batch_wait_ms=600`。

自定义配置中若仍有 `consensus.pipeline_window`，请删除该项后重新启动；包括值为 0 的配置也会报废弃参数错误，当前代码不再提供流水线模式。

斐波那契使用 `uint64_t` 模 2^64 加法，溢出行为有明确定义。结果进入状态摘要，不能被编译器当作无用计算消除。状态为简单账户值、版本号和依赖旧状态的摘要，因此相同交易的不同执行顺序会反映在结果中。

## 发送测试负载

```bash
# 向叶子 1 发送 1000 笔交易，总目标到达率 200 tx/s
python3 scripts/cluster.py load --shard 1 --count 1000 --rate 200 --seed 42 --timeout 60

# 按批次轮流向所有叶子发送，总共 200 笔、全局目标速率 100 tx/s
python3 scripts/cluster.py load --count 200 --rate 100 --seed 42

# 两个叶子分片各 10000 笔；--rate 10000 是全局目标速率，约为每片 5000 tx/s
python3 scripts/cluster.py load --count 20000 --rate 10000 --timeout 120

# 重复请求测试：两次使用相同前缀、seed、count、batch 和目标
python3 scripts/cluster.py load --shard 1 --count 40 --rate 100 --id-prefix duplicate-test --seed 42
python3 scripts/cluster.py load --shard 1 --count 40 --rate 100 --id-prefix duplicate-test --seed 42

# 测试 NCA=5 的完整二层跨片提交；结果会显示真实完成 TPS 与延时
python3 scripts/cluster.py load --participants 1,2 --count 40 --rate 100 --batch 8 --timeout 30
```

`rate` 是全局目标交易到达率，按请求批次发送，不是指定系统 TPS。单条 `load` 命令只接受一组 `--shard`、`--count`、`--rate`；这些参数重复时会报错。省略 `--shard` 时，批次按叶子分片轮流分配；如需两个独立客户端同时发送，可在两个终端分别运行单片 `load`。客户端默认给交易加随机运行前缀；`--seed` 固定账户和值的序列，`--id-prefix` 再固定交易标识。实际发出的负载保存在 `client-*.workload.json`，便于检查。

多层手动测试：

```bash
./stop_all.sh
./start_all.sh --config config/three_layer.json
# 同父叶子：NCA=5
python3 scripts/cluster.py load --participants 1,2 --count 64 --rate 100 --batch 8 --timeout 120
# 不同子树：NCA=7
python3 scripts/cluster.py load --participants 1,3 --count 64 --rate 100 --batch 8 --timeout 120
# 三个参与叶子：NCA=7
python3 scripts/cluster.py load --participants 1,2,3 --count 64 --rate 100 --batch 8 --timeout 120
python3 scripts/cluster.py status
./stop_all.sh
```

完整跨片请求成功时均显示 `ordered_only=0`。四层可改用 `config/four_layer.json`，参与者 `1,8` 的 NCA 是根 9，参与者 `1,3` 的 NCA 是协调者 7。

跨片交易由 `accesses` 为每个参与叶子声明一个 key 和输入值，目标是其最近公共祖先（NCA）。两个以上不重复叶子可共同参与；三层、四层及非均匀深度拓扑均走完整执行路径。中间协调者不为祖先的订单重复发起交易共识。新请求若重放相同交易 ID 与内容，客户端显示 `duplicates`，不计入完成 TPS；同一 ID 对应不同内容会返回 `id_conflict`。

片内请求默认最多 `batch_size` 笔，跨片请求默认最多 `cross_shard_batch_size` 笔；`--batch` 可降低请求大小，超过对应上限会报错。协调分片会把参与分片集合相同的请求合并成至多 `cross_shard_batch_size` 笔的 PBFT 跨片批次。示例配置下，`--batch 8` 的 8 个请求可合成一个 64 笔跨片批次。此处按参与分片集合组批，不要求 key 或读写集完全相同；批前读快照按唯一 key 复用，批内按交易顺序更新 working state。客户端请求首次发送到目标分片的副本 0、1，即 `f+1=2` 个不同节点；备份转交给当前主节点，超过 500 ms 未确认才向全部 4 个副本重试，后续重试逐步退避。客户端验证副本 Ed25519 签名，等到 `f+1=2` 个不同副本返回相同结果后计为确认。

所有请求成功确认后，终端同一行显示 `completed_tps`、`avg_latency_s`、`p50_s`、`p95_s`、`p99_s`；若超时或交易报错，则显示 `incomplete=true` 和已确认交易的延时，并返回非零退出码。结果文件 `client-*.json` 也保存这些指标、逐交易确认耗时和完整输入。交易延时统一以秒为单位，逐笔记录使用 `latency_s`，完成时刻使用相对于测试开始的 `completion_s`。延时口径为客户端发送请求到收到两个一致副本回复；平均值和分位数按已确认交易统计。跨片回复必须等到所有参与叶子最终写入并返回各自三副本 ACK，因而计入真实完成延时；多层跨片也按此标准确认完成。这里的平均 TPS 包括本次客户端的发送及排空时间，**不是饱和稳态吞吐**。批内交易共用请求发送时间，确认在整批回复时观察到。重复测试中客户端确认数表示收到的有效回复，是否发生新的执行要看节点累计执行数；不要拿重放请求测试计算业务吞吐。

`--timeout` 从开始发送时计时，包含发送和排空。`--count 4000 --rate 100` 光发送至少约 40 秒，因此 `--timeout 30` 必定显示 `incomplete=true`。小批次跨片负载还需留出多轮 PBFT 和跨片证明交换时间；可先从较小 count 验证，再按实际完成速率增加超时。

例如 4000 笔跨片交易以 `--rate 100 --batch 8` 发送，即使系统处理更快，整轮平均 TPS 也不能超过约 100。要测试更高处理能力，应提高 `--rate` 并同时查看延时；发送速度超过处理速度时，排队延时会升高。

## 运行目录和启停

未指定 `--run-dir` 时，每次启动创建新的 `runtime/日期-唯一标识/`，`runtime/latest` 指向最近一次普通运行。显式指定运行目录的实例与自动测试不改变这个指向，应使用 `--run-dir` 操作它们：

```text
runtime/latest/
  config.json                  # 本次解析后的完整配置和节点端口
  manifest.json                # PID、节点目录、运行标识
  keys/                        # 本次实验专用密钥，不纳入 Git
  shard1/node0/
    node.log                   # 进程标准输出和错误
    status.json                # 每约 100 ms 更新的状态快照
    events.jsonl               # prepared、提交、视图切换、状态同步等事件
    commits.jsonl              # 提交证书、交易值摘要和状态摘要
    network.jsonl              # 可选的消息延迟调度记录
  client-*.json
  client-*.workload.json
```

客户端取得两个回复时，其余副本可能仍在接收最后一份完成证明。检查排空时应等待 `pending_requests=0`、`pending_cst_batches=0`、`staged_cst_batches=0`、待发送网络队列清空，再比较同片四副本的 `state_digest`、`kv_digest`、`chain_digest` 和 `applied_batches`；一键 benchmark 会自动完成这些检查。

`status` 会检查 PID 的实际命令行，死亡节点不会因为留下旧快照而显示为存活。停止脚本只停止该运行目录登记且身份仍匹配的进程；先发 SIGTERM，等待退出后才处理残留进程。不会按名称杀掉其他集群。

```bash
python3 scripts/cluster.py status --json
./stop_all.sh --run-dir /absolute/path/to/a/run
./start_all.sh --config config/single_shard.json --run-dir /absolute/path/to/a/new-run
```

每次 `start` 是一轮新实验，初始状态清空、生成新的 run ID 和密钥。旧日志不删除。已存在的运行目录禁止覆盖；重复启动当前运行或端口冲突会直接报错并避免留下半启动集群。

## 清理日志和实验记录

在项目根目录执行：

```bash
make clean
```

此命令先停止 `runtime/`、`test-results/` 中登记且进程身份匹配的节点，再删除整个 `build/`，清空运行记录和测试/性能结果（包括日志、密钥、客户端输入/结果、JSON/CSV 报告和性能基线），并删除 Python 缓存及顶层 `shard*/node.log`。`runtime/.lifecycle.lock` 保留用于启停互斥；源码、`config/`、文档、论文和分片身份文件保留。

正在运行的 `load`、`benchmark` 或测试需要先结束，否则清理会报错并保留记录，避免它们继续生成新数据。仓库外自定义 `--run-dir`、`--output-dir`、`--output` 不自动清理；目录是符号链接时只删除链接。新的工程专项测试记录也统一保存在 `test-results/engineering-*/`，以前生成在系统临时目录的记录不在本命令范围内。

执行清理时不要同时启动新的负载、benchmark 或测试任务。

清理后重新运行需要先 `make`，再启动集群。需要保留的实验报告或基线应在清理前复制到其他目录。

## 当前实现边界

- 包括真实 PBFT 三阶段、批处理、Ed25519 签名、检查点、带 prepared 证明的视图切换，以及滞后但未重启副本的状态追赶。
- 包括独立分片的排序和叶子的本地执行，以及所有分片对之间的延迟探测。
- 完整跨片执行支持多层及两个以上参与叶子。NCA 收齐实际参与叶子的完成证明后直接回复；多协调者的轮次封闭证书会增加 PBFT 控制开销，空封闭不增加业务交易数。SharPer、状态重分区和自适应扩缩容尚未实现。
- 每个参与叶子的合成交易访问一个 key；尚未支持任意合约动态提取多 key 读写集。
- 所需依赖未齐时不写入正式 KV、不推进叶子应用序号。当前没有带证书的 abort/超时回收，参与分片永久失效可能令交易等待和客户端超时；若一叶已经写入，另一叶之后不可用，不能保证两片同一物理时刻可见。
- 叶子按批次顺序等待依赖并执行，当前批次未完成时不处理后续业务槽。协调者仍可提前排序有限数量的跨片批次；这不表示叶子并行执行。片内请求仍优先选择，持续片内负载下的跨片公平调度尚未实现。尚未实现单副本内多 CPU 线程执行。
- 进程崩溃后在同一次运行中重启该身份还未提供完整 WAL 恢复。节点检测到已有提交日志会拒绝启动，防止丢失投票状态后重新投票。请停止整个集群并新建实验；暂停后恢复进程可通过状态追赶恢复。
- 状态、请求去重索引在内存中，适合有限负载的验收。检查点会清理旧共识消息，历史状态和去重信息仍随有效交易增长；不是生产存储引擎。
- 附加延迟是应用层消息模型，不模拟链路带宽、丢包或 TCP 拥塞控制。节点进程会共享本机 CPU，规模实验需要后续独立规划资源。

第一阶段改造前的源码和运行入口快照保留在历史标签 `stage1-pbft` 的 `legacy/pre-stage1/` 中，当前工作区已移除该目录。旧格式配置文件现在位于 `config/`；`shard*/shardId`、`shard*/lldb_commands.txt` 位于对应分片目录，均不由新入口读取；`shard*/node.log` 是历史日志。历史 PDF 不改动。旧 `llb_start_all.sh` 仅提示使用新的启动方式。

多层实现的详细设计见 [docs/MULTILAYER_DESIGN.md](docs/MULTILAYER_DESIGN.md)，本轮验收与复测记录见 [docs/MULTILAYER_VALIDATION.md](docs/MULTILAYER_VALIDATION.md)。
