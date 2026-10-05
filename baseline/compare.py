#!/usr/bin/env python3
"""Compare Arbor and a baseline using exactly the same unsigned client workload."""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark as b


def validate_comparison_configs(arbor_cfg, baseline_cfg, baseline, separate_config=False):
    """Authorize only an explicit AHL topology comparison, keeping costs equal."""
    if baseline not in ("saguaro", "sharper", "ahl"):
        raise ValueError("baseline 必须为 saguaro、sharper 或 ahl")
    c.validate_method_config(arbor_cfg, "arbor")
    c.validate_method_config(baseline_cfg, baseline)
    if separate_config and baseline != "ahl":
        raise ValueError("--baseline-config 仅适用于 AHL 的两层拓扑比较")
    fingerprints = {"arbor": b.config_fingerprint(arbor_cfg), baseline: b.config_fingerprint(baseline_cfg)}
    if fingerprints["arbor"] != fingerprints[baseline] and not (baseline == "ahl" and separate_config):
        raise ValueError("两种方法必须使用相同配置；AHL 不同拓扑须显式指定 --baseline-config")
    arbor_parent, arbor_leaves = c.topology(arbor_cfg)
    baseline_parent, baseline_leaves = c.topology(baseline_cfg)
    if arbor_leaves != baseline_leaves:
        raise ValueError("两种方法的叶子分片 ID 集合必须完全相同")
    for field in ("replicas_per_shard", "host", "consensus", "execution"):
        if arbor_cfg[field] != baseline_cfg[field]:
            raise ValueError(f"两种方法的 {field} 必须完全相同")
    for field in ("intra_shard_delay_ms", "trace"):
        if arbor_cfg["network"][field] != baseline_cfg["network"][field]:
            raise ValueError(f"两种方法的 network.{field} 必须完全相同")
    common = sorted(set(arbor_parent) & set(baseline_parent))
    delays = {}
    for index, a in enumerate(common):
        for d in common[index + 1:]:
            values = [cfg["network"]["resolved_links"].get(f"{a}:{d}",
                      cfg["network"]["default_inter_shard_delay_ms"]) for cfg in (arbor_cfg, baseline_cfg)]
            if values[0] != values[1]:
                raise ValueError(f"共同分片 {a}/{d} 的实际单向延迟必须相同")
            delays[f"{a}:{d}"] = values[0]
    context = {"validated": True, "baseline": baseline, "separate_config": bool(separate_config),
               "config_fingerprints": fingerprints, "leaves": arbor_leaves,
               "common_shard_delays_ms": delays,
               "topology_difference": {"arbor_parents": arbor_parent, "baseline_parents": baseline_parent,
                   "arbor_only_shards": sorted(set(arbor_parent) - set(baseline_parent)),
                   "baseline_only_shards": sorted(set(baseline_parent) - set(arbor_parent)),
                   "different": arbor_parent != baseline_parent}}
    context["comparison_group"] = hashlib.sha256(json.dumps(context, sort_keys=True,
                                separators=(",", ":")).encode()).hexdigest()
    return context


def bind_comparison(row, context, shared):
    """Bind the actual replay file to the prevalidated configuration pair."""
    row["comparison_group"] = context["comparison_group"]
    row["shared_workload_sha256"] = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
    return row


def paired_comparison(arbor, baseline_result, baseline="saguaro", comparison_context=None):
    if baseline not in ("saguaro", "sharper", "ahl"):
        raise ValueError("baseline 必须为 saguaro、sharper 或 ahl")
    item = {name: arbor.get(name) for name in ("mode", "repeat", "count", "rate", "batch", "seed")}
    item.update(comparable=False, baseline=baseline, workload_sha256=arbor.get("workload_sha256"))
    fields = ("mode", "repeat", "count", "rate", "batch", "seed", "shard", "participants",
              "workload_sha256")
    same_config = arbor.get("config_fingerprint") == baseline_result.get("config_fingerprint")
    context = comparison_context or {}
    different_ahl_config = (baseline == "ahl" and context.get("validated") is True
        and context.get("baseline") == baseline and context.get("separate_config") is True
        and bool(context.get("comparison_group"))
        and all(row.get("comparison_group") == context["comparison_group"]
            and row.get("config_fingerprint") == context.get("config_fingerprints", {}).get(method)
            for row, method in ((arbor, "arbor"), (baseline_result, baseline)))
        and bool(arbor.get("shared_workload_sha256"))
        and all(row.get("input_workload_sha256") == row.get("workload_sha256")
                == row.get("shared_workload_sha256") == arbor["shared_workload_sha256"]
                for row in (arbor, baseline_result)))
    if arbor.get("method") != "arbor" or baseline_result.get("method") != baseline:
        item["reason"] = "方法标识不匹配"
    elif not (same_config or different_ahl_config):
        item["reason"] = "配置不同且未通过显式 AHL 双拓扑验证，不计算比值"
    elif not arbor.get("workload_sha256") or any(arbor.get(k) != baseline_result.get(k) for k in fields):
        item["reason"] = "配置或实际负载内容不同，不计算比值"
    elif any(row.get("status") != "PASS" or not b.finite_number(row.get("completed_tps"), True)
             or any(not b.finite_number(row.get(field)) for field in b.METRICS[1:])
             for row in (arbor, baseline_result)):
        item["reason"] = "至少一种方法未完整通过，不计算比值"
    else:
        item.update(comparable=True, arbor_tps=arbor["completed_tps"],
                    arbor_avg_latency_s=arbor["avg_latency_s"], arbor_p95_s=arbor["p95_s"])
        item[baseline + "_tps"] = baseline_result["completed_tps"]
        item["arbor_over_" + baseline + "_tps"] = arbor["completed_tps"] / baseline_result["completed_tps"]
        item[baseline + "_avg_latency_s"] = baseline_result["avg_latency_s"]
        item[baseline + "_p95_s"] = baseline_result["p95_s"]
    return item


def summarize_pairs(pairs):
    groups = {}
    for pair in pairs:
        key = (pair.get("baseline", "saguaro"), pair["mode"], pair["count"], pair["rate"], pair["batch"], pair["seed"])
        groups.setdefault(key, []).append(pair)
    result = []
    for rows in groups.values():
        item = {field: rows[0][field] for field in ("mode", "count", "rate", "batch", "seed")}
        baseline = item["baseline"] = rows[0].get("baseline", "saguaro")
        item.update(runs=len(rows), comparable=all(row["comparable"] for row in rows))
        if item["comparable"]:
            for field in ("arbor_tps", baseline + "_tps", "arbor_avg_latency_s", baseline + "_avg_latency_s",
                          "arbor_p95_s", baseline + "_p95_s"):
                item["median_" + field] = statistics.median(row[field] for row in rows)
            item["arbor_over_" + baseline + "_tps"] = item["median_arbor_tps"] / item["median_" + baseline + "_tps"]
        else:
            item["reason"] = "包含未通过或负载不一致的轮次，整组比值留空"
        result.append(item)
    return result


def save(folder, report):
    baseline = report.get("baseline", "saguaro")
    label = {"saguaro": "Saguaro", "sharper": "SharPer", "ahl": "AHL"}[baseline]
    report["paired_aggregates"] = summarize_pairs(report["method_comparisons"])
    if len(report["method_comparisons"]) != report.get("expected_pairs", 0):
        # A completed subset must not look like a finished repeated experiment.
        for group in report["paired_aggregates"]:
            for key in list(group):
                if key.startswith("median_") or key.startswith("arbor_over_"):
                    del group[key]
            group.update(comparable=False, reason="轮次未完整结束，整组比值留空")
    report["status"] = ("FAIL" if any(row["status"] != "PASS" for row in report["cases"]) else
                        "PASS" if len(report["method_comparisons"]) == report.get("expected_pairs", 0)
                        and report["method_comparisons"] and all(pair["comparable"] for pair in report["method_comparisons"])
                        else "INCOMPLETE")
    b.save_report(folder, report)
    if report["status"] == "INCOMPLETE":
        for group in report["aggregates"]:
            if group["status"] != "FAIL":
                group["status"] = "INCOMPLETE"
            for key in group:
                if key.startswith("median_"):
                    group[key] = None
        c.write(folder / "summary.json", report)
    lines = [f"# Arbor / {label} 同负载比较", "", f"比较状态：{report['status']}。", "",
             "每一对运行复用同一份 unsigned workload，包括请求/交易 ID、参与分片、key 和 value。",
             "各方法顺序启动独立集群；每轮都检查客户端完整完成、全部副本状态收敛及协议队列排空。",
             "TPS 统计客户端确认的唯一交易，延时单位为秒。", "",
             f"| 模式 | 提交速率 | 请求大小 | 轮次 | Arbor TPS 中位数 | {label} TPS 中位数 | TPS 比值 | Arbor p95(s) | {label} p95(s) |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in report["paired_aggregates"]:
        common = f"| {item['mode']} | {item['rate']:g} | {item['batch']} | {item['runs']}"
        if item["comparable"]:
            lines.append(common + f" | {item['median_arbor_tps']:.2f} | {item['median_' + baseline + '_tps']:.2f} | "
                         f"{item['arbor_over_' + baseline + '_tps']:.3f} | {item['median_arbor_p95_s']:.6f} | "
                         f"{item['median_' + baseline + '_p95_s']:.6f} |")
        else:
            lines.append(common + (" | — | — | — | — | — |" if report["status"] == "INCOMPLETE"
                                   else " | FAIL | FAIL | — | — | — |"))
    failures = [row for row in report["cases"] if row["status"] != "PASS"]
    if failures:
        lines += ["", "失败轮次：", ""]
        lines += [f"- {row['method']} / {row['mode']} / repeat={row['repeat']}: " +
                  "; ".join(row["failure_reasons"]) for row in failures]
    context = report.get("comparison_context", {})
    if context.get("topology_difference", {}).get("different"):
        lines += ["", "## 双拓扑配置", "",
                  "Arbor 保留原层级；AHL 使用一个上层根分片。叶子 ID、PBFT/组批、执行成本、片内延迟及共同分片的实际链路延迟已验证一致。",
                  "节点数和经过的协议路径不同，这是本次比较的一部分。两份配置与 SHA256 保存在 summary.json。", ""]
        for method, config in report.get("method_configs", {}).items():
            lines += [f"### {method}", "", "```text", c.format_topology(config["config"]), "```", ""]
    lines += ["", "详细指标、输入 SHA256 和每轮日志路径见 `summary.json`、`summary.csv`。", ""]
    (folder / "summary.md").write_text("\n".join(lines))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/two_layer.json")
    parser.add_argument("--baseline", choices=("saguaro", "sharper", "ahl"), default="saguaro")
    parser.add_argument("--baseline-config", type=Path, help="AHL 使用独立两层配置；Arbor 保留 --config 拓扑")
    parser.add_argument("--mode", choices=("intra", "cross", "all"), default="cross")
    parser.add_argument("--shard", type=int, default=1)
    parser.add_argument("--participants", default="1,2")
    parser.add_argument("--count", type=int, default=4000)
    rate_group = parser.add_mutually_exclusive_group()
    rate_group.add_argument("--rate", type=float)
    rate_group.add_argument("--rates", type=b.parse_rates)
    parser.add_argument("--batch", type=int, default=8, help="每个客户端请求的交易数；两种方法完全相同")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timeout", type=float, help="每轮客户端超时秒数，默认 count/rate+60")
    parser.add_argument("--drain-timeout", type=float, default=30)
    parser.add_argument("--output-dir", type=Path, help="必须是尚不存在的新目录")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    c.integer(args.count, "count", 1, 100000000)
    c.integer(args.repeat, "repeat", 1, 100)
    rates = args.rates or ([args.rate] if args.rate is not None else [1000.0, 4000.0])
    if any(not b.finite_number(rate, True) for rate in rates):
        raise ValueError("rate 必须是有限正数")
    if not b.finite_number(args.drain_timeout, True) or (args.timeout is not None and
            not b.finite_number(args.timeout, True)):
        raise ValueError("timeout 和 drain-timeout 必须是有限正数")
    source = c.read(args.config)
    cfg = c.validate(source)
    baseline_source = c.read(args.baseline_config) if args.baseline_config else source
    baseline_cfg = c.validate(baseline_source)
    comparison_context = validate_comparison_configs(cfg, baseline_cfg, args.baseline, args.baseline_config is not None)
    participants = sorted(int(p) for p in args.participants.split(","))
    b.validate_selection(cfg, args.mode, args.shard, participants)
    b.validate_selection(baseline_cfg, args.mode, args.shard, participants)
    cases = []
    for mode in (("intra", "cross") if args.mode == "all" else (args.mode,)):
        limit = cfg["consensus"]["batch_size" if mode == "intra" else "cross_shard_batch_size"]
        c.integer(args.batch, "batch", 1, limit)
        for rate in rates:
            last_batch = (args.count - 1) % args.batch + 1
            if args.timeout is not None and args.timeout <= (args.count - last_batch) / rate:
                raise ValueError("timeout 太短，负载尚未发完就会结束")
            for trial in range(1, args.repeat + 1):
                cases.append(dict(mode=mode, count=args.count, rate=rate, batch=args.batch,
                                  repeat=trial, seed=args.seed, shard=args.shard if mode == "intra" else None,
                                  participants=participants if mode == "cross" else []))
    if not args.skip_build:
        subprocess.run(["make", "build/bin/arbor_node", "build/bin/" + args.baseline + "_node"], cwd=ROOT, check=True)
    binaries = {method: c.binary_for_method(method) for method in ("arbor", args.baseline)}
    for path in binaries.values():
        if not path.is_file():
            raise ValueError(f"找不到 {path}，请先 make")
    folder = (args.output_dir or ROOT / "test-results" / (
        "compare-methods-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "kind": "arbor-" + args.baseline + "-exact-workload", "baseline": args.baseline, "config": cfg,
              "config_source": str(args.config.resolve()),
              "environment": {"host": platform.node(), "system": platform.platform(), "cpus": os.cpu_count()},
              "created_at": datetime.datetime.now().astimezone().isoformat(),
              "revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
              "working_tree_dirty": bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                                         capture_output=True, text=True).stdout.strip()),
              "binary_sha256": {method: hashlib.sha256(path.read_bytes()).hexdigest() for method, path in binaries.items()},
              "expected_pairs": len(cases), "cases": [], "comparisons": [], "method_comparisons": []}
    c.write(folder / "config-snapshot.json", source)
    c.write(folder / "baseline-config-snapshot.json", baseline_source)
    report["comparison_context"] = comparison_context
    report["method_configs"] = {
        "arbor": {"config": cfg, "source": str(args.config.resolve()),
                  "input_sha256": hashlib.sha256(args.config.read_bytes()).hexdigest(),
                  "snapshot": str(folder / "config-snapshot.json"),
                  "snapshot_sha256": hashlib.sha256((folder / "config-snapshot.json").read_bytes()).hexdigest()},
        args.baseline: {"config": baseline_cfg, "source": str((args.baseline_config or args.config).resolve()),
                  "input_sha256": hashlib.sha256((args.baseline_config or args.config).read_bytes()).hexdigest(),
                  "snapshot": str(folder / "baseline-config-snapshot.json"),
                  "snapshot_sha256": hashlib.sha256((folder / "baseline-config-snapshot.json").read_bytes()).hexdigest()}}
    print(f"同负载比较结果目录: {folder}", flush=True)
    try:
        for index, case in enumerate(cases, 1):
            timeout = args.timeout if args.timeout is not None else case["count"] / case["rate"] + 60
            shared = c.prepare_workload(cfg, case["count"], case["rate"], case["seed"], f"compare:{index:03d}",
                shard=case["shard"], participants=case["participants"] or None, batch=case["batch"], timeout=timeout)
            case_folder = folder / f"case-{index:03d}"
            case_folder.mkdir()
            shared_path = case_folder / "shared-workload.json"
            c.write(shared_path, shared)
            order = ("arbor", args.baseline) if case["repeat"] % 2 else (args.baseline, "arbor")
            results = {}
            for method in order:
                print(f"[{index}/{len(cases)}] {method} {case['mode']} count={case['count']} "
                      f"rate={case['rate']:g} batch={case['batch']} repeat={case['repeat']}", flush=True)
                row = b.run_case(case, source if method == "arbor" else baseline_source,
                                 case_folder / method, timeout, args.drain_timeout, method=method, workload=shared_path)
                bind_comparison(row, comparison_context, shared_path)
                results[method] = row
                report["cases"].append(row)
                save(folder, report)
                print((f"PASS {method} tps={row['completed_tps']:.2f} avg_s={row['avg_latency_s']:.6f} "
                       f"p95_s={row['p95_s']:.6f}" if row["status"] == "PASS" else
                       f"FAIL {method}: " + "; ".join(row["failure_reasons"])), flush=True)
            pair = paired_comparison(results["arbor"], results[args.baseline], args.baseline, comparison_context)
            report["method_comparisons"].append(pair)
            save(folder, report)
            print((f"Arbor / {args.baseline} TPS={pair['arbor_over_' + args.baseline + '_tps']:.3f}" if pair["comparable"]
                   else pair["reason"]), flush=True)
    except BaseException:
        save(folder, report)
        raise
    print(f"汇总: {folder / 'summary.md'}", flush=True)
    return 0 if len(report["method_comparisons"]) == len(cases) and all(
        pair["comparable"] for pair in report["method_comparisons"]) else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("比较已中止，已停止本轮集群，已完成数据保留。", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
