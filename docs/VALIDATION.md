# 第一阶段验收记录

日期：2026-09-13。目标项目：`/Users/tanghaibo_office/Desktop/ATLAS/sourceCode/simulateHieraChain`。

目录整理说明：本页与 `validation-source-hashes.json` 记录的是当时的文件路径；示例配置现位于 `config/`，旧格式配置也集中于该目录，原始快照可在历史标签 `stage1-pbft` 的 `legacy/pre-stage1/` 中查阅，当前工作区已移除该目录。

## 验收结果

- 最终目标目录 Release 构建通过，C++17 / `-O2 -Wall -Wextra -Wpedantic`，未输出编译警告。
- 自动测试 **12 / 12 通过**，失败 0，错误 0，耗时 14.31 秒。
- 相同 C++ 源码在 AddressSanitizer + UndefinedBehaviorSanitizer 构建下 **12 / 12 通过**，耗时 17.02 秒；未发现 ASan/UBSan 报告。macOS 下未启用 LeakSanitizer，因此不把它表述为泄漏检测通过。
- 已实际运行 README 的默认一键启动、32 笔片内交易、链路探测、一键停止流程；12 个节点均已停止。
- 修改前的 21 个源码/入口/配置快照 SHA-256 核验通过，原根目录旧配置内容保持原样。
- `git diff --check` 通过。

## 已验证场景

| 场景 | 结果 |
|---|---|
| 1 分片、4 副本正常共识与执行 | 通过，状态摘要一致 |
| 单个备份节点离线 | 其余三个副本仍提交 |
| 主节点退出 | 带证明的视图切换后继续提交 |
| prepared 值跨视图保留 | 缺失恢复值的 NEW_VIEW 被拒绝，正确恢复后执行 |
| 只剩两个节点 | 即使重复发送匹配 COMMIT，也不能形成三个不同投票 |
| 重复请求、不同请求 ID 携带相同交易 | 交易只执行一次 |
| 无效签名 | 拒绝，不改变状态 |
| 检查点与暂停副本追赶 | 恢复一致状态后继续处理 |
| 7 分片、28 节点三层拓扑 | 叶子执行和三个协调者排序均通过 |
| 分片对延迟、默认值、反向及片内延迟 | 通过 |
| 同时发往四个远端副本 | 没有把链路等待逐个串行累加 |
| 启停、端口复用、端口冲突、独立运行隔离 | 通过 |

## 本机链路探测结果

下面是自动测试中的低负载结果。测量值包含真实 TCP、签名处理和进程调度开销，不要求等于配置值。

| 链路 | 单向附加延迟 | 预期附加 RTT | 实测平均 RTT |
|---|---:|---:|---:|
| 1:0 → 1:2 | 1 ms | 2 ms | 4.19 ms |
| 1:0 → 2:1 | 50 ms | 100 ms | 102.41 ms |
| 1:0 → 5:0 | 10 ms | 20 ms | 22.03 ms |
| 2:0 → 5:0 | 20 ms | 40 ms | 43.20 ms |
| 2:1 → 1:0 | 50 ms | 100 ms | 103.07 ms |

另一次按 README 操作的 1:0 → 2:0 探测，单向配置 50 ms，平均 RTT 为 103.27 ms。

## 环境与证据

- 环境：macOS-15.6-arm64-arm-64bit，arm64。
- 编译器：Apple Clang 17；OpenSSL 3 来自本机 Homebrew。
- 测试汇总：[summary.json](../test-results/20260913-212031-160051ba/summary.json)。
- 完整测试输出：[test-output.log](../test-results/20260913-212031-160051ba/test-output.log)。
- 每个场景的配置、客户端结果及节点日志均保留在该测试目录。
- C++ Sanitizer 测试汇总：[sanitizer-summary.json](sanitizer-summary.json)。
- 对应文件版本：[validation-source-hashes.json](validation-source-hashes.json)。

本轮已在 macOS arm64 实测；Linux 提供构建方式，但本轮没有 Linux 执行环境。以上是第一阶段功能验收，不是论文性能实验，也不意味着穷尽所有 Byzantine 故障情形。跨片完整执行、SharPer、重分片、动态扩缩容和崩溃后原运行恢复不在本轮交付范围内。
