#!/usr/bin/env python3
"""Check shared PBFT recovery with real keys, without starting network workers."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_pbft_recovery", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
BIN = ROOT / "build/bin/test_pbft_recovery"


class PbftRecovery(unittest.TestCase):
    def test_authenticated_view_recovery_is_bounded_immutable_and_keeps_head_deadlines(self):
        self.assertTrue(BIN.is_file(), "build first with make build/bin/test_pbft_recovery")
        with tempfile.TemporaryDirectory(prefix="pbft-recovery-") as folder:
            run = Path(folder)
            keys, node_directory = run / "keys", run / "nodes"
            keys.mkdir(mode=0o700)
            node_directory.mkdir()
            cfg = c.validate({"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": 31000,
                              "shards": [{"id": 1, "parent": 5}, {"id": 2, "parent": 5},
                                         {"id": 5, "parent": None}],
                              "consensus": {"batch_size": 8, "batch_wait_ms": 5,
                                            "cross_shard_batch_size": 8, "cross_shard_batch_wait_ms": 10,
                                            "view_timeout_ms": 1500, "checkpoint_batches": 4},
                              "execution": {"fib_iterations": 1},
                              "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                                          "shard_links": [], "trace": False}})
            cfg.update(method="saguaro", run_id=uuid.uuid4().hex)
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
            self.assertEqual(report["method"], "saguaro")
            self.assertGreaterEqual(len(report["checks"]), 35)
            required = (
                "exact stored VIEW_CHANGE replay avoids repeated deep validation",
                "a changed signed body cannot use the exact-envelope replay shortcut",
                "integer-to-float body tampering cannot reuse the exact-envelope authentication cache",
                "a changed signature cannot reuse a previously authenticated VIEW_CHANGE fingerprint",
                "changed unsigned envelope fields use normal authentication and preserve the first stored vote",
                "re-signed invalid checkpoint evidence is validated and cannot replace cached VIEW_CHANGE",
                "re-signed duplicated PREPARE voters cannot bypass VIEW_CHANGE proof validation",
                "a fourth valid VIEW_CHANGE cannot rewrite the already signed NEW_VIEW candidate",
                "pending NEW_VIEW retransmits the identical signed candidate before local installation",
                "superseded NEW_VIEW cannot be retransmitted in a higher target view",
                "obsolete installed VIEW_CHANGE and NEW_VIEW are discarded without recovery work or deadline changes",
                "NEW_VIEW below the current target is discarded before validating its nested recovery evidence",
                "first genuine head PREPARE quorum starts a fresh COMMIT-phase deadline",
                "duplicate PREPARE and repeated advance cannot renew an already prepared head deadline",
                "future-slot preparation cannot postpone recovery for the blocked execution head",
                "head preparation cannot renew a deadline after view change has started",
                "preparation of an old-view head cannot extend the installed view's deadline",
            )
            for check in required:
                self.assertIn(check, report["checks"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
