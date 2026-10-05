# AHL 验证记录

日期：2026-10-05。原项目基于提交 `a5050e5892311bcc14de0554c2543da121a10489`，本轮先在隔离副本完成实现与验证，再同步至项目。

## 构建和回归

- `make -j3 all`：Arbor、Saguaro、SharPer、AHL 四个二进制构建成功，无编译警告。
- `make test`：187 项 Python 测试全部通过，网络传输和增量状态摘要 C++ 检查通过，SharPer 恢复测试的 26 个协议检查通过。
- AHL 新增 13 项工具测试和 6 项真实集群测试，均包含在上述完整回归中。
- 真实集群覆盖两方/三方跨片、不同参与集合统一根协调、四阶段实际 PBFT 证书、空闲叶子不执行、片内执行、重试/alias 去重、协调者与参与方 forward 故障恢复、原 Arbor NCA 路由提示重映射、二进制拒绝多层 AHL 配置。
- 工具检查 AHL 二进制识别、两层限制、锁与 2PC 排空、无效计数拒绝、双配置许可、双方读取同一个文件、实际输入/重放文件 SHA256 一致、共同延时或执行/共识参数不同时拒绝比较。

测试过程中修正了测试目录初始化与同 RID 已完成交易重试的断言。后者按共享 Saguaro 契约返回 `duplicate`，测试要求副本执行计数、KV/state/chain 摘要和 PBFT 槽位不增加；没有为此修改 Saguaro 协议。

## 双拓扑实跑

Arbor 使用 `config/three_layer_cross100.json`（7 片、28 节点），AHL 使用 `config/ahl_two_layer.json`（5 片、20 节点）。双方叶子为 1、2、3、4，共识/执行/片内和共同链路的实际延时一致。

```bash
python3 -B baseline/compare_mixed.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json \
  --count 1000 --rate 5000 --batch 10 --repeat 1 --seed 42 \
  --timeout 180 --drain-timeout 60 --skip-build \
  --output-dir test-results/ahl-multilayer-mixed-smoke

python3 -B baseline/compare.py --baseline ahl \
  --config config/three_layer_cross100.json \
  --baseline-config config/ahl_two_layer.json \
  --mode all --participants 1,2 --shard 1 \
  --count 200 --rate 1000 --batch 10 --repeat 1 --seed 42 \
  --timeout 180 --drain-timeout 60 --skip-build \
  --output-dir test-results/ahl-fixed-smoke
```

| 实跑用例 | Arbor | AHL | 配对验证 |
|---|---:|---:|---|
| 1000 笔跨片：90% 双片、10% 三片 | 完成 1000 | 完成 1000 | PASS |
| 200 笔片内，分片 1 | 完成 200 | 完成 200 | PASS |
| 200 笔两方跨片，分片 1/2 | 完成 200 | 完成 200 | PASS |

混合用例中，分片 1、2、3 的每个副本各执行 700 笔，分片 4 不执行。AHL 根 7 的每个副本协调并完成全部 1000 笔，根的业务执行数为零；Arbor 的根 7 完成 700 笔，另 300 笔由 NCA 5 完成。双方各片四副本摘要和执行/完成计数收敛，AHL 锁与协议队列排空，混合用例网络失败计数为零。

这三项为功能验收的单轮短负载，尚不作为稳定性能结论。正式测 TPS 应增加交易数和重复轮数，保留所有失败记录。节点数和架构差异是本次用户指定比较的一部分。

## 证据目录

本次隔离验证位于：

```text
/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/ahl-implementation-20261005/
  build.log
  regression.log
  mixed-comparison.log
  fixed-comparison.log
  staged-repo/test-results/ahl-multilayer-mixed-smoke/
  staged-repo/test-results/ahl-fixed-smoke/
```

对比目录内保存共享 unsigned workload、双方配置快照与 SHA256、客户端日志和结果、执行前后各副本状态、JSON/CSV/Markdown 汇总。复制的源代码与编译二进制在部署清单中记录 SHA256；`make clean` 不会删除 baseline 源码/文档，但会清理项目 runtime/test-results 的生成结果。
