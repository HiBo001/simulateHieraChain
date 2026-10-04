#!/usr/bin/env python3
"""Exercise Saguaro with real four-process PBFT and the shared client."""
import contextlib
import copy
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_saguaro", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("saguaro-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)
TWO_LAYER = [(1, 5), (2, 5), (5, None)]
THREE_LAYER = [(1, 5), (2, 5), (3, 6), (4, 6), (5, 7), (6, 7), (7, None)]


def free_range(count):
    for base in range(31000, 59000, 43):
        held = []
        try:
            for port in range(base, base + count):
                sock = socket.socket()
                held.append(sock)
                sock.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for sock in held:
                sock.close()
    raise RuntimeError("no free ports for Saguaro integration test")


@contextlib.contextmanager
def running(name, topology=TWO_LAYER, consensus=None, method="saguaro"):
    raw = {"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": free_range(4 * len(topology)),
           "shards": [{"id": sid, "parent": parent} for sid, parent in topology],
           "consensus": {"batch_size": 8, "batch_wait_ms": 5, "cross_shard_batch_size": 8,
                         "cross_shard_batch_wait_ms": 10, "view_timeout_ms": 1500,
                         "checkpoint_batches": 4},
           "execution": {"fib_iterations": 1},
           "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                       "shard_links": [], "trace": False}}
    raw["consensus"].update(consensus or {})
    config = TEST_ROOT / (name + ".config.json")
    c.write(config, raw)
    run = c.start(config, TEST_ROOT / name, method=method)
    try:
        yield run
    finally:
        c.stop_run(run)


def wait_status(run, predicate, timeout=30):
    deadline, rows = time.monotonic() + timeout, []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        if predicate(rows):
            return rows
        time.sleep(.1)
    raise AssertionError("Saguaro did not settle: " + json.dumps(rows))


def job(run, name, participants=None, shard=None, count=16, batch=4, shared=False):
    cfg = c.read(run / "config.json")
    workload = c.prepare_workload(cfg, count, 1000, 31415, name, participants=participants,
                                 shard=shard, batch=batch, timeout=45)
    if shared:
        for request in workload["requests"]:
            for tx in request["txs"]:
                if len(tx["participants"]) > 1:
                    for access in tx["accesses"]:
                        access["key"] = f"account:{access['shard']}:shared"
                    tx["key"] = tx["accesses"][0]["key"]
                else:
                    tx["key"] = f"account:{tx['participants'][0]}:shared"
    return workload


def client(run, name, workload):
    source, output = run / (name + ".workload.json"), run / (name + ".result.json")
    c.write(source, workload)
    binary = c.run_binary(run)
    rc = subprocess.run([str(binary), "client", str(run / "config.json"), str(source), str(output)],
                        timeout=workload["timeout_s"] + 10).returncode
    return rc, c.read(output)


def assert_complete(case, rc, result, count):
    case.assertEqual(rc, 0, result)
    case.assertEqual(result["completed_requests"], result["requests"])
    case.assertEqual(result["executed_transactions"], count)
    case.assertEqual(result["errors"], 0)
    case.assertEqual(result["ordered_only_transactions"], 0)
    case.assertGreater(result["completed_tps"], 0)
    case.assertGreaterEqual(result["p95_s"], 0)


def settled(run, workloads, alive_only=False, timeout=30):
    cfg = c.read(run / "config.json")
    parent, leaves = c.topology(cfg)
    executed, ordered = {sid: set() for sid in parent}, {sid: set() for sid in parent}
    for workload in workloads:
        for request in workload["requests"]:
            for tx in request["txs"]:
                for sid in tx["participants"]:
                    executed[sid].add(tx["id"])
                if len(tx["participants"]) > 1:
                    ordered[request["target"]].add(tx["id"])

    def finished(rows):
        selected = [row for row in rows if row["alive"] or not alive_only]
        for row in selected:
            sid = row["shard"]
            if not row["alive"] or row.get("changing_view") or row.get("method") != "saguaro":
                return False
            if row["executed_transactions"] != len(executed[sid]):
                return False
            if row["ordered_cst_transactions"] != len(ordered[sid]):
                return False
            if sid not in leaves and row["completed_cst_transactions"] != len(ordered[sid]):
                return False
            for field in ("pending_requests", "dedup_waiting_requests", "pending_cst_batches",
                          "staged_cst_batches", "sag_active_batches", "sag_pending_prepares",
                          "sag_pending_decisions", "sag_pending_completions", "sag_held_locks"):
                if row.get(field, 0):
                    return False
        for sid in parent:
            peers = [row for row in selected if row["shard"] == sid]
            if len(peers) < 3 or (not alive_only and len(peers) != 4):
                return False
            for field in ("state_digest", "chain_digest", "kv_digest", "applied_batches"):
                if len({row[field] for row in peers}) != 1:
                    return False
        return True
    return wait_status(run, finished, timeout)


def journal(run, shard, replica=0):
    path = run / f"shard{shard}" / f"node{replica}" / "commits.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()]


def assert_real_pbft(case, run, alive_only=False):
    """Business phases must carry real local prepared/commit certificates."""
    for node in c.read(run / "manifest.json")["nodes"]:
        if alive_only and not c.is_our_process(node):
            continue
        for entry in journal(run, node["shard"], node["replica"]):
            certificate = entry["certificate"]
            proposal = certificate["proposal"]["body"]
            signers = {vote["body"]["from"] for vote in certificate["commits"]}
            case.assertGreaterEqual(len(signers), 3)
            case.assertGreaterEqual(len({vote["body"]["from"] for vote in certificate["prepares"]}), 2)
            for vote in certificate["commits"] + certificate["prepares"]:
                body = vote["body"]
                case.assertEqual(body["shard"], node["shard"])
                case.assertEqual(body["seq"], entry["seq"])
                case.assertEqual(body["digest"], proposal["digest"])


def assert_invalid_init_proofs_rejected(case, run):
    cfg = c.read(run / "config.json")
    proof = next(entry["certificate"]["proposal"]["body"]["value"]["init"]
                 for entry in journal(run, 1)
                 if entry["certificate"]["proposal"]["body"]["value"].get("sag_action") == "PREPARE")
    for vote in proof["votes"]:
        case.assertNotIn("record", vote, "QC must carry the large semantic record only once")
        case.assertNotIn("record", vote["body"])
        case.assertEqual(vote["body"]["phase"], "INIT")
        case.assertEqual(vote["body"]["batch"], proof["record"]["batch"])
        case.assertEqual(vote["body"]["record_digest"], digest(packed(proof["record"])))
    short = copy.deepcopy(proof); short["votes"] = short["votes"][:2]
    repeated = copy.deepcopy(proof); repeated["votes"] = [repeated["votes"][0]] * 3
    changed = copy.deepcopy(proof)
    changed["record"]["requests"][0]["body"]["txs"][0]["accesses"][0]["value"] += 1
    source = next(node for node in cfg["nodes"] if (node["shard"], node["replica"]) == (5, 0))
    destination = next(node for node in cfg["nodes"] if (node["shard"], node["replica"]) == (1, 0))
    fields = ("executed_transactions", "kv_digest", "state_digest", "applied_batches")
    for bad in (short, repeated, changed):
        before = next(row for row in c.statuses(run) if (row["shard"], row["replica"]) == (1, 0))
        body = {"type": "SAG_QC", "run": cfg["run_id"], "shard": 5, "from": 0,
                "view": before["view"], "target": 1, "proof": bad}
        envelope = c.signed(body, source["private_key"], run)
        c.send_frame(destination["host"], destination["port"], envelope)
        rows = wait_status(run, lambda rows: next(row for row in rows if
                            (row["shard"], row["replica"]) == (1, 0))["rejected_messages"]
                            > before["rejected_messages"], timeout=5)
        after = next(row for row in rows if (row["shard"], row["replica"]) == (1, 0))
        case.assertEqual(tuple(after[field] for field in fields), tuple(before[field] for field in fields))


def packed(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def expected_serial_cross_kv(workload):
    """Independent application replay; no protocol QCs or implementation calls."""
    kv = {}
    initial = {"version": 0, "value": 0, "digest": digest("initial")}
    for request in workload["requests"]:
        for tx in request["txs"]:
            accesses = {access["shard"]: access for access in tx["accesses"]}
            owners = sorted(tx["participants"])
            old = {owner: copy.deepcopy(kv.setdefault(owner, {}).get(accesses[owner]["key"], initial))
                   for owner in owners}
            following = {}
            for owner in owners:
                dependency = packed(old[owner]) + "".join(packed(old[remote]) for remote in owners if remote != owner)
                following[owner] = {"version": old[owner]["version"] + 1,
                    "value": (accesses[owner]["value"] + 1 + sum(old[remote]["value"] for remote in owners if remote != owner))
                             & ((1 << 64) - 1),
                    "fib": 1, "digest": digest(dependency + packed(tx) + str(owner))}
            for owner in owners:
                kv[owner][accesses[owner]["key"]] = following[owner]
    return {owner: digest(packed(accounts)) for owner, accounts in kv.items()}


def stop_node(run, shard, replica):
    node = next(node for node in c.read(run / "manifest.json")["nodes"]
                if (node["shard"], node["replica"]) == (shard, replica))
    if c.is_our_process(node):
        os.kill(node["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 5
    while c.is_our_process(node) and time.monotonic() < deadline:
        time.sleep(.05)
    if c.is_our_process(node):
        raise AssertionError("failed to stop selected forward")


class SaguaroIntegration(unittest.TestCase):
    def test_two_layer_shared_keys_and_real_two_phase_consensus(self):
        with running("two-layer") as run:
            workload = job(run, "two-layer", [1, 2], count=24, shared=True)
            self.assertTrue(all(request["target"] == 5 for request in workload["requests"]))
            assert_complete(self, *client(run, "load", workload), 24)
            rows = settled(run, [workload])
            assert_real_pbft(self, run)
            assert_invalid_init_proofs_rejected(self, run)
            # INIT/decision at the coordinator and prepare/finalize at each leaf
            # require separate actual PBFT slots, not a simulated 2PC timer.
            for sid in (1, 2, 5):
                self.assertGreaterEqual(next(row for row in rows if row["shard"] == sid)["applied_batches"], 2)
                values = [entry["certificate"]["proposal"]["body"]["value"] for entry in journal(run, sid)]
                actions = {value.get("sag_action") for value in values}
                self.assertEqual(actions, {"INIT", "DECIDE"} if sid == 5 else {"PREPARE", "FINISH"})
                self.assertTrue(all("cst_orders" not in value and "cst_frontier" not in value for value in values))
                if sid == 5:
                    submitted = [request["body"]["txs"] for value in values if value["sag_action"] == "INIT"
                                 for request in value["requests"]]
                    for request in workload["requests"]:
                        self.assertIn(request["txs"], submitted, "the full signed client request must remain intact")

    def test_three_layer_lca_and_three_participant_transactions(self):
        with running("three-layer", THREE_LAYER) as run:
            workloads = []
            for index, participants in enumerate(([1, 2], [1, 3], [1, 2, 3])):
                workload = job(run, f"route-{index}", participants, count=8, shared=True)
                self.assertEqual(workload["requests"][0]["target"], 5 if index == 0 else 7)
                workloads.append(workload)
                assert_complete(self, *client(run, f"route-{index}", workload), 8)
                rows = settled(run, workloads)
            self.assertTrue(all(row["executed_transactions"] == 0 and row["ordered_cst_transactions"] == 0
                                for row in rows if row["shard"] in (4, 6)))
            assert_real_pbft(self, run)

    def test_intra_execution_and_cross_replay_do_not_execute_twice(self):
        with running("intra-and-replay") as run:
            intra = job(run, "intra", shard=1, count=8, shared=True)
            cross = job(run, "cross", [1, 2], count=8, shared=True)
            assert_complete(self, *client(run, "intra", intra), 8)
            assert_complete(self, *client(run, "cross", cross), 8)
            before = settled(run, [intra, cross])
            replay = copy.deepcopy(cross)
            for request in replay["requests"]:
                request["id"] += ":alias"
            rc, result = client(run, "replay", replay)
            self.assertEqual(rc, 0)
            self.assertEqual(result["completed_requests"], result["requests"])
            self.assertEqual(result["executed_transactions"] + result["duplicate_transactions"], 8)
            after = settled(run, [intra, cross])
            self.assertEqual([(row["shard"], row["replica"], row["kv_digest"], row["executed_transactions"])
                              for row in before],
                             [(row["shard"], row["replica"], row["kv_digest"], row["executed_transactions"])
                              for row in after])
            mixed = job(run, "mixed-old-new", [1, 2], count=1, batch=1, shared=True)
            mixed["requests"][0]["txs"].insert(0, copy.deepcopy(cross["requests"][0]["txs"][0]))
            rc, result = client(run, "mixed-old-new", mixed)
            self.assertEqual(rc, 0, result)
            self.assertEqual(result["completed_requests"], 1)
            self.assertEqual(result["executed_transactions"], 1)
            self.assertEqual(result["duplicate_transactions"], 1)
            self.assertEqual(result["ordered_only_transactions"], 0)
            self.assertEqual(result["errors"], 0)
            settled(run, [intra, cross, mixed])
            assert_real_pbft(self, run)

    def test_overlapping_coordinators_and_intra_lock_conflicts_complete(self):
        with running("conflicts", THREE_LAYER) as run:
            workloads = [job(run, "local-cross", [1, 2], count=8, batch=2, shared=True),
                         job(run, "root-cross", [1, 3], count=8, batch=2, shared=True),
                         job(run, "local-intra", shard=1, count=8, batch=2, shared=True)]
            clients = []
            try:
                for index, workload in enumerate(workloads):
                    source, output = run / f"concurrent-{index}.workload.json", run / f"concurrent-{index}.result.json"
                    c.write(source, workload)
                    process = subprocess.Popen([str(c.binary_for_method("saguaro")), "client", str(run / "config.json"),
                                                str(source), str(output)])
                    clients.append((process, output))
                for process, output in clients:
                    assert_complete(self, process.wait(timeout=60), c.read(output), 8)
                settled(run, workloads)
                assert_real_pbft(self, run)
            finally:
                for process, _ in clients:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait(timeout=5)

    def test_abort_releases_locks_without_partial_application(self):
        with running("abort-atomicity", THREE_LAYER, consensus={"view_timeout_ms": 5000}) as run:
            paused = [node for node in c.read(run / "manifest.json")["nodes"] if node["shard"] == 2]
            workloads = [job(run, "held-local", [1, 2], count=1, batch=1, shared=True),
                         job(run, "conflicting-root", [1, 3], count=1, batch=1, shared=True)]
            clients = []
            resumed = False
            try:
                for node in paused:
                    os.kill(node["pid"], signal.SIGSTOP)
                for index, workload in enumerate(workloads):
                    source, output = run / f"atomic-{index}.workload.json", run / f"atomic-{index}.result.json"
                    c.write(source, workload)
                    process = subprocess.Popen([str(c.run_binary(run)), "client", str(run / "config.json"),
                                                str(source), str(output)])
                    clients.append((process, output))
                    if index == 0:
                        wait_status(run, lambda rows: all(row.get("sag_held_locks", 0) > 0
                                    for row in rows if row["shard"] == 1), timeout=10)
                rows = wait_status(run, lambda rows: any(row.get("sag_aborted_batches", 0) > 0
                                   for row in rows if row["shard"] == 7), timeout=15)
                self.assertTrue(all(row["executed_transactions"] == 0
                                    for row in rows if row["shard"] in (1, 2, 3)))
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                resumed = True
                for process, output in clients:
                    assert_complete(self, process.wait(timeout=60), c.read(output), 1)
                settled(run, workloads)
                assert_real_pbft(self, run)
            finally:
                if not resumed:
                    for node in paused:
                        if c.is_our_process(node):
                            os.kill(node["pid"], signal.SIGCONT)
                for process, _ in clients:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait(timeout=5)

    def test_coordinator_and_participant_forward_view_change(self):
        with running("view-change") as run:
            before = job(run, "before", [1, 2], count=4, shared=True)
            assert_complete(self, *client(run, "before", before), 4)
            settled(run, [before])
            stop_node(run, 5, 0)
            stop_node(run, 1, 0)
            after = job(run, "after", [1, 2], count=8, shared=True)
            assert_complete(self, *client(run, "after", after), 8)
            rows = settled(run, [before, after], alive_only=True)
            self.assertTrue(all(row["view"] >= 1 for row in rows if row["shard"] in (1, 5) and row["alive"]))
            assert_real_pbft(self, run, alive_only=True)

    def test_forward_failure_while_participant_holds_prepared_locks(self):
        with running("midphase-view-change", consensus={"view_timeout_ms": 1500}) as run:
            paused = [node for node in c.read(run / "manifest.json")["nodes"] if node["shard"] == 2]
            workload = job(run, "midphase", [1, 2], count=4, batch=4, shared=True)
            process = None
            resumed = False
            try:
                for node in paused:
                    os.kill(node["pid"], signal.SIGSTOP)
                source, output = run / "midphase.workload.json", run / "midphase.result.json"
                c.write(source, workload)
                process = subprocess.Popen([str(c.run_binary(run)), "client", str(run / "config.json"),
                                            str(source), str(output)])
                wait_status(run, lambda rows: all(row.get("sag_held_locks", 0) > 0
                            and row["executed_transactions"] == 0 for row in rows if row["shard"] == 1), timeout=10)
                stop_node(run, 5, 0)
                stop_node(run, 1, 0)
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                resumed = True
                assert_complete(self, process.wait(timeout=60), c.read(output), 4)
                rows = settled(run, [workload], alive_only=True)
                self.assertTrue(all(row["view"] >= 1 for row in rows
                                    if row["alive"] and row["shard"] in (1, 5)))
                assert_real_pbft(self, run, alive_only=True)
            finally:
                if not resumed:
                    for node in paused:
                        if c.is_our_process(node):
                            os.kill(node["pid"], signal.SIGCONT)
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=5)

    def test_checkpoint_catchup_restores_two_phase_records_and_counts(self):
        with running("checkpoint-catchup", consensus={"checkpoint_batches": 2}) as run:
            cfg = c.read(run / "config.json")
            paused = [node for node in c.read(run / "manifest.json")["nodes"]
                      if node["shard"] in (1, 5) and node["replica"] == 3]
            resumed = False
            workloads = []
            try:
                for node in paused:
                    os.kill(node["pid"], signal.SIGSTOP)
                for index in range(3):
                    workload = job(run, f"catchup-{index}", [1, 2], count=8, batch=4, shared=True)
                    workloads.append(workload)
                    assert_complete(self, *client(run, f"catchup-{index}", workload), 8)
                wait_status(run, lambda rows: all(row["stable_seq"] >= 2 and
                            (row["executed_transactions"] == 24 if row["shard"] == 1
                             else row["ordered_cst_transactions"] == 24)
                            for row in rows if row["shard"] in (1, 5) and row["replica"] != 3))
                for node in paused:
                    source = next(peer for peer in cfg["nodes"] if
                                  (peer["shard"], peer["replica"]) == (node["shard"], 0))
                    envelope = c.signed({"type": "SYNC_REQUEST", "run": cfg["run_id"],
                        "shard": node["shard"], "from": 3, "view": 0, "after": 0, "stable_seq": 0},
                        node["private_key"], run)
                    c.send_frame(source["host"], source["port"], envelope)
                time.sleep(.2)
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                resumed = True
                settled(run, workloads, timeout=35)
                for node in paused:
                    path = run / f"shard{node['shard']}" / "node3" / "events.jsonl"
                    events = [json.loads(line) for line in path.read_text().splitlines()]
                    self.assertTrue(any(event["event"] == "state_sync" and event["seq"] >= 2
                                        for event in events), events)
                assert_real_pbft(self, run)
            finally:
                if not resumed:
                    for node in paused:
                        if c.is_our_process(node):
                            os.kill(node["pid"], signal.SIGCONT)

    def test_serial_shared_key_values_match_independent_replay_and_arbor(self):
        shared_workload, observed = None, {}
        for method in ("arbor", "saguaro"):
            with running("serial-semantics-" + method, method=method) as run:
                if shared_workload is None:
                    shared_workload = job(run, "same-semantic-input", [1, 2], count=3, batch=3, shared=True)
                assert_complete(self, *client(run, "load", shared_workload), 3)
                expected = expected_serial_cross_kv(shared_workload)
                rows = wait_status(run, lambda rows: all(
                    row["executed_transactions"] == (3 if row["shard"] in (1, 2) else 0)
                    and (row["shard"] not in expected or row["kv_digest"] == expected[row["shard"]])
                    for row in rows))
                observed[method] = {sid: next(row["kv_digest"] for row in rows if row["shard"] == sid)
                                    for sid in (1, 2)}
                self.assertEqual(observed[method], expected)
        self.assertEqual(observed["arbor"], observed["saguaro"])

    def test_comparator_replays_one_identical_unsigned_workload(self):
        raw = {"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": free_range(12),
               "shards": [{"id": sid, "parent": parent} for sid, parent in TWO_LAYER],
               "consensus": {"batch_size": 8, "batch_wait_ms": 5, "cross_shard_batch_size": 8,
                             "cross_shard_batch_wait_ms": 10, "view_timeout_ms": 3000,
                             "checkpoint_batches": 4}, "execution": {"fib_iterations": 1},
               "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                           "shard_links": [], "trace": False}}
        config, output = TEST_ROOT / "comparison.config.json", TEST_ROOT / "comparison"
        c.write(config, raw)
        result = subprocess.run(["python3", str(ROOT / "baseline/compare.py"), "--config", str(config),
                                 "--participants", "1,2", "--count", "16", "--rate", "1000", "--batch", "4",
                                 "--repeat", "1", "--timeout", "30", "--drain-timeout", "20",
                                 "--output-dir", str(output), "--skip-build"], timeout=100)
        self.assertEqual(result.returncode, 0)
        report = c.read(output / "summary.json")
        self.assertEqual({row["method"] for row in report["cases"]}, {"arbor", "saguaro"})
        self.assertTrue(all(row["status"] == "PASS" for row in report["cases"]), report)
        workload_paths = list(output.rglob("workload.json"))
        self.assertGreaterEqual(len(workload_paths), 2)
        workloads = [c.read(path) for path in workload_paths]
        self.assertTrue(all(workload == workloads[0] for workload in workloads[1:]), workload_paths)


if __name__ == "__main__":
    unittest.main(verbosity=2)
