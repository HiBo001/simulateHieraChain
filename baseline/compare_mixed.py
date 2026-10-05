#!/usr/bin/env python3
"""Compare Arbor with SharPer/Saguaro on one shared mixed cross-shard workload."""
import argparse
import copy
import datetime
import hashlib
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark as b
import benchmark_mixed as mixed
import compare


def prepare_mixed_workload(cfg, count=10000, rate=5000, batch=10, seed=42, timeout=None):
    """90% two-shard and 10% three-shard transactions on the first three leaves."""
    c.integer(count, "count", 10, 100000000)
    c.integer(batch, "batch", 1, cfg["consensus"]["cross_shard_batch_size"])
    if not b.finite_number(rate, True):
        raise ValueError("rate 必须为有限正数")
    if timeout is None:
        timeout = count / rate + 180
    if not b.finite_number(timeout, True):
        raise ValueError("timeout 必须为有限正数")
    leaves = c.topology(cfg)[1]
    if len(leaves) < 3:
        raise ValueError("默认 90/10 负载需要至少三个叶子分片；其他负载请用 --workload")
    a, d, e = leaves[:3]
    three_count = (count + 5) // 10
    two_count = count - three_count
    pair_count, remainder = divmod(two_count, 3)
    groups = [([a, d], pair_count + (remainder > 0)),
              ([a, e], pair_count + (remainder > 1)),
              ([d, e], pair_count), ([a, d, e], three_count)]
    workload = {"rate": rate, "timeout_s": timeout, "seed": seed, "requests": []}
    for index, (participants, total) in enumerate(groups):
        if not total:
            continue
        part = c.prepare_workload(cfg, total, rate, seed + index, f"compare-mixed:{seed}:{index}",
                                  participants=participants, batch=batch, timeout=timeout)
        workload["requests"].extend(part["requests"])
    random.Random(seed).shuffle(workload["requests"])
    mixed.expectations(cfg, workload)
    return workload


def comparison_row(result, workload, repeat):
    expected, client = result["expected"], result.get("client", {})
    count = expected["transactions"]
    return dict(result, mode="mixed", repeat=repeat, count=count, rate=workload["rate"],
                batch=max(len(request["txs"]) for request in workload["requests"]), seed=workload.get("seed"),
                shard=None, participants=[], executed_transactions=client.get("executed_transactions", 0),
                completed_requests=client.get("completed_requests", 0), requests=client.get("requests", 0),
                elapsed_s=client.get("elapsed_s"),
                sent_bytes_per_tx=result.get("node_metrics", {}).get("bytes_sent", 0) / count)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_cross100.json")
    parser.add_argument("--baseline", choices=("saguaro", "sharper"), default="sharper")
    parser.add_argument("--workload", type=Path, help="复用 unsigned JSON；与 count/rate/batch/seed 互斥")
    parser.add_argument("--count", type=int, help="生成的交易数，默认 10000")
    parser.add_argument("--rate", type=float, help="生成的每秒提交交易数，默认 5000")
    parser.add_argument("--batch", type=int, help="生成的每请求交易数，默认 10")
    parser.add_argument("--seed", type=int, help="生成随机种子，默认 42")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=float, help="两种方法使用相同的客户端超时")
    parser.add_argument("--drain-timeout", type=float, default=30)
    parser.add_argument("--output-dir", type=Path, help="必须是尚不存在的新目录")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    c.integer(args.repeat, "repeat", 1, 100)
    if not b.finite_number(args.drain_timeout, True) or (
            args.timeout is not None and not b.finite_number(args.timeout, True)):
        raise ValueError("timeout 和 drain-timeout 必须是有限正数")
    if args.workload and any(value is not None for value in (args.count, args.rate, args.batch, args.seed)):
        raise ValueError("--workload 与 --count/--rate/--batch/--seed 互斥")
    source = c.read(args.config)
    cfg = c.validate(source)
    if args.workload:
        workload = c.read(args.workload)
        mixed.expectations(cfg, workload)
        workload = copy.deepcopy(workload)
        if args.timeout is not None:
            workload["timeout_s"] = args.timeout
    else:
        workload = prepare_mixed_workload(cfg, args.count if args.count is not None else 10000,
                                         args.rate if args.rate is not None else 5000,
                                         args.batch if args.batch is not None else 10,
                                         args.seed if args.seed is not None else 42, args.timeout)
    expected = mixed.expectations(cfg, workload)
    if workload["timeout_s"] <= (expected["transactions"] - len(workload["requests"][-1]["txs"])) / workload["rate"]:
        raise ValueError("timeout 太短，负载尚未发完就会结束")
    methods = ("arbor", args.baseline)
    if not args.skip_build:
        subprocess.run(["make", *("build/bin/" + method + "_node" for method in methods)], cwd=ROOT, check=True)
    binaries = {method: c.binary_for_method(method) for method in methods}
    for binary in binaries.values():
        if not binary.is_file():
            raise ValueError(f"找不到 {binary}，请先 make")
    folder = (args.output_dir or ROOT / "test-results" / (
        "compare-mixed-" + args.baseline + "-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") +
        uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    shared = folder / "shared-workload.json"
    config_snapshot = folder / "config-snapshot.json"
    c.write(shared, workload)
    c.write(config_snapshot, source)
    report = {"schema_version": 1, "kind": "arbor-" + args.baseline + "-mixed-exact-workload",
              "baseline": args.baseline, "config": cfg, "config_source": str(args.config.resolve()),
              "workload_source": str(args.workload.resolve()) if args.workload else None,
              "input_workload_sha256": hashlib.sha256(args.workload.read_bytes()).hexdigest() if args.workload else None,
              "shared_workload": str(shared), "workload_sha256": hashlib.sha256(shared.read_bytes()).hexdigest(),
              "participants_per_transaction": expected["participants_per_transaction"], "groups": expected["groups"],
              "environment": {"host": platform.node(), "system": platform.platform(), "cpus": os.cpu_count()},
              "created_at": datetime.datetime.now().astimezone().isoformat(),
              "revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
              "working_tree_dirty": bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                                        capture_output=True, text=True).stdout.strip()),
              "binary_sha256": {method: hashlib.sha256(binary.read_bytes()).hexdigest() for method, binary in binaries.items()},
              "expected_pairs": args.repeat, "cases": [], "comparisons": [], "method_comparisons": []}
    print(f"混合负载比较结果目录: {folder}", flush=True)
    compare.save(folder, report)
    try:
        for repeat in range(1, args.repeat + 1):
            order = methods if repeat % 2 else tuple(reversed(methods))
            results = {}
            for method in order:
                print(f"[{repeat}/{args.repeat}] {method} shared_sha256={report['workload_sha256']}", flush=True)
                result = mixed.run(config_snapshot, shared, folder / f"repeat-{repeat:03d}" / method,
                                   method, drain_timeout=args.drain_timeout)
                row = comparison_row(result, workload, repeat)
                results[method] = row
                report["cases"].append(row)
                compare.save(folder, report)
            pair = compare.paired_comparison(results["arbor"], results[args.baseline], args.baseline)
            report["method_comparisons"].append(pair)
            compare.save(folder, report)
            print((f"Arbor / {args.baseline} TPS={pair['arbor_over_' + args.baseline + '_tps']:.3f}"
                   if pair["comparable"] else pair["reason"]), flush=True)
    except BaseException:
        compare.save(folder, report)
        raise
    print(f"汇总: {folder / 'summary.md'}", flush=True)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("比较已中止，已停止本轮集群，已完成数据保留。", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
