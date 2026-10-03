#!/usr/bin/env python3
"""Isolated, repeatable intra/cross-shard performance runs; standard library only."""
import argparse
import copy
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import socket
import statistics
import subprocess
import sys
import time
import uuid

import cluster as c

METRICS = ("completed_tps", "avg_latency_s", "p50_s", "p95_s", "p99_s")
COUNTERS = ("messages_sent", "messages_received", "bytes_sent", "bytes_received",
            "network_connect_attempts", "network_connections_reused", "network_failures",
            "network_socket_errors", "network_connect_errors", "network_write_errors",
            "network_timeout_errors", "network_queue_errors", "network_parse_errors",
            "network_oversized_errors", "inbox_dropped", "view_changes")


def config_fingerprint(cfg):
    simulation = {name: copy.deepcopy(cfg[name]) for name in
                  ("replicas_per_shard", "host", "shards", "consensus", "execution", "network")}
    return hashlib.sha256(json.dumps(simulation, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def validate_selection(cfg, mode, shard, participants):
    parent, leaves = c.topology(cfg)
    if mode in ("intra", "all") and shard not in leaves:
        raise ValueError("片内 --shard 必须是叶子分片")
    if mode in ("cross", "all"):
        if len(participants) < 2:
            raise ValueError("--participants 必须包含至少两个不同的叶子分片")
        coordinator = c.lca(cfg, participants)
        if coordinator in leaves:
            raise ValueError("跨片交易必须由参与叶子的公共祖先协调")


def settlement_errors(case, cfg, rows):
    parent, leaves = c.topology(cfg)
    expected = {(sid, r) for sid in parent for r in range(4)}
    actual = {(r.get("shard"), r.get("replica")) for r in rows}
    problems = []
    if actual != expected or len(rows) != len(expected):
        return ["节点状态不完整"]
    coordinator = c.lca(cfg, case["participants"]) if case["mode"] == "cross" else None
    for sid in parent:
        peers = [r for r in rows if r["shard"] == sid]
        if not all(r.get("alive") and r.get("ready") and not r.get("changing_view") for r in peers):
            problems.append(f"分片 {sid} 有节点未就绪")
        for field in ("state_digest", "kv_digest", "chain_digest", "applied_batches"):
            if any(field not in r for r in peers) or len({r.get(field) for r in peers}) != 1:
                problems.append(f"分片 {sid} 的 {field} 未收敛")
        wanted = case["count"] if (case["mode"] == "intra" and sid == case["shard"]) or (
            case["mode"] == "cross" and sid in case["participants"]) else 0
        if any(r.get("executed_transactions") != wanted for r in peers):
            problems.append(f"分片 {sid} 执行计数不符")
        ordered = case["count"] if case["mode"] == "cross" and sid == coordinator else 0
        if any(r.get("ordered_cst_transactions") != ordered for r in peers):
            problems.append(f"分片 {sid} 跨片排序计数不符")
        if sid not in leaves and any(
                r.get("completed_cst_transactions") != ordered for r in peers):
            problems.append(f"协调片 {sid} 完成计数不符")
        # Demand-driven empty closes consume real coordinator PBFT slots.
        # Client completion is insufficient if another coordinator still owes
        # a requested round, even when its business queues are already empty.
        if sid not in leaves and any(
                r.get("rounds_requested", 0) > r.get("cst_round", 0) for r in peers):
            problems.append(f"协调片 {sid} 仍有未封闭轮次")
        if any(r.get(field, 0) for r in peers for field in (
                "pending_requests", "pending_cst_batches", "staged_cst_batches", "dedup_waiting_requests",
                "network_queue", "network_buffered_bytes")):
            problems.append(f"分片 {sid} 尚有待处理交易")
    return problems


def settled(case, cfg, rows):
    return not settlement_errors(case, cfg, rows)


def finite_number(value, positive=False):
    return type(value) in (int, float) and math.isfinite(value) and (value > 0 if positive else value >= 0)


def summarize_case(case, cfg, client_rc, result, before, after, drained):
    problems = settlement_errors(case, cfg, after)
    if not drained:
        problems.append("等待副本排空/收敛超时")
    if client_rc != 0 or result.get("completed_requests") != result.get("requests"):
        problems.append("客户端未成功确认全部请求")
    if result.get("executed_transactions") != case["count"]:
        problems.append("客户端执行数不等于发送交易数")
    if result.get("errors", 0) or result.get("ordered_only_transactions", 0) or result.get("duplicate_transactions", 0):
        problems.append("负载出现错误、仅排序或重复交易")
    if not finite_number(result.get("elapsed_s"), True) or any(
            not finite_number(result.get(field), field == "completed_tps") for field in METRICS):
        problems.append("客户端 TPS/延时指标缺失或无效")
    before_index = {(r["shard"], r["replica"]): r for r in before}
    metrics = {field: sum(max(0, r.get(field, 0) - before_index.get(
        (r["shard"], r["replica"]), {}).get(field, 0)) for r in after) for field in COUNTERS}
    row = dict(case, config_fingerprint=config_fingerprint(cfg),
               status="FAIL" if problems else "PASS", failure_reasons=problems,
               client_returncode=client_rc, executed_transactions=result.get("executed_transactions", 0),
               completed_requests=result.get("completed_requests", 0), requests=result.get("requests", 0),
               elapsed_s=result.get("elapsed_s"), node_metrics=metrics,
               per_shard={str(s["id"]): {
                   "executed_each_replica": [r.get("executed_transactions") for r in after if r["shard"] == s["id"]],
                   "applied_batches_each_replica": [r.get("applied_batches") for r in after if r["shard"] == s["id"]],
                   "cst_order_index_each_replica": [r.get("cst_order_index", 0) for r in after if r["shard"] == s["id"]],
               } for s in cfg["shards"]})
    for field in METRICS:
        value = result.get(field)
        row[field] = value if not problems else None
    row["sent_bytes_per_tx"] = metrics["bytes_sent"] / case["count"]
    return row


def comparison_key(row):
    return (row["mode"], row["count"], row["rate"], row["batch"], row["seed"], row.get("shard"),
            tuple(row.get("participants", [])), row["config_fingerprint"])


def group_rows(rows):
    groups = {}
    for row in rows:
        groups.setdefault(comparison_key(row), []).append(row)
    return groups


def validate_report(report):
    if not isinstance(report, dict) or report.get("schema_version") != 1 or not isinstance(report.get("cases"), list):
        raise ValueError("baseline 必须是 schema_version=1 且包含 cases 数组的 summary.json")
    for row in report["cases"]:
        try:
            comparison_key(row)
            if row["status"] not in ("PASS", "FAIL"):
                raise ValueError("baseline 用例状态无效")
            if row["status"] == "PASS" and (not finite_number(row.get("completed_tps"), True) or
                                              not finite_number(row.get("p95_s"))):
                raise ValueError("baseline 成功用例缺少有效 TPS/p95 指标")
        except (KeyError, TypeError) as error:
            raise ValueError("baseline 用例缺少必要的参数字段") from error


def compare_reports(current, baseline):
    if current.get("schema_version") != 1 or baseline.get("schema_version") != 1:
        raise ValueError("性能结果 schema_version 不兼容")
    old_groups = group_rows(baseline["cases"])
    comparisons = []
    for key, new in group_rows(current["cases"]).items():
        old = old_groups.get(key, [])
        item = {k: new[0][k] for k in ("mode", "count", "rate", "batch")}
        item["comparable"] = False
        if not old:
            item["reason"] = "基线没有相同配置和负载参数的用例"
        elif any(r["status"] != "PASS" or not finite_number(r.get("completed_tps"), True) or
                 not finite_number(r.get("p95_s")) for r in new + old):
            item["reason"] = "存在未成功完成的轮次，不计算加速比"
        elif (current.get("environment") and baseline.get("environment") and
              current["environment"] != baseline["environment"]):
            item["reason"] = "运行机器/系统环境不同，不计算加速比"
        else:
            new_tps = statistics.median(r["completed_tps"] for r in new)
            old_tps = statistics.median(r["completed_tps"] for r in old)
            item.update(comparable=True, new_tps=new_tps, old_tps=old_tps, tps_speedup=new_tps / old_tps,
                        new_runs=len(new), old_runs=len(old),
                        new_p95_s=statistics.median(r["p95_s"] for r in new),
                        old_p95_s=statistics.median(r["p95_s"] for r in old))
        comparisons.append(item)
    return comparisons


def free_ports(host, count):
    # start() rechecks under its lifecycle lock. Never take over occupied ports.
    for base in range(32000, 60000 - count, 31):
        sockets = []
        try:
            for port in range(base, base + count):
                sock = socket.socket()
                sockets.append(sock)
                sock.bind((host, port))
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise ValueError("找不到足够的连续空闲端口")


def wait_settled(case, cfg, run, seconds):
    deadline, consecutive, rows = time.monotonic() + seconds, 0, []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        consecutive = consecutive + 1 if settled(case, cfg, rows) else 0
        if consecutive >= 2:
            return True, rows
        time.sleep(.25)
    return False, rows


def run_case(case, source, folder, timeout, drain_timeout):
    raw = copy.deepcopy(source)
    raw["base_port"] = free_ports(raw.get("host", "127.0.0.1"), 4 * len(raw["shards"]))
    folder.mkdir(parents=True)
    config_path, run = folder / "source-config.json", None
    c.write(config_path, raw)
    try:
        run = c.start(config_path, folder / "run")
        cfg = c.read(run / "config.json")
        # One client: capture output without adding an extra statistics polling
        # process during the timed load. End-of-run status is checked separately.
        before = c.statuses(run)
        workload = c.prepare_workload(cfg, case["count"], case["rate"], case["seed"], "benchmark",
            shard=case["shard"], participants=case["participants"] or None,
            batch=case["batch"], timeout=timeout)
        job, output = folder / "workload.json", folder / "client.json"
        c.write(job, workload)
        started = time.monotonic()
        with (folder / "client.log").open("w") as log:
            proc = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"), str(job), str(output)],
                                    stdout=log, stderr=subprocess.STDOUT)
            try:
                client_rc = proc.wait(timeout=timeout + 15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
                client_rc = 124
            except BaseException:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
                raise
        result = c.read(output) if output.exists() else {}
        good_client = client_rc == 0 and result.get("executed_transactions") == case["count"]
        drained, after = wait_settled(case, cfg, run, drain_timeout if good_client else min(2, drain_timeout))
        c.write(folder / "status-before.json", before)
        c.write(folder / "status-after.json", after)
        row = summarize_case(case, cfg, client_rc, result, before, after, drained)
        row.update(run_dir=str(run), client_result=str(output), client_log=str(folder / "client.log"),
                   case_wall_s=time.monotonic() - started, timeout_s=timeout,
                   minimum_send_s=(case["count"] - len(workload["requests"][-1]["txs"])) / case["rate"])
        return row
    finally:
        if run is not None:
            c.stop_run(run)


def save_report(folder, report):
    report["aggregates"] = []
    for rows in group_rows(report["cases"]).values():
        item = {field: rows[0][field] for field in ("mode", "count", "rate", "batch")}
        item.update(runs=len(rows), status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL")
        for field in METRICS:
            item["median_" + field] = statistics.median(r[field] for r in rows) if item["status"] == "PASS" else None
        report["aggregates"].append(item)
    c.write(folder / "summary.json", report)
    columns = ("mode", "repeat", "count", "rate", "batch", "status", "executed_transactions",
               "elapsed_s", "completed_tps", "avg_latency_s", "p50_s", "p95_s", "p99_s",
               "sent_bytes_per_tx", "messages_sent", "bytes_sent", "network_connect_attempts",
               "network_connections_reused", "network_failures", "view_changes", "failure_reasons", "run_dir")
    with (folder / "summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for row in report["cases"]:
            record = dict(row, **row["node_metrics"])
            record["failure_reasons"] = "; ".join(row["failure_reasons"])
            writer.writerow({field: record.get(field) for field in columns})


def parse_rates(value):
    try:
        rates = [float(v) for v in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("--rates 使用逗号分隔的正数") from error
    if not rates or any(not math.isfinite(r) or r <= 0 for r in rates) or len(set(rates)) != len(rates):
        raise argparse.ArgumentTypeError("--rates 必须是不同的有限正数")
    return rates


def main(argv=None):
    parser = argparse.ArgumentParser(description="一键片内/跨片性能测试；独立集群，输出 CSV/JSON")
    parser.add_argument("--config", type=Path, default=c.ROOT / "config/two_layer.json")
    parser.add_argument("--mode", choices=("intra", "cross", "all"), default="all")
    parser.add_argument("--shard", type=int, default=1)
    parser.add_argument("--participants", default="1,2")
    parser.add_argument("--count", type=int, default=4000)
    parser.add_argument("--rates", type=parse_rates, default=parse_rates("1000,4000"))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--intra-batch", type=int, help="默认使用配置 batch_size")
    parser.add_argument("--cross-batch", type=int, default=8, help="默认8，保持小客户端请求的跨片组批测试")
    parser.add_argument("--timeout", type=float, help="每轮客户端超时秒数，默认 count/rate+60")
    parser.add_argument("--drain-timeout", type=float, default=30)
    parser.add_argument("--output-dir", type=Path, help="必须是尚不存在的新目录")
    parser.add_argument("--baseline", type=Path, help="之前运行的 summary.json；仅比较相同配置和负载")
    parser.add_argument("--skip-build", action="store_true", help="使用当前二进制，不自动 make")
    args = parser.parse_args(argv)
    c.integer(args.count, "count", 1, 100000000)
    c.integer(args.repeat, "repeat", 1, 100)
    if not math.isfinite(args.drain_timeout) or args.drain_timeout <= 0:
        raise ValueError("drain-timeout 必须是有限正数")
    if args.timeout is not None and (not math.isfinite(args.timeout) or args.timeout <= 0):
        raise ValueError("timeout 必须是有限正数")
    source = c.read(args.config)
    cfg = c.validate(source)
    participants = sorted(int(p) for p in args.participants.split(","))
    validate_selection(cfg, args.mode, args.shard, participants)
    cases = []
    for mode in (("intra", "cross") if args.mode == "all" else (args.mode,)):
        batch = args.intra_batch if mode == "intra" else args.cross_batch
        if batch is None:
            batch = cfg["consensus"]["batch_size"]
        limit = cfg["consensus"]["batch_size" if mode == "intra" else "cross_shard_batch_size"]
        c.integer(batch, mode + "-batch", 1, limit)
        for rate in args.rates:
            last_batch = (args.count - 1) % batch + 1
            if args.timeout is not None and args.timeout <= (args.count - last_batch) / rate:
                raise ValueError("timeout 太短，尚未发完负载就会结束；请增加 timeout")
            for trial in range(1, args.repeat + 1):
                cases.append(dict(mode=mode, count=args.count, rate=rate, batch=batch, repeat=trial,
                                  seed=args.seed, shard=args.shard if mode == "intra" else None,
                                  participants=participants if mode == "cross" else []))
    baseline = c.read(args.baseline) if args.baseline else None
    if baseline is not None:
        validate_report(baseline)
    if not args.skip_build:
        subprocess.run(["make"], cwd=c.ROOT, check=True)
    if not c.BIN.is_file():
        raise ValueError("找不到节点二进制，请先 make")
    folder = (args.output_dir or c.ROOT / "test-results" / (
        "benchmark-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=c.ROOT, text=True, capture_output=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=c.ROOT, text=True, capture_output=True).stdout.strip()
    report = {"schema_version": 1, "config": cfg, "config_source": str(args.config.resolve()),
              "environment": {"host": platform.node(), "system": platform.platform(), "cpus": os.cpu_count()},
              "created_at": datetime.datetime.now().astimezone().isoformat(), "revision": revision,
              "working_tree_dirty": bool(dirty), "binary_sha256": hashlib.sha256(c.BIN.read_bytes()).hexdigest(),
              "cases": [], "comparisons": []}
    c.write(folder / "config-snapshot.json", source)
    print(f"结果目录: {folder}", flush=True)
    print("每轮新集群；TPS只统计客户端确认的唯一交易；延时单位为秒。", flush=True)
    try:
        for i, case in enumerate(cases, 1):
            print(f"[{i}/{len(cases)}] {case['mode']} count={case['count']} rate={case['rate']:g} "
                  f"batch={case['batch']} repeat={case['repeat']}", flush=True)
            if math.ceil(case["count"] / case["batch"]) < 10:
                print("请求批次数少于10，发送呈明显突发；测持续吞吐时请增加 count 或减小客户端 batch。", flush=True)
            timeout = args.timeout if args.timeout is not None else case["count"] / case["rate"] + 60
            row = run_case(case, source, folder / f"case-{i:03d}", timeout, args.drain_timeout)
            report["cases"].append(row)
            save_report(folder, report)
            if row["status"] == "PASS":
                print(f"PASS tps={row['completed_tps']:.2f} avg_s={row['avg_latency_s']:.6f} "
                      f"p95_s={row['p95_s']:.6f} p99_s={row['p99_s']:.6f} "
                      f"connects={row['node_metrics']['network_connect_attempts']} "
                      f"reused={row['node_metrics']['network_connections_reused']} "
                      f"network_failures={row['node_metrics']['network_failures']}", flush=True)
            else:
                print("FAIL " + "; ".join(row["failure_reasons"]), flush=True)
        if baseline:
            report["comparisons"] = compare_reports(report, baseline)
            for item in report["comparisons"]:
                print(f"对照 {item['mode']} rate={item['rate']:g}: " + (
                    f"TPS中位数 {item['old_tps']:.2f} -> {item['new_tps']:.2f}, "
                    f"{item['tps_speedup']:.3f}倍" if item["comparable"] else item["reason"]), flush=True)
        save_report(folder, report)
        for item in report["aggregates"]:
            print(f"汇总 {item['mode']} rate={item['rate']:g} batch={item['batch']} runs={item['runs']}: " + (
                f"TPS中位数={item['median_completed_tps']:.2f} p95中位数_s={item['median_p95_s']:.6f}"
                if item["status"] == "PASS" else "FAIL，统计留空"), flush=True)
    except BaseException:
        save_report(folder, report)
        raise
    print(f"汇总: {folder / 'summary.csv'}\n原始数据: {folder / 'summary.json'}", flush=True)
    return 0 if all(row["status"] == "PASS" for row in report["cases"]) else 2


def interrupted(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("测试已中止，已停止本轮创建的节点，已完成结果保留。", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
