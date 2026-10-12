#!/usr/bin/env python3
"""Serial performance matrix for Arbor, Saguaro, SharPer and AHL (stdlib only)."""
import argparse
import csv
import datetime
import itertools
import json
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
import benchmark as b
import benchmark_mixed as mixed
import benchmark_ordering_windows as windows
import cluster as c
import compare
import workload_locality as locality

METHODS = ("arbor", "saguaro", "sharper", "ahl")
PROFILES = {
    "smoke": dict(count=200, rates=[100.0], ratios=[.05], repeat=1),
    "rate": dict(count=10000, rates=[100., 500., 1000., 2000., 5000.], ratios=[.05], repeat=3),
    "locality": dict(count=10000, rates=[1000.], ratios=[0., .05, .25, .5, 1.], repeat=3),
    "full": dict(count=10000, rates=[100., 500., 1000., 2000., 5000.],
                 ratios=[0., .05, .25, .5, 1.], repeat=3),
}


def ratio_values(value):
    try:
        values = [float(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("使用逗号分隔的 0..1 比例") from error
    if not values or len(values) != len(set(values)) or any(
            not b.finite_number(item) or not 0 <= item <= 1 for item in values):
        raise argparse.ArgumentTypeError("比例须为不同的有限 0..1 数值")
    return values


def prepare(args):
    defaults = PROFILES[args.profile]
    for key in ("count", "rates", "ratios", "repeat"):
        if getattr(args, key) is None:
            setattr(args, key, defaults[key])
    c.integer(args.count, "count", 1, 100000000)
    c.integer(args.repeat, "repeat", 1, 100)
    c.integer(args.window_blocks, "window-blocks", 0, 32)
    c.integer(args.empty_ms, "empty-ms", 1, 10000)
    if not b.finite_number(args.drain_timeout, True):
        raise ValueError("drain-timeout 必须为有限正数")
    if not b.finite_number(args.three_shard_ratio) or not 0 <= args.three_shard_ratio <= 1:
        raise ValueError("three-shard-ratio 必须为 0..1")
    if args.timeout is None:
        args.timeout = args.count / min(args.rates) + 180
    if not b.finite_number(args.timeout, True) or args.timeout <= args.count / min(args.rates):
        raise ValueError("timeout 必须长于最慢速率下的发送时间 count/rate")
    reference_raw = windows.baseline_config(c.read(args.config))
    reference = c.validate(reference_raw)
    arbor_raw = (windows.parameter_config(reference_raw, args.window_blocks, args.empty_ms)
                 if args.window_blocks else reference_raw)
    raw = dict(arbor=arbor_raw, saguaro=reference_raw, sharper=reference_raw,
               ahl=windows.baseline_config(c.read(args.ahl_config)))
    configs = {method: c.validate(value) for method, value in raw.items()}
    c.integer(args.batch, "batch", 1, min(cfg["consensus"]["cross_shard_batch_size"] for cfg in configs.values()))
    costs = {method: compare.validate_comparison_configs(
        windows.common_cost_config(configs["arbor"]), windows.common_cost_config(configs[method]),
        method, separate_config=method == "ahl") for method in METHODS[1:]}
    # Validate every matrix point before compiling or starting any process.
    for ratio, rate in itertools.product(args.ratios, args.rates):
        sample = locality.prepare_locality_workload(reference, args.count, rate, args.batch,
            args.seed, args.timeout, cross_cluster_ratio=ratio, three_shard_ratio=args.three_shard_ratio)
        for method in METHODS:
            mixed.expectations(configs[method], sample, method)
    return reference, raw, configs, costs


def save(folder, report):
    report["aggregates"] = windows.aggregates(report)
    paired = windows.pairs(report)
    report["paired_aggregates"] = []
    for ratio, rate, method in itertools.product(report["settings"]["ratios"],
                                               report["settings"]["rates"], METHODS[1:]):
        phase = f"cross-cluster-{ratio:g}"
        rows = [row for row in paired if row["phase"] == phase and row["rate"] == rate and row["baseline"] == method]
        usable = len(rows) == report["settings"]["repeat"] and all(row["comparable"] for row in rows)
        report["paired_aggregates"].append(dict(phase=phase, rate=rate, baseline=method,
            comparable=usable, median_arbor_over_baseline_tps=statistics.median(
                row["arbor_over_baseline_tps"] for row in rows) if usable else None))
    failed = report["failure_reasons"] or any(row["status"] == "FAIL" for row in report["cases"])
    complete = report["finished"] and all(row["status"] == "PASS" for row in report["aggregates"])
    report["status"] = "FAIL" if failed else "PASS" if complete else "INCOMPLETE"
    c.write(folder / "summary.json", report)
    columns = ("phase", "rate", "method", "m", "empty_ms", "status", "passed", "expected_runs",
               *("median_" + name for name in b.METRICS), "min_completed_tps", "max_completed_tps")
    with (folder / "summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(report["aggregates"])
    columns = ("phase", "rate", "repeat", "seed", "method", "status", *b.METRICS,
               "count", "executed_transactions", "elapsed_s", "batch", "sent_bytes_per_tx",
               "workload_sha256", "config_fingerprint", "run_dir", "failure_reasons")
    with (folder / "cases.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for row in report["cases"]:
            writer.writerow(dict(row, failure_reasons=json.dumps(row["failure_reasons"], ensure_ascii=False)))
    lines = ["# Arbor 与全部 baseline 性能测试", "", f"状态：{report['status']}", "",
        f"Arbor 窗口 m={report['settings']['window_blocks']}，空块间隔 {report['settings']['empty_ms']} ms；m=0 表示关闭窗口。",
        "负载为 100% 跨片；cross-cluster 是跨 cluster 比例，不是跨片交易占比。", "",
        "每个 ratio/rate/repeat 内四方法使用同一 unsigned 文件，顺序运行独立新集群；每轮种子加一。",
        "全部计划轮次 PASS 才显示该组中位数，失败或未完成保留空值。延时单位为秒。", "",
        "| 跨 cluster 比例 | 发送 tx/s | 方法 | PASS | TPS 中位数 | 平均延时 s | p95 s | p99 s |",
        "|---|---:|---|---:|---:|---:|---:|---:|"]
    for row in report["aggregates"]:
        values = [row["median_" + name] for name in ("completed_tps", "avg_latency_s", "p95_s", "p99_s")]
        rendered = ["—" if value is None else f"{value:.6g}" for value in values]
        lines.append(f"| {row['phase'].removeprefix('cross-cluster-')} | {row['rate']:g} | {row['method']} | "
                     f"{row['passed']}/{row['expected_runs']} | " + " | ".join(rendered) + " |")
    lines += ["", "| 跨 cluster 比例 | 发送 tx/s | baseline | Arbor / baseline TPS 配对比中位数 |",
              "|---|---:|---|---:|"]
    for row in report["paired_aggregates"]:
        value = row["median_arbor_over_baseline_tps"]
        lines.append(f"| {row['phase'].removeprefix('cross-cluster-')} | {row['rate']:g} | {row['baseline']} | "
                     + ("—" if value is None else f"{value:.6g}") + " |")
    lines += ["", "TPS=完整确认的唯一交易数/客户端 elapsed_s；包含发送和等待确认，不含集群启动及后续副本收敛等待。",
              "当前是新集群有限负载测试，没有排除预热；不能直接视为稳态最大 TPS。",
              "Arbor/Saguaro/SharPer 默认 36 个节点，AHL 默认 28 个节点；这是架构比较。",
              "SharPer 使用当前升序预约实现，AHL 使用当前两层 PBFT+2PC 实现。",
              "原始延时和节点日志保存在各方法目录；网络字节包含测量后排空检查的开销。", ""]
    for row in report["cases"]:
        for error in row["failure_reasons"]:
            lines.append(f"- {row['phase']}/{row['rate']:g}/{row['repeat']}/{row['method']}：{error}")
    lines.extend("- " + error for error in report["failure_reasons"])
    (folder / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="smoke")
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_locality.json")
    parser.add_argument("--ahl-config", type=Path, default=ROOT / "config/ahl_two_layer_locality.json")
    parser.add_argument("--count", type=int)
    parser.add_argument("--rates", type=b.parse_rates)
    parser.add_argument("--cross-cluster-ratios", dest="ratios", type=ratio_values)
    parser.add_argument("--repeat", type=int)
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--three-shard-ratio", type=float, default=.10)
    parser.add_argument("--window-blocks", type=int, default=1, help="Arbor only: 默认 1；0 关闭新窗口")
    parser.add_argument("--empty-ms", type=int, default=200, help="Arbor only: 默认 200 ms")
    parser.add_argument("--timeout", type=float, help="默认 count/min(rates)+180 秒")
    parser.add_argument("--drain-timeout", type=float, default=60)
    parser.add_argument("--output-dir", type=Path, help="须为新目录")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="校验完整矩阵并打印参数，不编译、不启动、不写文件")
    args = parser.parse_args(argv)
    reference, raw, configs, costs = prepare(args)
    settings = {name: getattr(args, name) for name in ("profile", "count", "rates", "ratios", "repeat",
        "batch", "seed", "three_shard_ratio", "window_blocks", "empty_ms", "timeout", "drain_timeout")}
    settings["baseline_methods"] = list(METHODS[1:])
    cases = 4 * len(args.ratios) * len(args.rates) * args.repeat
    print(json.dumps(dict(settings=settings, expected_cases=cases), ensure_ascii=False, indent=2), flush=True)
    if args.dry_run:
        return 0
    folder = (args.output_dir or ROOT / "test-results" / ("all-methods-" +
        datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    if folder.exists():
        raise ValueError("output-dir 已存在，请指定新目录以保留旧结果")
    if not args.skip_build:
        subprocess.run(["make", *(f"build/bin/{method}_node" for method in METHODS)], cwd=ROOT, check=True)
    binaries = {method: c.binary_for_method(method) for method in METHODS}
    if any(not path.is_file() for path in binaries.values()):
        raise ValueError("方法二进制不存在，请运行 make all 或去掉 --skip-build")
    folder.mkdir(parents=True, exist_ok=False)
    snapshots = {}
    for method in METHODS:
        snapshots[method] = folder / (method + "-config.json")
        c.write(snapshots[method], raw[method])
    report = dict(schema_version=1, kind="four-method-performance-matrix", settings=settings,
        expected_cases=cases, created_at=datetime.datetime.now().astimezone().isoformat(),
        environment=dict(host=platform.node(), system=platform.platform(), cpus=os.cpu_count(), python=sys.version),
        revision=subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
        working_tree_dirty=bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip()),
        binary_sha256={method: windows.digest(path) for method, path in binaries.items()},
        source_sha256=windows.source_hashes(), cost_comparisons=costs,
        protocol_difference_allowlist=["consensus." + field for field in windows.WINDOW_FIELDS],
        method_configs={method: dict(path=str(snapshots[method]), sha256=windows.digest(snapshots[method]),
            fingerprint=b.config_fingerprint(configs[method]), config=configs[method]) for method in METHODS},
        planned_groups=[], cases=[], workloads={}, failure_reasons=[], finished=False)
    for ratio, rate, method in itertools.product(args.ratios, args.rates, METHODS):
        report["planned_groups"].append(dict(phase=f"cross-cluster-{ratio:g}", rate=rate, method=method,
            m=args.window_blocks if method == "arbor" else None,
            empty_ms=args.empty_ms if method == "arbor" and args.window_blocks else None,
            expected_runs=args.repeat))
    print(f"结果目录：{folder}；共 {cases} 个独立集群用例", flush=True)
    save(folder, report)
    try:
        for point, (ratio, rate) in enumerate(itertools.product(args.ratios, args.rates)):
            phase = f"cross-cluster-{ratio:g}"
            for repeat in range(1, args.repeat + 1):
                block = folder / phase / f"rate-{rate:g}" / f"repeat-{repeat:03d}"
                block.mkdir(parents=True)
                shared = block / "shared-workload.json"
                workload = locality.prepare_locality_workload(reference, args.count, rate, args.batch,
                    args.seed + repeat - 1, args.timeout, cross_cluster_ratio=ratio,
                    three_shard_ratio=args.three_shard_ratio)
                c.write(shared, workload)
                report["workloads"][str(shared)] = dict(sha256=windows.digest(shared),
                    locality=locality.workload_statistics(reference, workload))
                offset = (point + repeat - 1) % len(METHODS)
                for method in METHODS[offset:] + METHODS[:offset]:
                    print(f"{phase} rate={rate:g} repeat={repeat}/{args.repeat} method={method}", flush=True)
                    row = windows.replay_case(snapshots[method], shared, block / method, method,
                        phase, repeat, args.window_blocks if method == "arbor" else None,
                        args.empty_ms if method == "arbor" and args.window_blocks else None,
                        configs[method], report)
                    report["cases"].append(row)
                    save(folder, report)
        report["finished"] = True
        report["completed_at"] = datetime.datetime.now().astimezone().isoformat()
    except BaseException as error:
        report["failure_reasons"].append(f"{type(error).__name__}: {error}")
        save(folder, report)
        raise
    save(folder, report)
    print(f"汇总：{folder / 'summary.md'}", flush=True)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("测试已中止，本轮节点已清理，已完成结果保留。", file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
