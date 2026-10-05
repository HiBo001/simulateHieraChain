# 配置目录

- `single_shard.json`、`two_layer.json`、`three_layer.json`、`four_layer.json`：当前仿真系统可运行的示例。通过 `./start_all.sh --config config/two_layer.json` 选择。
- `accessControlList`、`networkConfig`、`shardsTopology`、`workloadProfile`、`topShardId`：上一版原型的旧格式配置，仅供查阅；当前启动器不读取。
- `shard1/`、`shard2/`、`shard5/` 留在项目根目录，保存对应旧分片的 `shardId`、LLDB 命令和历史日志。第一阶段改造前的快照可在历史标签 `stage1-pbft` 的 `legacy/pre-stage1/` 中查阅，当前工作区已移除该目录。

每轮实验解析后的配置写入 `runtime/<运行编号>/config.json`，属于运行结果。配置格式和参数见项目根目录的 `README.md`。

跨片批处理参数也统一写在配置文件的 `consensus` 中：`cross_shard_batch_size` 限制一个协调 PBFT 批次的交易数，`cross_shard_batch_wait_ms` 限制等待凑批的时间。`--batch` 只控制客户端每个请求的交易数。

流水线协议已移除，`consensus.pipeline_window` 不再是有效配置项。迁移之前的配置时请删除该字段，即使其值为 `0` 也需要删除；启动器会明确报错，不会静默忽略该字段。已有运行目录包含旧配置时，请停止旧节点并用新的配置重新启动。

完整跨片执行支持多层树和至少两个不重复的参与叶子。`three_layer.json` 包含 7 个分片（28 节点）；`four_layer.json` 是非均匀深度的四层树，包含 9 个分片（36 节点），叶子为 1、2、3、4、8：`1,2` 的 NCA 为 5，`1,3` 的 NCA 为 7，`1,8` 的 NCA 为 9。多协调者通过认证轮次封闭确定跨层顺序；这些控制槽不算业务交易。所有例子仍使用 JSON 定义拓扑，分片运行目录不放在 `config/` 内。

## AHL 两层配置

`ahl_two_layer.json` 用于 AHL：叶子 1、2、3、4 都直接接到唯一上层 7。它对应 `three_layer_cross100.json` 的同一批叶子，共识、执行、片内延时和保留下来的链路延时相同；中间分片 5、6 及相关链路被移除。1↔7=10 ms，3↔7=30 ms；2↔7、4↔7 使用两份配置相同的默认值 20 ms。

```text
7
├── 1
├── 2
├── 3
└── 4
```

`--method ahl` 强制所有非根分片为根的直接子片、至少两个叶子。需要其他数量或 ID 时可编辑或新增该目录内的 JSON；`two_layer.json` 也能用于两叶子的 AHL。原 Arbor/Saguaro/SharPer 拓扑校验规则保持原样。

与 Arbor 多层比较时使用 `--config config/three_layer_cross100.json --baseline-config config/ahl_two_layer.json --baseline ahl`，不会静默把 Arbor 的拓扑压平；每轮运行目录仍归入 runtime/test-results，不放入 config。

## 访问局部性测试配置

`three_layer_locality.json` 保留原三层树的父关系，增加叶子 8、9，使两个 cluster 都包含三个叶子：

```text
7
├── 5
│   ├── 1
│   ├── 2
│   └── 8
└── 6
    ├── 3
    ├── 4
    └── 9
```

cluster 按叶子的直接父节点定义，故为 `{1,2,8}` 与 `{3,4,9}`。根 7 协调跨 cluster 交易；各 cluster 内的跨片交易分别由 5、6 协调。新拓扑共有 9 个分片、36 个 PBFT 副本进程，与 Euro-Par 论文实验中的两个三叶 cluster 结构一致。

`ahl_two_layer_locality.json` 保留相同六个叶子，将它们全部直接接到唯一上层 7，删除中间片 5、6；共有 7 个分片、28 个副本进程。AHL 的负载仍由上述 Arbor 参考树生成，不能根据展平后的拓扑重新分类 cluster。SharPer 使用相同的参考配置来启动节点和配置网络，但祖先片不参与跨片共识。

两份新配置均保留旧 `three_layer_cross100.json` 的完整共识、执行及既有分片对延时：片内 1 ms，默认片间 20 ms，1↔2/3↔4 为 10 ms，1↔3 为 50 ms。新增同 cluster 的 1↔8、2↔8、3↔9、4↔9 为 10 ms，叶子 8↔5、9↔6 为 5 ms。AHL 删除与 5、6 相关的链路，共同保留分片之间的实际延时仍一致。base_port 分别为 19800、20000；比较脚本启动独立运行目录并管理测试端口。

默认混合负载为 100% 跨片、90% 两方/10% 三方，其中只有 5% 跨 cluster。负载生成和四方法对比说明见 [../docs/WORKLOAD_LOCALITY.md](../docs/WORKLOAD_LOCALITY.md)。旧四叶配置保留供固定参与方测试和既有 workload 重放；在旧配置自动生成前三叶均匀 90/10 模式时应加 `--uniform`。
