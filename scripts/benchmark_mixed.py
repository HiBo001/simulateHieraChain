#!/usr/bin/env python3
"""Replay one unsigned mixed cross-shard workload in an isolated real cluster."""
import argparse
import copy
import datetime
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time
import uuid

import cluster as c
from benchmark import config_fingerprint, free_ports, METRICS, COUNTERS


def expectations(cfg, workload, method="arbor"):
    """Derive every leaf and coordinator's exact count, including mixed LCAs."""
    parent, leaves = c.topology(cfg)
    if not isinstance(workload, dict):
        raise ValueError("负载必须为 JSON 对象")
    rate = workload.get("rate")
    timeout = workload.get("timeout_s")
    if type(rate) not in (int, float) or not math.isfinite(rate) or rate <= 0:
        raise ValueError("负载 rate 必须为正数")
    if type(timeout) not in (int, float) or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("负载 timeout_s 必须为正数")
    requests = workload.get("requests")
    if not isinstance(requests, list) or not requests:
        raise ValueError("负载 requests 必须为非空数组")
    executed, ordered = {sid: 0 for sid in parent}, {sid: 0 for sid in parent}
    groups, sizes, seen_requests, seen_transactions = {}, {}, set(), set()
    for request in requests:
        if not isinstance(request, dict):
            raise ValueError("请求必须为 JSON 对象")
        rid, txs = request.get("id"), request.get("txs")
        if not isinstance(rid, str) or not rid or rid in seen_requests:
            raise ValueError("请求 ID 缺失或重复")
        seen_requests.add(rid)
        if not isinstance(txs, list) or not 1 <= len(txs) <= cfg["consensus"]["cross_shard_batch_size"]:
            raise ValueError("每个请求的交易数必须在 cross_shard_batch_size 范围内")
        group = None
        for tx in txs:
            if not isinstance(tx, dict):
                raise ValueError("交易必须为 JSON 对象")
            tid, ps = tx.get("id"), tx.get("participants")
            if not isinstance(tid, str) or not tid or tid in seen_transactions:
                raise ValueError("交易 ID 缺失或重复")
            if (not isinstance(ps, list) or any(type(sid) is not int for sid in ps)
                    or len(ps) < 2 or len(set(ps)) != len(ps) or ps != sorted(ps)
                    or not set(ps).issubset(leaves)):
                raise ValueError("负载必须全部为跨片交易，participants 为升序且互异的叶子分片")
            if group is not None and tuple(ps) != group:
                raise ValueError("同一请求内的交易必须具有相同的参与分片")
            group = tuple(ps)
            if request.get("target") != c.lca(cfg, ps):
                raise ValueError("请求 target 必须是参与分片的最近公共祖先")
            accesses = tx.get("accesses")
            if (not isinstance(accesses, list) or any(not isinstance(a, dict) for a in accesses)
                    or any(type(a.get("shard")) is not int for a in accesses)
                    or sorted(a.get("shard", -1) for a in accesses) != ps):
                raise ValueError("交易 accesses 必须覆盖每个参与分片一次")
            seen_transactions.add(tid)
            for sid in ps:
                executed[sid] += 1
            if method != "sharper":
                ordered[request["target"]] += 1
            label = ",".join(map(str, ps))
            groups[label] = groups.get(label, 0) + 1
            sizes[str(len(ps))] = sizes.get(str(len(ps)), 0) + 1
    return {"transactions": len(seen_transactions), "requests": len(requests),
            "executed": executed, "ordered": ordered, "groups": groups,
            "participants_per_transaction": sizes}


def settlement_errors(cfg, expected, rows, method):
    parent, leaves = c.topology(cfg)
    wanted_nodes = {(sid, replica) for sid in parent for replica in range(4)}
    if not isinstance(rows, list) or len(rows) != len(wanted_nodes) or any(not isinstance(r, dict) for r in rows):
        return ["节点状态不完整"]
    if any(type(r.get("shard")) is not int or type(r.get("replica")) is not int for r in rows):
        return ["节点身份字段无效"]
    if {(r["shard"], r["replica"]) for r in rows} != wanted_nodes:
        return ["节点状态不完整"]
    required_counts = ("applied_batches", "executed_transactions", "ordered_cst_transactions",
                       "completed_cst_transactions", "pending_requests", "pending_cst_batches",
                       "staged_cst_batches", "dedup_waiting_requests", "network_queue", "network_buffered_bytes")
    if method == "saguaro":
        required_counts += ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions",
                            "sag_pending_completions", "sag_held_locks")
    elif method == "arbor":
        required_counts += ("rounds_requested", "cst_round")
    elif method == "sharper":
        required_counts += ("sharper_active_batches", "sharper_pending_batches", "sharper_waiting_execution")
    else:
        return ["未知测试方法"]
    for row in rows:
        if any(type(row.get(field)) is not int or row[field] < 0 for field in required_counts):
            return ["节点计数字段缺失或无效"]
        if any(type(row.get(field)) is not bool for field in ("ready", "alive", "changing_view")):
            return ["节点就绪状态字段缺失或无效"]
        if any(not isinstance(row.get(field), str) or not row[field]
               for field in ("state_digest", "kv_digest", "chain_digest")):
            return ["节点摘要字段缺失或无效"]
        if "method" in row and row["method"] != method:
            return ["节点运行方法与测试方法不符"]
    problems = []
    for sid in parent:
        peers = [r for r in rows if r["shard"] == sid]
        if any(not r.get("alive") or not r.get("ready") or r.get("changing_view") for r in peers):
            problems.append(f"分片 {sid} 有节点未就绪")
        for field in ("state_digest", "kv_digest", "chain_digest", "applied_batches"):
            if any(field not in r for r in peers) or len({r.get(field) for r in peers}) != 1:
                problems.append(f"分片 {sid} 的 {field} 未收敛")
        if any(r.get("executed_transactions") != expected["executed"][sid] for r in peers):
            problems.append(f"分片 {sid} 执行计数不符")
        ordered = 0 if method == "sharper" else expected["ordered"][sid]
        if any(r.get("ordered_cst_transactions") != ordered for r in peers):
            problems.append(f"分片 {sid} 跨片排序计数不符")
        if sid not in leaves or method == "sharper":
            if any(r.get("completed_cst_transactions") != ordered for r in peers):
                problems.append(f"协调片 {sid} 完成计数不符")
            if method == "arbor" and any(r.get("rounds_requested", 0) > r.get("cst_round", 0) for r in peers):
                problems.append(f"协调片 {sid} 仍有未封闭轮次")
        queue_fields = ("pending_requests", "pending_cst_batches", "staged_cst_batches",
                        "dedup_waiting_requests", "network_queue", "network_buffered_bytes")
        if any(r.get(field, 0) for r in peers for field in queue_fields):
            problems.append(f"分片 {sid} 尚有待处理交易或网络消息")
        if method == "saguaro":
            fields = ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions",
                      "sag_pending_completions", "sag_held_locks")
            if any(field not in r or r[field] for r in peers for field in fields):
                problems.append(f"分片 {sid} 仍有未结束的 2PC 或未释放的锁")
        elif method == "sharper":
            fields = ("sharper_active_batches", "sharper_pending_batches", "sharper_waiting_execution")
            if any(r[field] for r in peers for field in fields):
                problems.append(f"分片 {sid} 仍有未结束的 SharPer 共识或依赖执行")
    return problems


def wait_settled(cfg, expected, run, method, seconds):
    deadline, consecutive, rows = time.monotonic() + seconds, 0, []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        consecutive = consecutive + 1 if not settlement_errors(cfg, expected, rows, method) else 0
        if consecutive >= 2:
            return True, rows
        time.sleep(.25)
    return False, rows


def wait_client(proc, log_path, timeout):
    """Keep the complete log and expose new client progress every five seconds."""
    deadline = time.monotonic() + timeout
    # A separate read descriptor follows the file while Popen writes through
    # its existing descriptor. No reader thread or pipe can hold shutdown open.
    buffer = ""
    with Path(log_path).open() as progress:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(proc.args, timeout)
            try:
                result = proc.wait(timeout=min(5, remaining))
            except subprocess.TimeoutExpired:
                result = None
            buffer += progress.read()
            lines = buffer.split("\n")
            buffer = lines.pop()
            if result is not None and buffer:
                lines.append(buffer)
                buffer = ""
            for line in lines:
                if line.startswith("progress ") or line.startswith("completed="):
                    print(line, flush=True)
            if result is not None:
                return result


def run(config_path, workload_path, output_dir, method, timeout=None, drain_timeout=30):
    raw, workload = c.read(config_path), c.read(workload_path)
    validated = c.validate(raw)
    expected = expectations(validated, workload, method)
    workload = copy.deepcopy(workload)
    if timeout is not None:
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("--timeout 必须为正数")
        workload["timeout_s"] = timeout
    timeout = workload["timeout_s"]
    binary = c.binary_for_method(method)
    if not binary.is_file():
        raise ValueError(f"找不到 {binary}，请先运行 make")
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    raw["base_port"] = free_ports(validated["host"], 4 * len(validated["shards"]))
    config, job, result_path = (output_dir / name for name in ("source-config.json", "workload.json", "client.json"))
    c.write(config, raw)
    c.write(job, workload)
    report = {"schema_version": 1, "method": method, "status": "FAIL", "expected": expected,
              "config_source": str(Path(config_path).resolve()), "config_fingerprint": config_fingerprint(validated),
              "workload_source": str(Path(workload_path).resolve()),
              "input_workload_sha256": hashlib.sha256(Path(workload_path).read_bytes()).hexdigest(),
              "workload_sha256": hashlib.sha256(job.read_bytes()).hexdigest(),
              "timeout_s": timeout, "client_result": str(result_path), "failure_reasons": []}
    node_run, proc = None, None
    try:
        node_run = c.start(config, output_dir / "run", method=method)
        cfg, before = c.read(node_run / "config.json"), c.statuses(node_run)
        c.write(output_dir / "status-before.json", before)
        print(f"压测 {method}: {expected['transactions']} 笔全跨片交易，参与分组 {expected['groups']}；结果目录 {output_dir}", flush=True)
        client_log = output_dir / "client.log"
        with client_log.open("w") as log:
            proc = subprocess.Popen([str(binary), "client", str(node_run / "config.json"), str(job), str(result_path)],
                                    stdout=log, stderr=subprocess.STDOUT)
            try:
                client_rc = wait_client(proc, client_log, timeout + 15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=5)
                client_rc = 124
        result = c.read(result_path) if result_path.is_file() else {}
        good_client = client_rc == 0 and result.get("executed_transactions") == expected["transactions"]
        drained, after = wait_settled(cfg, expected, node_run, method, drain_timeout if good_client else min(2, drain_timeout))
        c.write(output_dir / "status-after.json", after)
        errors = settlement_errors(cfg, expected, after, method)
        if not drained:
            errors.append("等待副本排空/收敛超时")
        if client_rc != 0 or result.get("completed_requests") != expected["requests"] or result.get("requests") != expected["requests"]:
            errors.append("客户端未确认全部请求")
        if result.get("executed_transactions") != expected["transactions"]:
            errors.append("客户端执行计数不符")
        if any(result.get(field, 0) for field in ("errors", "ordered_only_transactions", "duplicate_transactions")):
            errors.append("客户端出现错误、仅排序或重复交易")
        for field in METRICS:
            value = result.get(field)
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0 or (field == "completed_tps" and value == 0):
                errors.append(f"客户端 {field} 缺失或无效")
        prior = {(r["shard"], r["replica"]): r for r in before}
        counters = {field: sum(max(0, r.get(field, 0) - prior[(r["shard"], r["replica"])].get(field, 0))
                               for r in after) for field in COUNTERS}
        if any(counters[field] for field in ("network_oversized_errors", "network_parse_errors",
                                             "network_queue_errors", "inbox_dropped")):
            errors.append("节点发生超限消息、解析失败、队列失败或丢弃消息")
        # Once f+1 matching replies arrive, the client closes its listener.
        # Late replies from other healthy replicas can therefore fail to connect
        # even though all transactions and replica states have converged.
        notes = []
        if counters["network_failures"]:
            notes.append("检测到网络失败计数；可能包含客户端退出后的迟到回复失败，不能单独据此判定协议失败；请结合各类计数和收敛结果。")
        report.update(status="FAIL" if errors else "PASS", client_returncode=client_rc,
                      failure_reasons=errors, notes=notes, node_metrics=counters, client=result,
                      run_dir=str(node_run))
        report.update({field: result.get(field) if not errors else None for field in METRICS})
        print(f"{report['status']} completed={result.get('executed_transactions', 0)}/{expected['transactions']} "
              f"completed_tps={report['completed_tps']} avg_latency_s={report['avg_latency_s']} "
              f"p95_s={report['p95_s']} network_failures={counters['network_failures']}", flush=True)
        for error in errors:
            print("  " + error)
    except BaseException as error:
        report["failure_reasons"].append(f"{type(error).__name__}: {error}")
        raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=5)
        c.write(output_dir / "summary.json", report)
        if node_run is not None:
            c.stop_run(node_run)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True, help="复用已有 unsigned workload JSON")
    parser.add_argument("--method", choices=c.METHODS, default="saguaro")
    parser.add_argument("--timeout", type=float, help="可选覆盖客户端超时；默认保持负载内容")
    parser.add_argument("--drain-timeout", type=float, default=30)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.drain_timeout) or args.drain_timeout <= 0:
        parser.error("--drain-timeout 必须为正数")
    folder = args.output_dir or c.ROOT / "test-results" / ("mixed-" + args.method + "-" +
             datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
    try:
        result = run(args.config, args.workload, folder, args.method, args.timeout, args.drain_timeout)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + "\n")
    raise SystemExit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
