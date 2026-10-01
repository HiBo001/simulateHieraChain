# 第二阶段 A：二层跨片排序下发

本阶段只扩展固定二层拓扑：一个根协调分片和恰好两个叶子分片。协调分片的 PBFT 顺序被带证明地传播到参与叶子，并由每个叶子的四个副本再次通过本片 PBFT 纳入本地顺序。它**尚未执行或提交跨片交易**；片内交易的现有执行路径保持不变。

## 交易与证书

`load --participants 1,2` 现在为每笔交易生成 `accesses`，其中每个参与叶子都有一个明确的本地 key 和 value。客户端签名覆盖整笔交易；协调分片校验 `participants` 与读写访问所属分片一致，并校验其 NCA 正是该协调分片。

协调分片提交一个请求批次后，从 PBFT 的 PREPREPARE、两个不同备份节点的 PREPARE 和至少三个不同副本的 COMMIT 构造证书。各协调副本把证书放在签名的 `CST_ORDER` 消息中，通过配置的分片间延时发往每个参与叶子的四个副本。所有协调副本都可转发，叶子按 `协调分片ID:序号` 去重，并按协调分片序号连续进入本片 PBFT；若先收到后续序号，会等待缺失批次。

叶子收到证书后，独立验证客户端签名、交易参与集合、NCA、提案摘要、主节点身份，以及 PREPARE/COMMIT 签名与不同副本法定人数。证书无效时不会进入待排序池。叶子的主节点把有效证书放进本片 PBFT 提案；其他副本在接受提案前重复验证。叶子 PBFT 提交后只更新 `leaf_ordered_cst_transactions` 与有序批次记录，**不更新账户状态或 `executed_transactions`**。

包含三个以上叶子的二层拓扑以及三层拓扑仍保留第一阶段的协调者排序测试。来自不同协调分片的跨片批次可能产生顺序冲突；这需要后续多层排序协议处理，本阶段不向三层拓扑的叶子下发这些批次。

## 验收

在新的二层集群中执行；若当前已有集群，先运行 `./stop_all.sh`：

```bash
make
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py load --participants 1,2 --count 40 --rate 100 --batch 8 --timeout 30
python3 scripts/cluster.py status
```

客户端当前报告 `completed=0 ordered_only=40 completed_tps=0`，因为它只收到协调者排序回复，不能把该结果当作跨片交易完成。稍等状态文件刷新后，协调分片四副本的 `ordered_only=40`，叶子 1、2 四副本各自的 `leaf_ordered_cst=40`，两个叶子的 `executed=0`。同一分片四副本的状态摘要应一致。每个叶子的 `commits.jsonl` 中可见嵌入协调分片证书的本地 PBFT 提交。

自动测试：`python3 tests/test_stage2a.py`。它启动 12 个节点，检查两片叶子的排序收敛、真实提交证书以及篡改 COMMIT 签名、缺少跨片访问集后的拒绝行为。下一次交付将在这些已排序批次上实现叶子执行、依赖交换和最终完成确认。
