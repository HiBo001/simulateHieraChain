#!/usr/bin/env python3
"""Compare four methods on one Arbor-defined, locality-aware business workload."""
import argparse
import copy
import csv
import datetime
import hashlib
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import cluster as c
import benchmark as b
import benchmark_mixed as mixed
import compare
import compare_mixed
import workload_locality as locality

METHODS = ('arbor', 'saguaro', 'sharper', 'ahl')
LABELS = dict(arbor='Arbor', saguaro='Saguaro', sharper='SharPer', ahl='AHL')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(folder, report):
    """Never aggregate an incomplete repeat set or hide an unsuccessful run."""
    report['aggregates'] = []
    for method in METHODS:
        rows = [row for row in report['cases'] if row['method'] == method]
        complete = len(rows) == report['repeat']
        passed = all(row['status'] == 'PASS' for row in rows)
        same_input = all(row['input_workload_sha256'] == row['workload_sha256']
                         == report['workload_sha256'] for row in rows)
        usable = complete and passed and same_input
        aggregate = dict(method=method, runs=len(rows), passed=sum(row['status'] == 'PASS' for row in rows),
                         expected_runs=report['repeat'], status='PASS' if usable else
                         'FAIL' if not passed or not same_input else 'INCOMPLETE')
        for field in b.METRICS:
            aggregate['median_' + field] = statistics.median(row[field] for row in rows) if usable else None
        aggregate['min_completed_tps'] = min(row['completed_tps'] for row in rows) if usable else None
        aggregate['max_completed_tps'] = max(row['completed_tps'] for row in rows) if usable else None
        report['aggregates'].append(aggregate)
    report['paired_aggregates'] = compare.summarize_pairs(report['method_comparisons'])
    for group in report['paired_aggregates']:
        if group['runs'] != report['repeat']:
            baseline = group['baseline']
            for key in list(group):
                if key.startswith('median_') or key == 'arbor_over_' + baseline + '_tps':
                    del group[key]
            group.update(comparable=False, reason='轮次未完整结束，整组比值留空')
    finished = len(report['cases']) == 4 * report['repeat'] and len(report['method_comparisons']) == 3 * report['repeat']
    failures = report['failure_reasons'] or any(row['status'] != 'PASS' for row in report['cases'])
    report['status'] = 'FAIL' if failures else 'PASS' if finished and all(
        group['comparable'] for group in report['paired_aggregates']) else 'INCOMPLETE'
    c.write(folder / 'summary.json', report)
    columns = ('method', 'repeat', 'status', 'count', 'executed_transactions', 'elapsed_s',
               *b.METRICS, 'workload_sha256', 'config_fingerprint', 'failure_reasons', 'run_dir')
    with (folder / 'summary.csv').open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=columns, extrasaction='ignore')
        writer.writeheader()
        for row in report['cases']:
            writer.writerow(dict(row, failure_reasons='; '.join(row['failure_reasons'])))
    stats = report['locality']
    lines = ['# Arbor / Saguaro / SharPer / AHL 同负载比较', '',
             f"比较状态：{report['status']}；每种方法 {report['repeat']} 轮。", '',
             f"以 Arbor 父关系定义 cluster：{stats['clusters']}。",
             f"业务交易 {stats['transactions']} 笔；cluster 内 {stats['intra_cluster_transactions']} 笔；"
             f"跨 cluster {stats['cross_cluster_transactions']} 笔（{stats['cross_cluster_ratio']:.2%}）；"
             f"参与片数分布 {stats['participants_per_transaction']}。", '',
             '四种方法顺序启动独立集群，读取同一 unsigned 文件。AHL 的两层拓扑不会重新定义业务 cluster。',
             '全部重复轮次完整执行、副本收敛、队列和锁排空、实际文件 SHA256 一致，才汇总 TPS 和秒单位延时。', '',
             '| 方法 | 完整通过 | TPS 中位数 | TPS 范围 | 平均延时中位数（秒） | p95 中位数（秒） |',
             '|---|---:|---:|---:|---:|---:|']
    for row in report['aggregates']:
        prefix = f"| {LABELS[row['method']]} | {row['passed']}/{report['repeat']}"
        if row['status'] == 'PASS':
            lines.append(prefix + f" | {row['median_completed_tps']:.3f} | {row['min_completed_tps']:.3f}–"
                         f"{row['max_completed_tps']:.3f} | {row['median_avg_latency_s']:.6f} | {row['median_p95_s']:.6f} |")
        else:
            lines.append(prefix + ' | — | — | — | — |')
    lines += ['', '配对比较：', '']
    for group in report['paired_aggregates']:
        baseline = group['baseline']
        lines.append(f"- Arbor / {LABELS[baseline]} TPS 中位数比："
                     + (f"{group['arbor_over_' + baseline + '_tps']:.3f}" if group['comparable'] else group['reason']))
    lines += ['', '逐轮结果：', '', '| 轮次 | 方法 | 状态 | 完成交易 | TPS | 平均延时（秒） | p95（秒） |',
              '|---|---|---|---:|---:|---:|---:|']
    for row in report['cases']:
        values = ['—' if row.get(field) is None else f"{row[field]:.6f}" for field in
                  ('completed_tps', 'avg_latency_s', 'p95_s')]
        lines.append(f"| {row['repeat']} | {LABELS[row['method']]} | {row['status']} | "
                     f"{row['executed_transactions']}/{row['count']} | " + ' | '.join(values) + ' |')
    for row in report['cases']:
        for message in row.get('failure_reasons', []) + row.get('notes', []):
            lines.append(f"- 轮次 {row['repeat']} / {LABELS[row['method']]}：{message}")
    lines += ['', '拓扑和实现范围：', '']
    for method, config in report['method_configs'].items():
        lines += [f"{LABELS[method]}：", '', '```text', c.format_topology(config['config']), '```', '']
    lines += ['AHL 节点数量及层级不同，这是本次架构比较的一部分。SharPer 保留当前实现的升序预约及恢复能力限制。',
              '网络延时、共识与执行成本以及叶子 ID 已通过配对检查。原始配置、负载 SHA256、每轮完整状态与失败记录见 summary.json。', '']
    (folder / 'summary.md').write_text('\n'.join(lines))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=ROOT / 'config/three_layer_locality.json')
    parser.add_argument('--ahl-config', type=Path, default=ROOT / 'config/ahl_two_layer_locality.json')
    compare_mixed.add_workload_arguments(parser)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--drain-timeout', type=float, default=60)
    parser.add_argument('--output-dir', type=Path, help='必须是尚不存在的新目录')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args(argv)
    c.integer(args.repeat, 'repeat', 1, 100)
    if not b.finite_number(args.drain_timeout, True) or (args.timeout is not None and not b.finite_number(args.timeout, True)):
        raise ValueError('timeout 和 drain-timeout 必须是有限正数')
    source, ahl_source = c.read(args.config), c.read(args.ahl_config)
    cfg, ahl_cfg = c.validate(source), c.validate(ahl_source)
    configs = dict(arbor=cfg, saguaro=cfg, sharper=cfg, ahl=ahl_cfg)
    contexts = {method: compare.validate_comparison_configs(cfg, configs[method], method,
                separate_config=method == 'ahl') for method in METHODS if method != 'arbor'}
    workload, expected = compare_mixed.load_or_prepare_workload(cfg, args)
    for method in METHODS:
        mixed.expectations(configs[method], workload, method)
    if not args.skip_build:
        subprocess.run(['make', *(f'build/bin/{method}_node' for method in METHODS)], cwd=ROOT, check=True)
    binaries = {method: c.binary_for_method(method) for method in METHODS}
    for binary in binaries.values():
        if not binary.is_file():
            raise ValueError(f'找不到 {binary}，请先 make')
    folder = (args.output_dir or ROOT / 'test-results' / ('compare-all-' +
        datetime.datetime.now().strftime('%Y%m%d-%H%M%S-') + uuid.uuid4().hex[:8])).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    shared = folder / 'shared-workload.json'
    c.write(shared, workload)
    arbor_snapshot, ahl_snapshot = folder / 'config-snapshot.json', folder / 'ahl-config-snapshot.json'
    c.write(arbor_snapshot, source)
    c.write(ahl_snapshot, ahl_source)
    snapshots = dict(arbor=arbor_snapshot, saguaro=arbor_snapshot, sharper=arbor_snapshot, ahl=ahl_snapshot)
    report = dict(schema_version=1, kind='four-methods-exact-workload', repeat=args.repeat,
                  expected_cases=4 * args.repeat, expected_pairs=3 * args.repeat,
                  created_at=datetime.datetime.now().astimezone().isoformat(),
                  environment=dict(host=platform.node(), system=platform.platform(), cpus=os.cpu_count()),
                  revision=subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
                  working_tree_dirty=bool(subprocess.run(['git', 'status', '--porcelain'], cwd=ROOT, capture_output=True, text=True).stdout.strip()),
                  binary_sha256={method: digest(path) for method, path in binaries.items()},
                  workload_source=str(args.workload.resolve()) if args.workload else None,
                  input_workload_sha256=digest(args.workload) if args.workload else None,
                  shared_workload=str(shared), workload_sha256=digest(shared),
                  locality=locality.workload_statistics(cfg, workload),
                  groups=expected['groups'], participants_per_transaction=expected['participants_per_transaction'],
                  comparison_contexts=contexts, cases=[], method_comparisons=[], failure_reasons=[])
    report['method_configs'] = {method: dict(config=configs[method], snapshot=str(snapshots[method]),
        snapshot_sha256=digest(snapshots[method]), fingerprint=b.config_fingerprint(configs[method])) for method in METHODS}
    print(f'四方法同负载比较结果目录：{folder}', flush=True)
    print(f"Arbor 参考 cluster：{report['locality']['clusters']}；跨 cluster "
          f"{report['locality']['cross_cluster_transactions']}/{expected['transactions']} "
          f"({report['locality']['cross_cluster_ratio']:.2%})", flush=True)
    save(folder, report)
    try:
        for repeat in range(1, args.repeat + 1):
            # Rotate the starting method; no two methods share CPU resources.
            offset = (repeat - 1) % len(METHODS)
            order = METHODS[offset:] + METHODS[:offset]
            results = {}
            for method in order:
                if digest(shared) != report['workload_sha256']:
                    raise ValueError('共享业务负载在测试中被修改')
                print(f"[{repeat}/{args.repeat}] {method} workload_sha256={report['workload_sha256']}", flush=True)
                result = mixed.run(snapshots[method], shared, folder / f'repeat-{repeat:03d}' / method,
                                   method, drain_timeout=args.drain_timeout)
                row = compare_mixed.comparison_row(result, workload, repeat)
                violations = []
                if row['method'] != method or row['config_fingerprint'] != report['method_configs'][method]['fingerprint']:
                    violations.append('实际方法或配置指纹与预校验配置不符')
                if not row.get('input_workload_sha256') == row.get('workload_sha256') == digest(shared) == report['workload_sha256']:
                    violations.append('实际读取或回放负载 SHA256 与共同输入不符')
                if violations:
                    row['status'] = 'FAIL'
                    row['failure_reasons'].extend(violations)
                    row.update({field: None for field in b.METRICS})
                report['cases'].append(row)
                results[method] = row
                save(folder, report)
            for method in METHODS[1:]:
                arbor = compare.bind_comparison(copy.deepcopy(results['arbor']), contexts[method], shared)
                baseline = compare.bind_comparison(copy.deepcopy(results[method]), contexts[method], shared)
                report['method_comparisons'].append(compare.paired_comparison(arbor, baseline, method, contexts[method]))
            save(folder, report)
    except BaseException as error:
        report['failure_reasons'].append(f'{type(error).__name__}: {error}')
        save(folder, report)
        raise
    report['completed_at'] = datetime.datetime.now().astimezone().isoformat()
    save(folder, report)
    print(f"汇总：{folder / 'summary.md'}", flush=True)
    return 0 if report['status'] == 'PASS' else 2


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, b.interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('比较已中止，已停止本轮集群，已完成数据保留。', file=sys.stderr)
        sys.exit(130)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
