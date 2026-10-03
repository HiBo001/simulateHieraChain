# 第二阶段 B：二层跨片执行与直接完成确认

> 2026-10-03 后续多层扩展说明：本文保留第二阶段 B 的两层、两叶业务协议实例。当前完整执行已支持多层树及至少两个参与叶子，新增认证轮次、祖先前沿和投影水位见 [MULTILAYER_DESIGN.md](MULTILAYER_DESIGN.md)。单协调者两叶正常业务仍为 3 次分片 PBFT；多协调者还包含真实轮次封闭 PBFT，不能直接把本阶段槽数推广到整个多层集群。本文历史结果不代替多层扩展验收。

本阶段支持一个协调分片、恰好两个直属叶子的完整跨片路径。2026-10-02 按用户更正，删除旧 `READY → 协调 DECISION PBFT → 叶子 FINALIZE PBFT → DONE` 流程。**本阶段的两层两叶实例正常一批共 3 次分片 PBFT：协调排序一次，两叶各一次；上层收齐叶子完成证明后直接结束。** 以下描述保留本阶段修订，测试结论以相应版本输出为准。

## 协议流程

1. 协调分片按参与分片集合合并完整客户端签名请求，由 `cross_shard_batch_size` 和 `cross_shard_batch_wait_ms` 控制提案大小及等待阈值。PBFT 排序生成批次证书和连续 `cst_order_index`，forward 下发 `CST_ORDER` 到两叶全部四副本。根只记录确定性订单元数据，不对执行完成结果再次共识。
2. 两叶分别验证根证书，由各自唯一主节点提出一次本片 PBFT，将订单纳入本片连续顺序。PBFT commit 后在临时上下文保存批初唯一 key 的读快照、逐笔输入、Fibonacci 结果和重复标记；**依赖不足时 KV、applied、state digest 均不提前变化。** 后续本片业务不越过当前槽。
3. 每个叶副本将 `CST_PREPARED` 交本片 forward；收齐三个不同副本的匹配签名后，forward 形成 `CST_PREPARED_QC`，向本片副本备份并发远片。record 仅存一份，签名票绑定其摘要。两叶依赖记录按 leaf ID 排序形成执行上下文摘要，不把可变三签子集放进业务身份。
4. 本片与远片依赖 QC 均齐全且验证通过后，叶子在**同一已共识槽**内按交易顺序计算 working state，并只把本片结果写入正式 KV。本地新值 = 本地声明值 + 对方旧值 + Fibonacci 结果（`uint64_t` 模 2^64）；同 key 后续交易读取前一笔 working state 的新值。此时一次性更新去重、applied、摘要和日志，无第二轮 FINALIZE。
5. 每叶副本生成绑定 batch key、order digest、execution digest、result digest 的 `CST_ACK`，交本片 forward。forward 收齐三签形成 `CST_ACK_QC` 回根。根验证、按参与叶 ID 去重，收齐全部参与叶子的有效完成证据后直接回复 `executed` 并释放在途窗口；客户端收到两个不同根副本的一致认证回复才计入 TPS 和秒延时。

不存在上层提交决定、下层最终化共识或 DONE 业务/传输回执。Prepared 与 ACK 三签聚合不是新增 PBFT，没有新的 PREPREPARE/PREPARE/COMMIT 轮次。

## 重试与状态追赶

节点使用带长度前缀的持久 TCP 长连接；每个分片对单独配置附加单向延迟。根 ORDER/完成回复经过根↔叶链路，双方依赖经过叶↔叶链路。

正常完成 QC 每个 forward view 主动发送一次后，不以无限 ACK 定时广播等待 DONE。根未完成 ORDER 重发触发叶子完成 QC 补发；根片内部结果查询补取其他根副本保留的 QC 和 ORDER 证书。叶子本片完成票/QC 尚未凑齐时仍每 2 秒重试，当前执行槽依赖未齐时也约每 2 秒重发准备票/QC并查询。快叶完成后仍保留历史依赖记录/QC 或 witness，由当前 forward 回应 `CST_DEPENDENCY_QUERY`/`CST_DEPENDENCY_PROOFS`，供慢叶或接替节点补取。

forward 仍与 PBFT primary 绑定，初始 node0，视图切换后一起接替；跨片源只有 forward，目标四副本各自验证。若 forward 追赶后缺少旧依赖档案，可片内广播 QUERY；备份只回复片内请求，forward 取得证据后再转发对片。临时准备记录不进入共识 state；提交日志保存 `execution_witness={batch_key, proofs}`，proofs 按 leaf ID 排序并保留三签。SYNC 检查点后缀带按叶子本地 seq 对应的 witness，落后副本按原槽重演。

已 commit 后等依赖不单独计入 PBFT 超时，但当前 view 同槽已签投票仍限频重发到执行完成，以帮助尚未拿到提交证书的同片副本。换 view 时保留原完整提交证明，并只导入与恢复值一致的原证书；已有提交证明的原槽直接继续准备/等待，不要求再次签票。

SYNC 也可分享已 committed 尚未 applied 的证书；接收者未取得有效 witness 时仅进入同槽准备/等待，seq、KV 和执行完成日志仍不前移。NEW_VIEW 安装后，仍持有此类证书的副本分享一次 SYNC 证明，补足所选三个 VIEW_CHANGE 可能遗漏的唯一完整 commit 证据；不新增投票或执行完成声明。

根 `cst_orders` 记录 `{order_digest, order_index, requests, duplicates}`，完成证明和客户端完成结果是易失缓存；追赶后通过片内 `CST_RESULT_QUERY`/`CST_RESULT_PROOFS` 取得原 ORDER 证书和两叶 ACK QC，再恢复结果，不新增根共识。SYNC 的 `execution_witnesses` 为 `[{seq,witness}, ...]`。证据档案仍依赖存活副本保留，不构成持久化 WAL。

根按数值 order index 补取未恢复结果，先恢复原批再恢复引用旧项的后续混合批。叶子的待补完成 QC 用活动集合维护，健康根全部订单完成时不再扫描历史补取；历史证据仍保留供故障恢复。

## 手动验收

在项目根目录中停止旧集群、编译并启动新实验：

```bash
make
./start_all.sh --config config/two_layer.json
python3 scripts/cluster.py load --participants 1,2 --count 40 --rate 100 --batch 8 --timeout 30
sleep 1
python3 scripts/cluster.py status
./stop_all.sh
```

客户端应显示 `completed=40 ordered_only=0 requests=5/5`，并显示完成 TPS 与秒延时。协调四副本最终 `ordered_only=40`、`executed=0`、`completed_cst=40`；两叶各自 `leaf_ordered_cst=40`、`executed=40`、`staged_cst=0`。同片四副本 state/kv digest 收敛。不能把两个叶子的 executed 相加算成 80 笔业务完成。

对于一个全新跨片批次，查看各节点 `commits.jsonl`：根增加一个 ORDER 槽，两叶各增加一个已执行槽；不应有 `cst_decisions`/`cst_finalizations` 提案。叶子提交记录带执行 witness；根收到完成 QC 不应新增日志槽。

自动验收：

```bash
python3 tests/test_stage2a.py
python3 tests/test_stage2b.py
python3 -B tests/test_engineering.py
make test
```

本轮需要验证 3 次 PBFT 槽数、同 key 顺序、远端 read 影响本地 write、错误订单/执行摘要 ACK 拒绝、参与片依赖缺失时等待、单副本离线、forward 接替、依赖查询和检查点后 witness 追赶。旧流程的通过结果不能代替新流程验收。

请求和交易去重仍保持：原 request ID 重试等待或返回原结果；完成交易换请求 ID 返回 `duplicate`，不另开共识；混合新旧交易保持客户端签名内容，旧项跳过执行；同交易 ID 配不同内容拒绝。根追赶后的混合批若缺旧项原完成缓存，先恢复其原证据，再复制原结果并标记 duplicate。工程说明见 [ENGINEERING_REVIEW.md](ENGINEERING_REVIEW.md)，详细设计见 [CURRENT_IMPLEMENTATION_DESIGN.md](CURRENT_IMPLEMENTATION_DESIGN.md)。

## 本阶段范围及后续仍保留的边界

- 本阶段当时的完整执行限定根加双叶；该限制已由后续多层扩展解除，当前多层及更多参与叶子的跨片请求走完整执行路径，不返回仅排序成功。
- 等依赖期间暂停整个叶子的后续业务，尚未支持按 key 并行等待、精确读写集分组或冲突图调度。
- 相关叶子全部执行完成、根收齐证据才向客户端确认，但两叶物理写入时刻可能不同；没有全局可见性屏障。
- 当前没有 abort、回滚、取消或超时释放。永久缺少远端依赖会令叶子等待；客户端超时不取消服务器业务。
- 持续发心跳却扣留数据的 forward 尚无专用遗漏检测和单独替换机制。
- 账户执行为可重复模拟程序，不是一般智能合约；同 run 崩溃重启仍缺 WAL 恢复。
- 旧文档记录的 98.20、230.40、258.34 TPS 属于旧 6 次 PBFT 路径。新路径须保持配置、count/rate/batch/seed 和机器条件重跑 benchmark，不预先承诺性能倍数。
