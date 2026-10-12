# Baseline 原运行方式恢复

本轮按用户要求恢复 baseline 到新增 Arbor 窗口机制之前的配置及运行方式，保留 Arbor 的窗口实现。原始参考提交为 `bd50d32c9d6d212ab8d7c722742180bbf44c1d1b`。

## 保持原样的内容

`baseline/` 的 17 个原有文件及 `config/` 的 14 个原有配置文件均与改动前快照字节一致。Saguaro、SharPer、AHL 的共识、2PC、重试、去重、组批选择、网络与执行实现不变。

原混合负载比较脚本继续默认每请求 10 笔；原单参与组 compare.py 的默认 8 笔也保持不变。`three_layer_locality.json` 与 `ahl_two_layer_locality.json` 的 PBFT 上限仍为 1000、跨片上限 100、等待 10/600 ms，其他配置使用各自原值。例如 two_layer.json 的跨片上限原本为 64，仍保持 64，未统一改成 100。

撤回上一轮“每请求 100 笔、固定实际业务批 100 笔”的实验方式及其 custom_allocation 低负载联合配额。当前继续由各方法原来的组批逻辑决定实际填充量，不修改 baseline 去迎合 Arbor；所有方法仍可重放同一原始负载文件。

## 新参数只进入 Arbor

共享配置校验不再给所有方法自动插入 `ordering_window_blocks=0` 或 `empty_block_interval_ms=50`。未显式配置窗口的输入，规范化结果和配置指纹与旧版一致。

启动 baseline 时，方法配置检查会在独立副本上剔除两个 Arbor 专有字段，不会污染 Arbor 配置。新参数实验脚本仅给 Arbor 配置写入 m/空块参数，baseline 快照保持原配置，不含这两个新字段。Arbor 原有 `ordering_window_blocks=0` 关闭窗口和 m1/200 等显式配置仍然可用。

## 原混合负载比较

```bash
python3 -B baseline/compare_all.py \
  --count 10000 --rate 5000 --batch 10 \
  --repeat 3 --timeout 180 --drain-timeout 60
```

以上使用原始配置和各方法原有运行方式，Arbor 不启用新增窗口。

比较 Arbor m1/200 与保持原方式的 baseline：

```bash
python3 -B scripts/benchmark_ordering_windows.py \
  --parameter-pairs 1:200 \
  --rates 5000 --count 10000 --batch 10 \
  --repeat 3 --seed 1042 --timeout 180 --drain-timeout 30 \
  --baseline-methods saguaro,sharper,ahl
```

仍使用 100% 跨片、90% 两方/10% 三方、5% 跨 cluster 的原始局部性生成规则；没有新联合配额。各方法使用相同请求、参与方、key、value、请求顺序及发送速率。恢复没有重做性能扫描；之前的 100 笔整批结果不用于代表当前性能。

## 验证

```bash
python3 -B tests/test_baseline_compatibility.py
python3 -B tests/test_ordering_window_benchmark.py
```

100 项工具及兼容性测试全部通过。使用修改前的原始 baseline 二进制，Saguaro、SharPer、AHL 各自按原 two_layer.json 配置重放同一份 40 笔负载（每请求 10 笔），均全部完成并收敛；实际 runtime 配置、指纹、输入哈希符合旧版，不含窗口字段。Arbor 三层 m=1/2/4 各 24 笔真实 PBFT 回归也通过。此次小规模回放只验证功能，不能作为新的性能对比。

核对原始配置/源码 SHA256、旧版规范化配置及配置指纹、baseline 参数隔离、原工作负载生成结果与各方法回放兼容性。详细验证记录保存在本次工作区的 `baseline-restore-20261012/`；旧 100 笔整批实验保留历史证据，不作为当前运行指令。
