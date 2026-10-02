#!/usr/bin/env python3
"""One certified coordinator ORDER and one execution slot at each leaf per batch."""
import copy
import importlib.util
import json
from pathlib import Path
import socket
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("stage2a-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)


def free_range(count):
    for base in range(23000, 59000, 37):
        sockets = []
        try:
            for port in range(base, base + count):
                s = socket.socket()
                sockets.append(s)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for s in sockets:
                s.close()
    raise RuntimeError("no free test ports")


class CrossShardOrdering(unittest.TestCase):
    def test_certified_order_reaches_both_leaves_and_rejects_forgery(self):
        raw = c.read(ROOT / "config/two_layer.json")
        raw["base_port"] = free_range(12)
        raw["consensus"] = {"batch_size": 8, "batch_wait_ms": 5,
                            "view_timeout_ms": 1200, "checkpoint_batches": 4}
        config = TEST_ROOT / "two-layer.json"
        c.write(config, raw)
        run = c.start(config, TEST_ROOT / "cluster")
        try:
            rc, result = c.load(run, count=40, rate=80, participants=[1, 2],
                                batch=8, timeout=20)
            self.assertEqual(rc, 0)
            self.assertEqual(result["ordered_only_transactions"], 0)
            self.assertEqual(result["executed_transactions"], 40)
            for request in result["workload"]["requests"]:
                for tx in request["txs"]:
                    self.assertEqual([a["shard"] for a in tx["accesses"]], [1, 2])
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                rows = c.statuses(run)
                if all(sum(r["leaf_ordered_cst_transactions"] == 40 and
                           r["executed_transactions"] == 40 and r["staged_cst_batches"] == 0
                           for r in rows if r["shard"] == leaf) == 4
                       for leaf in [1, 2]):
                    break
                time.sleep(.1)
            else:
                self.fail("certified CST order did not reach both leaves: " + json.dumps(rows))
            for shard in [1, 2]:
                leaf_rows = [r for r in rows if r["shard"] == shard]
                self.assertEqual(len({r["state_digest"] for r in leaf_rows}), 1)
                self.assertTrue(all(r["ordered_cst_transactions"] == 0 and r["last_cst_seq"] == 5 for r in leaf_rows))
            coordinator = [r for r in rows if r["shard"] == 5]
            self.assertTrue(all(r["ordered_cst_transactions"] == 40 for r in coordinator))
            self.assertTrue(all(r["executed_transactions"] == 0 for r in coordinator))
            self.assertTrue(all(r["applied_batches"] == 5 for r in rows))
            self.assertTrue(all(r["completed_cst_transactions"] == 40 for r in coordinator))
            for shard in (1, 2, 5):
                for replica in range(4):
                    entries = [json.loads(line) for line in
                               (run / f"shard{shard}" / f"node{replica}" / "commits.jsonl")
                               .read_text().splitlines()]
                    self.assertEqual([entry["seq"] for entry in entries], [1, 2, 3, 4, 5])
                    for entry in entries:
                        value = entry["certificate"]["proposal"]["body"]["value"]
                        self.assertNotIn("cst_decisions", value)
                        self.assertNotIn("cst_finalizations", value)
                        if shard in (1, 2):
                            witness = entry["execution_witness"]
                            self.assertEqual([proof["record"]["shard"] for proof in witness["proofs"]], [1, 2])
                            for proof in witness["proofs"]:
                                self.assertEqual(len(proof["votes"]), 3)
                                self.assertEqual(len({vote["body"]["from"] for vote in proof["votes"]}), 3)
            cfg = c.read(run / "config.json")
            leaf = next(n for n in cfg["nodes"] if n["shard"] == 1 and n["replica"] == 0)
            commits = [json.loads(line) for line in (Path(leaf["directory"]) / "commits.jsonl").read_text().splitlines()]
            orders = [entry["certificate"]["proposal"]["body"]["value"]["cst_orders"][0]
                      for entry in commits if "cst_orders" in entry["certificate"]["proposal"]["body"]["value"]]
            self.assertEqual([cert["proposal"]["body"]["value"]["cst_order_index"] for cert in orders],
                             [1, 2, 3, 4, 5])
            self.assertTrue(all(len(cert["commits"]) >= 3 for cert in orders))
            forged = copy.deepcopy(orders[0])
            forged["commits"][0]["signature"] = "00" * 64
            source = next(n for n in cfg["nodes"] if n["shard"] == 5 and n["replica"] == 0)
            envelope = c.signed({"type": "CST_ORDER", "run": cfg["run_id"], "shard": 5,
                                 "from": 0, "view": 0, "target": 1, "certificate": forged},
                                source["private_key"], run)
            before = c.read(Path(leaf["directory"]) / "status.json")["rejected_messages"]
            c.send_frame(leaf["host"], leaf["port"], envelope)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if c.read(Path(leaf["directory"]) / "status.json")["rejected_messages"] > before:
                    break
                time.sleep(.05)
            else:
                self.fail("forged coordinator certificate was not rejected")
            bad_request = copy.deepcopy(result["workload"]["requests"][0])
            bad_request["id"] = "missing-access-set"
            bad_request["txs"] = [bad_request["txs"][0]]
            del bad_request["txs"][0]["accesses"]
            bad_request.update(type="CLIENT", run=cfg["run_id"],
                               reply={"host": "127.0.0.1", "port": 9})
            bad_envelope = c.signed(bad_request, cfg["client_private_key"], run)
            coordinator_node = next(n for n in cfg["nodes"] if n["shard"] == 5 and n["replica"] == 0)
            before = c.read(Path(coordinator_node["directory"]) / "status.json")["rejected_messages"]
            c.send_frame(coordinator_node["host"], coordinator_node["port"], bad_envelope)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if c.read(Path(coordinator_node["directory"]) / "status.json")["rejected_messages"] > before:
                    break
                time.sleep(.05)
            else:
                self.fail("cross-shard transaction without per-shard accesses was accepted")
        finally:
            c.stop_run(run)


if __name__ == "__main__":
    print(f"第二阶段测试日志保留在: {TEST_ROOT}", flush=True)
    unittest.main(verbosity=2)
