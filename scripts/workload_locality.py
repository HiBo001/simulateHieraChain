#!/usr/bin/env python3
"""Deterministic cross-shard workloads with a reference tree's access locality.

Participant sets are sampled uniformly among leaf subsets for the requested
arity/locality bucket, once per homogeneous client request. Transactions in a
request consequently share their participant set. No list of all C(n, 3)
subsets is materialized, and rare cross-cluster subsets need no rejection loop.
"""
import copy
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import json
import math
import random

import cluster as c


def _configuration(cfg):
    if not isinstance(cfg, dict):
        raise ValueError("参考分片配置必须为 JSON 对象")
    raw = copy.deepcopy(cfg)
    if isinstance(raw.get("network"), dict):
        raw["network"].pop("resolved_links", None)
    try:
        return c.validate(raw)
    except (TypeError, KeyError) as exc:
        raise ValueError("参考分片配置格式无效") from exc


def leaf_clusters(cfg):
    """Return {parent-id string: sorted leaves}, using only direct parents."""
    parent, leaves = c.topology(_configuration(cfg))
    return _group_reference({str(sid): parent[sid] for sid in leaves})


def _group_reference(reference):
    grouped = {}
    for sid, ancestor in reference.items():
        key = "null" if ancestor is None else str(ancestor)
        grouped.setdefault(key, []).append(int(sid))
    for group in grouped.values():
        group.sort()
    return dict(sorted(grouped.items(), key=lambda item: (item[0] == "null", int(item[0]) if item[0] != "null" else 0)))


def _reference(cfg, reference_parent=None):
    parent, leaves = c.topology(cfg)
    if reference_parent is None:
        return {str(sid): parent[sid] for sid in leaves}
    if not isinstance(reference_parent, dict) or set(reference_parent) != {str(sid) for sid in leaves}:
        raise ValueError("locality.reference_parent 的键必须恰好是当前配置的所有叶子 ID 字符串")
    leaf_set = set(leaves)
    for ancestor in reference_parent.values():
        if type(ancestor) is not int or ancestor <= 0 or ancestor in leaf_set:
            raise ValueError("locality.reference_parent 的父节点必须为正整数且不能是叶子分片")
    return {str(sid): reference_parent[str(sid)] for sid in leaves}


def _ratio(value, name):
    if type(value) not in (int, float) or not 0 <= value <= 1 or not math.isfinite(value):
        raise ValueError(f"{name} 必须为 0..1 范围内的有限数")
    return value


def _positive(value, name):
    try:
        valid = type(value) in (int, float) and value > 0 and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(f"{name} 必须为有限正数")
    return value


def _round_count(total, ratio):
    return int((Decimal(total) * Decimal(str(ratio))).quantize(Decimal(1), rounding=ROUND_HALF_UP))


def _bucket_counts(count, cross_cluster_ratio, three_shard_ratio):
    three = _round_count(count, three_shard_ratio)
    two = count - three
    cross = _round_count(count, cross_cluster_ratio)
    # Preserve both rounded margins; assign the cross/three intersection by
    # round-half-up and clamp only to the mathematically possible intersection.
    cross_three = int((Decimal(cross) * Decimal(three) / Decimal(count)).quantize(
        Decimal(1), rounding=ROUND_HALF_UP))
    cross_three = min(min(cross, three), max(max(0, cross - two), cross_three))
    return {"intra2": two - cross + cross_three, "cross2": cross - cross_three,
            "intra3": three - cross_three, "cross3": cross_three}


def _choose_index(weights, rng):
    total = sum(weights)
    if total <= 0:
        raise ValueError("该局部性桶没有可用的参与分片组合")
    ticket = rng.randrange(total)
    for index, weight in enumerate(weights):
        if ticket < weight:
            return index
        ticket -= weight
    raise AssertionError("weighted participant sampler overflow")


class _ParticipantSampler:
    """O(number of clusters) weighted sampling, with exact subset weights."""

    def __init__(self, clusters):
        self.clusters = list(clusters.values())
        self.sizes = [len(leaves) for leaves in self.clusters]
        self.total = sum(self.sizes)
        self.intra = {arity: [math.comb(size, arity) if size >= arity else 0
                              for size in self.sizes] for arity in (2, 3)}
        count = len(self.sizes)
        self.suffix_total = [0] * (count + 1)
        self.suffix_pairs = [0] * (count + 1)
        for index in range(count - 1, -1, -1):
            size = self.sizes[index]
            self.suffix_total[index] = size + self.suffix_total[index + 1]
            self.suffix_pairs[index] = (self.suffix_pairs[index + 1]
                                        + size * self.suffix_total[index + 1])
        self.cross_pair_first = [size * self.suffix_total[index + 1]
                                 for index, size in enumerate(self.sizes)]
        self.cross_three_double = [(math.comb(size, 2) if size >= 2 else 0) * (self.total - size)
                                   for size in self.sizes]
        self.cross_three_distinct = [size * self.suffix_pairs[index + 1]
                                     for index, size in enumerate(self.sizes)]
        self.available = {"intra2": sum(self.intra[2]), "intra3": sum(self.intra[3]),
                          "cross2": sum(self.cross_pair_first),
                          "cross3": sum(self.cross_three_double) + sum(self.cross_three_distinct)}

    def sample(self, bucket, rng):
        arity = int(bucket[-1])
        if bucket.startswith("intra"):
            index = _choose_index(self.intra[arity], rng)
            return sorted(rng.sample(self.clusters[index], arity))
        if arity == 2:
            first = _choose_index(self.cross_pair_first, rng)
            weights = [size if index > first else 0 for index, size in enumerate(self.sizes)]
            second = _choose_index(weights, rng)
            return sorted([rng.choice(self.clusters[first]), rng.choice(self.clusters[second])])
        pattern = _choose_index([sum(self.cross_three_double), sum(self.cross_three_distinct)], rng)
        if pattern == 0:
            doubled = _choose_index(self.cross_three_double, rng)
            singleton = _choose_index([size if index != doubled else 0
                                        for index, size in enumerate(self.sizes)], rng)
            return sorted(rng.sample(self.clusters[doubled], 2) + [rng.choice(self.clusters[singleton])])
        first = _choose_index(self.cross_three_distinct, rng)
        second = _choose_index([size * self.suffix_total[index + 1] if index > first else 0
                                for index, size in enumerate(self.sizes)], rng)
        third = _choose_index([size if index > second else 0 for index, size in enumerate(self.sizes)], rng)
        return sorted([rng.choice(self.clusters[index]) for index in (first, second, third)])


def workload_statistics(cfg, workload, *, reference_parent=None):
    """Count actual transactions against the Arbor reference tree, not hints.

    This function does not infer locality from request.target: AHL may reroute
    that hint to its single root while keeping the exact participant workload.
    """
    cfg = _configuration(cfg)
    _, leaves = c.topology(cfg)
    leaf_set = set(leaves)
    reference = _reference(cfg, reference_parent)
    clusters = _group_reference(reference)
    if not isinstance(workload, dict) or not isinstance(workload.get("requests"), list) or not workload["requests"]:
        raise ValueError("负载 requests 必须为非空数组")
    groups, sizes = {}, {}
    buckets = dict.fromkeys(("intra2", "cross2", "intra3", "cross3"), 0)
    seen_requests, seen_transactions = set(), set()
    total, intra, max_batch = 0, 0, 0
    for request in workload["requests"]:
        if not isinstance(request, dict):
            raise ValueError("请求必须为 JSON 对象")
        rid, txs = request.get("id"), request.get("txs")
        if not isinstance(rid, str) or not rid or rid in seen_requests:
            raise ValueError("请求 ID 缺失或重复")
        seen_requests.add(rid)
        if not isinstance(txs, list) or not txs:
            raise ValueError("请求 txs 必须为非空数组")
        max_batch = max(max_batch, len(txs))
        request_group = None
        for tx in txs:
            if not isinstance(tx, dict):
                raise ValueError("交易必须为 JSON 对象")
            tid, ps = tx.get("id"), tx.get("participants")
            if not isinstance(tid, str) or not tid or tid in seen_transactions:
                raise ValueError("交易 ID 缺失或重复")
            seen_transactions.add(tid)
            if (not isinstance(ps, list) or any(type(sid) is not int for sid in ps)
                    or len(ps) < 2 or len(set(ps)) != len(ps) or ps != sorted(ps)
                    or not set(ps).issubset(leaf_set)):
                raise ValueError("participants 必须是升序、互异且至少两个的参考树叶子分片")
            group = tuple(ps)
            if request_group is not None and request_group != group:
                raise ValueError("同一请求内的交易必须具有相同的参与分片")
            request_group = group
            is_intra = len({reference[str(sid)] for sid in ps}) == 1
            bucket = ("intra" if is_intra else "cross") + str(len(ps))
            buckets[bucket] = buckets.get(bucket, 0) + 1
            label = ",".join(map(str, ps))
            groups[label] = groups.get(label, 0) + 1
            sizes[str(len(ps))] = sizes.get(str(len(ps)), 0) + 1
            total += 1
            intra += is_intra
    cross = total - intra
    return {"schema_version": 1, "clusters": clusters, "cluster_count": len(clusters), "reference_parent": reference,
            "transactions": total, "requests": len(workload["requests"]),
            "intra_cluster_transactions": intra, "cross_cluster_transactions": cross,
            "cross_cluster_ratio": cross / total,
            "three_shard_ratio": sizes.get("3", 0) / total,
            "participants_per_transaction": dict(sorted(sizes.items())),
            "groups": dict(sorted(groups.items())), "arity_locality_counts": buckets,
            "max_request_transactions": max_batch}


def _same_statistic(left, right):
    """Strict integer counts: JSON true must never satisfy an expected 1."""
    if isinstance(right, dict):
        return (isinstance(left, dict) and set(left) == set(right)
                and all(_same_statistic(left[key], value) for key, value in right.items()))
    if isinstance(right, list):
        return (isinstance(left, list) and len(left) == len(right)
                and all(_same_statistic(a, b) for a, b in zip(left, right)))
    if type(right) is int:
        return type(left) is int and left == right
    if type(right) is float:
        try:
            return type(left) in (int, float) and math.isfinite(left) and left == right
        except OverflowError:
            return False
    return type(left) is type(right) and left == right


def validate_locality_metadata(cfg, workload, method="arbor"):
    """Reject stale/forged locality labels and return recomputed statistics.

    AHL keeps the Arbor reference parents even though its runtime topology is
    flat. Other methods must use the exact direct parents of their input tree.
    Legacy unsigned files without a locality object remain supported.
    """
    if not isinstance(workload, dict):
        raise ValueError("负载必须为 JSON 对象")
    if "locality" not in workload:
        return None
    if method not in c.METHODS:
        raise ValueError(f"未知方法 {method!r}")
    metadata = workload["locality"]
    if not isinstance(metadata, dict):
        raise ValueError("负载 locality 必须为 JSON 对象")
    cfg = _configuration(cfg)
    saved_parent = metadata.get("reference_parent")
    if saved_parent is None:
        raise ValueError("locality 缺少 reference_parent")
    reference = _reference(cfg, saved_parent)
    if method != "ahl" and reference != _reference(cfg):
        raise ValueError("locality.reference_parent 与 Arbor 参考树的即刻父分片不一致")
    stats = workload_statistics(cfg, workload, reference_parent=reference)
    for field, value in stats.items():
        if field not in metadata or not _same_statistic(metadata[field], value):
            raise ValueError(f"locality.{field} 与真实交易统计不一致")
    requested = ("requested_cross_cluster_ratio", "requested_three_shard_ratio")
    if any(field in metadata for field in requested):
        if not all(field in metadata for field in requested):
            raise ValueError("locality 必须同时声明两个 requested 比例")
        cross = _ratio(metadata[requested[0]], requested[0])
        three = _ratio(metadata[requested[1]], requested[1])
        if stats["arity_locality_counts"] != _bucket_counts(stats["transactions"], cross, three):
            raise ValueError("locality 的四桶计数不符合 requested 比例的确定性舍入规则")
    if "batch" in metadata:
        c.integer(metadata["batch"], "locality.batch", 1, cfg["consensus"]["cross_shard_batch_size"])
        if stats["max_request_transactions"] > metadata["batch"]:
            raise ValueError("locality.batch 小于实际请求交易数")
    return stats


def prepare_locality_workload(cfg, count=10000, rate=5000, batch=10, seed=42, timeout=None,
                              *, cross_cluster_ratio=.05, three_shard_ratio=.10):
    """Build 100% cross-shard traffic with exact rounded arity/locality margins.

    Four buckets are divided into client batches first, then each batch samples
    its participants. With default 10000/10 this produces exactly 1000 requests
    and counts intra2=8550, cross2=450, intra3=950, cross3=50.
    """
    cfg = _configuration(cfg)
    c.integer(count, "count", 1, 100000000)
    c.integer(batch, "跨片 batch", 1, cfg["consensus"]["cross_shard_batch_size"])
    _positive(rate, "rate")
    if type(seed) is not int:
        raise ValueError("seed 必须为整数")
    timeout = _positive(count / rate + 180 if timeout is None else timeout, "timeout")
    cross_cluster_ratio = _ratio(cross_cluster_ratio, "cross_cluster_ratio")
    three_shard_ratio = _ratio(three_shard_ratio, "three_shard_ratio")
    counts = _bucket_counts(count, cross_cluster_ratio, three_shard_ratio)
    clusters = leaf_clusters(cfg)
    sampler = _ParticipantSampler(clusters)
    for bucket, transactions in counts.items():
        if transactions and not sampler.available[bucket]:
            message = ("同 cluster" if bucket.startswith("intra") else "跨 cluster") + f" {bucket[-1]} 方交易"
            raise ValueError(f"参考拓扑不能生成 {message}（需要 {transactions} 笔）；请扩大 cluster 或调整拓扑/比例")
    reference = {str(sid): parent for parent, group in clusters.items() for sid in group}
    topology_tag = hashlib.sha256(json.dumps(reference, sort_keys=True).encode()).hexdigest()[:12]
    rng = random.Random(seed)
    workload = {"rate": rate, "timeout_s": timeout, "seed": seed, "requests": []}
    for bucket, transactions in counts.items():
        remaining, index = transactions, 0
        while remaining:
            amount = min(batch, remaining)
            ps = sampler.sample(bucket, rng)
            prefix = f"locality:{seed}:{topology_tag}:{bucket}:{index}"
            part = c.prepare_workload(cfg, amount, rate, rng.getrandbits(64), prefix,
                                      participants=ps, batch=batch, timeout=timeout)
            workload["requests"].extend(part["requests"])
            remaining -= amount
            index += 1
    rng.shuffle(workload["requests"])
    last_request_count = len(workload["requests"][-1]["txs"])
    if timeout <= (count - last_request_count) / rate:
        raise ValueError("timeout 太短，负载尚未发完就会结束；请增大 timeout 或提高 rate")
    stats = workload_statistics(cfg, workload)
    stats.update({"requested_cross_cluster_ratio": cross_cluster_ratio,
                  "requested_three_shard_ratio": three_shard_ratio,
                  "batch": batch,
                  "rounding": "round-half-up arity/cross margins; round and clamp their intersection",
                  "sampling": "uniform leaf subsets conditioned on arity and locality, once per homogeneous request"})
    workload["locality"] = stats
    return workload
