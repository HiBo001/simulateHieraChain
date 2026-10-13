#!/usr/bin/env python3
"""Compare the whole-window barrier with fixed ancestor streaming on exact input."""
import argparse
import csv
import datetime
import itertools
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
import benchmark_ordering_windows as w
import cluster as c
import workload_locality as locality

STRATEGIES = ("barrier", "streaming")


def save(folder, report):
    report["aggregates"], report["paired_aggregates"] = [], []
    for empty_ms, rate in itertools.product(report["settings"]["empty_ms_values"], report["settings"]["rates"]):
        grouped = {}
        for strategy in STRATEGIES:
            rows = [row for row in report["cases"] if row["strategy"] == strategy and
                    row["empty_ms"] == empty_ms and row["rate"] == rate]
            usable = len(rows) == report["settings"]["repeat"] and all(row["status"] == "PASS" for row in rows)
            aggregate = dict(strategy=strategy, empty_ms=empty_ms, rate=rate, passed=sum(row["status"] == "PASS" for row in rows),
                             expected_runs=report["settings"]["repeat"], status="PASS" if usable else "INCOMPLETE/FAIL")
            for metric in b.METRICS:
                aggregate["median_" + metric] = statistics.median(row[metric] for row in rows) if usable else None
            report["aggregates"].append(aggregate)
            grouped[strategy] = {row["repeat"]: row for row in rows} if usable else {}
        usable = all(len(grouped[strategy]) == report["settings"]["repeat"] for strategy in STRATEGIES)
        ratios = []
        if usable:
            for repeat in grouped["barrier"]:
                old, new = grouped["barrier"][repeat], grouped["streaming"][repeat]
                if any(old[field] != new[field] for field in ("workload_sha256", "count", "rate", "seed", "batch")):
                    usable = False;break
                ratios.append(new["completed_tps"] / old["completed_tps"])
        report["paired_aggregates"].append(dict(empty_ms=empty_ms, rate=rate, comparable=usable,
            median_streaming_over_barrier_tps=statistics.median(ratios) if usable else None))
    failed = report["failure_reasons"] or any(row["status"] != "PASS" for row in report["cases"])
    report["status"] = "FAIL" if failed else "PASS" if report["finished"] and all(
        row["comparable"] for row in report["paired_aggregates"]) else "INCOMPLETE"
    c.write(folder / "summary.json", report)
    columns = ("strategy", "empty_ms", "rate", "status", "passed", "expected_runs", *("median_" + metric for metric in b.METRICS))
    with (folder / "summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns);writer.writeheader();writer.writerows(report["aggregates"])
    lines = ["# 固定祖先流式策略与窗口屏障比较", "", f"状态：{report['status']}", "",
        "每个 interval/rate/repeat 内两种策略读取同一 unsigned 文件，同一二进制、独立串行集群；仅 ancestor_streaming 不同。",
        "100%跨片、90%双方/10%三方、5%跨cluster；保留原组批和共识成本。当前为有限负载测量。", "",
        "| 空块 ms | rate | 策略 | PASS | TPS中位数 | 平均延时 s | p95 s | p99 s |",
        "|---:|---:|---|---:|---:|---:|---:|---:|"]
    for row in report["aggregates"]:
        values = [row["median_" + metric] for metric in ("completed_tps", "avg_latency_s", "p95_s", "p99_s")]
        lines.append(f"| {row['empty_ms']} | {row['rate']:g} | {row['strategy']} | {row['passed']}/{row['expected_runs']} | "
                     + " | ".join("—" if value is None else f"{value:.6g}" for value in values) + " |")
    for row in report["paired_aggregates"]:
        ratio = row["median_streaming_over_barrier_tps"]
        lines.append(f"- empty={row['empty_ms']} ms / rate={row['rate']:g}：流式/屏障 TPS 配对比中位数 "
                     + ("—" if ratio is None else f"{ratio:.4f}"))
    for row in report["cases"]:
        for error in row["failure_reasons"]: lines.append(f"- {row['strategy']}/{row['empty_ms']}/{row['repeat']}：{error}")
    lines.extend("- " + error for error in report["failure_reasons"])
    (folder / "summary.md").write_text("\n".join(lines) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_locality.json")
    parser.add_argument("--count", type=int, default=2000)
    parser.add_argument("--rates", type=b.parse_rates, default=[1000.])
    parser.add_argument("--empty-ms-values", type=w.comma_values, default=[50, 200])
    parser.add_argument("--m", type=int, default=1)
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--drain-timeout", type=float, default=60)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    c.integer(args.repeat, "repeat", 1, 100)
    c.integer(args.m, "m", 1, 32)
    if not b.finite_number(args.timeout, True) or args.timeout <= args.count / min(args.rates):
        raise ValueError("timeout 必须长于最慢发送时间")
    if not b.finite_number(args.drain_timeout, True): raise ValueError("drain-timeout 必须为有限正数")
    raw = w.baseline_config(c.read(args.config));reference = c.validate(raw)
    configurations = {}
    for interval, strategy in itertools.product(args.empty_ms_values, STRATEGIES):
        candidate = w.parameter_config(raw, args.m, interval)
        candidate["consensus"]["ancestor_streaming"] = strategy == "streaming"
        configurations[(interval, strategy)] = (candidate, c.validate(candidate))
    for rate in args.rates:
        locality.prepare_locality_workload(reference, args.count, rate, args.batch, args.seed, args.timeout)
    if not args.skip_build: subprocess.run(["make", "build/bin/arbor_node"], cwd=ROOT, check=True)
    binary = c.binary_for_method("arbor")
    if not binary.is_file(): raise ValueError("请先 make build/bin/arbor_node")
    folder = (args.output_dir or ROOT / "test-results" / ("streaming-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    settings = {name: getattr(args, name) for name in ("count", "rates", "empty_ms_values", "m", "batch", "repeat", "seed", "timeout", "drain_timeout")}
    report = dict(kind="ancestor-streaming-exact-input", settings=settings, binary_sha256={"arbor": w.digest(binary)},
        source_sha256=w.source_hashes(), cases=[], workloads={}, failure_reasons=[], finished=False)
    base = ROOT / "BASE_REVISION.json"
    report.update(created_at=datetime.datetime.now().astimezone().isoformat(),
        base_revision=c.read(base)["revision"] if base.is_file() else subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
        working_tree_dirty=True if base.is_file() else bool(subprocess.run(
            ["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip()),
        environment=dict(host=platform.node(), system=platform.platform(), cpus=os.cpu_count()),
        configuration_difference_allowlist=["consensus.ancestor_streaming"], config_snapshots={})
    snapshots = {}
    for key, (candidate, cfg) in configurations.items():
        path = folder / f"config-{key[1]}-{key[0]}.json";c.write(path, candidate);snapshots[key] = path
        report["config_snapshots"][path.name] = dict(path=str(path), sha256=w.digest(path),
                                                    fingerprint=b.config_fingerprint(cfg))
    save(folder, report)
    print(f"结果目录：{folder}", flush=True)
    try:
        for point, (interval, rate) in enumerate(itertools.product(args.empty_ms_values, args.rates)):
            for repeat in range(1, args.repeat + 1):
                block = folder / f"empty-{interval}" / f"rate-{rate:g}" / f"repeat-{repeat:03d}"
                block.mkdir(parents=True)
                shared = block / "shared-workload.json"
                c.write(shared, locality.prepare_locality_workload(reference, args.count, rate, args.batch,
                                                                  args.seed + repeat - 1, args.timeout))
                report["workloads"][str(shared)] = dict(sha256=w.digest(shared))
                order = STRATEGIES if (point + repeat) % 2 else STRATEGIES[::-1]
                for strategy in order:
                    cfg = configurations[(interval, strategy)][1]
                    print(f"empty={interval} rate={rate:g} repeat={repeat} strategy={strategy}", flush=True)
                    row = w.replay_case(snapshots[(interval, strategy)], shared, block / strategy, "arbor", strategy,
                                        repeat, args.m, interval, cfg, report)
                    row["strategy"] = strategy;report["cases"].append(row);save(folder, report)
        report["finished"] = True
        report["completed_at"] = datetime.datetime.now().astimezone().isoformat()
    except BaseException as error:
        report["failure_reasons"].append(f"{type(error).__name__}: {error}");save(folder, report);raise
    save(folder, report)
    print(f"汇总：{folder / 'summary.md'}", flush=True)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr);sys.exit(1)
