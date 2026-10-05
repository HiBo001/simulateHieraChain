#!/usr/bin/env python3
"""Run pure C++ protocol recovery checks with keys and no cluster processes."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_sharper_recovery", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
BIN = ROOT / "build/bin/test_sharper_recovery"


class RecoveryEvidence(unittest.TestCase):
    def test_authenticated_recovery_rejects_malformed_and_conflicting_evidence(self):
        self.assertTrue(BIN.is_file(), "build first with make build/bin/test_sharper_recovery")
        with tempfile.TemporaryDirectory(prefix="sharper-recovery-") as folder:
            run = Path(folder)
            keys, node_directory = run / "keys", run / "node"
            keys.mkdir(mode=0o700)
            node_directory.mkdir()
            cfg = c.validate({"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": 31000,
                              "shards": [{"id": 1, "parent": 5}, {"id": 2, "parent": 5},
                                         {"id": 3, "parent": 5}, {"id": 5, "parent": None}],
                              "consensus": {"batch_size": 8, "batch_wait_ms": 5, "cross_shard_batch_size": 8,
                                            "cross_shard_batch_wait_ms": 10, "view_timeout_ms": 1500,
                                            "checkpoint_batches": 4}, "execution": {"fib_iterations": 1},
                              "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                                          "shard_links": [], "trace": False}})
            cfg.update(method="sharper", run_id=uuid.uuid4().hex)
            cfg["client_private_key"], cfg["client_public_key"] = c.keypair(keys, "client")
            cfg["nodes"] = []
            for shard in cfg["shards"]:
                for replica in range(4):
                    sid = shard["id"]
                    private, public = c.keypair(keys, f"s{sid}-n{replica}")
                    cfg["nodes"].append({"shard": sid, "replica": replica, "host": "127.0.0.1",
                                         "port": 31000 + len(cfg["nodes"]), "private_key": private,
                                         "public_key": public, "directory": str(node_directory)})
            config = run / "config.json"
            c.write(config, cfg)
            result = subprocess.run([str(BIN), str(config), str(node_directory)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertFalse(report["network_started"])
            self.assertGreaterEqual(len(report["checks"]), 26)
            self.assertIn("one higher view reservation cannot replace global prepared value", report["checks"])
            self.assertIn("signed malformed recovery COMMIT rejected", report["checks"])
            self.assertIn("origin atomically reserves origin_seq before its first propose call returns", report["checks"])
            self.assertIn("a ready foreign batch cannot steal an already advertised origin_seq even with an earlier key", report["checks"])
            self.assertIn("newly ready foreign work starts a fresh local progress deadline", report["checks"])
            self.assertIn("duplicate foreign proposal and ACCEPT cannot refresh the active deadline", report["checks"])
            self.assertIn("NEW_VIEW releases an unselected unprepared local reservation", report["checks"])
            self.assertIn("NEW_VIEW atomically reclaims the promoted origin's unselected advertised head", report["checks"])
            self.assertIn("foreign work cannot steal the origin head immediately after NEW_VIEW installation", report["checks"])
            self.assertIn("NEW_VIEW never cancels or rewrites an already cast COMMIT omitted from its certificate", report["checks"])
            self.assertIn("unsafe NEW_VIEW rejection occurs before any slot, view, or COMMIT is partially replaced", report["checks"])
            self.assertIn("cold participant installs the recovered assignment using the VC's complete ACCEPT vector", report["checks"])
            fifo_checks = (
                "A B A client arrival sequence never merges its separated A requests",
                "A A B arrival sequence combines exactly the contiguous A prefix",
                "an indivisible capacity barrier cannot be skipped to take a later fitting request",
                "a closed participant prefix still respects the original minimum batch wait",
                "a closed different-group prefix proposes after the minimum wait without waiting 600 milliseconds",
                "open partial cross-shard prefix genuinely waits for its configured 600-millisecond batching deadline",
                "a full contiguous prefix proposes without inheriting the partial-prefix batching wait",
                "duplicate CLIENT delivery never changes FIFO position or allocates another arrival entry",
                "erasePending removes only its own FIFO request and index",
                "snapshot pending-index rebuild preserves the unfinished request arrival order",
                "snapshot rebuild prunes completed pending residue from both FIFO indices",
                "overlapping request aliases are deferred without creating duplicate FIFO consensus candidates",
                "repeated owner or deferred alias delivery cannot start another consensus for the same transaction",
            )
            for check in fifo_checks:
                self.assertIn(check, report["checks"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
