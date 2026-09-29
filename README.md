# Arbor 仿真系统：第一阶段

第一阶段提供可手动配置拓扑的多分片 PBFT 底座。每个分片固定 4 个独立 C++ 进程，通过 TCP 交换真实签名消息。叶子副本在共识提交后实际执行斐波那契计算并更新内存状态；协调者副本只排序。

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

`two_layer.json` 定义叶子 1、2 和协调者 5，总共启动 **12 个节点进程**。节点编号为 0、1、2、3，初始主节点为 0。单分片示例在 `config/single_shard.json`；三层 7 分片、28 节点示例在 `config/three_layer.json`。

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

详细手动测试见 [TESTING.md](TESTING.md)，协议与实现说明见 [docs/STAGE1_DESIGN.md](docs/STAGE1_DESIGN.md)。

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

探测请求由指定源副本发出，PONG 从目标副本返回，RTT 使用源副本的单调时钟计算。探测结果还会写入运行目录中的 `probe-*.json`。开启 `network.trace` 后，各节点额外输出 `network.jsonl`，记录 `enqueue_ms`、`due_ms`、`release_ms`、`delay_ms` 和目标端口。`release_ms` 是开始非阻塞连接/发送的时间，不是对端收到完整消息的时间。

## 共识与执行参数

```json
"consensus": {
  "batch_size": 32,
  "batch_wait_ms": 10,
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
| `view_timeout_ms` | 等待交易取得进展的基础超时，默认 2000 ms；应明显大于正常共识耗时 |
| `checkpoint_batches` | 每隔多少个批次广播检查点，默认 16，允许 1–32 |
| `fib_iterations` | 每笔叶子交易实际执行的迭代斐波那契循环数，默认 10000 |

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

# 测试 NCA=5 的协调者真实共识，只排序，不是已完成跨片交易
python3 scripts/cluster.py load --participants 1,2 --count 40 --rate 100
```

`rate` 是全局目标交易到达率，按请求批次发送，不是指定系统 TPS。单条 `load` 命令只接受一组 `--shard`、`--count`、`--rate`；这些参数重复时会报错。省略 `--shard` 时，批次按叶子分片轮流分配；如需两个独立客户端同时发送，可在两个终端分别运行单片 `load`。客户端默认给交易加随机运行前缀；`--seed` 固定账户和值的序列，`--id-prefix` 再固定交易标识。实际发出的负载保存在 `client-*.workload.json`，便于检查。

每个客户端请求包含最多 `batch_size` 笔交易，也可通过 `--batch` 降低。请求首次广播到目标分片全部 4 个副本，超时重发。客户端验证副本 Ed25519 签名，等到 `f+1=2` 个不同副本返回相同结果后计为确认。

所有请求成功确认后，终端同一行显示 `completed_tps`、`avg_latency_s`、`p50_s`、`p95_s`、`p99_s`；若超时或交易报错，则显示 `incomplete=true` 和已确认交易的延时，并返回非零退出码。结果文件 `client-*.json` 也保存这些指标、逐交易确认耗时和完整输入。交易延时统一以秒为单位，逐笔记录使用 `latency_s`，完成时刻使用相对于测试开始的 `completion_s`。延时口径为客户端发送请求到收到两个一致副本回复；平均值和分位数按已确认交易统计。协调者仅排序的跨片样例得到的是排序回复延时，并非跨片交易执行完成延时。这里的平均 TPS 包括本次客户端的发送及排空时间，**不是饱和稳态吞吐**。批内交易共用请求发送时间，确认在整批回复时观察到。重复测试中客户端确认数表示收到的有效回复，是否发生新的执行要看节点累计执行数；不要拿重放请求测试计算业务吞吐。

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

`status` 会检查 PID 的实际命令行，死亡节点不会因为留下旧快照而显示为存活。停止脚本只停止该运行目录登记且身份仍匹配的进程；先发 SIGTERM，等待退出后才处理残留进程。不会按名称杀掉其他集群。

```bash
python3 scripts/cluster.py status --json
./stop_all.sh --run-dir /absolute/path/to/a/run
./start_all.sh --config config/single_shard.json --run-dir /absolute/path/to/a/new-run
```

每次 `start` 是一轮新实验，初始状态清空、生成新的 run ID 和密钥。旧日志不删除。已存在的运行目录禁止覆盖；重复启动当前运行或端口冲突会直接报错并避免留下半启动集群。

## 第一阶段边界

- 包括真实 PBFT 三阶段、批处理、Ed25519 签名、检查点、带 prepared 证明的视图切换，以及滞后但未重启副本的状态追赶。
- 包括独立分片的排序和叶子的本地执行，以及所有分片对之间的延迟探测。
- 协调者收到跨片样例后只产生 `ordered_only` 数量，不向叶子执行跨片事务，也不计入 `completed_tps`。Arbor 的读写集交换、跨片 ACK 闭环、多层冲突调度、SharPer、重分片和扩缩容尚未实现。
- 正常路径每片最多一个新批次在途，视图恢复可涉及多个历史序号；这一阶段不以极限性能为目标。
- 进程崩溃后在同一次运行中重启该身份还未提供完整 WAL 恢复。节点检测到已有提交日志会拒绝启动，防止丢失投票状态后重新投票。请停止整个集群并新建实验；暂停后恢复进程可通过状态追赶恢复。
- 状态、请求去重索引在内存中，适合有限负载的验收。检查点会清理旧共识消息，历史状态和去重信息仍随有效交易增长；不是生产存储引擎。
- 附加延迟是应用层消息模型，不模拟链路带宽、丢包或 TCP 拥塞控制。节点进程会共享本机 CPU，规模实验需要后续独立规划资源。

原运行入口和源码在部署本阶段时备份至 `legacy/pre-stage1/`，其中保留了修改前的工作区内容。旧格式配置文件现在位于 `config/`；`shard*/shardId`、`shard*/lldb_commands.txt` 位于对应分片目录，均不由新入口读取；`shard*/node.log` 是历史日志。历史 PDF 不改动。旧 `llb_start_all.sh` 仅提示使用新的启动方式。
