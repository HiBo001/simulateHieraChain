#!/usr/bin/env python3
"""Black-box tests: real processes, TCP, signatures, PBFT and Fibonacci execution."""
import contextlib
import copy
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / (time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)


def free_range(count):
    # Bind the entire consecutive range, not just the first port.
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


@contextlib.contextmanager
def running(name, config="single_shard.json", modify=None):
    raw = c.read(ROOT / "config" / config)
    raw["base_port"] = free_range(4 * len(raw["shards"]))
    raw["consensus"] = {"batch_size": 8, "batch_wait_ms": 5, "view_timeout_ms": 700, "checkpoint_batches": 4}
    if modify:
        modify(raw)
    path = TEST_ROOT / (name + ".config.json")
    c.write(path, raw)
    run = c.start(path, TEST_ROOT / name)
    try:
        yield run
    finally:
        c.stop_run(run)


def stop_node(run, shard, replica):
    n = next(n for n in c.read(run / "manifest.json")["nodes"] if n["shard"] == shard and n["replica"] == replica)
    if c.is_our_process(n):
        os.kill(n["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and c.is_our_process(n):
        time.sleep(.03)
    return n


def wait_state(run, shard, expected, alive=4, ordered=0, timeout=10):
    deadline = time.monotonic() + timeout
    rows = []
    while time.monotonic() < deadline:
        rows = [r for r in c.statuses(run) if r["shard"] == shard and r["alive"]]
        if len(rows) == alive and all(r["executed_transactions"] == expected and r["ordered_cst_transactions"] == ordered for r in rows):
            if len({r["state_digest"] for r in rows}) == 1:
                return rows
        time.sleep(.1)
    raise AssertionError("replicas did not converge: " + json.dumps(rows))


class Configuration(unittest.TestCase):
    def test_topology_and_defaults(self):
        cfg = c.validate(c.read(ROOT / "config/three_layer.json"))
        self.assertEqual(c.topology(cfg)[1], [1, 2, 3, 4])
        self.assertEqual(c.lca(cfg, [1, 2]), 5)
        self.assertEqual(c.lca(cfg, [1, 3, 4]), 7)
        self.assertEqual(cfg["network"]["resolved_links"]["2:1"], 10)

    def test_invalid_configs(self):
        good = c.read(ROOT / "config/two_layer.json")
        variants = []
        x = copy.deepcopy(good); x["shards"].append({"id": 1, "parent": 5}); variants.append(x)
        x = copy.deepcopy(good); x["shards"][0]["parent"] = 100; variants.append(x)
        x = copy.deepcopy(good); x["shards"][0]["parent"] = 2; x["shards"][1]["parent"] = 1; variants.append(x)
        x = copy.deepcopy(good); x["shards"][0]["parent"] = None; variants.append(x)
        x = copy.deepcopy(good); x["network"]["shard_links"][0]["delay_ms"] = -1; variants.append(x)
        x = copy.deepcopy(good); x["network"]["shard_links"].append({"shards": [2, 1], "delay_ms": 5}); variants.append(x)
        x = copy.deepcopy(good); x["network"]["shard_links"][0]["shards"] = [1, 999]; variants.append(x)
        x = copy.deepcopy(good); x["replicas_per_shard"] = 3; variants.append(x)
        x = copy.deepcopy(good); x["base_port"] = 65535; variants.append(x)
        x = copy.deepcopy(good); x["consensus"]["batch_size"] = 0; variants.append(x)
        x = copy.deepcopy(good); x["consensus"]["cross_shard_batch_size"] = 1001; variants.append(x)
        x = copy.deepcopy(good); x["consensus"]["cross_shard_batch_wait_ms"] = -1; variants.append(x)
        for raw in variants:
            with self.subTest(config=raw), self.assertRaises(ValueError):
                c.validate(raw)

    def test_removed_pipeline_setting_is_rejected(self):
        # Both old settings must fail explicitly rather than silently selecting
        # a protocol that has been removed from the executable.
        for value in (0, 8):
            with self.subTest(pipeline_window=value):
                raw = c.read(ROOT / "config/two_layer.json")
                raw["consensus"]["pipeline_window"] = value
                with self.assertRaisesRegex(ValueError, "pipeline_window"):
                    c.validate(raw)

    def test_workload_reproducibility(self):
        cfg = c.validate(c.read(ROOT / "config/three_layer.json"))
        a = c.prepare_workload(cfg, 101, 100, 42, "test")
        b = c.prepare_workload(cfg, 101, 100, 42, "test")
        self.assertEqual(a, b)
        self.assertEqual(sum(len(r["txs"]) for r in a["requests"]), 101)
        ids = [t["id"] for r in a["requests"] for t in r["txs"]]
        self.assertEqual(len(ids), len(set(ids)))
        cross = c.validate(c.read(ROOT / "config/two_layer.json"))
        small_requests = c.prepare_workload(cross, 16, 100, 42, "batch-test", participants=[1, 2], batch=8)
        self.assertEqual([len(r["txs"]) for r in small_requests["requests"]], [8, 8])
        self.assertTrue(all("batch_limit" not in r for r in small_requests["requests"]))


class Integration(unittest.TestCase):
    def test_normal_checkpoint_duplicate_and_signature(self):
        with running("normal") as run:
            rc, result = c.load(run, count=80, rate=200, seed=7, prefix="repeat", shard=1, timeout=12)
            self.assertEqual(rc, 0)
            self.assertEqual(result["executed_transactions"], 80)
            latencies = [t["latency_s"] for t in result["timings"]]
            self.assertEqual(len(latencies), 80)
            self.assertTrue(all(0 <= seconds <= 12 for seconds in latencies))
            self.assertTrue(all("completion_s" in t for t in result["timings"]))
            self.assertAlmostEqual(result["avg_latency_s"], sum(latencies) / len(latencies), places=6)
            self.assertLessEqual(result["p50_s"], result["p95_s"])
            self.assertLessEqual(result["p95_s"], result["p99_s"])
            rows = wait_state(run, 1, 80)
            self.assertGreaterEqual(max(r["stable_seq"] for r in rows), 4)
            rc, _ = c.load(run, count=80, rate=200, seed=7, prefix="repeat", shard=1, timeout=8)
            self.assertEqual(rc, 0)
            wait_state(run, 1, 80)
            # Same transaction through two distinct signed request IDs: one
            # actual execution, including when the leader puts both in a batch.
            cfg = c.read(run / "config.json")
            job = c.prepare_workload(cfg, 1, 1000, 10, "same-tx", shard=1)
            other = copy.deepcopy(job["requests"][0])
            other["id"] = "same-tx:another-request"
            job["requests"].append(other)
            c.write(run / "same-tx.workload.json", job)
            rc = subprocess.run([str(c.BIN), "client", str(run / "config.json"), str(run / "same-tx.workload.json"), str(run / "same-tx.result.json")]).returncode
            self.assertEqual(rc, 0)
            self.assertEqual(c.read(run / "same-tx.result.json")["executed_transactions"], 1)
            wait_state(run, 1, 81)
            cfg = c.read(run / "config.json")
            n = cfg["nodes"][0]
            before = c.read(Path(n["directory"]) / "status.json")["rejected_messages"]
            forged = {"body": {"type": "COMMIT", "run": cfg["run_id"], "shard": 1, "from": 1, "view": 0, "seq": 11, "digest": "0" * 64}, "signature": "00" * 64}
            c.send_frame(n["host"], n["port"], forged)
            time.sleep(.3)
            self.assertGreater(c.read(Path(n["directory"]) / "status.json")["rejected_messages"], before)
            wait_state(run, 1, 81)
            # An authenticated packet from the removed protocol is rejected
            # even though its signer is a real member of this shard.
            status_path = Path(n["directory"]) / "status.json"
            baseline = c.read(status_path)
            self.assertFalse(any(name.startswith(("pipe_", "pipeline")) for name in baseline))
            removed_message = c.signed({"type": "PIPE_ARCHIVE_QUERY", "run": cfg["run_id"],
                                        "shard": 1, "from": 1, "view": baseline["view"],
                                        "target": 1}, cfg["nodes"][1]["private_key"], run)
            c.send_frame(n["host"], n["port"], removed_message)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                after = c.read(status_path)
                if after["rejected_messages"] > baseline["rejected_messages"]:
                    break
                time.sleep(.05)
            self.assertGreater(after["rejected_messages"], baseline["rejected_messages"])
            for field in ("executed_transactions", "ordered_cst_transactions", "applied_batches",
                          "state_digest", "chain_digest", "kv_digest", "view"):
                self.assertEqual(after[field], baseline[field], field)
            self.assertTrue(all(not any(name.startswith(("pipe_", "pipeline")) for name in row)
                                for row in c.statuses(run)))

            # Bypassing the Python validator must not revive the removed
            # setting in either executable entry point.
            workload_path = run / "removed-setting.workload.json"
            c.write(workload_path, c.prepare_workload(cfg, 1, 100, 11, "removed-setting", shard=1))
            for value in (0, 8):
                obsolete = copy.deepcopy(cfg)
                obsolete["consensus"]["pipeline_window"] = value
                obsolete_path = run / f"removed-setting-{value}.json"
                c.write(obsolete_path, obsolete)
                commands = [
                    [str(c.BIN), "client", str(obsolete_path), str(workload_path),
                     str(run / f"removed-setting-{value}.result.json")],
                    [str(c.BIN), "node", str(obsolete_path), "1", "0",
                     str(run / f"removed-setting-{value}.node")],
                ]
                for command in commands:
                    with self.subTest(entrypoint=command[1], pipeline_window=value):
                        rejected = subprocess.run(command, capture_output=True, text=True, timeout=3)
                        self.assertEqual(rejected.returncode, 1, rejected.stdout + rejected.stderr)
                        self.assertIn("pipeline_window", rejected.stderr)

    def test_backup_offline(self):
        with running("backup-offline") as run:
            stop_node(run, 1, 3)
            rc, _ = c.load(run, count=40, rate=100, shard=1, timeout=10)
            self.assertEqual(rc, 0)
            rows = wait_state(run, 1, 40, alive=3)
            self.assertTrue(all(r["view"] == 0 for r in rows))

    def test_primary_failure_and_checkpoint_recovery(self):
        with running("primary-failure") as run:
            self.assertEqual(c.load(run, count=64, rate=160, shard=1, timeout=12)[0], 0)
            wait_state(run, 1, 64)
            stop_node(run, 1, 0)
            self.assertEqual(c.load(run, count=48, rate=160, shard=1, timeout=15)[0], 0)
            rows = wait_state(run, 1, 112, alive=3)
            self.assertTrue(all(r["view"] >= 1 and r["primary"] != 0 for r in rows))

    def test_two_nodes_cannot_commit_even_with_duplicate_votes(self):
        with running("two-nodes") as run:
            stop_node(run, 1, 2)
            stop_node(run, 1, 3)
            cfg = c.read(run / "config.json")
            # Capture the real proposal digest, then allow the two live nodes
            # to reach PREPARED using one additional valid fixture PREPARE.
            # Repeating one matching COMMIT must still not create a third voter.
            job = c.prepare_workload(cfg, 8, 100, 1, "quorum", shard=1, timeout=2.5)
            c.write(run / "quorum.workload.json", job)
            process = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"), str(run / "quorum.workload.json"), str(run / "quorum.result.json")])
            try:
                deadline = time.monotonic() + 2
                proposal = None
                while time.monotonic() < deadline:
                    lines = (Path(cfg["nodes"][0]["directory"]) / "events.jsonl").read_text().splitlines()
                    events = [json.loads(line) for line in lines]
                    proposal = next((e for e in events if e["event"] == "preprepare"), None)
                    if proposal:
                        break
                    time.sleep(.01)
                self.assertIsNotNone(proposal)
                for kind, who in [("PREPARE", 2), ("COMMIT", 1)]:
                    vote = c.signed({"type": kind, "run": cfg["run_id"], "shard": 1, "from": who, "view": 0, "seq": 1, "digest": proposal["digest"]}, cfg["nodes"][who]["private_key"], run)
                    for _ in range(12):
                        for n in cfg["nodes"][:2]:
                            c.send_frame(n["host"], n["port"], vote)
                rc = process.wait(timeout=8)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
            result = c.read(run / "quorum.result.json")
            self.assertEqual(rc, 2)
            self.assertEqual(result["completed_requests"], 0)
            for n in cfg["nodes"][:2]:
                self.assertIn('"event":"prepared"', (Path(n["directory"]) / "events.jsonl").read_text())
            wait_state(run, 1, 0, alive=2)

    def test_new_view_must_preserve_prepared_value(self):
        # Construct an authenticated prepared-but-uncommitted certificate. This
        # isolates the recovery rule without relying on a race when killing a node.
        def tune(raw):
            raw["consensus"]["view_timeout_ms"] = 5000
        with running("prepared-recovery", modify=tune) as run:
            cfg = c.read(run / "config.json")
            keys = [n["private_key"] for n in cfg["nodes"]]
            def replica_msg(kind, who, view, **fields):
                return c.signed(dict(type=kind, run=cfg["run_id"], shard=1, **{"from": who}, view=view, **fields), keys[who], run)
            request = c.prepare_workload(cfg, 1, 100, 1, "prepared", shard=1)["requests"][0]
            request.update(type="CLIENT", run=cfg["run_id"], reply={"host": "127.0.0.1", "port": 9})
            value = {"requests": [c.signed(request, cfg["client_private_key"], run)]}
            def digest(j):
                import hashlib
                return hashlib.sha256(json.dumps(j, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()
            d = digest(value)
            pp = replica_msg("PREPREPARE", 0, 0, seq=1, digest=d, value=value)
            prepared = {"proposal": pp, "prepares": [replica_msg("PREPARE", r, 0, seq=1, digest=d) for r in [1, 2]]}
            initial = {"seq": 0, "chain": __import__("hashlib").sha256(b"arbor-genesis").hexdigest(), "kv": {}, "seen": {}, "requests": {}, "executed": 0, "ordered_cst": 0, "cst_batches": {}, "cst_seen": {}, "leaf_ordered_cst": 0, "last_cst_seq": 0, "cst_finalized": {}, "cst_orders": {}, "cst_order_index": 0, "cst_round": 0, "cst_indices": {}, "participant_indices": {}, "cst_rounds": {}}
            checkpoint = {"seq": 0, "state": initial, "proof": []}
            vcs = [replica_msg("VIEW_CHANGE", r, 1, stable=checkpoint, prepared=[prepared] if r == 2 else []) for r in [1, 2, 3]]
            bad = replica_msg("NEW_VIEW", 1, 1, changes=vcs, proposals=[])
            for n in cfg["nodes"]:
                c.send_frame(n["host"], n["port"], bad)
            time.sleep(.3)
            self.assertTrue(all(r["view"] == 0 and r["executed_transactions"] == 0 for r in c.statuses(run)))
            pp1 = replica_msg("PREPREPARE", 1, 1, seq=1, digest=d, value=value)
            good = replica_msg("NEW_VIEW", 1, 1, changes=vcs, proposals=[pp1])
            for n in cfg["nodes"]:
                c.send_frame(n["host"], n["port"], good)
            rows = wait_state(run, 1, 1)
            self.assertTrue(all(r["view"] == 1 for r in rows))

    def test_new_view_reuses_original_committed_certificate(self):
        # Recovery must disseminate the existing commit proof rather than
        # require a new quorum to commit the same slot in the next view.
        def tune(raw):
            raw["consensus"]["view_timeout_ms"] = 5000
        with running("committed-recovery", modify=tune) as run:
            cfg = c.read(run / "config.json")
            def msg(kind, who, view, **fields):
                return c.signed(dict(type=kind, run=cfg["run_id"], shard=1,
                                     **{"from": who}, view=view, **fields),
                                cfg["nodes"][who]["private_key"], run)
            request = c.prepare_workload(cfg, 1, 100, 1, "committed", shard=1)["requests"][0]
            request.update(type="CLIENT", run=cfg["run_id"], reply={"host": "127.0.0.1", "port": 9})
            value = {"requests": [c.signed(request, cfg["client_private_key"], run)]}
            d = __import__("hashlib").sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                                       separators=(",", ":")).encode()).hexdigest()
            certificate = {"proposal": msg("PREPREPARE", 0, 0, seq=1, digest=d, value=value),
                           "prepares": [msg("PREPARE", who, 0, seq=1, digest=d) for who in (1, 2)],
                           "commits": [msg("COMMIT", who, 0, seq=1, digest=d) for who in (0, 1, 2)]}
            initial = {"seq": 0, "chain": __import__("hashlib").sha256(b"arbor-genesis").hexdigest(),
                       "kv": {}, "seen": {}, "requests": {}, "executed": 0, "ordered_cst": 0,
                       "cst_batches": {}, "cst_seen": {}, "leaf_ordered_cst": 0, "last_cst_seq": 0,
                       "cst_finalized": {}, "cst_orders": {}, "cst_order_index": 0, "cst_round": 0, "cst_indices": {}, "participant_indices": {}, "cst_rounds": {}}
            checkpoint = {"seq": 0, "state": initial, "proof": []}
            changes = [msg("VIEW_CHANGE", who, 1, stable=checkpoint,
                           prepared=[certificate] if who == 2 else []) for who in (1, 2, 3)]
            proposal = msg("PREPREPARE", 1, 1, seq=1, digest=d, value=value)
            # A field named commits is not evidence unless every vote verifies.
            malformed = copy.deepcopy(changes)
            bad = copy.deepcopy(certificate)
            bad["commits"] = [bad["commits"][0]] * 3
            malformed[1] = msg("VIEW_CHANGE", 2, 1, stable=checkpoint, prepared=[bad])
            for node in cfg["nodes"]:
                c.send_frame(node["host"], node["port"],
                             msg("NEW_VIEW", 1, 1, changes=malformed, proposals=[proposal]))
            time.sleep(.2)
            self.assertTrue(all(row["view"] == 0 for row in c.statuses(run)))
            recovered = msg("NEW_VIEW", 1, 1, changes=changes, proposals=[proposal])
            for node in cfg["nodes"]:
                c.send_frame(node["host"], node["port"], recovered)
            rows = wait_state(run, 1, 1)
            self.assertTrue(all(row["view"] == 1 and row["applied_batches"] == 1 for row in rows))
            for node in cfg["nodes"]:
                entries = [json.loads(line) for line in
                           (Path(node["directory"]) / "commits.jsonl").read_text().splitlines()]
                self.assertEqual(len(entries), 1)
                self.assertEqual(entries[0]["certificate"]["proposal"]["body"]["view"], 0)

    def test_lagging_replica_catches_up(self):
        with running("catch-up") as run:
            cfg = c.read(run / "manifest.json")
            n = cfg["nodes"][3]
            os.kill(n["pid"], signal.SIGSTOP)
            try:
                self.assertEqual(c.load(run, count=96, rate=150, shard=1, timeout=15)[0], 0)
            finally:
                os.kill(n["pid"], signal.SIGCONT)
            wait_state(run, 1, 96, timeout=15)
            self.assertEqual(c.load(run, count=16, rate=100, shard=1, timeout=10)[0], 0)
            wait_state(run, 1, 112)

    def test_pair_delays_and_nonblocking_broadcast(self):
        def tune(raw):
            raw["network"]["trace"] = True
            raw["network"]["shard_links"] = [{"shards": [1, 2], "delay_ms": 50}, {"shards": [1, 5], "delay_ms": 10}]
        with running("pair-delay", "two_layer.json", tune) as run:
            a = c.probe(run, "1:0", "2:1", 3)
            b = c.probe(run, "1:0", "5:0", 3)
            fallback = c.probe(run, "2:0", "5:0", 3)
            reverse = c.probe(run, "2:1", "1:0", 2)
            local = c.probe(run, "1:0", "1:2", 2)
            for result, expected in [(a,100),(b,20),(fallback,40),(reverse,100),(local,2)]:
                self.assertEqual(result["expected_added_rtt_ms"], expected)
                self.assertGreaterEqual(min(result["samples_ms"]), expected - 1)
                self.assertLess(max(result["samples_ms"]), expected + 150)
            self.assertGreater(a["mean_ms"] - b["mean_ms"], 60)
            # Simultaneous signed probe jobs to four distinct remote replicas.
            cfg = c.read(run / "config.json")
            src = cfg["nodes"][0]
            envs = [c.signed({"type":"PROBE","run":cfg["run_id"],"source":"1:0","dst_shard":2,"dst_replica":r,"id":f"parallel-{r}"}, cfg["client_private_key"], run) for r in range(4)]
            for env in envs:
                c.send_frame(src["host"], src["port"], env)
            time.sleep(.5)
            readings = c.read(Path(src["directory"]) / "status.json")["probes"]
            for r in range(4):
                self.assertLess(readings[f"parallel-{r}"]["rtt_ms"], 200)
            # A slow inter-shard link does not delay independent intra-shard PBFT.
            self.assertEqual(c.load(run, count=24, rate=100, shard=1, timeout=10)[0], 0)
            wait_state(run, 1, 24)

    def test_large_checkpoint_does_not_stall_following_batches(self):
        def tune(raw):
            raw["consensus"].update(batch_size=1000, batch_wait_ms=10,
                                    view_timeout_ms=2000, checkpoint_batches=16)
            raw["execution"] = {"fib_iterations": 1}
        with running("large-checkpoint", modify=tune) as run:
            # The second load crosses checkpoint sequence 16 with 1000-tx batches.
            for count, rate, expected in [(10000, 3000, 10000), (10000, 4000, 20000)]:
                rc, result = c.load(run, count=count, rate=rate, shard=1, timeout=20)
                self.assertEqual(rc, 0)
                self.assertEqual(result["executed_transactions"], count)
                wait_state(run, 1, expected, timeout=20)

    def test_three_layer_all_shards_and_full_execution_accounting(self):
        def tune(raw):
            raw["consensus"].update(view_timeout_ms=3000, cross_shard_batch_wait_ms=30)
            raw["execution"] = {"fib_iterations": 1}
        with running("three-layer", "three_layer.json", modify=tune) as run:
            self.assertEqual(len(c.statuses(run)), 28)
            self.assertEqual(c.load(run, count=16, rate=100, shard=1, timeout=20)[0], 0)
            wait_state(run, 1, 16)
            leaf_counts = {1: 16, 2: 0, 3: 0, 4: 0}
            for participants, coordinator in [([1,2],5),([3,4],6),([1,3],7)]:
                rc, result = c.load(run, count=8, rate=100, participants=participants, timeout=30)
                self.assertEqual(rc, 0)
                self.assertEqual(result["executed_transactions"], 8)
                self.assertEqual(result["ordered_only_transactions"], 0)
                wait_state(run, coordinator, 0, ordered=8, timeout=20)
                for leaf in participants:
                    leaf_counts[leaf] += 8
                    wait_state(run, leaf, leaf_counts[leaf], timeout=20)

    def test_restart_script_and_port_conflict(self):
        latest = ROOT / "runtime/latest"
        latest_before = os.readlink(latest) if latest.is_symlink() else None
        raw = c.read(ROOT / "config/single_shard.json")
        raw["base_port"] = free_range(4)
        config = TEST_ROOT / "restart.config.json"
        c.write(config, raw)
        run1, run2 = TEST_ROOT / "restart-a", TEST_ROOT / "restart-b"
        subprocess.run([str(ROOT / "start_all.sh"), "--config", str(config), "--run-dir", str(run1)], check=True)
        try:
            with self.assertRaises(OSError):
                c.start(config, TEST_ROOT / "port-conflict")
        finally:
            subprocess.run([str(ROOT / "stop_all.sh"), "--run-dir", str(run1)], check=True)
        self.assertFalse(any(r["alive"] for r in c.statuses(run1)))
        subprocess.run([str(ROOT / "start_all.sh"), "--config", str(config), "--run-dir", str(run2)], check=True)
        try:
            self.assertEqual(c.load(run2, count=8, rate=100, shard=1, timeout=8)[0], 0)
        finally:
            c.stop_run(run2)
        self.assertEqual(os.readlink(latest) if latest.is_symlink() else None, latest_before)


if __name__ == "__main__":
    print(f"测试日志保留在: {TEST_ROOT}", flush=True)
    started = time.monotonic()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__)))
    report = {"tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), "elapsed_s": time.monotonic()-started, "passed": result.wasSuccessful(), "details": [str(test)+"\n"+trace for test, trace in result.failures + result.errors]}
    c.write(TEST_ROOT / "summary.json", report)
    raise SystemExit(0 if result.wasSuccessful() else 1)
