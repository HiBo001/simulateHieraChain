#!/usr/bin/env python3
"""Serial, exact-input parameter sweep for Arbor's multi-layer ordering windows.

The measured client times stop before benchmark_mixed pauses background empty
blocks for strict settlement. Only complete PASS runs may enter aggregates or
parameter selection. A finite sweep identifies a measured Pareto frontier; it
does not establish a universal optimal parameter or guarantee baseline wins.
"""
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
import random
import signal
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "baseline"))
import cluster as c
import benchmark as b
import benchmark_mixed as mixed
import compare
import compare_mixed
import workload_locality as locality

WINDOW_FIELDS = ("ordering_window_blocks", "empty_block_interval_ms")
BASELINES = ("saguaro", "sharper", "ahl")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_hashes():
    result = {}
    for directory in ("source", "include", "scripts", "baseline"):
        for path in sorted((ROOT / directory).rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".h", ".hpp", ".inc", ".py"):
                result[str(path.relative_to(ROOT))] = digest(path)
    if (ROOT / "Makefile").is_file():
        result["Makefile"] = digest(ROOT / "Makefile")
    return result


def comma_values(text, *, integer=True):
    try:
        values = [int(item) if integer else float(item) for item in text.split(",")]
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError("请使用逗号分隔的正数") from error
    if not values or any(not math.isfinite(value) or value <= 0 for value in values):
        raise argparse.ArgumentTypeError("参数列表必须包含有限正数")
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError("参数列表不能重复")
    return values


def baseline_methods(text):
    if text.lower() == "none":
        return []
    values = text.split(",")
    if not values or len(set(values)) != len(values) or any(value not in BASELINES for value in values):
        raise argparse.ArgumentTypeError("使用 saguaro,sharper,ahl 的子集，或 none")
    return values


def parameter_pairs(text):
    try:
        result = []
        for item in text.split(","):
            m, interval = item.split(":")
            pair = (int(m), int(interval))
            if min(pair) <= 0:
                raise ValueError("both values must be positive")
            result.append(pair)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError("使用 m:empty_ms 对列表，例如 1:50,8:100") from error
    if not result or len(set(result)) != len(result):
        raise argparse.ArgumentTypeError("参数对列表不能为空或重复")
    return result


def parameter_config(raw, m, empty_ms):
    result = copy.deepcopy(raw)
    result.setdefault("consensus", {}).update(ordering_window_blocks=m, empty_block_interval_ms=empty_ms)
    c.validate(result)
    return result


def baseline_config(raw):
    """Keep the old baseline settings, without any Arbor window parameters."""
    result = copy.deepcopy(raw)
    consensus = result.get("consensus", {})
    if isinstance(consensus, dict):
        for name in WINDOW_FIELDS:
            consensus.pop(name, None)
    c.validate(result)
    return result


def common_cost_config(cfg):
    """Normalize ONLY the two explicitly Arbor-specific protocol settings.

    All remaining PBFT/execution/network fields and real runtime fingerprints
    stay intact. Normalization is for comparison validation, not replay.
    """
    result = copy.deepcopy(cfg)
    for name in WINDOW_FIELDS:
        result["consensus"].pop(name, None)
    result["network"].pop("resolved_links", None)
    return c.validate(result)


def validate_costs(arbor_cfg, baseline_cfg, method):
    context = compare.validate_comparison_configs(common_cost_config(arbor_cfg),
        common_cost_config(baseline_cfg), method, separate_config=method == "ahl")
    return dict(shared_cost_validation=context,
                protocol_difference_allowlist=["consensus." + name for name in WINDOW_FIELDS],
                actual_config_fingerprints={"arbor": b.config_fingerprint(arbor_cfg),
                                            method: b.config_fingerprint(baseline_cfg)},
                actual_arbor_window={name: arbor_cfg["consensus"][name] for name in WINDOW_FIELDS},
                baseline_window_mode=False)


def aggregates(report):
    result = []
    for planned in report["planned_groups"]:
        fields = ("phase", "method", "m", "empty_ms", "rate")
        rows = [row for row in report["cases"] if all(row[field] == planned[field] for field in fields)]
        complete = len(rows) == planned["expected_runs"]
        passed = complete and all(row["status"] == "PASS" for row in rows)
        entry = dict(planned, runs=len(rows), passed=sum(row["status"] == "PASS" for row in rows),
                     status="PASS" if passed else "FAIL" if any(row["status"] == "FAIL" for row in rows) else "INCOMPLETE")
        for name in b.METRICS:
            entry["median_" + name] = statistics.median(row[name] for row in rows) if passed else None
        entry["min_completed_tps"] = min(row["completed_tps"] for row in rows) if passed else None
        entry["max_completed_tps"] = max(row["completed_tps"] for row in rows) if passed else None
        result.append(entry)
    return result


def pareto_frontier(rows):
    """Maximize TPS while minimizing mean, p95, and p99 latency at one rate."""
    def dominates(a, d):
        av = (a["median_completed_tps"], -a["median_avg_latency_s"], -a["median_p95_s"], -a["median_p99_s"])
        dv = (d["median_completed_tps"], -d["median_avg_latency_s"], -d["median_p95_s"], -d["median_p99_s"])
        return all(x >= y for x, y in zip(av, dv)) and any(x > y for x, y in zip(av, dv))
    usable = [row for row in rows if row["status"] == "PASS" and row["method"] == "arbor"]
    return [row for row in usable if not any(other is not row and dominates(other, row) for other in usable)]


def select_candidates(report, top):
    rows = aggregates(report)
    rates = report["settings"]["rates"]
    eligible = []
    for m, interval in report["settings"]["parameter_pairs"]:
        matching = [row for row in rows if row["phase"] == "screen" and row["method"] == "arbor"
                    and row["m"] == m and row["empty_ms"] == interval]
        if len(matching) != len(rates) or any(row["status"] != "PASS" for row in matching):
            continue
        hits = sum(any(item["m"] == m and item["empty_ms"] == interval for item in pareto_frontier(
                   [row for row in rows if row["phase"] == "screen" and row["rate"] == rate])) for rate in rates)
        # TPS ranking is normalized by the offered rate; low-rate achieved TPS
        # otherwise cannot be compared numerically to saturation measurements.
        throughput = min(row["median_completed_tps"] / row["rate"] for row in matching)
        p95 = statistics.mean(row["median_p95_s"] for row in matching)
        eligible.append(dict(m=m, empty_ms=interval, frontier_rates=hits,
                             min_offered_rate_fraction=throughput, mean_p95_s=p95))
    eligible.sort(key=lambda item: (-item["frontier_rates"], -item["min_offered_rate_fraction"],
                                   item["mean_p95_s"], item["m"], item["empty_ms"]))
    return eligible[:top]


def pairs(report):
    results = []
    lookup = {(row["phase"], row["repeat"], row["rate"], row["method"]): row
              for row in report["cases"] if row["method"] != "arbor"}
    for arbor in (row for row in report["cases"] if row["method"] == "arbor"):
        for method in report["settings"]["baseline_methods"]:
            baseline = lookup.get((arbor["phase"], arbor["repeat"], arbor["rate"], method))
            item = {field: arbor[field] for field in ("phase", "repeat", "m", "empty_ms", "rate", "seed")}
            item.update(baseline=method, comparable=False)
            if baseline is None:
                item["reason"] = "基线轮次尚未完成"
            elif any(row["status"] != "PASS" for row in (arbor, baseline)):
                item["reason"] = "至少一种方法未完整通过"
            elif any(arbor.get(field) != baseline.get(field) for field in
                     ("input_workload_sha256", "workload_sha256", "count", "batch", "seed", "rate")):
                item["reason"] = "真实输入或负载参数不同"
            else:
                item.update(comparable=True, workload_sha256=arbor["workload_sha256"],
                    arbor_over_baseline_tps=arbor["completed_tps"] / baseline["completed_tps"],
                    arbor_avg_latency_s=arbor["avg_latency_s"], baseline_avg_latency_s=baseline["avg_latency_s"],
                    arbor_p95_s=arbor["p95_s"], baseline_p95_s=baseline["p95_s"],
                    arbor_p99_s=arbor["p99_s"], baseline_p99_s=baseline["p99_s"])
            results.append(item)
    return results


def save(folder, report):
    report["aggregates"] = aggregates(report)
    report["paired_comparisons"] = pairs(report)
    report["pareto_frontiers"] = {}
    for phase in sorted({group["phase"] for group in report["planned_groups"]}):
        report["pareto_frontiers"][phase] = {
            str(rate): [{"m": row["m"], "empty_ms": row["empty_ms"]} for row in pareto_frontier(
                [row for row in report["aggregates"] if row["phase"] == phase and row["rate"] == rate])]
            for rate in report["settings"]["rates"]}
    failed = bool(report["failure_reasons"]) or any(row["status"] == "FAIL" for row in report["cases"])
    complete = report.get("finished", False) and all(row["status"] == "PASS" for row in report["aggregates"])
    report["status"] = "FAIL" if failed else "PASS" if complete else "INCOMPLETE"
    c.write(folder / "summary.json", report)
    columns = ("phase", "repeat", "method", "m", "empty_ms", "rate", "seed", "count", "batch", "status",
               *b.METRICS, "input_workload_sha256", "workload_sha256", "config_fingerprint", "run_dir", "failure_reasons")
    with (folder / "cases.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for row in report["cases"]:
            writer.writerow(dict(row, failure_reasons=json.dumps(row.get("failure_reasons", []), ensure_ascii=False)))
    lines = ["# Ordering window parameter experiment", "", f"Status: {report['status']}", "",
             "100% cross-shard; 95% within-cluster, 5% across-cluster; 90% two-party, 10% three-party.", "",
             "All methods in each seed/rate block replay the same unsigned JSON bytes in fresh, serial clusters.", "",
             "Arbor-only window settings are the sole comparison cost exception; AHL retains its explicit two-layer topology.", "",
             "Latency units: seconds. TPS is completed transactions / complete client elapsed time, including drain of submitted work.", "",
             "Each measurement begins after fresh cluster readiness; no warm-up work is excluded. Results are finite-run measurements, not steady-state capacity estimates.", "",
             "Low offered rates cap achieved TPS; saturation tests alone do not establish latency at normal offered rates.", "",
             "No failed/incomplete aggregate has a TPS or latency estimate. Pareto results apply only to this finite matrix and host.", "",
             "| Phase | Method | m | Empty interval ms | Offered tx/s | PASS runs | TPS median | Avg latency s | p95 s | p99 s |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in report["aggregates"]:
        values = [row["median_" + metric] for metric in ("completed_tps", "avg_latency_s", "p95_s", "p99_s")]
        formatted = ["—" if value is None else f"{value:.6g}" for value in values]
        lines.append(f"| {row['phase']} | {row['method']} | {row['m'] or '—'} | {row['empty_ms'] or '—'} | "
                     f"{row['rate']:g} | {row['passed']}/{row['expected_runs']} | " + " | ".join(formatted) + " |")
    lines += ["", "Pareto frontiers:", "", "```json", json.dumps(report["pareto_frontiers"], indent=2), "```", ""]
    if report.get("selected_candidates"):
        lines += ["Confirmation candidates selected from screen results; confirmation uses independent seeds:", "",
                  "```json", json.dumps(report["selected_candidates"], indent=2), "```", ""]
    for row in report["cases"]:
        for message in row.get("failure_reasons", []):
            lines.append(f"- {row['phase']}/{row['method']}/m={row['m']}/empty={row['empty_ms']}/rate={row['rate']}: {message}")
    (folder / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def replay_case(config, shared, output, method, phase, repeat, m, empty_ms, cfg, report):
    before_hash = digest(shared)
    if before_hash != report["workloads"][str(shared)]["sha256"]:
        raise ValueError("共同负载文件在测试中被修改")
    binary = c.binary_for_method(method)
    if digest(binary) != report["binary_sha256"][method]:
        raise ValueError("实验中的方法二进制已变化")
    result = mixed.run(config, shared, output, method, drain_timeout=report["settings"]["drain_timeout"])
    workload = c.read(shared)
    row = compare_mixed.comparison_row(result, workload, repeat)
    row.update(phase=phase, m=m, empty_ms=empty_ms)
    # The full workload and per-transaction timings remain in client.json.
    # Repeating them in every outer summary made the 20-case report exceed
    # 170 MiB and added avoidable serialization between measurements.
    row["client"] = {name: value for name, value in result.get("client", {}).items()
                     if value is None or type(value) in (str, bool, int, float)}
    row["client_evidence"] = dict(path=result.get("client_result"),
        full_workload_and_transaction_timings_retained=("workload" in result.get("client", {})
                                                        and "timings" in result.get("client", {})))
    errors = []
    if row["method"] != method or row["config_fingerprint"] != b.config_fingerprint(cfg):
        errors.append("实际方法或真实配置指纹不符")
    if not row.get("input_workload_sha256") == row.get("workload_sha256") == before_hash == digest(shared):
        errors.append("共同输入和实际回放的 SHA256 不符")
    if digest(binary) != report["binary_sha256"][method]:
        errors.append("方法二进制在该轮执行时变化")
    if row.get("status") == "PASS" and any(not b.finite_number(row.get(metric), metric == "completed_tps") for metric in b.METRICS):
        errors.append("PASS 行缺少有效完整性能统计")
    if errors:
        row["status"] = "FAIL"
        row.setdefault("failure_reasons", []).extend(errors)
    if row["status"] != "PASS":
        row.update({metric: None for metric in b.METRICS})
    return row


def run_phase(folder, report, phase, combinations, repeats, seed_start, configurations):
    methods = report["settings"]["baseline_methods"]
    rates = report["settings"]["rates"]
    for rate in rates:
        for m, interval in combinations:
            report["planned_groups"].append(dict(phase=phase, method="arbor", m=m, empty_ms=interval,
                                                  rate=rate, expected_runs=repeats))
        for method in methods:
            report["planned_groups"].append(dict(phase=phase, method=method, m=None, empty_ms=None,
                                                  rate=rate, expected_runs=repeats))
    save(folder, report)
    for repeat in range(1, repeats + 1):
        seed = seed_start + repeat - 1
        for rate_index, rate in enumerate(rates):
            block = folder / phase / f"repeat-{repeat:03d}" / f"rate-{rate:g}"
            block.mkdir(parents=True)
            reference = configurations["reference"]
            count = report["settings"]["confirm_count"] if phase == "confirm" else report["settings"]["count"]
            workload = locality.prepare_locality_workload(reference, count, rate,
                report["settings"]["batch"], seed, report["settings"]["timeout"],
                cross_cluster_ratio=.05, three_shard_ratio=.10)
            shared = block / "shared-workload.json"
            c.write(shared, workload)
            report["workloads"][str(shared)] = dict(phase=phase, repeat=repeat, seed=seed, rate=rate,
                sha256=digest(shared), locality=locality.workload_statistics(reference, workload))
            # Deterministic shuffle avoids systematically putting small m or
            # every baseline at the beginning/end of a thermally changing run.
            schedule = [("arbor", m, interval) for m, interval in combinations] + [(method, None, None) for method in methods]
            random.Random(f"window-case-order:{phase}:{seed}:{rate_index}").shuffle(schedule)
            for method, m, interval in schedule:
                key = f"arbor-m{m}-empty{interval}" if method == "arbor" else method
                snapshot, cfg = configurations[key]
                print(f"{phase} repeat={repeat}/{repeats} rate={rate:g} {key} input_sha256={digest(shared)}", flush=True)
                row = replay_case(snapshot, shared, block / key, method, phase, repeat, m, interval, cfg, report)
                report["cases"].append(row)
                save(folder, report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_locality.json")
    parser.add_argument("--ahl-config", type=Path, default=ROOT / "config/ahl_two_layer_locality.json")
    parser.add_argument("--m-values", type=comma_values, help="Cartesian m values; default 1,2,4,8")
    parser.add_argument("--empty-ms-values", type=comma_values, help="Cartesian intervals; default 5,10,20,50,100")
    parser.add_argument("--parameter-pairs", type=parameter_pairs,
                        help="Explicit m:empty_ms pairs, e.g. 1:50,8:100; excludes the Cartesian flags")
    parser.add_argument("--rates", type=lambda value: comma_values(value, integer=False), default=[5000.0])
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--count", type=int, default=6000)
    parser.add_argument("--confirm-count", type=int, default=10000, help="Transactions per confirmation run; separate from screen --count")
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--baseline-methods", type=baseline_methods, default=list(BASELINES))
    parser.add_argument("--confirm-top", type=int, default=0, help="0: sweep only; N: one-round screen then independent-seed repeated confirmation of N candidates")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--drain-timeout", type=float, default=60)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    if args.parameter_pairs is not None:
        if args.m_values is not None or args.empty_ms_values is not None:
            raise ValueError("--parameter-pairs 与 --m-values/--empty-ms-values 互斥")
        pairs_to_test = args.parameter_pairs
    else:
        pairs_to_test = [(m, interval) for m in (args.m_values or [1, 2, 4, 8])
                         for interval in (args.empty_ms_values or [5, 10, 20, 50, 100])]
    c.integer(args.repeat, "repeat", 1, 100)
    c.integer(args.count, "count", 1, 100000000)
    c.integer(args.confirm_count, "confirm-count", 1, 100000000)
    c.integer(args.confirm_top, "confirm-top", 0, len(pairs_to_test))
    if not b.finite_number(args.timeout, True) or not b.finite_number(args.drain_timeout, True):
        raise ValueError("timeout 和 drain-timeout 必须为有限正数")
    source = c.read(args.config)
    reference = c.validate(baseline_config(source))
    parent, leaves = c.topology(reference)
    root = next(sid for sid in parent if parent[sid] is None)
    if all(parent[leaf] == root for leaf in leaves):
        raise ValueError("窗口实验必须使用至少三层 Arbor 参考树")
    c.integer(args.batch, "batch", 1, reference["consensus"]["cross_shard_batch_size"])
    largest_count = max(args.count, args.confirm_count) if args.confirm_top else args.count
    if args.timeout <= largest_count / min(args.rates):
        raise ValueError("timeout 必须长于最慢 rate 下的完整提交时间")
    baseline_raw = {method: baseline_config(c.read(args.ahl_config) if method == "ahl" else source)
                    for method in args.baseline_methods}
    baseline_cfg = {method: c.validate(raw) for method, raw in baseline_raw.items()}
    candidate_raw = {f"arbor-m{m}-empty{interval}": parameter_config(source, m, interval) for m, interval in pairs_to_test}
    candidate_cfg = {key: c.validate(raw) for key, raw in candidate_raw.items()}
    costs = {key: {method: validate_costs(cfg, baseline_cfg[method], method) for method in args.baseline_methods}
             for key, cfg in candidate_cfg.items()}
    methods = ("arbor", *args.baseline_methods)
    if not args.skip_build:
        subprocess.run(["make", *(f"build/bin/{method}_node" for method in methods)], cwd=ROOT, check=True)
    binaries = {method: c.binary_for_method(method) for method in methods}
    if any(not path.is_file() for path in binaries.values()):
        raise ValueError("方法二进制不存在，请先 make")
    folder = (args.output_dir or ROOT / "test-results" / ("ordering-windows-" +
        datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    snapshots = folder / "config-snapshots"
    snapshots.mkdir()
    configurations = {"reference": reference}
    for key, raw in {**candidate_raw, **baseline_raw}.items():
        snapshot = snapshots / (key + ".json")
        c.write(snapshot, raw)
        configurations[key] = (snapshot, c.validate(raw))
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip())
    report = dict(schema_version=1, kind="ordering-window-finite-parameter-matrix", status="INCOMPLETE",
        created_at=datetime.datetime.now().astimezone().isoformat(), revision=revision, working_tree_dirty=dirty,
        environment=dict(host=platform.node(), system=platform.platform(), cpus=os.cpu_count(), python=sys.version),
        binary_sha256={method: digest(path) for method, path in binaries.items()},
        source_sha256=source_hashes(),
        settings=dict(parameter_pairs=pairs_to_test, rates=args.rates, count=args.count,
            confirm_count=args.confirm_count, batch=args.batch,
            seed=args.seed, timeout=args.timeout, drain_timeout=args.drain_timeout,
            baseline_methods=args.baseline_methods, repeat=args.repeat, confirm_top=args.confirm_top),
        config_source=str(args.config.resolve()), ahl_config_source=str(args.ahl_config.resolve()),
        config_snapshots={key: dict(path=str(value[0]), sha256=digest(value[0]),
            actual_fingerprint=b.config_fingerprint(value[1])) for key, value in configurations.items() if key != "reference"},
        cost_comparisons=costs, planned_groups=[], cases=[], workloads={}, failure_reasons=[], finished=False)
    save(folder, report)
    try:
        phase = "screen" if args.confirm_top else "sweep"
        run_phase(folder, report, phase, pairs_to_test, 1 if args.confirm_top else args.repeat, args.seed, configurations)
        if args.confirm_top:
            report["selected_candidates"] = select_candidates(report, args.confirm_top)
            if not report["selected_candidates"]:
                report["failure_reasons"].append("没有在全部 rates 上完整 PASS 的候选，未进行确认实验")
            else:
                selected = [(row["m"], row["empty_ms"]) for row in report["selected_candidates"]]
                run_phase(folder, report, "confirm", selected, args.repeat, args.seed + 1000, configurations)
        report["finished"] = True
        report["completed_at"] = datetime.datetime.now().astimezone().isoformat()
    except BaseException as error:
        report["failure_reasons"].append(f"{type(error).__name__}: {error}")
        save(folder, report)
        raise
    save(folder, report)
    print(f"Results: {folder / 'summary.md'}", flush=True)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("实验已中止，已完成数据保留。", file=sys.stderr)
        raise SystemExit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
