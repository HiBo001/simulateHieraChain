# 配置目录

- `single_shard.json`、`two_layer.json`、`three_layer.json`：当前仿真系统可运行的示例。通过 `./start_all.sh --config config/two_layer.json` 选择。
- `accessControlList`、`networkConfig`、`shardsTopology`、`workloadProfile`、`topShardId`：上一版原型的旧格式配置，仅供查阅；当前启动器不读取。
- `shard1/`、`shard2/`、`shard5/` 留在项目根目录，保存对应旧分片的 `shardId`、LLDB 命令和历史日志。第一阶段改造前的完整快照保留在 `legacy/pre-stage1/`。

每轮实验解析后的配置写入 `runtime/<运行编号>/config.json`，属于运行结果。配置格式和参数见项目根目录的 `README.md`。
