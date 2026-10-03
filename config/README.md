# 配置目录

- `single_shard.json`、`two_layer.json`、`three_layer.json`、`four_layer.json`：当前仿真系统可运行的示例。通过 `./start_all.sh --config config/two_layer.json` 选择。
- `accessControlList`、`networkConfig`、`shardsTopology`、`workloadProfile`、`topShardId`：上一版原型的旧格式配置，仅供查阅；当前启动器不读取。
- `shard1/`、`shard2/`、`shard5/` 留在项目根目录，保存对应旧分片的 `shardId`、LLDB 命令和历史日志。第一阶段改造前的快照可在历史标签 `stage1-pbft` 的 `legacy/pre-stage1/` 中查阅，当前工作区已移除该目录。

每轮实验解析后的配置写入 `runtime/<运行编号>/config.json`，属于运行结果。配置格式和参数见项目根目录的 `README.md`。

跨片批处理参数也统一写在配置文件的 `consensus` 中：`cross_shard_batch_size` 限制一个协调 PBFT 批次的交易数，`cross_shard_batch_wait_ms` 限制等待凑批的时间。`--batch` 只控制客户端每个请求的交易数。

流水线协议已移除，`consensus.pipeline_window` 不再是有效配置项。迁移之前的配置时请删除该字段，即使其值为 `0` 也需要删除；启动器会明确报错，不会静默忽略该字段。已有运行目录包含旧配置时，请停止旧节点并用新的配置重新启动。

完整跨片执行支持多层树和至少两个不重复的参与叶子。`three_layer.json` 包含 7 个分片（28 节点）；`four_layer.json` 是非均匀深度的四层树，包含 9 个分片（36 节点），叶子为 1、2、3、4、8：`1,2` 的 NCA 为 5，`1,3` 的 NCA 为 7，`1,8` 的 NCA 为 9。多协调者通过认证轮次封闭确定跨层顺序；这些控制槽不算业务交易。所有例子仍使用 JSON 定义拓扑，分片运行目录不放在 `config/` 内。
