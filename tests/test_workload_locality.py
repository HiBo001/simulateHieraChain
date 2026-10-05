#!/usr/bin/env python3
"""Offline checks for exact access-locality workloads and immutable generation."""
import contextlib
import copy
import io
import itertools
import json
import math
from pathlib import Path
import random
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import generate_mixed_workload as generate
import workload_locality as locality


def config(groups=((1, 2, 8), (3, 4, 9))):
    shards = [{"id": sid, "parent": 100 + index} for index, leaves in enumerate(groups) for sid in leaves]
    shards.extend({"id": 100 + index, "parent": 1000} for index in range(len(groups)))
    shards.append({"id": 1000, "parent": None})
    return {"shards": shards, "consensus": {"batch_size": 1000, "cross_shard_batch_size": 100},
            "execution": {"fib_iterations": 1}}


class LocalityGeneration(unittest.TestCase):
    def setUp(self):
        self.cfg = config()

    def test_default_exact_four_buckets_and_all_leaves(self):
        workload = locality.prepare_locality_workload(self.cfg)
        stats = workload["locality"]
        self.assertEqual(stats["clusters"], {"100": [1, 2, 8], "101": [3, 4, 9]})
        self.assertEqual(stats["reference_parent"], {"1": 100, "2": 100, "3": 101, "4": 101, "8": 100, "9": 101})
        self.assertEqual(stats["transactions"], 10000)
        self.assertEqual(stats["requests"], 1000)
        self.assertEqual(stats["cluster_count"], 2)
        self.assertEqual(stats["participants_per_transaction"], {"2": 9000, "3": 1000})
        self.assertEqual(stats["arity_locality_counts"], {"intra2": 8550, "cross2": 450, "intra3": 950, "cross3": 50})
        self.assertEqual(stats["cross_cluster_ratio"], .05)
        self.assertEqual(stats["cross_cluster_transactions"], 500)
        self.assertEqual(stats["intra_cluster_transactions"], 9500)
        used, tx_ids, request_ids = set(), set(), set()
        validated = c.validate(self.cfg)
        for request in workload["requests"]:
            request_ids.add(request["id"])
            self.assertEqual(len(request["txs"]), 10)
            ps = request["txs"][0]["participants"]
            self.assertEqual(request["target"], c.lca(validated, ps))
            for tx in request["txs"]:
                self.assertEqual(tx["participants"], ps)
                self.assertEqual([a["shard"] for a in tx["accesses"]], ps)
                self.assertEqual(tx["key"], tx["accesses"][0]["key"])
                tx_ids.add(tx["id"])
                used.update(ps)
        self.assertEqual(used, {1, 2, 3, 4, 8, 9})
        self.assertEqual(len(tx_ids), 10000)
        self.assertEqual(len(request_ids), 1000)
        self.assertEqual(locality.validate_locality_metadata(self.cfg, workload), locality.workload_statistics(self.cfg, workload))

    def test_seed_reproduces_every_id_value_key_and_request_order(self):
        first = locality.prepare_locality_workload(self.cfg, count=1234, seed=17)
        self.assertEqual(first, locality.prepare_locality_workload(c.validate(self.cfg), count=1234, seed=17))
        changed = locality.prepare_locality_workload(self.cfg, count=1234, seed=18)
        self.assertNotEqual(first["requests"], changed["requests"])
        self.assertEqual(first["locality"]["arity_locality_counts"], changed["locality"]["arity_locality_counts"])

    def test_small_counts_have_deterministic_rounded_margins(self):
        for total in (1, 5, 9, 10, 13, 25, 51, 101, 999):
            with self.subTest(total=total):
                workload = locality.prepare_locality_workload(self.cfg, count=total, batch=8)
                stats = locality.workload_statistics(self.cfg, workload)
                buckets = stats["arity_locality_counts"]
                self.assertEqual(sum(buckets.values()), total)
                self.assertEqual(buckets, locality._bucket_counts(total, .05, .1))
                self.assertEqual(stats["cross_cluster_ratio"], (buckets["cross2"] + buckets["cross3"]) / total)
                self.assertEqual(stats["three_shard_ratio"], (buckets["intra3"] + buckets["cross3"]) / total)
                self.assertLessEqual(stats["requests"], math.ceil(total / 8) + 3)
                self.assertEqual(locality.validate_locality_metadata(self.cfg, workload), stats)

    def test_boundary_ratios_and_unavailable_buckets(self):
        for cross in (0, 1):
            for three in (0, 1):
                workload = locality.prepare_locality_workload(self.cfg, count=31,
                    cross_cluster_ratio=cross, three_shard_ratio=three)
                stats = workload["locality"]
                self.assertEqual(stats["cross_cluster_ratio"], cross)
                self.assertEqual(stats["three_shard_ratio"], three)
        cases = [(config(((1, 2), (3, 4))), {}),
                 (config(((1, 2, 3),)), {}),
                 (config(((1,), (2,), (3,))), {"three_shard_ratio": 0}),
                 (config(((1,), (2,))), {"cross_cluster_ratio": 1, "three_shard_ratio": 1})]
        for cfg, kwargs in cases:
            with self.subTest(cfg=cfg), self.assertRaisesRegex(ValueError, "参考拓扑不能生成"):
                locality.prepare_locality_workload(cfg, count=100, **kwargs)
        # A rounded zero bucket does not impose an unnecessary topology need.
        locality.prepare_locality_workload(config(((1, 2),)), count=1)

    def test_invalid_inputs_are_rejected_before_generation(self):
        for field in ("cross_cluster_ratio", "three_shard_ratio"):
            for bad in (-.1, 1.1, 10 ** 400, math.nan, math.inf, -math.inf, True, "0.05", None):
                with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                    locality.prepare_locality_workload(self.cfg, **{field: bad})
        cases = [("count", 0), ("count", True), ("count", 2.5), ("count", 100000001),
                 ("batch", 0), ("batch", True), ("batch", 101), ("batch", 2.5),
                 ("rate", 0), ("rate", True), ("rate", math.nan), ("rate", math.inf), ("rate", 10 ** 400),
                 ("rate", 1e-320),
                 ("timeout", 0), ("timeout", True), ("timeout", math.nan), ("timeout", math.inf),
                 ("seed", True), ("seed", None), ("seed", 3.5)]
        for field, bad in cases:
            with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                locality.prepare_locality_workload(self.cfg, **{field: bad})

    def test_constructive_sampler_matches_all_small_subsets(self):
        clusters = {"10": [1], "20": [2, 3], "30": [4, 5, 6, 7]}
        sampler = locality._ParticipantSampler(clusters)
        parent = {leaf: cluster for cluster, leaves in clusters.items() for leaf in leaves}
        rng = random.Random(32)
        for bucket in ("intra2", "cross2", "intra3", "cross3"):
            same_parent = bucket.startswith("intra")
            expected = {ps for ps in itertools.combinations(sorted(parent), int(bucket[-1]))
                        if (len({parent[sid] for sid in ps}) == 1) == same_parent}
            self.assertEqual(sampler.available[bucket], len(expected))
            observed = {tuple(sampler.sample(bucket, rng)) for _ in range(2000)}
            self.assertEqual(observed, expected)

    def test_timeout_must_allow_last_request_to_be_submitted(self):
        valid = locality.prepare_locality_workload(self.cfg, count=113, rate=10, batch=8, seed=7)
        threshold = (113 - len(valid["requests"][-1]["txs"])) / 10
        for timeout in (threshold, threshold / 2):
            with self.subTest(timeout=timeout), self.assertRaisesRegex(ValueError, "负载尚未发完"):
                locality.prepare_locality_workload(self.cfg, count=113, rate=10, batch=8, seed=7, timeout=timeout)
        generated = locality.prepare_locality_workload(self.cfg, count=113, rate=10, batch=8,
                                                       seed=7, timeout=threshold + .01)
        self.assertEqual(generated["requests"], valid["requests"])

    def test_large_leaf_set_needs_no_participant_combination_list(self):
        # 1000 leaves would have >166 million triples, but the sampler stores
        # only cluster-sized weight arrays and the requested transaction file.
        clusters = {str(2000 + group): list(range(1 + 10 * group, 11 + 10 * group)) for group in range(100)}
        sampler = locality._ParticipantSampler(clusters)
        self.assertEqual(sampler.available["cross3"], math.comb(1000, 3) - 100 * math.comb(10, 3))
        self.assertEqual(len(sampler.cross_three_distinct), 100)
        self.assertEqual(len(sampler.sample("cross3", random.Random(7))), 3)


class LocalityValidation(unittest.TestCase):
    def setUp(self):
        self.cfg = config()
        self.workload = locality.prepare_locality_workload(self.cfg, count=1000)

    def test_ahl_retains_reference_locality_after_flattening(self):
        flat = copy.deepcopy(self.cfg)
        leaves = c.topology(c.validate(flat))[1]
        flat["shards"] = [{"id": sid, "parent": 1000} for sid in leaves] + [{"id": 1000, "parent": None}]
        self.assertEqual(locality.workload_statistics(flat, self.workload)["cross_cluster_ratio"], 0)
        reference = self.workload["locality"]["reference_parent"]
        stats = locality.workload_statistics(flat, self.workload, reference_parent=reference)
        self.assertEqual(stats["cross_cluster_ratio"], .05)
        self.assertEqual(stats["arity_locality_counts"], {"intra2": 855, "cross2": 45, "intra3": 95, "cross3": 5})
        self.assertEqual(locality.validate_locality_metadata(flat, self.workload, "ahl"), stats)
        for method in ("arbor", "saguaro", "sharper"):
            with self.subTest(method=method), self.assertRaisesRegex(ValueError, "即刻父分片"):
                locality.validate_locality_metadata(flat, self.workload, method)

    def test_forged_statistics_and_ratio_are_rejected(self):
        mutations = [lambda meta: meta.update(cross_cluster_transactions=0),
                     lambda meta: meta.update(cross_cluster_ratio=.5),
                     lambda meta: meta.update(cross_cluster_ratio=10 ** 400),
                     lambda meta: meta.update(three_shard_ratio=10 ** 400),
                     lambda meta: meta.update(cluster_count=True),
                     lambda meta: meta["arity_locality_counts"].update(cross2=46),
                     lambda meta: meta["participants_per_transaction"].update({"2": 899}),
                     lambda meta: meta["groups"].update({"1,2": 999}),
                     lambda meta: meta["clusters"]["100"].reverse(),
                     lambda meta: meta.update(requested_cross_cluster_ratio=.15),
                     lambda meta: meta.pop("requested_three_shard_ratio"),
                     lambda meta: meta.update(batch=1),
                     lambda meta: meta.pop("cross_cluster_ratio")]
        for mutate in mutations:
            altered = copy.deepcopy(self.workload)
            mutate(altered["locality"])
            with self.subTest(metadata=altered["locality"]), self.assertRaises(ValueError):
                locality.validate_locality_metadata(self.cfg, altered)
        for value in (None, False, [], "local"):
            altered = copy.deepcopy(self.workload)
            altered["locality"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                locality.validate_locality_metadata(self.cfg, altered)

    def test_reference_mapping_keys_values_and_topology_are_checked(self):
        for mutation in (lambda ref: ref.pop("1"), lambda ref: ref.update({"10": 100}),
                         lambda ref: ref.update({"1": True}), lambda ref: ref.update({"1": 0}),
                         lambda ref: ref.update({"1": -1}), lambda ref: ref.update({"1": "100"}),
                         lambda ref: ref.update({"1": 3}), lambda ref: ref.update({"1": 101})):
            altered = copy.deepcopy(self.workload)
            mutation(altered["locality"]["reference_parent"])
            with self.subTest(ref=altered["locality"]["reference_parent"]), self.assertRaises(ValueError):
                locality.validate_locality_metadata(self.cfg, altered)

    def test_actual_participants_ids_and_homogeneous_request_are_checked(self):
        changes = [lambda job: job["requests"][0]["txs"][0].update(participants=[1, 99]),
                   lambda job: job["requests"][0]["txs"][0].update(participants=[2, 1]),
                   lambda job: job["requests"][0]["txs"][0].update(participants=[1, True]),
                   lambda job: job["requests"][0]["txs"][0].update(participants=[1, 2, 8]),
                   lambda job: job["requests"][0]["txs"][0].update(id=job["requests"][1]["txs"][0]["id"]),
                   lambda job: job["requests"][0].update(id=job["requests"][1]["id"]),
                   lambda job: job["requests"][0].update(txs=[])]
        for mutate in changes:
            altered = copy.deepcopy(self.workload)
            mutate(altered)
            with self.subTest(request=altered["requests"][0]), self.assertRaises(ValueError):
                locality.workload_statistics(self.cfg, altered)

    def test_legacy_file_without_locality_is_supported(self):
        del self.workload["locality"]
        self.assertIsNone(locality.validate_locality_metadata(self.cfg, self.workload))


class LocalityCLI(unittest.TestCase):
    def test_generation_creates_parents_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source, output = folder / "config.json", folder / "workloads/mixed.json"
            c.write(source, config())
            argv = ["--config", str(source), "--count", "1000", "--output", str(output)]
            with contextlib.redirect_stdout(io.StringIO()) as out:
                workload = generate.main(argv)
            self.assertIn("跨 cluster=50 (5.00%)", out.getvalue())
            self.assertEqual(json.loads(output.read_text()), workload)
            old = output.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                generate.main(argv)
            self.assertEqual(raised.exception.code, 2)
            self.assertEqual(output.read_bytes(), old)

    def test_invalid_topology_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source, output = folder / "config.json", folder / "new/mixed.json"
            c.write(source, config(((1, 2), (3, 4))))
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                generate.main(["--config", str(source), "--output", str(output)])
            self.assertFalse(output.exists())
            self.assertFalse(output.parent.exists())

    def test_timeout_too_short_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source, output = folder / "config.json", folder / "new/mixed.json"
            c.write(source, config())
            with contextlib.redirect_stderr(io.StringIO()) as error, self.assertRaises(SystemExit) as raised:
                generate.main(["--config", str(source), "--count", "1000", "--rate", "10",
                               "--timeout", "1", "--output", str(output)])
            self.assertEqual(raised.exception.code, 2)
            self.assertIn("负载尚未发完", error.getvalue())
            self.assertFalse(output.exists())
            self.assertFalse(output.parent.exists())


if __name__ == "__main__":
    unittest.main()
