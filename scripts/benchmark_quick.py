#!/usr/bin/env python3
"""One bounded correctness/performance group; stop immediately on invalid evidence."""
import argparse
import copy
import csv
import datetime
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark as b
import benchmark_guard as guards
import benchmark_mixed as mixed
import benchmark_ordering_windows as w
import cluster as c
import workload_locality as locality

ARBOR_CASES = ("barrier", "streaming", "lower_first", "lower_2", "lower_10", "lower_bundle")
BASELINE_CASES = ("saguaro", "sharper", "ahl")
CASES = ARBOR_CASES + BASELINE_CASES
DEFAULT_CASES = ("lower_first",) + BASELINE_CASES
METHODS = ("arbor", "saguaro", "sharper", "ahl")
CLEANUP_RESERVE = 10


def method_for_case(name):
    return "arbor" if name in ARBOR_CASES else name


def selected_cases(args):
    requested = tuple(getattr(args, "cases", None) or DEFAULT_CASES)
    if len(requested) != len(set(requested)) or any(name not in CASES for name in requested):
        raise ValueError("--cases must contain distinct supported cases")
    return tuple(name for name in CASES if name in requested)


def stream_slot(ancestors, position, lower_first=False, lower_blocks_per_root=1, leaf_batch_blocks=1):
    """Mirror the certified schedule: each lower ancestor gets r blocks per Root block."""
    if (not ancestors or position < 1 or lower_blocks_per_root < 1 or leaf_batch_blocks not in (1, 2) or
            leaf_batch_blocks == 2 and (not lower_first or lower_blocks_per_root != 2)):
        raise ValueError("invalid ancestor stream position")
    if not lower_first or len(ancestors) == 1:
        return ancestors[(position - 1) % len(ancestors)], 1 + (position - 1) // len(ancestors)
    lower_count = len(ancestors) - 1
    cycle, offset = divmod(position - 1, lower_blocks_per_root * lower_count + 1)
    if offset == lower_blocks_per_root * lower_count:
        return ancestors[-1], cycle + 1
    if leaf_batch_blocks == 2:
        return ancestors[offset // 2], cycle * 2 + offset % 2 + 1
    return ancestors[offset % lower_count], cycle * lower_blocks_per_root + offset // lower_count + 1


def stream_position(ancestors, coordinator, block, lower_first=False, lower_blocks_per_root=1, leaf_batch_blocks=1):
    """Inverse of stream_slot; raw block heights are not comparable under a weighted schedule."""
    if (not ancestors or coordinator not in ancestors or block < 1 or lower_blocks_per_root < 1 or
            leaf_batch_blocks not in (1, 2) or
            leaf_batch_blocks == 2 and (not lower_first or lower_blocks_per_root != 2)):
        raise ValueError("invalid ancestor stream block")
    index = ancestors.index(coordinator)
    if not lower_first or len(ancestors) == 1:
        return (block - 1) * len(ancestors) + index + 1
    lower_count, width = len(ancestors) - 1, lower_blocks_per_root * (len(ancestors) - 1) + 1
    if index == lower_count:
        return (block - 1) * width + lower_blocks_per_root * lower_count + 1
    cycle, offset = divmod(block - 1, lower_blocks_per_root)
    if leaf_batch_blocks == 2:
        return cycle * width + index * 2 + offset + 1
    return cycle * width + offset * lower_count + index + 1


def audit_leaf_order(run, cfg, strategy):
    """Read applied leaf journals: exact ancestor stream or full window barrier."""
    streaming = cfg["consensus"].get("ancestor_streaming", strategy in ("streaming", "lower_first", "lower_2", "lower_10", "lower_bundle"))
    lower_first = streaming and cfg["consensus"].get("ancestor_order", "root_first") == "lower_first"
    lower_blocks_per_root = cfg["consensus"].get("ancestor_lower_blocks_per_root", 1)
    leaf_batch_blocks = cfg["consensus"].get("ancestor_leaf_batch_blocks", 1)
    parent, leaves = c.topology(cfg)
    checked, proof_count, business_count, business_slots, bundled_slots = 0, 0, 0, 0, 0
    for leaf in leaves:
        ancestors, sid = [], parent[leaf]
        while sid is not None:
            ancestors.append(sid);sid = parent[sid]
        if not lower_first:
            ancestors.reverse()
        replica_orders = []
        for replica in range(cfg["replicas_per_shard"]):
            directory = Path(run) / f"shard{leaf}/node{replica}"
            cursor, sequence, executed = 0, [], set()
            with (directory / "commits.jsonl").open() as journal:
                for line in journal:
                    value = json.loads(line)["certificate"]["proposal"]["body"]["value"]
                    if "cst_frontier" not in value:
                        continue
                    frontier, orders = value["cst_frontier"], value.get("cst_orders", [])
                    if not frontier or len(orders) > leaf_batch_blocks or leaf_batch_blocks == 2 and len(frontier) > 32:
                        raise ValueError(f"leaf {leaf}/{replica}: malformed frontier/orders")
                    bodies = [proof["proposal"]["body"] for proof in frontier]
                    if streaming:
                        applicable = []
                        for index, body in enumerate(bodies):
                            cursor += 1
                            if (body["shard"], body["value"]["cst_round"]) != stream_slot(
                                    ancestors, cursor, lower_first, lower_blocks_per_root, leaf_batch_blocks):
                                raise ValueError(f"leaf {leaf}/{replica}: skipped or reordered ancestor slot {cursor}")
                            involved = any(leaf in tx["participants"] for request in body["value"]["requests"]
                                           for tx in request["body"]["txs"])
                            if involved:
                                applicable.append(index)
                        if len(applicable) != len(orders):
                            raise ValueError(f"leaf {leaf}/{replica}: skipped applicable business block")
                        if any(order["proposal"] != frontier[index]["proposal"]
                               for order, index in zip(orders, applicable)):
                            raise ValueError(f"leaf {leaf}/{replica}: business differs from stream head")
                        if orders:
                            if leaf_batch_blocks == 1 or bodies[-1]["shard"] == ancestors[-1]:
                                if applicable != [len(bodies) - 1]:
                                    raise ValueError(f"leaf {leaf}/{replica}: skipped applicable business block")
                            else:
                                final = bodies[-1]
                                coordinator, block = final["shard"], final["value"]["cst_round"]
                                if block % 2 or any(bodies[index]["shard"] != coordinator or
                                        bodies[index]["value"]["cst_round"] not in (block - 1, block)
                                        for index in applicable):
                                    raise ValueError(f"leaf {leaf}/{replica}: bundle crosses coordinator pair or closes early")
                        elif value.get("cst_stream_progress") is not True or (leaf_batch_blocks == 1 and len(frontier) != 32):
                            raise ValueError(f"leaf {leaf}/{replica}: malformed stream progress")
                    elif orders:
                        candidate = orders[0]["proposal"]["body"]
                        m = cfg["consensus"]["ordering_window_blocks"]
                        window = 1 + (candidate["value"]["cst_round"] - 1) // m
                        if (len(bodies) != len(ancestors) or {body["shard"] for body in bodies} != set(ancestors) or
                                any(body["value"]["cst_round"] != window * m for body in bodies)):
                            raise ValueError(f"leaf {leaf}/{replica}: ancestor window not fully closed")
                        manifested = sorted((ancestors.index(body["shard"]), body["shard"], entry["block"])
                            for body in bodies for entry in
                            body["value"]["cst_window_manifest"][str(leaf)]["orders"])
                        pending = [position for position in manifested if position[1:] not in executed]
                        wanted = (ancestors.index(candidate["shard"]), candidate["shard"], candidate["value"]["cst_round"])
                        if not pending or pending[0] != wanted:
                            raise ValueError(f"leaf {leaf}/{replica}: skipped manifest business order")
                    else:
                        raise ValueError(f"leaf {leaf}/{replica}: barrier contains stream progress")
                    for order in orders:
                        body = order["proposal"]["body"]
                        position = ((stream_position(ancestors, body["shard"], body["value"]["cst_round"],
                                     lower_first, lower_blocks_per_root, leaf_batch_blocks), body["seq"]) if streaming else
                                    (body["value"]["cst_round"], ancestors.index(body["shard"]), body["shard"], body["seq"]))
                        sequence.append(position);executed.add((body["shard"], body["value"]["cst_round"]))
                    proof_count += len(frontier);business_count += len(orders)
                    business_slots += bool(orders);bundled_slots += len(orders) == 2
            if sequence != sorted(sequence):
                raise ValueError(f"leaf {leaf}/{replica}: business order is not monotonic")
            if streaming and c.read(directory / "status.json").get("ancestor_stream_cursor", 0) != cursor:
                raise ValueError(f"leaf {leaf}/{replica}: durable cursor differs from applied journal")
            replica_orders.append(sequence);checked += 1
        if any(sequence != replica_orders[0] for sequence in replica_orders):
            raise ValueError(f"leaf {leaf}: replica business order differs")
    if not business_count:
        raise ValueError("order audit found no applied business blocks")
    return dict(status="PASS", checked_leaf_replicas=checked, frontier_proofs=proof_count,
                business_blocks=business_count, business_pbft_slots=business_slots,
                bundled_pbft_slots=bundled_slots)


def prepare(args):
    cases = selected_cases(args)
    c.integer(args.correctness_count, "correctness-count", 100, 300)
    c.integer(args.count, "count", 1000, 2000)
    c.integer(args.empty_ms, "empty-ms", 1, 10000)
    arbor_batch_wait = getattr(args, "arbor_cross_shard_batch_wait_ms", None)
    if arbor_batch_wait is not None:
        c.integer(arbor_batch_wait, "arbor-cross-shard-batch-wait-ms", 0, 10000)
    if any(not b.finite_number(value, True) for value in
           (args.rate, args.budget, args.case_timeout, args.no_progress_timeout, args.drain_timeout)):
        raise ValueError("rates and time budgets must be finite positive values")
    if args.case_timeout > 60 or args.budget <= CLEANUP_RESERVE + 10:
        raise ValueError("case-timeout must be <=60s; budget must leave startup and cleanup time")
    if args.case_timeout <= args.count / args.rate:
        raise ValueError("case-timeout must exceed workload submission time")
    if args.phase == "performance" and not args.correctness_report:
        raise ValueError("--phase performance requires --correctness-report with matching PASS evidence")
    raw = w.baseline_config(c.read(args.config));reference = c.validate(raw)
    barrier = w.parameter_config(raw, 1, args.empty_ms)
    barrier["consensus"]["ancestor_streaming"] = False
    comparison_arbor = c.validate(barrier)
    if arbor_batch_wait is not None:
        barrier["consensus"]["cross_shard_batch_wait_ms"] = arbor_batch_wait
    streaming = copy.deepcopy(barrier);streaming["consensus"]["ancestor_streaming"] = True
    lower_first = copy.deepcopy(streaming);lower_first["consensus"]["ancestor_order"] = "lower_first"
    lower_2 = copy.deepcopy(lower_first);lower_2["consensus"]["ancestor_lower_blocks_per_root"] = 2
    lower_10 = copy.deepcopy(lower_first);lower_10["consensus"]["ancestor_lower_blocks_per_root"] = 10
    lower_bundle = copy.deepcopy(lower_2);lower_bundle["consensus"]["ancestor_leaf_batch_blocks"] = 2
    configurations = dict(barrier=barrier, streaming=streaming, lower_first=lower_first, lower_2=lower_2, lower_10=lower_10,
                          lower_bundle=lower_bundle,
                          saguaro=raw, sharper=raw,
                          ahl=w.baseline_config(c.read(args.ahl_config)))
    configurations = {name: configurations[name] for name in cases}
    normalized = {name: c.validate_method_config(c.validate(cfg), method_for_case(name))
                  for name, cfg in configurations.items()}
    c.integer(args.batch, "batch", 1, min(cfg["consensus"]["cross_shard_batch_size"] for cfg in normalized.values()))
    reference_arbor = c.validate(barrier)
    costs = {name: w.validate_costs(comparison_arbor, normalized[name], name)
             for name in cases if name in BASELINE_CASES}
    for name, cost in costs.items():
        cost["actual_config_fingerprints"]["arbor"] = b.config_fingerprint(reference_arbor)
        cost["actual_arbor_case_fingerprints"] = {case: b.config_fingerprint(normalized[case])
                                                for case in cases if case in ARBOR_CASES}
        cost["actual_arbor_cross_shard_batch_wait_ms"] = reference_arbor["consensus"]["cross_shard_batch_wait_ms"]
        cost["baseline_cross_shard_batch_wait_ms"] = normalized[name]["consensus"]["cross_shard_batch_wait_ms"]
        configured_wait = comparison_arbor["consensus"]["cross_shard_batch_wait_ms"]
        if arbor_batch_wait is not None and arbor_batch_wait != configured_wait:
            # Only this explicit Arbor tuning override is normalized for shared
            # cost validation. Actual runtime snapshots retain the new timeout.
            cost["protocol_difference_allowlist"].append("consensus.cross_shard_batch_wait_ms")
            cost["arbor_batch_wait_override"] = dict(configured_ms=configured_wait,
                effective_ms=arbor_batch_wait,
                baseline_ms=normalized[name]["consensus"]["cross_shard_batch_wait_ms"])
    if any(w.common_cost_config(normalized[name]) != w.common_cost_config(reference_arbor)
           for name in cases if name in ARBOR_CASES):
        raise ValueError("Arbor configurations differ outside allowed ordering settings")
    for cfg in normalized.values():
        if cfg["consensus"]["view_timeout_ms"] != reference["consensus"]["view_timeout_ms"]:
            raise ValueError("all methods must retain the same original view_timeout_ms")
    workloads = {phase: locality.prepare_locality_workload(reference, count, args.rate, args.batch,
                    args.seed, args.case_timeout, cross_cluster_ratio=.05, three_shard_ratio=.10)
                 for phase, count in (("correctness", args.correctness_count), ("performance", args.count))}
    for name, cfg in normalized.items():
        for workload in workloads.values():
            mixed.expectations(cfg, workload, method_for_case(name))
    return configurations, normalized, workloads, costs


def save(folder, report, started):
    report["elapsed_wall_s"] = time.monotonic() - started
    c.write(folder / "summary.json", report)
    fields = ("phase", "case", "method", "status", "valid_performance_sample", "count", "elapsed_wall_s", *b.METRICS,
              "failure_reasons")
    with (folder / "summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore");writer.writeheader()
        for row in report["cases"]:
            writer.writerow(dict(row, failure_reasons=json.dumps(row["failure_reasons"], ensure_ascii=False)))
    lines = ["# 短周期正确性与吞吐测试", "", f"状态：{report['status']}；总耗时 {report['elapsed_wall_s']:.2f} s", "",
        f"每阶段 {len(report['settings']['cases'])} 种配置串行复用同一 unsigned 输入，各次使用全新状态；不注入故障、不重试。", "",
        "| 阶段 | 配置 | 状态 | TPS | 耗时 s |", "|---|---|---|---:|---:|"]
    for row in report["cases"]:
        tps = row.get("completed_tps")
        lines.append(f"| {row['phase']} | {row['case']} | {row['status']} | " +
                     ("—" if tps is None else f"{tps:.3f}") + f" | {row.get('elapsed_wall_s', 0):.2f} |")
    lines += ["", "TPS仅显示完整PASS样本，使用客户端完成时间；整轮预算包括启动、排空和清理。",
              "Leaf profile的等待时间与CPU处理时间口径不同，详见每个case的summary.json。", ""]
    for row in report["cases"]:
        lines.extend(f"- {row['phase']}/{row['case']}：{reason}" for reason in row["failure_reasons"])
    lines.extend("- " + reason for reason in report["failure_reasons"])
    (folder / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def reuse_correctness(path, report):
    prior = c.read(path)
    for name in ("source_sha256", "binary_sha256", "correctness_workload_sha256"):
        if prior.get(name) != report[name]:
            raise ValueError(f"correctness evidence mismatch: {name}")
    cases = tuple(report["settings"]["cases"])
    if any(prior.get("config_sha256", {}).get(name) != report["config_sha256"][name] for name in cases):
        raise ValueError("correctness evidence mismatch: config_sha256")
    workload = Path(path).resolve().parent / "correctness/shared-workload.json"
    if not workload.is_file() or w.digest(workload) != report["correctness_workload_sha256"]:
        raise ValueError("correctness workload evidence is missing or changed")
    rows = [row for row in prior.get("cases", []) if row.get("phase") == "correctness" and row.get("case") in cases]
    if (len(rows) != len(cases) or {row["case"] for row in rows} != set(cases) or
            any(row.get("status") != "PASS" or row.get("valid_performance_sample") is not True for row in rows)):
        raise ValueError("all selected matching correctness cases must have passed")
    if any(row.get("order_audit", {}).get("status") != "PASS" for row in rows if row["case"] in ARBOR_CASES):
        raise ValueError("correctness evidence lacks applied ancestor order audit")
    report["reused_correctness"] = dict(path=str(Path(path).resolve()), sha256=w.digest(path), cases=rows)


def run_case(snapshot, shared, output, name, cfg, report, deadline, args):
    method = method_for_case(name)
    binary = c.binary_for_method(method)
    if w.digest(binary) != report["binary_sha256"][method] or w.source_hashes() != report["source_sha256"]:
        raise ValueError("source or binary changed after benchmark preparation")
    if w.digest(snapshot) != report["config_sha256"][name]:
        raise ValueError("configuration snapshot changed")
    expected_sha = report["workloads"][output.parent.name]["sha256"]
    if w.digest(shared) != expected_sha:
        raise ValueError("shared unsigned workload changed")
    guard = guards.RunGuard(overall_deadline=deadline, case_seconds=args.case_timeout,
                            no_progress_seconds=args.no_progress_timeout)
    result = mixed.run(snapshot, shared, output, method, drain_timeout=args.drain_timeout, guard=guard)
    row = {key: value for key, value in result.items() if key != "client"}
    row["client"] = {key: value for key, value in result.get("client", {}).items()
                     if value is None or type(value) in (str, bool, int, float)}
    errors = list(result.get("failure_reasons", []))
    if result.get("method") != method or result.get("config_fingerprint") != b.config_fingerprint(cfg):
        errors.append("actual method/configuration fingerprint mismatch")
    if not result.get("input_workload_sha256") == result.get("workload_sha256") == expected_sha == w.digest(shared):
        errors.append("actual replay differs from shared unsigned workload")
    if w.digest(binary) != report["binary_sha256"][method] or w.source_hashes() != report["source_sha256"]:
        errors.append("source/binary changed during case")
    valid = result.get("status") == "PASS" and result.get("valid_performance_sample") is True and not errors
    if valid and any(not b.finite_number(result.get(metric), metric == "completed_tps") for metric in b.METRICS):
        valid = False;errors.append("missing complete finite performance metrics")
    if valid and name in ARBOR_CASES:
        try:
            row["order_audit"] = audit_leaf_order(result["run_dir"], cfg, name)
        except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
            valid = False;errors.append(f"applied order audit: {error}")
    row.update(status="PASS" if valid else "INVALID", valid_performance_sample=valid, failure_reasons=errors)
    if not valid:
        row.update({metric: None for metric in b.METRICS})
        for metric in b.METRICS:
            row["client"].pop(metric, None)
        row["partial_metrics_evidence"] = result.get("client_result")
    row["count"] = mixed.expectations(cfg, c.read(shared), method)["transactions"]
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_lower_first.json")
    parser.add_argument("--ahl-config", type=Path, default=ROOT / "config/ahl_two_layer_locality.json")
    parser.add_argument("--phase", choices=("correctness", "performance", "all"), default="all")
    parser.add_argument("--cases", nargs="+", choices=CASES,
                        help="default: lower_first saguaro sharper ahl (Arbor 1:1 immediate lower to Root); baselines run first for performance")
    parser.add_argument("--correctness-report", type=Path)
    parser.add_argument("--correctness-count", type=int, default=200)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--rate", type=float, default=1000)
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--empty-ms", type=int, default=50)
    parser.add_argument("--arbor-cross-shard-batch-wait-ms", type=int,
                        help="override Arbor business batch deadline only; baseline settings stay as configured")
    parser.add_argument("--budget", type=float, default=300)
    parser.add_argument("--case-timeout", type=float, default=60)
    parser.add_argument("--no-progress-timeout", type=float, default=15)
    parser.add_argument("--drain-timeout", type=float, default=10)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    started = time.monotonic();deadline = started + args.budget - CLEANUP_RESERVE
    configurations, normalized, workloads, costs = prepare(args)
    folder = (args.output_dir or ROOT / "test-results" / ("quick-" +
        datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    phases = ("correctness", "performance") if args.phase == "all" else (args.phase,)
    report = dict(schema_version=2, kind="bounded-configuration-diagnostics", status="RUNNING", finished=False,
        created_at=datetime.datetime.now().astimezone().isoformat(), environment=dict(host=platform.node(),
            system=platform.platform(), cpus=os.cpu_count()), settings={key: getattr(args, key) for key in
            ("phase", "correctness_count", "count", "rate", "batch", "seed", "empty_ms", "budget", "case_timeout",
            "no_progress_timeout", "drain_timeout", "arbor_cross_shard_batch_wait_ms")}, costs=costs, cases=[], workloads={}, failure_reasons=[])
    report["settings"]["cases"] = list(selected_cases(args))
    snapshots = {}
    for name, cfg in configurations.items():
        snapshots[name] = folder / f"config-{name}.json";c.write(snapshots[name], cfg)
    report["config_sha256"] = {name: w.digest(path) for name, path in snapshots.items()}
    for phase, workload in workloads.items():
        directory = folder / phase;directory.mkdir()
        shared = directory / "shared-workload.json";c.write(shared, workload)
        report["workloads"][phase] = dict(path=str(shared), sha256=w.digest(shared))
    report["correctness_workload_sha256"] = report["workloads"]["correctness"]["sha256"]
    report["cases"] = [dict(phase=phase, case=name, method=method_for_case(name),
        status="NOT_RUN", valid_performance_sample=False, failure_reasons=[], **{metric: None for metric in b.METRICS})
        for phase in phases for name in (BASELINE_CASES + ARBOR_CASES if phase == "performance" else CASES)
        if name in configurations]
    save(folder, report, started)
    print(f"结果目录：{folder}", flush=True)
    try:
        if not args.skip_build:
            subprocess.run(["make", *("build/bin/" + name + "_node" for name in METHODS)], cwd=ROOT,
                           check=True, timeout=max(.1, deadline - time.monotonic()))
        report["source_sha256"] = w.source_hashes()
        report["binary_sha256"] = {method: w.digest(c.binary_for_method(method)) for method in METHODS}
        if args.phase == "performance":
            reuse_correctness(args.correctness_report, report)
        for index, planned in enumerate(report["cases"]):
            if deadline - time.monotonic() < min(30, args.case_timeout):
                report["status"] = "BUDGET_EXHAUSTED"
                report["failure_reasons"].append("overall budget insufficient to start another case; no retry")
                break
            phase, name = planned["phase"], planned["case"]
            shared = Path(report["workloads"][phase]["path"])
            print(f"{phase} {name}: one shared input, fresh state", flush=True)
            case_started = time.monotonic()
            try:
                row = run_case(snapshots[name], shared, folder / phase / name, name, normalized[name], report,
                               deadline, args)
            except (KeyboardInterrupt, SystemExit) as error:
                report["cases"][index].update(status="INVALID", valid_performance_sample=False,
                    elapsed_wall_s=time.monotonic() - case_started,
                    failure_reasons=[f"{type(error).__name__}: interrupted; owned nodes cleaned up"],
                    partial_metrics_evidence=str(folder / phase / name / "summary.json"))
                raise
            except Exception as error:
                row = dict(planned, status="INVALID", failure_reasons=[f"{type(error).__name__}: {error}"])
            row.update(phase=phase, case=name, method=planned["method"], elapsed_wall_s=time.monotonic() - case_started)
            report["cases"][index] = row
            if row["status"] != "PASS":
                report["status"] = "INVALID"
                report["failure_reasons"].append(f"stopped after {phase}/{name}; later cases were not run")
            save(folder, report, started)
            if row["status"] != "PASS":
                break
        else:
            report.update(status="PASS", finished=True)
    except BaseException as error:
        report.update(status="INVALID")
        report["failure_reasons"].append(f"{type(error).__name__}: {error}")
        save(folder, report, started)
        raise
    report["completed_at"] = datetime.datetime.now().astimezone().isoformat();save(folder, report, started)
    print(f"{report['status']} total_elapsed_s={report['elapsed_wall_s']:.2f}; {folder / 'summary.md'}", flush=True)
    return 0 if report["status"] == "PASS" else 2


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr);sys.exit(1)
