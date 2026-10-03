# 配置目录

- `single_shard.json`、`two_layer.json`、`three_layer.json`：当前仿真系统可运行的示例。通过 `./start_all.sh --config config/two_layer.json` 选择。
- `accessControlList`、`networkConfig`、`shardsTopology`、`workloadProfile`、`topShardId`：上一版原型的旧格式配置，仅供查阅；当前启动器不读取。
- `shard1/`、`shard2/`、`shard5/` 留在项目根目录，保存对应旧分片的 `shardId`、LLDB 命令和历史日志。第一阶段改造前的快照可在历史标签 `stage1-pbft` 的 `legacy/pre-stage1/` 中查阅，当前工作区已移除该目录。

每轮实验解析后的配置写入 `runtime/<运行编号>/config.json`，属于运行结果。配置格式和参数见项目根目录的 `README.md`。

二层跨片批处理参数也统一写在配置文件的 `consensus` 中：`cross_shard_batch_size` 限制一个协调 PBFT 批次的交易数，`cross_shard_batch_wait_ms` 限制等待凑批的时间。`--batch` 只控制客户端每个请求的交易数。

流水线协议已移除，`consensus.pipeline_window` 不再是有效配置项。迁移之前的配置时请删除该字段，即使其值为 `0` 也需要删除；启动器会明确报错，不会静默忽略该字段。已有运行目录包含旧配置时，请停止旧节点并用新的配置重新启动。

当前完整跨片执行使用一个根、两个直接叶子的二层拓扑，参与者为这两个叶子。多层拓扑仍支持片内交易及跨片交易的最近公共祖先排序；多层跨片结果为 `ordered_only`，不计入已执行 TPS。
