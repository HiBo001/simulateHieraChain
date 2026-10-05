# Saguaro 间歇性 PBFT 恢复卡顿修复（2026-10-06）

本文记录局部性跨片负载下的一次 Saguaro 停滞排查，以及共享 PBFT 恢复代码的工程修复。此前的 Saguaro 新工作计时、恢复消息压缩、协调者冲突避让和退避说明保留在 `docs/SAGUARO_STALL_FIX.md`，本文不覆盖它们。

## 1. 观察到的停滞与证据边界

用户保留的失败运行目录为：

```text
test-results/compare-all-20261005-232957-54f65a2c/repeat-002/saguaro/
```

该轮客户端提交了 10000 笔交易、1000 个请求。`client.log` 中完成数在约 75 秒达到 6010，此后约 80、85、90、95、100 秒的进度行仍为 6010，最终由用户中断。`client.json` 保留了已确认的 601/1000 个请求；`summary.json` 记录 `FAIL` 和 `KeyboardInterrupt`。因此这是一次有节点证据的持续停滞观察，不能把中断前的部分完成 TPS 当作完整负载性能。

保存的输入是 100% 跨片交易，其中 90% 跨两片、10% 跨三片；5% 的交易跨 cluster。两个 cluster 为 `[1,2,8]` 和 `[3,4,9]`，父协调分片分别为 5、6，根为 7。Saguaro 使用参与方最近公共祖先作为 2PC 协调者。

本次排查使用同一份保存输入，其文件 SHA-256 为：

```text
04fe7c589a2525b4afacf3ac2e7f46b61f546d9ac08813f91939045ae867ff3f
```

关键原配置为：

| 配置 | 数值 |
|---|---:|
| 每个分片的 PBFT 副本数 | 4 |
| 客户端请求交易数 | 10 |
| 客户端目标发送速率 | 5000 笔/秒 |
| 客户端超时 | 180 秒 |
| `batch_size` / `batch_wait_ms` | 1000 / 10 |
| `cross_shard_batch_size` / `cross_shard_batch_wait_ms` | 100 / 600 |
| `view_timeout_ms` | 2000 |
| `checkpoint_batches` | 16 |
| `fib_iterations` | 1 |

修复前，本机用该输入连续三轮均完整通过。这说明该故障具有间歇性；本次没有在本机精准重复出用户那一次停滞。下面的故障位置来自用户保留的失败日志，代码问题则由源码检查和独立回归用例确认。不能把“本机旧版三轮成功”解释为旧版没有问题，也不能把修复后有限轮数成功解释为已排除全部故障。

## 2. 直接阻塞位置

四个协调分片 5 的副本最后都停在 `applied_batches=75`、`stable_seq=64`，已排序 610 笔、已完成 600 笔，仍有 399 个待处理请求和一个在途跨片批次。

各节点的 `commits.jsonl` 显示，第 75 个本地 PBFT 槽的值是 `INIT`，业务批次 ID 为 `5:75`。叶子分片 1 的最后一个已应用槽为 97、分片 8 为 105，两者都是该批的 `PREPARE`，最后各持有 10 个锁，等待 2PC 后续决定。协调分片 5 的事件日志出现了第 76 槽的 PREPREPARE 和部分 prepared 事件，却没有该槽的 `committed_local` 或应用记录。

因此，直接阻塞位置是**协调分片 5 的第 76 个 PBFT 槽未完成共识**。参与方保留锁、等待 DECISION 是这个阻塞的后续表现，不足以单独证明发生了 2PC 锁环。事件日志没有保存第 76 槽的完整提案值，本文不推断它具体是 INIT、DECIDE 或其他恢复值。

### 2.1 达到 prepared 后很快换主

以下行号相对于失败目录中的对应文件；`steady_ms` 是同一主机的单调时钟毫秒值，差值再换算为秒。

| 事件 | `run/shard5/` 下的文件与行号 | `steady_ms` |
|---|---|---:|
| node0，view 10，接受槽 76 | `node0/events.jsonl:203` | 569914030.440000 |
| node0，槽 76 首次 prepared | `node0/events.jsonl:205` | 569915699.922708 |
| node0，开始切换到 view 11 | `node0/events.jsonl:206` | 569916197.367041 |
| node3，view 10，接受槽 76 | `node3/events.jsonl:514` | 569913901.233500 |
| node3，槽 76 首次 prepared | `node3/events.jsonl:515` | 569914710.136333 |
| node3，开始切换到 view 11 | `node3/events.jsonl:517` | 569915952.957916 |

node0 从首次 prepared 到换主只有 **0.497444 秒**，node3 只有 **1.242822 秒**，均小于配置的 2 秒。旧代码接受首次 PREPREPARE 时更新 `lastProgress`，首次达到 prepared 时却没有更新；后续超时仍从前一个阶段的进展起算，留给 COMMIT 阶段的时间可能已经很短。

这不表示 prepared 后整个 PBFT 必须完成，也不表示每收到一张票都应延长超时。需要计入的是从未 prepared 到具有真实准备证明的**首次状态转换**。

### 2.2 同一新 view 安装时间相差很大

view 10 的新主为 node2，各节点的安装时间如下：

| 节点 | 文件与行号 | 安装 view 10 的 `steady_ms` |
|---|---|---:|
| node2，新主 | `node2/events.jsonl:486` | 569911248.062166 |
| node3 | `node3/events.jsonl:513` | 569912745.042250 |
| node0 | `node0/events.jsonl:202` | 569912775.885708 |
| node1 | `node1/events.jsonl:404` | 569920637.702875 |

最快与最慢节点的安装时间相差 **9.389641 秒**。node2 在安装后 **2.708136 秒**就开始切换到 view 11（`node2/events.jsonl:492`）；此时 node1 还未安装 view 10。各副本反复错过共同处理同一个 view 的时间窗口，最终第 76 槽没有提交。

保存的配置关闭了网络 trace，没有逐条 VC/NV 接收或验证耗时记录。因此，这些日志能够证明 view 安装与阶段推进严重不同步，不能直接量化重复 VIEW_CHANGE/NEW_VIEW 的比例，也不能断言最初拖延完全由某一种验签、CPU 调度或网络事件引起。

## 3. 两项工程问题

### 3.1 恢复消息重放会重复做大规模验证、构造和发送

旧处理路径先完整验证每个 VIEW_CHANGE，再按 `(view, sender)` 插入已收集证据。即使消息已经收到过，`emplace` 未插入新证据，仍会调用 `maybeNewView()`。

VIEW_CHANGE 包含稳定状态和 prepared/committed 证明；NEW_VIEW 又包含三个 VIEW_CHANGE 和恢复提案。重复验证不是只验证一个很小的签名，还可能重新计算检查点状态摘要、验证其中的各份证明。

旧 `maybeNewView()` 在满足法定人数时，每次都会重新选择证明、构造恢复 PREPREPARE、签名并广播 NEW_VIEW。新主的自身消息也通过本地收件箱处理，构造 NEW_VIEW 与安装它之间存在排队时间；这段时间里重复 VC 可以反复触发构造。后来到达的另一份合法 VC 还可能改变重新构造时选取的三个证据。一个已经签发的目标 view 应保留同一个恢复候选，通过重传完成传播。

这两个路径会增加恢复期间的工作和流量。代码检查确认了它们存在，但原失败日志没有记录逐条包类型，不能据此给出该轮的重复率或各项工作的 CPU 占比。

### 3.2 首次 prepared 没有开始新的阶段等待期限

首次达到 prepared 是准备阶段取得真实进展；随后节点发送 COMMIT，等待提交证明。旧共享 `advance()` 没有在这个转换点更新进度，容易在准备阶段已经消耗大部分期限后立即换主。上面的 0.497444 秒与 1.242822 秒间隔显示了该计时遗漏在用户失败运行中的实际表现。

此前 Saguaro 的 `sagActivateWork()` 修复解决的是空闲或等待远端之后出现**新工作**时的计时。本次补上的是**同一个 PBFT 槽首次进入 prepared**时的阶段进展，两者保留并共同生效。

## 4. 实现方式与安全边界

修改集中在共享 `source/main.cpp`。没有修改 `baseline/saguaro/protocol.inc` 的 2PC 决定或锁语义。

### 4.1 完整消息指纹复用

只有一份 VIEW_CHANGE 通过原有完整验证，而且首次成功插入 `viewChanges[view][sender]` 后，才保存它的完整 `e.dump()` 的 SHA-256 指纹。

下次处理消息时，只有同一分片、同一个 view、同一签名者的原证据仍然存在，且**整个信封的序列化指纹相同**，才可以直接丢弃这份已认证的重放。指纹包括 body、signature 和额外信封字段。

这里不能仅使用 JSON 结构相等：JSON 库可能把整数 `0` 和浮点数 `0.0` 判为相等，但它们的序列化正文不同，签名也绑定不同正文。指纹让这种数值表示修改、签名修改和额外字段修改都回到正常验证路径。

不同信封仍执行成员身份/签名验证和完整 `validVC()`；无效证明不进入缓存，同一签名者后来提供的不同合法证明也不替换原先已经收集的第一份证据。缓存不增加票数、不影响恢复值选择、不更新进度期限。

指纹元数据最多覆盖 64 个活跃 view，每个 view 最多四个已认证成员。开始换主、安装新 view 和插入新证据时进行修剪；原 `viewChanges` 证据保持原有生命周期。没有缓存或缓存被修剪时，消息退回正常验证，而不是放宽验证。

已经落后于已安装 view 的本片 VC/NV 可以在昂贵验证前丢弃；低于当前目标 view 的 NEW_VIEW 也提前丢弃。仅高于已安装 view、但低于当前目标 view 的 VIEW_CHANGE，仍保留原有认证和证据收集规则，不因这次修复改变换主选择行为。

### 4.2 每个目标 view 使用不可变 NEW_VIEW

本节点担任目标 view 主节点、收齐三份不同成员的有效 VIEW_CHANGE，并包含自己的证据后，按原恢复规则构造和签发一个 NEW_VIEW。该 view 的候选已存在时，不再重新构造；新 VC 到达不会改写这个已签名候选。

专用重传逻辑每至少 250 ms 重传同一个候选，覆盖两种情况：

- NEW_VIEW 已构造，但主节点尚未从自身收件箱安装它。
- 主节点已安装 NEW_VIEW，需要向落后的其他副本继续传播。

目标 view 提升后立即停止重传旧候选。构造及重传本身都不刷新 `lastProgress`。接收方仍按照原规则验证三份不同成员的证明、检查点、canonical 恢复值以及提案签名，才安装新 view。

### 4.3 仅首次 prepared 刷新队首进度

共享 `advance()` 只有同时满足以下条件才更新 `lastProgress`：

1. 当前尚未进入换主。
2. 提案属于已安装的当前 view。
3. 槽号为 `applied + 1`，即实际等待执行的队首。
4. 本槽首次从未 prepared 转为 prepared，并具有原规则要求的有效准备证明。

重复 PREPARE、重复 `advance()`、非队首未来槽、旧 view 或已开始换主的槽，都不能通过这个分支延长队首等待期限。更新计时不等于提交：节点仍须收齐原有提交证明并按顺序应用。

## 5. 保留的协议与配置

本次不增加共识阶段，不改变正常 2PC 的 INIT、参与方 PREPARE/VOTE、协调者 DECIDE、参与方 FINISH/ACK 流程。协调者仍为 NCA，已有锁、ABORT 与完整请求重试逻辑保持不变。

每片仍为四副本 PBFT；普通 prepared 仍要求两个不同备份的有效 PREPARE，提交仍要求三份有效 COMMIT；NEW_VIEW 仍要求三份不同成员的有效 VIEW_CHANGE，并包含新主自己的证据。缓存和重传不替代这些门槛。

原输入、组批大小、组批等待、换主超时、检查点间隔、执行负载及网络延时保持原值。Arbor 的参与方组批、未来 PREPREPARE 缓存，SharPer 的原子本地预约与恢复证明修复，以及此前客户端 f+1 初发、去重、持久 TCP、forward 聚合和状态摘要优化均保留。共享 PBFT 修改会由四个方法重新编译使用，所以还需要验证 Arbor、SharPer 和 AHL 没有回归。

这次修复不补充完整 SharPer Algorithm 4 的跨片取消与重新提案机制，也不声称覆盖所有 Byzantine 故障路径。

## 6. 回归与实际复测

在项目根目录可执行：

```bash
make
make test-pbft-recovery
make test
```

新增纯协议回归使用真实独立 Ed25519 密钥，不启动网络服务。覆盖首次 VC 验证、完全相同重放、正文/数值表示/签名/额外字段篡改、无效检查点与重复票、不同合法证据不替换原票、法定人数不足、不可变 NEW_VIEW、安装前后重传、过时候选停止重传，以及首次 prepared 和重复/未来/旧 view 准备证明的计时边界。

实际复测应串行运行独立空状态集群，复用上述原始 unsigned 输入，不重新生成另一份随机负载。通过判据包括客户端完整确认 10000 笔、1000 个请求，各副本预期计数正确且状态/链/KV 摘要收敛，相关队列和 Saguaro 锁排空，以及没有超限、解析、队列或收件箱丢弃错误。保留每轮输入指纹、节点状态、事件日志和汇总，不能只检查客户端 TPS。

网络连接/写失败的原始计数要单独保留核对：客户端取得两个匹配回复后会关闭监听，迟到回复可能产生这些计数；不能把它们与消息超限、解析错误或收件箱丢弃混为一项。

新增节点状态计数用于后续定位：

| 状态字段 | 含义 |
|---|---|
| `pbft_vc_validations` | 调用完整 `validVC()` 的次数，包括失败调用及恢复时的再次验证；不是不同有效 VC 的数量。 |
| `pbft_vc_exact_replays` | 命中完整已认证信封指纹、提前丢弃的 VC 重放次数。 |
| `pbft_new_view_builds` | 本节点实际构造 NEW_VIEW 候选的次数。 |
| `pbft_new_view_retries` | 专用逻辑重传已有 NEW_VIEW 候选的次数。 |

这些计数没有回填到旧失败运行，不能拿新版本计数推算旧日志缺失的重复率。

### 验收记录

下表日志和汇总保存在本次验证目录：

```text
/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/saguaro-stall-review-20261005
```

| 验证 | 最终结果 | 证据目录或日志 |
|---|---|---|
| 四方法重新编译 | PASS | `build-and-protocol-tests.log` |
| 新增纯 PBFT 恢复回归 | PASS | `build-and-protocol-tests.log` |
| 完整 `make test` | PASS；21 个 Python 测试套件、223 个用例，以及原有 C++ 网络和摘要测试 | `full-tests.log` |
| Saguaro，相同 10000 笔输入，8 轮 | 8/8 PASS，每轮 10000/10000 完成，副本收敛、锁与队列排空 | `reproduction-saguaro-fixed/summary.json` |
| Saguaro，相同输入，压测中退出协调片主节点 | PASS；分片 5/node0 在 stable_seq=16、view=0、仍有 4 个活动批次时退出，幸存者安装 view=1；10000/10000 完成，各片至少三副本收敛、业务队列与锁排空 | `fault-saguaro-after/summary.json` |
| Arbor，相同输入补充回归 | 1/1 PASS，10000/10000 完成并收敛 | `reproduction-arbor-fixed-shared/summary.json` |
| SharPer，相同输入补充回归 | 1/1 PASS，10000/10000 完成并收敛 | `reproduction-sharper-fixed-shared/summary.json` |
| AHL，相同输入，保留既定两层拓扑 | 1/1 PASS，10000/10000 完成并收敛 | `reproduction-ahl-fixed-shared/summary.json` |

**结论：**本次完整回归、Saguaro 8 轮相同输入、单协调主节点故障恢复，以及 Arbor、SharPer、AHL 各一轮相同输入均通过。Saguaro 的 8 轮普通测试均保持 view=0；换主路径另外由纯协议单测、完整集成测试和单节点故障验证覆盖。故障测试只要求存活副本的业务队列及锁排空，不要求对已退出节点的网络重连队列为空；额外核对 `sag_active_access_keys`、`sag_retry_waiting_requests` 也均为 0。旧版三轮成功与修复后若干轮成功不是交替、配对的性能实验；本次不能据此声称 TPS 提升，也不能计算修复前后加速比例。用户原失败运行、修复前快照和各轮验证结果应继续保留。

## 7. 用户复测命令

项目根目录执行以下命令，会生成相同局部性规则的负载，四个方法逐个复用，每轮启动独立集群并核对排空和副本收敛：

```bash
make -j2
make test-pbft-recovery
python3 baseline/compare_all.py --count 10000 --rate 5000 --batch 10 --timeout 180 --repeat 3
```

若要复播这次保存的逐笔输入而不重新生成负载：

```bash
python3 scripts/benchmark_mixed.py --method saguaro \
  --config config/three_layer_locality.json \
  --workload "/Users/tanghaibo_office/Documents/ChatGPT/Arbor系统搭建/saguaro-stall-review-20261005/shared-workload.json" \
  --drain-timeout 60
```

先停止自己之前手工启动的集群，避免并行压测干扰结果。原故障日志、修复前快照及本次验证结果均保留。
