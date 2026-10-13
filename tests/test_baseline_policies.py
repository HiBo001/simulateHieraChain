#!/usr/bin/env python3
"""Method-specific replicated admission and 2PC safety without network workers."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_baseline_policies", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


class BaselinePolicies(unittest.TestCase):
    def test_method_specific_admission_commit_and_timeout_safety(self):
        binaries = {method: ROOT / f"build/bin/test_{method}_policies" for method in ("saguaro", "ahl")}
        for method, binary in binaries.items():
            self.assertTrue(binary.is_file(), f"build first with make {binary.relative_to(ROOT)}")
        with tempfile.TemporaryDirectory(prefix="baseline-policies-") as folder:
            run = Path(folder)
            keys = run / "keys"
            keys.mkdir(mode=0o700)
            cfg = c.validate({"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": 31000,
                              "shards": [{"id": owner, "parent": 5} for owner in (1, 2, 3, 4)] +
                                        [{"id": 5, "parent": None}],
                              "consensus": {"batch_size": 8, "batch_wait_ms": 10,
                                            "cross_shard_batch_size": 8, "cross_shard_batch_wait_ms": 100,
                                            "view_timeout_ms": 2000, "checkpoint_batches": 16},
                              "execution": {"fib_iterations": 1},
                              "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                                          "shard_links": [], "trace": False}})
            cfg.update(run_id=uuid.uuid4().hex)
            cfg["client_private_key"], cfg["client_public_key"] = c.keypair(keys, "client")
            cfg["nodes"] = []
            for shard in cfg["shards"]:
                for replica in range(4):
                    sid = shard["id"]
                    private, public = c.keypair(keys, f"s{sid}-n{replica}")
                    cfg["nodes"].append({"shard": sid, "replica": replica, "host": "127.0.0.1",
                                         "port": 31000 + len(cfg["nodes"]), "private_key": private,
                                         "public_key": public})
            for method, binary in binaries.items():
                with self.subTest(method=method):
                    output = run / method
                    output.mkdir()
                    cfg["method"] = method
                    config = run / f"{method}-config.json"
                    c.write(config, cfg)
                    result = subprocess.run([str(binary), str(config), str(output)],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    report = json.loads(result.stdout)
                    self.assertFalse(report["network_started"])
                    self.assertEqual(report["method"], method)
                    self.assertEqual(report["domain_policy"], method == "saguaro")
                    self.assertGreaterEqual(len(report["checks"]), 29)
                    print(f"{method}: {len(report['checks'])} authenticated in-memory checks passed")
            cross_raw = {field: cfg[field] for field in ("replicas_per_shard", "host", "base_port",
                                                       "consensus", "execution", "network")}
            cross_raw["network"] = {field: value for field, value in cfg["network"].items()
                                    if field != "resolved_links"}
            cross_raw["shards"] = [{"id": 1, "parent": 5}, {"id": 2, "parent": 5},
                               {"id": 3, "parent": 6}, {"id": 4, "parent": 6},
                               {"id": 5, "parent": 7}, {"id": 6, "parent": 7},
                               {"id": 7, "parent": None}]
            cross = c.validate(cross_raw)
            cross.update(method="saguaro", run_id=cfg["run_id"],
                         client_private_key=cfg["client_private_key"], client_public_key=cfg["client_public_key"],
                         nodes=list(cfg["nodes"]))
            for sid in (6, 7):
                for replica in range(4):
                    private, public = c.keypair(keys, f"s{sid}-n{replica}")
                    cross["nodes"].append({"shard": sid, "replica": replica, "host": "127.0.0.1",
                                           "port": 31000 + len(cross["nodes"]), "private_key": private,
                                           "public_key": public})
            config = run / "cross-nca-config.json"
            c.write(config, cross)
            output = run / "cross-nca"
            output.mkdir()
            result = subprocess.run([str(binaries["saguaro"]), str(config), str(output), "cross-nca"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertFalse(report["network_started"])
            self.assertGreaterEqual(len(report["checks"]), 8)
            for name in (
                "missing cross-NCA votes schedule ABORT rather than inventing a COMMIT",
                "certified ABORT unblocks the other coordinator and prevents the abandoned batch's late PREPARE",
                "breaking cross-NCA inversion does not execute either transaction prematurely",
                "formerly blocked root transaction resumes with a valid YES vote after ABORT release",
            ):
                self.assertIn(name, report["checks"])
            print(f"saguaro cross-NCA: {len(report['checks'])} authenticated in-memory checks passed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
