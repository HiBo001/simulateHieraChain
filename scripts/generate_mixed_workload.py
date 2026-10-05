#!/usr/bin/env python3
"""Generate a shared 100% cross-shard workload from an Arbor reference tree."""
import argparse
import json
from pathlib import Path

import cluster as c
from workload_locality import prepare_locality_workload

ROOT = Path(__file__).resolve().parents[1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/three_layer_locality.json",
                        help="Arbor/Saguaro 参考树；SharPer/AHL 复用其生成的同一份业务负载")
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--rate", type=float, default=5000)
    parser.add_argument("--batch", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--cross-cluster-ratio", type=float, default=.05)
    parser.add_argument("--three-shard-ratio", type=float, default=.10)
    parser.add_argument("--output", type=Path, required=True, help="新的 unsigned JSON 路径；拒绝覆盖已有文件")
    args = parser.parse_args(argv)
    try:
        if args.output.exists():
            raise ValueError(f"负载文件已存在，拒绝覆盖: {args.output}")
        workload = prepare_locality_workload(c.read(args.config), args.count, args.rate, args.batch,
                                             args.seed, args.timeout,
                                             cross_cluster_ratio=args.cross_cluster_ratio,
                                             three_shard_ratio=args.three_shard_ratio)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation also prevents a file created after the existence
        # check from being replaced, unlike cluster.write's atomic overwrite.
        with args.output.open("x", encoding="utf-8") as output:
            output.write(json.dumps(workload, ensure_ascii=False, indent=2) + "\n")
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    stats = workload["locality"]
    print(f"已生成 {stats['transactions']} 笔跨片交易，{stats['requests']} 个请求: {args.output.resolve()}")
    print(f"cluster={stats['clusters']}；两方/三方={stats['participants_per_transaction']}")
    print(f"跨 cluster={stats['cross_cluster_transactions']} ({stats['cross_cluster_ratio']:.2%})；"
          f"四桶={stats['arity_locality_counts']}")
    return workload


if __name__ == "__main__":
    main()
