#!/usr/bin/env python3
"""Isolated regression checks for request limits, deduplication and forwarding."""
import contextlib
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_engineering", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("engineering-" + time.strftime("%Y%m%d-%H%M%S-") + os.urandom(4).hex())
TEST_ROOT.mkdir(parents=True)


def free_range(count):
    for base in range(38000, 59000, 29):
        sockets = []
        try:
            for port in range(base, base + count):
                sock = socket.socket()
                sockets.append(sock)
                sock.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("no free test ports")


@contextlib.contextmanager
def running(name, shard_links=None, consensus=None, intra_shard_delay_ms=1):
    raw = c.read(ROOT / "config/two_layer.json")
    raw["base_port"] = free_range(12)
    raw["consensus"] = {"batch_size": 8, "batch_wait_ms": 5,
                        "cross_shard_batch_size": 4, "cross_shard_batch_wait_ms": 300,
                        "view_timeout_ms": 1800, "checkpoint_batches": 4}
    raw["consensus"].update(consensus or {})
    raw["execution"] = {"fib_iterations": 1}
    raw["network"] = {"intra_shard_delay_ms": intra_shard_delay_ms, "default_inter_shard_delay_ms": 10,
                      "shard_links": shard_links or [], "trace": False}
    config = TEST_ROOT / (name + ".json")
    c.write(config, raw)
    run = c.start(config, TEST_ROOT / name)
    try:
        yield run
    finally:
        c.stop_run(run)


def wait_status(run, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    rows = []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        if predicate(rows):
            return rows
        time.sleep(.1)
    raise AssertionError("cluster did not reach expected state: " + json.dumps(rows))


def run_client(run, name, workload):
    job, output = run / (name + ".workload.json"), run / (name + ".result.json")
    c.write(job, workload)
    result = subprocess.run([str(c.BIN), "client", str(run / "config.json"),
                             str(job), str(output)], timeout=workload["timeout_s"] + 10)
    return result.returncode, c.read(output)


def stop_node(run, shard, replica):
    node = next(n for n in c.read(run / "manifest.json")["nodes"]
                if n["shard"] == shard and n["replica"] == replica)
    if c.is_our_process(node):
        os.kill(node["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and c.is_our_process(node):
        time.sleep(.03)
    if c.is_our_process(node):
        raise AssertionError("test node did not stop")


def packed(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(packed(value).encode()).hexdigest()


def node_for(cfg, shard, replica):
    return next(n for n in cfg["nodes"] if n["shard"] == shard and n["replica"] == replica)


def replica_message(run, cfg, shard, replica, kind, fields):
    body = {"type": kind, "run": cfg["run_id"], "shard": shard,
            "from": replica, "view": 0, **fields}
    return c.signed(body, node_for(cfg, shard, replica)["private_key"], run)


def receive_frame(connection):
    def exact(size):
        data = bytearray()
        while len(data) < size:
            chunk = connection.recv(size - len(data))
            if not chunk:
                raise ConnectionError("peer closed before delivering a complete test frame")
            data.extend(chunk)
        return bytes(data)
    size = struct.unpack("!I", exact(4))[0]
    if not 0 < size <= 16 * 1024 * 1024:
        raise AssertionError("invalid response frame length")
    return json.loads(exact(size))


def capture_completed_ack_proofs(run, cfg, key):
    """Use a stopped replica's authenticated recovery API to obtain real votes."""
    stop_node(run, 5, 3)
    receiver = node_for(cfg, 5, 3)
    query = replica_message(run, cfg, 5, 3, "CST_RESULT_QUERY", {"batch_keys": [key]})
    source = node_for(cfg, 5, 0)
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((receiver["host"], receiver["port"]))
        listener.listen(8)
        listener.settimeout(.2)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            # The first response may encounter the just-closed persistent TCP
            # connection. A repeat QUERY obtains a new connection, without
            # guessing or rebuilding the legitimate ACK digests in the test.
            c.send_frame(source["host"], source["port"], query)
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(2)
                try:
                    envelope = receive_frame(connection)
                except (ConnectionError, socket.timeout):
                    continue
                if envelope["body"]["type"] != "CST_RESULT_PROOFS":
                    continue
                item = next((item for item in envelope["body"]["proofs"]
                             if item["batch_key"] == key), None)
                if item is not None and len(item["leaves"]) == 2:
                    return {proof["shard"]: proof["votes"] for proof in item["leaves"]}
    raise AssertionError("coordinator did not return the real completed ACK proof")


def reply_from_one_replica(run, cfg, request, replica):
    """Inspect a replica's result, avoiding the client's f+1 majority masking it."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(8)
        listener.settimeout(.2)
        body = copy.deepcopy(request)
        body.update({"type": "CLIENT", "run": cfg["run_id"],
                     "reply": {"host": "127.0.0.1", "port": listener.getsockname()[1]}})
        envelope = c.signed(body, cfg["client_private_key"], run)
        node = node_for(cfg, body["target"], replica)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            c.send_frame(node["host"], node["port"], envelope)
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(2)
                try:
                    response = receive_frame(connection)["body"]
                except (ConnectionError, socket.timeout):
                    continue
                if response["type"] == "REPLY" and response["from"] == replica and \
                        response["request"] == body["id"]:
                    return response["results"]
    raise AssertionError("selected replica did not return the cached transaction result")


def rejected_after_send(run, cfg, destination, env):
    node = node_for(cfg, *destination)
    before = c.read(Path(node["directory"]) / "status.json")["rejected_messages"]
    c.send_frame(node["host"], node["port"], env)
    return wait_status(run, lambda rows: next(r for r in rows if
                       (r["shard"], r["replica"]) == destination)["rejected_messages"] > before,
                       timeout=3)


def assert_forward_logs(case, run):
    stages = set()
    for node in c.read(run / "manifest.json")["nodes"]:
        events = [json.loads(line) for line in
                  (Path(node["directory"]) / "events.jsonl").read_text().splitlines()]
        for event in events:
            if event["event"].startswith("cst_") and event["event"].endswith("_forwarded"):
                stages.add(event["event"])
                case.assertEqual(event["forward"], node["replica"])
                case.assertEqual(event["view"] % 4, node["replica"])
                if event["event"] in ("cst_prepared_forwarded", "cst_ack_forwarded"):
                    case.assertGreaterEqual(event["signers"], 3)
    case.assertTrue({"cst_order_forwarded", "cst_prepared_forwarded", "cst_ack_forwarded"}.issubset(stages))
    case.assertTrue({"cst_ready_forwarded", "cst_decision_forwarded", "cst_done_forwarded"}.isdisjoint(stages))


def journal_entries(run, shard, replica=0):
    return [json.loads(line) for line in
            (run / f"shard{shard}" / f"node{replica}" / "commits.jsonl").read_text().splitlines()]


def events_for(run, shard, replica=0):
    return [json.loads(line) for line in
            (run / f"shard{shard}" / f"node{replica}" / "events.jsonl").read_text().splitlines()]


def settled(run, leaf_counts, root_count, orders):
    def matches(rows):
        return all(r["alive"] and
                   (r["executed_transactions"] == leaf_counts[r["shard"]] and
                    r["staged_cst_batches"] == 0 if r["shard"] in leaf_counts else
                    r["ordered_cst_transactions"] == root_count and r["cst_order_index"] == orders)
                   for r in rows)
    return wait_status(run, matches)


def assert_no_invalid_proposals_or_view_changes(case, run):
    case.assertTrue(all(r["view_changes"] == 0 for r in c.statuses(run)))
    for node in c.read(run / "manifest.json")["nodes"]:
        events = [json.loads(line) for line in
                  (Path(node["directory"]) / "events.jsonl").read_text().splitlines()]
        case.assertFalse(any(event["event"] == "invalid_preprepare_value" for event in events))


class Configuration(unittest.TestCase):
    def setUp(self):
        self.cfg = c.validate(c.read(ROOT / "config/two_layer.json"))

    def test_cross_shard_default_uses_cross_shard_consensus_limit(self):
        limit = self.cfg["consensus"]["cross_shard_batch_size"]
        workload = c.prepare_workload(self.cfg, 2 * limit + 1, 100, 1, "default", participants=[1, 2])
        self.assertEqual([len(r["txs"]) for r in workload["requests"]], [limit, limit, 1])
        self.assertTrue(all(r["target"] == 5 for r in workload["requests"]))

    def test_explicit_cross_shard_request_cannot_exceed_consensus_limit(self):
        limit = self.cfg["consensus"]["cross_shard_batch_size"]
        with self.assertRaisesRegex(ValueError, r"跨片 --batch.*cross_shard_batch_size"):
            c.prepare_workload(self.cfg, limit + 1, 100, 1, "oversized",
                               participants=[1, 2], batch=limit + 1)

    def test_intra_shard_default_keeps_its_own_consensus_limit(self):
        limit = self.cfg["consensus"]["batch_size"]
        for selector in ({"shard": 1}, {"participants": [1]}, {}):
            with self.subTest(selector=selector):
                workload = c.prepare_workload(self.cfg, limit + 1, 100, 1, "local", **selector)
                self.assertEqual([len(r["txs"]) for r in workload["requests"]], [limit, 1])

    def test_explicit_zero_batch_is_rejected(self):
        with self.assertRaises(ValueError):
            c.prepare_workload(self.cfg, 1, 100, 1, "zero", participants=[1, 2], batch=0)


class Integration(unittest.TestCase):
    def test_default_cross_shard_batch_completes(self):
        with running("default-cross") as run:
            rc, result = c.load(run, count=12, rate=1000, seed=1,
                                prefix="default-cross", participants=[1, 2], timeout=15)
            self.assertEqual(rc, 0)
            self.assertEqual((result["executed_transactions"], result["completed_requests"]), (12, 3))
            settled(run, {1: 12, 2: 12}, 12, 3)
            assert_forward_logs(self, run)

    def test_indivisible_requests_fill_batch_without_waiting_for_unreachable_limit(self):
        # Six intact ten-transaction requests fit in 64; a seventh cannot.
        # A batch that cannot take its next request must be sent immediately,
        # even though its 60 transactions do not equal the configured limit.
        with running("indivisible-batch", consensus={
                "batch_size": 1000, "cross_shard_batch_size": 64,
                "cross_shard_batch_wait_ms": 2500, "view_timeout_ms": 5000}) as run:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 70, 1000, 25, "indivisible-batch",
                                         participants=[1, 2], batch=10, timeout=15)
            job, output = run / "indivisible.workload.json", run / "indivisible.result.json"
            c.write(job, workload)
            client = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"),
                                       str(job), str(output)])
            try:
                wait_status(run, lambda rows: any(
                    r["shard"] == 5 and r["replica"] == 0 and
                    r["applied_batches"] >= 1 and r["cst_order_index"] >= 1
                    for r in rows), timeout=1.5)
                first = journal_entries(run, 5)[0]["certificate"]["proposal"]["body"]["value"]
                self.assertEqual(len(first["requests"]), 6)
                self.assertEqual([len(request["body"]["txs"]) for request in first["requests"]], [10] * 6)
                self.assertEqual(sum(len(request["body"]["txs"]) for request in first["requests"]), 60)
                self.assertEqual(client.wait(timeout=20), 0)
                result = c.read(output)
                self.assertEqual((result["completed_requests"], result["executed_transactions"],
                                  result["duplicate_transactions"], result["errors"]), (7, 70, 0, 0))
                rows = wait_status(run, lambda rows: all(
                    r["alive"] and r["applied_batches"] == 2 and
                    (r["executed_transactions"] == 70 and r["staged_cst_batches"] == 0
                     if r["shard"] in (1, 2) else
                     r["ordered_cst_transactions"] == 70 and r["completed_cst_transactions"] == 70
                     and r["cst_order_index"] == 2)
                    for r in rows) and all(
                        len({r["state_digest"] for r in rows if r["shard"] == shard}) == 1
                        for shard in (1, 2, 5)))
                for shard in (1, 2, 5):
                    shard_rows = [r for r in rows if r["shard"] == shard]
                    self.assertEqual(len({r["kv_digest"] for r in shard_rows}), 1)
                    for replica in range(4):
                        self.assertEqual([entry["seq"] for entry in journal_entries(run, shard, replica)], [1, 2])
                orders = [entry["certificate"]["proposal"]["body"]["value"]
                          for entry in journal_entries(run, 5)]
                self.assertEqual([sum(len(request["body"]["txs"]) for request in value["requests"])
                                  for value in orders], [60, 10])
                assert_no_invalid_proposals_or_view_changes(self, run)
            finally:
                if client.poll() is None:
                    client.terminate()
                    try:
                        client.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait(timeout=5)

    def test_oversized_signed_request_cannot_block_following_valid_request(self):
        with running("oversized-signed") as run:
            cfg = c.read(run / "config.json")
            oversized = c.prepare_workload(cfg, 5, 1000, 6, "a-oversized",
                                          participants=[1, 2], batch=4, timeout=8)
            oversized["requests"][0]["txs"].extend(oversized["requests"][1]["txs"])
            valid = c.prepare_workload(cfg, 1, 1000, 7, "z-valid",
                                      participants=[1, 2], batch=1, timeout=8)
            oversized["requests"] = [oversized["requests"][0], valid["requests"][0]]
            rc, result = run_client(run, "oversized-signed", oversized)
            self.assertEqual(rc, 2)
            self.assertEqual(result["executed_transactions"], 1)
            settled(run, {1: 1, 2: 1}, 1, 1)

    def test_completed_transaction_alias_skips_new_consensus(self):
        with running("completed-alias") as run:
            cfg = c.read(run / "config.json")
            for selector, prefix in [({"shard": 1}, "local-alias"),
                                     ({"participants": [1, 2]}, "cross-alias")]:
                with self.subTest(selector=selector):
                    workload = c.prepare_workload(cfg, 1, 1000, 2, prefix, timeout=15, **selector)
                    rc, result = run_client(run, prefix, workload)
                    self.assertEqual((rc, result["executed_transactions"]), (0, 1))
                    before = settled(run, {1: 1 if selector.get("shard") else 2,
                                           2: 0 if selector.get("shard") else 1},
                                     0 if selector.get("shard") else 1,
                                     0 if selector.get("shard") else 1)
                    replay = copy.deepcopy(workload)
                    replay["requests"][0]["id"] += ":alias"
                    rc, result = run_client(run, prefix + "-replay", replay)
                    self.assertEqual(rc, 0)
                    self.assertEqual((result["executed_transactions"], result["duplicate_transactions"]), (0, 1))
                    time.sleep(.3)
                    after = c.statuses(run)
                    fields = ("applied_batches", "executed_transactions", "ordered_cst_transactions",
                              "cst_order_index", "finalized_cst_batches", "state_digest")
                    self.assertEqual([[r[f] for f in fields] for r in before],
                                     [[r[f] for f in fields] for r in after])

    def test_pending_transaction_alias_has_one_ordered_request(self):
        with running("pending-alias") as run:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 1, 1000, 3, "pending-alias",
                                         participants=[1, 2], batch=1, timeout=15)
            alias = copy.deepcopy(workload["requests"][0])
            alias["id"] += ":alias"
            workload["requests"].append(alias)
            rc, result = run_client(run, "pending-alias", workload)
            self.assertEqual(rc, 0)
            self.assertEqual(result["completed_requests"], 2)
            settled(run, {1: 1, 2: 1}, 1, 1)
            for replica in range(4):
                entries = [json.loads(line) for line in
                           (run / "shard5" / ("node" + str(replica)) / "commits.jsonl")
                           .read_text().splitlines()]
                requests = [request for entry in entries for request in
                            entry["certificate"]["proposal"]["body"]["value"]["requests"]]
                self.assertEqual(len(requests), 1)

    def test_mixed_requests_sharing_completed_transaction_finish_in_separate_batches(self):
        for name, selector in [("local-mixed", {"shard": 1}),
                               ("cross-mixed", {"participants": [1, 2]})]:
            with self.subTest(name=name), running(name) as run:
                cfg = c.read(run / "config.json")
                old = c.prepare_workload(cfg, 1, 1000, 13, name + "-old", timeout=20, **selector)
                rc, result = run_client(run, name + "-old", old)
                self.assertEqual((rc, result["executed_transactions"]), (0, 1))
                cross = "participants" in selector
                settled(run, {1: 1, 2: 1 if cross else 0}, 1 if cross else 0, 1 if cross else 0)
                mixed = c.prepare_workload(cfg, 2, 1000, 14, name + "-new",
                                          batch=1, timeout=20, **selector)
                for request in mixed["requests"]:
                    request["txs"].insert(0, copy.deepcopy(old["requests"][0]["txs"][0]))
                rc, result = run_client(run, name + "-mixed", mixed)
                self.assertEqual(rc, 0)
                self.assertEqual((result["completed_requests"], result["executed_transactions"],
                                  result["duplicate_transactions"]), (2, 2, 1))
                settled(run, {1: 3, 2: 3 if cross else 0}, 3 if cross else 0, 3 if cross else 0)
                wait_status(run, lambda rows: all(r["pending_requests"] == 0 and
                                                 r["dedup_waiting_requests"] == 0 for r in rows))
                assert_no_invalid_proposals_or_view_changes(self, run)

    def test_backup_alias_arriving_before_primary_canonical_is_cleared(self):
        for name, selector in [("local-reordered-alias", {"shard": 1}),
                               ("cross-reordered-alias", {"participants": [1, 2]})]:
            with self.subTest(name=name), running(name, consensus={"cross_shard_batch_wait_ms": 0},
                                                 intra_shard_delay_ms=200) as run:
                cfg = c.read(run / "config.json")
                workload = c.prepare_workload(cfg, 1, 1000, 15, name, batch=1, timeout=25, **selector)
                canonical = workload["requests"][0]
                shard = canonical["target"]
                backup = node_for(cfg, shard, 1)
                alias = copy.deepcopy(canonical)
                alias["id"] += ":backup-alias"
                # The backup's automatic forward waits 200 ms. Direct client
                # delivery reaches the primary first and gives it another ID
                # for the same transaction, while the backup retains alias.
                with socket.socket() as reply:
                    reply.bind(("127.0.0.1", 0))
                    reply.listen(8)
                    alias.update({"type": "CLIENT", "run": cfg["run_id"],
                                  "reply": {"host": "127.0.0.1", "port": reply.getsockname()[1]}})
                    envelope = c.signed(alias, cfg["client_private_key"], run)
                    c.send_frame(backup["host"], backup["port"], envelope)
                    deadline = time.monotonic() + 1
                    while time.monotonic() < deadline:
                        if c.read(Path(backup["directory"]) / "status.json")["pending_requests"] == 1:
                            break
                        time.sleep(.005)
                    else:
                        self.fail("backup did not retain its earlier alias request")
                    rc, result = run_client(run, name, workload)
                    self.assertEqual((rc, result["executed_transactions"]), (0, 1))
                    cross = "participants" in selector
                    settled(run, {1: 1, 2: 1 if cross else 0}, 1 if cross else 0, 1 if cross else 0)
                    wait_status(run, lambda rows: all(r["pending_requests"] == 0 and
                                                     r["dedup_waiting_requests"] == 0 for r in rows))
                    time.sleep(.3)
                    for replica in range(4):
                        entries = [json.loads(line) for line in
                                   (run / ("shard" + str(shard)) / ("node" + str(replica)) /
                                    "commits.jsonl").read_text().splitlines()]
                        requests = [request for entry in entries for request in
                                    entry["certificate"]["proposal"]["body"]["value"]["requests"]]
                        self.assertEqual([request["body"]["id"] for request in requests], [canonical["id"]])
                    assert_no_invalid_proposals_or_view_changes(self, run)

    def test_primary_and_forward_failure_recovers_per_shard(self):
        with running("forward-failure") as run:
            rc, _ = c.load(run, count=1, rate=1000, seed=4, prefix="before-failure",
                           participants=[1, 2], timeout=15)
            self.assertEqual(rc, 0)
            settled(run, {1: 1, 2: 1}, 1, 1)
            for shard in (1, 2, 5):
                stop_node(run, shard, 0)
            rc, result = c.load(run, count=4, rate=1000, seed=5, prefix="after-failure",
                                participants=[1, 2], timeout=30)
            self.assertEqual(rc, 0)
            self.assertEqual(result["executed_transactions"], 4)
            alive = wait_status(run, lambda rows: all(
                r["view"] >= 1 and r["forward_replica"] == r["primary"] and
                r["executed_transactions"] == 5 if r["shard"] in (1, 2) else
                r["view"] >= 1 and r["forward_replica"] == r["primary"] and
                r["ordered_cst_transactions"] == 5
                for r in rows if r["alive"]))
            self.assertEqual(sum(r["alive"] for r in alive), 9)
            assert_forward_logs(self, run)

    def test_forward_failure_while_dependency_batch_is_staged(self):
        with running("forward-staged", [{"shards": [1, 2], "delay_ms": 500}]) as run:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 4, 1000, 10, "staged-failure",
                                         participants=[1, 2], timeout=30)
            job, output = run / "staged.workload.json", run / "staged.result.json"
            c.write(job, workload)
            client = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"),
                                       str(job), str(output)])
            try:
                staged = wait_status(run, lambda rows: any(
                    r["shard"] == 1 and r["replica"] == 0 and r["staged_cst_batches"] == 1
                    and r["finalized_cst_batches"] == 0 for r in rows), timeout=5)
                self.assertTrue(any(r["shard"] == 1 and r["staged_cst_batches"] == 1
                                    for r in staged))
                stop_node(run, 1, 0)
                rc = client.wait(timeout=35)
                self.assertEqual(rc, 0)
                result = c.read(output)
                self.assertEqual(result["executed_transactions"], 4)
                rows = wait_status(run, lambda rows: all(
                    r["view"] >= 1 and r["forward_replica"] == r["primary"] and
                    r["executed_transactions"] == 4 and r["staged_cst_batches"] == 0
                    for r in rows if r["shard"] == 1 and r["alive"]))
                self.assertEqual(sum(r["alive"] for r in rows if r["shard"] == 1), 3)
                for replica in (1, 2, 3):
                    self.assertEqual([entry["seq"] for entry in journal_entries(run, 1, replica)], [1])
                assert_forward_logs(self, run)
            finally:
                if client.poll() is None:
                    client.terminate()
                    try:
                        client.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait(timeout=5)

    def test_incomplete_and_repeated_signers_do_not_form_cross_shard_quorums(self):
        with running("forged-quorums") as run:
            rc, _ = c.load(run, count=1, rate=1000, seed=8, prefix="quorum-source",
                           participants=[1, 2], timeout=15)
            self.assertEqual(rc, 0)
            before = settled(run, {1: 1, 2: 1}, 1, 1)
            cfg = c.read(run / "config.json")
            witness = journal_entries(run, 1)[0]["execution_witness"]
            key = witness["batch_key"]
            prepared = next(proof for proof in witness["proofs"] if proof["record"]["shard"] == 1)
            record, prepared_votes = prepared["record"], prepared["votes"]
            order_digest = record["order_digest"]
            self.assertEqual(record["shard"], 1)
            ack_votes = {
                shard: [replica_message(run, cfg, shard, replica, "CST_ACK",
                        {"target": 5, "batch_key": key, "order_digest": order_digest,
                         "execution_digest": "0" * 64,
                         "result_digest": "0" * 64}) for replica in (0, 1, 2)]
                for shard in (1, 2)
            }
            for malformed in ("insufficient", "repeated-signer"):
                transform = (lambda votes: votes[:2]) if malformed == "insufficient" else \
                            (lambda votes: [votes[0], votes[0], votes[0]])
                with self.subTest(kind="CST_PREPARED_QC", malformed=malformed):
                    env = replica_message(run, cfg, 1, 0, "CST_PREPARED_QC",
                                          {"target": 2, "proof": {"record": record,
                                           "votes": transform(prepared_votes)}})
                    rejected_after_send(run, cfg, (2, 1), env)
                with self.subTest(kind="CST_ACK_QC", malformed=malformed):
                    env = replica_message(run, cfg, 1, 0, "CST_ACK_QC",
                                          {"target": 5, "proof": transform(ack_votes[1])})
                    rejected_after_send(run, cfg, (5, 1), env)
                with self.subTest(kind="CST_DEPENDENCY_PROOFS", malformed=malformed):
                    bad_witness = copy.deepcopy(witness)
                    bad_witness["proofs"][0]["votes"] = transform(prepared_votes)
                    env = replica_message(run, cfg, 1, 0, "CST_DEPENDENCY_PROOFS",
                                          {"target": 2, "witnesses": [bad_witness]})
                    rejected_after_send(run, cfg, (2, 1), env)
            with self.subTest(kind="tampered-dependency-record"):
                bad_record = copy.deepcopy(record)
                first_key = next(iter(bad_record["reads"]))
                bad_record["reads"][first_key]["value"] += 1
                env = replica_message(run, cfg, 1, 0, "CST_PREPARED_QC",
                                      {"target": 2, "proof": {"record": bad_record,
                                                              "votes": prepared_votes}})
                rejected_after_send(run, cfg, (2, 1), env)
            after = c.statuses(run)
            self.assertEqual([r["state_digest"] for r in before], [r["state_digest"] for r in after])

    def test_empty_first_ack_digest_does_not_form_a_quorum(self):
        with running("empty-ack-digests") as run:
            rc, result = c.load(run, count=4, rate=1000, seed=23, prefix="empty-ack-digests",
                                participants=[1, 2], timeout=15)
            self.assertEqual((rc, result["executed_transactions"]), (0, 4))
            settled(run, {1: 4, 2: 4}, 4, 1)
            cfg = c.read(run / "config.json")
            proofs = capture_completed_ack_proofs(run, cfg, "5:1")
            self.assertEqual(len(proofs[1]), 3)
            self.assertEqual(len({vote["body"]["from"] for vote in proofs[1]}), 3)
            for index, field in enumerate(("order_digest", "execution_digest", "result_digest")):
                with self.subTest(field=field):
                    # A fresh key prevents an already cached full QC from
                    # rejecting a conflicting result and hiding faulty voting
                    # logic. The signatures are valid; the empty identity is not.
                    bad_votes = []
                    for position, vote in enumerate(proofs[1]):
                        body = copy.deepcopy(vote["body"])
                        body["batch_key"] = f"5:{999 + index}"
                        if position == 0:
                            body[field] = ""
                        bad_votes.append(c.signed(body,
                            node_for(cfg, 1, body["from"])["private_key"], run))
                    envelope = replica_message(run, cfg, 1, 0, "CST_ACK_QC",
                                               {"target": 5, "proof": bad_votes})
                    rejected_after_send(run, cfg, (5, 1), envelope)
            rows = c.statuses(run)
            self.assertTrue(all(row["completed_cst_transactions"] == 4 and row["applied_batches"] == 1
                                for row in rows if row["shard"] == 5 and row["alive"]))

    def test_committed_slot_waits_for_dependencies_without_spurious_view_change(self):
        # The link takes longer than view_timeout_ms, while all primaries keep
        # sending heartbeats. This is execution waiting, not failed PBFT.
        with running("dependency-wait", [{"shards": [1, 2], "delay_ms": 2400}],
                     consensus={"view_timeout_ms": 1000}) as run:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 4, 1000, 20, "dependency-wait",
                                         participants=[1, 2], timeout=20)
            job, output = run / "wait.workload.json", run / "wait.result.json"
            c.write(job, workload)
            client = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"),
                                       str(job), str(output)])
            try:
                rows = wait_status(run, lambda rows: all(
                    r["staged_cst_batches"] == 1 and r["applied_batches"] == 0
                    and r["executed_transactions"] == 0 and r["kv_entries"] == 0
                    for r in rows if r["shard"] in (1, 2)), timeout=5)
                self.assertTrue(all(r["applied_batches"] == 1 and r["completed_cst_transactions"] == 0
                                    for r in rows if r["shard"] == 5))
                self.assertTrue(all(not journal_entries(run, shard, replica)
                                    for shard in (1, 2) for replica in range(4)))
                # Equal applied positions must not suppress a committed but
                # unexecuted certificate. Receiving that certificate without
                # the remote witness must leave execution in its waiting slot.
                previous_events = len(events_for(run, 1))
                sync = replica_message(run, cfg, 1, 3, "SYNC_REQUEST",
                                       {"after": 0, "stable_seq": 0})
                primary = node_for(cfg, 1, 0)
                c.send_frame(primary["host"], primary["port"], sync)
                rows = wait_status(run, lambda _: any(
                    event["event"] == "sync_proofs_sent" and
                    event["destination_replica"] == 3 and event["after"] == 0 and
                    event["certificates"] == 1 and event["committed_waiting"] == 1 and
                    event["execution_witnesses"] == 0
                    for event in events_for(run, 1)[previous_events:]), timeout=2)
                self.assertTrue(all(r["staged_cst_batches"] == 1 and r["applied_batches"] == 0 and
                                    r["executed_transactions"] == 0 and r["kv_entries"] == 0
                                    for r in rows if r["shard"] in (1, 2)))
                time.sleep(1.3)
                rows = c.statuses(run)
                self.assertTrue(all(r["staged_cst_batches"] == 1 and r["applied_batches"] == 0
                                    for r in rows if r["shard"] in (1, 2)))
                self.assertTrue(all(r["view_changes"] == 0 for r in rows))
                self.assertEqual(client.wait(timeout=25), 0)
                self.assertEqual(c.read(output)["executed_transactions"], 4)
                rows = settled(run, {1: 4, 2: 4}, 4, 1)
                self.assertTrue(all(r["applied_batches"] == 1 and r["view_changes"] == 0 for r in rows))
                for shard in (1, 2, 5):
                    for replica in range(4):
                        entries = journal_entries(run, shard, replica)
                        self.assertEqual([entry["seq"] for entry in entries], [1])
                        value = entries[0]["certificate"]["proposal"]["body"]["value"]
                        self.assertNotIn("cst_decisions", value)
                        self.assertNotIn("cst_finalizations", value)
                assert_forward_logs(self, run)
            finally:
                if client.poll() is None:
                    client.terminate()
                    client.wait(timeout=5)

    def test_finished_leaf_still_serves_dependency_witness(self):
        with running("dependency-archive") as run:
            rc, result = c.load(run, count=4, rate=1000, seed=21, prefix="dependency-archive",
                                participants=[1, 2], timeout=15)
            self.assertEqual((rc, result["executed_transactions"]), (0, 4))
            before = settled(run, {1: 4, 2: 4}, 4, 1)
            cfg = c.read(run / "config.json")
            witness = journal_entries(run, 1)[0]["execution_witness"]
            self.assertEqual(len(witness["proofs"]), 2)
            sent = sum(event["event"] == "cst_dependency_proofs_sent" for event in events_for(run, 1))
            received = sum(event["event"] == "cst_dependency_proofs_received"
                           for event in events_for(run, 2))
            query = replica_message(run, cfg, 2, 0, "CST_DEPENDENCY_QUERY",
                                    {"target": 1, "batch_keys": [witness["batch_key"]]})
            target = node_for(cfg, 1, 0)
            c.send_frame(target["host"], target["port"], query)
            wait_status(run, lambda _: sum(event["event"] == "cst_dependency_proofs_sent"
                                          for event in events_for(run, 1)) > sent and
                        sum(event["event"] == "cst_dependency_proofs_received"
                            for event in events_for(run, 2)) > received, timeout=5)
            after = c.statuses(run)
            fields = ("state_digest", "applied_batches", "executed_transactions", "finalized_cst_batches")
            self.assertEqual([[row[field] for field in fields] for row in before],
                             [[row[field] for field in fields] for row in after])
            self.assertTrue(all(len(journal_entries(run, shard, replica)) == 1
                                for shard in (1, 2, 5) for replica in range(4)))

    def test_backups_reject_same_request_id_with_different_transactions(self):
        with running("conflicting-proposal") as run:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 2, 1000, 9, "request-conflict",
                                         participants=[1, 2], batch=1, timeout=15)
            bodies = workload["requests"]
            bodies[1]["id"] = bodies[0]["id"]
            requests = []
            for body in bodies:
                body.update({"type": "CLIENT", "run": cfg["run_id"],
                             "reply": {"host": "127.0.0.1", "port": cfg["base_port"]}})
                requests.append(c.signed(body, cfg["client_private_key"], run))
            value = {"requests": requests, "cst_order_index": 1}
            pp = replica_message(run, cfg, 5, 0, "PREPREPARE",
                                 {"seq": 1, "digest": digest(value), "value": value})
            for replica in (1, 2, 3):
                target = node_for(cfg, 5, replica)
                c.send_frame(target["host"], target["port"], pp)
            def rejected_values(_):
                return all(any(json.loads(line)["event"] == "invalid_preprepare_value"
                               for line in (Path(node_for(cfg, 5, replica)["directory"]) /
                                            "events.jsonl").read_text().splitlines())
                           for replica in (1, 2, 3))
            wait_status(run, rejected_values, timeout=3)
            rows = c.statuses(run)
            self.assertTrue(all(r["applied_batches"] == 0 and r["ordered_cst_transactions"] == 0
                                and r["executed_transactions"] == 0 for r in rows))

    def test_checkpoint_catchup_recovers_completed_cross_shard_replies(self):
        with running("completed-catchup", consensus={"checkpoint_batches": 2}) as run:
            cfg = c.read(run / "config.json")
            paused = node_for(c.read(run / "manifest.json"), 5, 3)
            os.kill(paused["pid"], signal.SIGSTOP)
            try:
                original = c.prepare_workload(cfg, 36, 1000, 11, "before-catchup",
                                             participants=[1, 2], timeout=30)
                rc, original_result = run_client(run, "before-catchup", original)
                self.assertEqual((rc, original_result["executed_transactions"]), (0, 36))
                # Root orders 10 and 11 contain a transaction first completed
                # in order 2. JSON/map iteration visits "5:10" before "5:2";
                # rebuilding results in that order must not invent a new
                # digest for the old transaction or pin it as canonical.
                old = copy.deepcopy(original["requests"][1]["txs"][0])
                old_digest = next(timing["result"]["digest"] for timing in original_result["timings"]
                                  if timing["id"] == old["id"])
                mixed = c.prepare_workload(cfg, 6, 1000, 24, "mixed-before-catchup",
                                          participants=[1, 2], batch=3, timeout=30)
                for request in mixed["requests"]:
                    request["txs"].insert(0, copy.deepcopy(old))
                rc, mixed_result = run_client(run, "mixed-before-catchup", mixed)
                self.assertEqual((rc, mixed_result["executed_transactions"],
                                  mixed_result["duplicate_transactions"]), (0, 6, 1))
                mixed_old_result = next(timing["result"] for timing in mixed_result["timings"]
                                        if timing["id"] == old["id"])
                self.assertEqual(mixed_old_result["digest"], old_digest)
                wait_status(run, lambda rows: all(r["completed_cst_transactions"] == 44 and
                            r["cst_order_index"] == 11 and r["stable_seq"] >= 10
                            for r in rows if r["shard"] == 5 and r["replica"] != 3))
                # Queue a real, authenticated checkpoint response while this
                # replica is paused, ensuring recovery exercises state sync.
                sync = replica_message(run, cfg, 5, 3, "SYNC_REQUEST", {"after": 0, "stable_seq": 0})
                primary = node_for(cfg, 5, 0)
                c.send_frame(primary["host"], primary["port"], sync)
                time.sleep(.2)
                os.kill(paused["pid"], signal.SIGCONT)
                rc, result = c.load(run, count=4, rate=1000, seed=12, prefix="after-catchup",
                                    participants=[1, 2], timeout=20)
                self.assertEqual((rc, result["executed_transactions"]), (0, 4))
                rows = wait_status(run, lambda rows: all(r["completed_cst_transactions"] == 48
                                   for r in rows if r["shard"] == 5), timeout=15)
                events = [json.loads(line) for line in
                          (Path(paused["directory"]) / "events.jsonl").read_text().splitlines()]
                self.assertTrue(any(event["event"] == "state_sync" and event["seq"] >= 10
                                    for event in events), "paused replica replayed messages without checkpoint catchup")
                before_orders = [r["cst_order_index"] for r in rows if r["shard"] == 5]
                # Inspect the recovered replica directly. A normal client's
                # two matching replies could hide one replica's bad cache.
                alias = copy.deepcopy(original["requests"][1])
                alias["id"] += ":single-old-alias"
                alias["txs"] = [old]
                for replica in range(4):
                    results = reply_from_one_replica(run, cfg, alias, replica)
                    self.assertEqual(len(results), 1)
                    self.assertEqual((results[0]["id"], results[0]["kind"], results[0]["digest"]),
                                     (old["id"], "duplicate", old_digest))
                replay = copy.deepcopy(original)
                replay["requests"] = replay["requests"][1:2]
                replay["requests"][0]["id"] += ":alias"
                rc, result = run_client(run, "recovered-alias", replay)
                self.assertEqual((rc, result["executed_transactions"], result["duplicate_transactions"]), (0, 0, 4))
                rows = wait_status(run, lambda rows: all(r["dedup_waiting_requests"] == 0
                                   for r in rows if r["shard"] == 5))
                self.assertEqual([r["cst_order_index"] for r in rows if r["shard"] == 5], before_orders)
            finally:
                if c.is_our_process(paused):
                    os.kill(paused["pid"], signal.SIGCONT)

    def test_leaf_checkpoint_catchup_replays_post_checkpoint_execution_witness(self):
        # Three ORDERs with checkpoint interval 2 leave slot 3 outside the
        # stable snapshot. Its certificate alone lacks the remote read state.
        with running("leaf-witness-catchup", consensus={"checkpoint_batches": 2}) as run:
            cfg = c.read(run / "config.json")
            paused = node_for(c.read(run / "manifest.json"), 1, 3)
            os.kill(paused["pid"], signal.SIGSTOP)
            try:
                rc, result = c.load(run, count=12, rate=1000, seed=22, prefix="leaf-witness-catchup",
                                    participants=[1, 2], timeout=25)
                self.assertEqual((rc, result["executed_transactions"]), (0, 12))
                wait_status(run, lambda rows: all(r["executed_transactions"] == 12 and
                            r["applied_batches"] == 3 and r["stable_seq"] >= 2
                            for r in rows if r["shard"] == 1 and r["replica"] != 3))
                source_entries = journal_entries(run, 1)
                self.assertEqual([entry["seq"] for entry in source_entries], [1, 2, 3])
                self.assertIn("execution_witness", source_entries[-1])
                sync = replica_message(run, cfg, 1, 3, "SYNC_REQUEST", {"after": 0, "stable_seq": 0})
                primary = node_for(cfg, 1, 0)
                c.send_frame(primary["host"], primary["port"], sync)
                time.sleep(.2)
                os.kill(paused["pid"], signal.SIGCONT)
                rows = settled(run, {1: 12, 2: 12}, 12, 3)
                leaf = [row for row in rows if row["shard"] == 1]
                self.assertEqual(len({row["state_digest"] for row in leaf}), 1)
                self.assertEqual(len({row["kv_digest"] for row in leaf}), 1)
                self.assertTrue(all(row["applied_batches"] == 3 for row in leaf))
                self.assertTrue(any(event["event"] == "state_sync" and event["seq"] >= 2
                                    for event in events_for(run, 1, 3)),
                                "paused leaf replayed messages without exercising checkpoint + witness catchup")
                replay = copy.deepcopy(result["workload"])
                replay["requests"] = replay["requests"][-1:]
                replay["requests"][0]["id"] += ":alias"
                rc, replay_result = run_client(run, "leaf-witness-replay", replay)
                self.assertEqual((rc, replay_result["executed_transactions"],
                                  replay_result["duplicate_transactions"]), (0, 0, 4))
                self.assertTrue(all(row["applied_batches"] == 3
                                    for row in settled(run, {1: 12, 2: 12}, 12, 3)))
            finally:
                if c.is_our_process(paused):
                    os.kill(paused["pid"], signal.SIGCONT)


if __name__ == "__main__":
    print("工程回归日志目录:", TEST_ROOT, flush=True)
    unittest.main(verbosity=2)
