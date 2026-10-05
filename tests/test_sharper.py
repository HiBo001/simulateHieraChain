#!/usr/bin/env python3
"""Black-box SharPer regressions using real four-replica leaf clusters."""
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
import sys
import threading
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_sharper", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("sharper-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)
TWO_LAYER = [(1, 5), (2, 5), (5, None)]
THREE_LAYER = [(1, 5), (2, 5), (3, 6), (4, 6), (5, 7), (6, 7), (7, None)]
SHARPER_QUEUES = ("sharper_active_batches", "sharper_pending_batches", "sharper_waiting_execution")


def packed(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def free_range(count):
    for base in range(33000, 59000, 43):
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
    raise RuntimeError("no free ports for SharPer integration test")


def raw_config(topology=TWO_LAYER, consensus=None, trace=False):
    raw = {"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": free_range(4 * len(topology)),
           "shards": [{"id": sid, "parent": parent} for sid, parent in topology],
           "consensus": {"batch_size": 8, "batch_wait_ms": 5, "cross_shard_batch_size": 8,
                         "cross_shard_batch_wait_ms": 10, "view_timeout_ms": 1500,
                         "checkpoint_batches": 4},
           "execution": {"fib_iterations": 1},
           "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                       "shard_links": [], "trace": trace}}
    raw["consensus"].update(consensus or {})
    return raw


@contextlib.contextmanager
def running(name, topology=TWO_LAYER, consensus=None, method="sharper", trace=False):
    config = TEST_ROOT / (name + ".config.json")
    c.write(config, raw_config(topology, consensus, trace))
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
    raise AssertionError("SharPer did not settle: " + json.dumps(rows))


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
    rc = subprocess.run([str(c.run_binary(run)), "client", str(run / "config.json"),
                         str(source), str(output)], timeout=workload["timeout_s"] + 10).returncode
    return rc, c.read(output)


def launch_client(run, name, workload):
    source, output = run / (name + ".workload.json"), run / (name + ".result.json")
    c.write(source, workload)
    return subprocess.Popen([str(c.run_binary(run)), "client", str(run / "config.json"),
                             str(source), str(output)]), output


def stop_client(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def assert_complete(case, rc, result, count):
    case.assertEqual(rc, 0, result)
    case.assertEqual(result["completed_requests"], result["requests"])
    case.assertEqual(result["executed_transactions"], count)
    case.assertEqual(result["errors"], 0)
    case.assertEqual(result["ordered_only_transactions"], 0)
    case.assertGreater(result["completed_tps"], 0)
    case.assertGreaterEqual(result["p95_s"], 0)


def settled(run, workloads, alive_only=False, timeout=30):
    """Only participating leaves execute/order; ancestors perform no consensus."""
    parent, leaves = c.topology(c.read(run / "config.json"))
    executed, cross = {sid: set() for sid in parent}, {sid: set() for sid in parent}
    for workload in workloads:
        for request in workload["requests"]:
            for tx in request["txs"]:
                for sid in tx["participants"]:
                    executed[sid].add(tx["id"])
                    if len(tx["participants"]) > 1:
                        cross[sid].add(tx["id"])

    def finished(rows):
        selected = [row for row in rows if row["alive"] or not alive_only]
        for row in selected:
            sid = row["shard"]
            if not row["alive"] or not row.get("ready") or row.get("changing_view") or row.get("method") != "sharper":
                return False
            if row.get("executed_transactions") != len(executed[sid]):
                return False
            if row.get("ordered_cst_transactions") != 0 or row.get("completed_cst_transactions") != 0:
                return False
            if row.get("leaf_ordered_cst_transactions") != len(cross[sid]):
                return False
            if not executed[sid] and row.get("applied_batches") != 0:
                return False
            for field in ("pending_requests", "dedup_waiting_requests", "pending_cst_batches",
                          "staged_cst_batches"):
                if row.get(field, 0):
                    return False
            if any(field not in row or row[field] != 0 for field in SHARPER_QUEUES):
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


def node_for(cfg, shard, replica):
    return next(node for node in cfg["nodes"] if (node["shard"], node["replica"]) == (shard, replica))


def assert_signature(case, run, cfg, envelope, checked, public_key=None):
    """Verify Ed25519 with OpenSSL, independently of the C++ verifier."""
    key = packed(envelope)
    if key in checked:
        return
    body = envelope["body"]
    if public_key is None:
        public_key = node_for(cfg, body["shard"], body["from"])["public_key"]
    token = uuid.uuid4().hex
    message, signature = run / (token + ".message"), run / (token + ".signature")
    try:
        message.write_bytes(packed(body).encode())
        signature.write_bytes(bytes.fromhex(envelope["signature"]))
        result = subprocess.run([c.openssl(), "pkeyutl", "-verify", "-pubin", "-inkey", public_key,
                                 "-rawin", "-in", str(message), "-sigfile", str(signature)],
                                capture_output=True, text=True, timeout=5)
        case.assertEqual(result.returncode, 0, result.stderr)
        checked.add(key)
    finally:
        message.unlink(missing_ok=True)
        signature.unlink(missing_ok=True)


def assert_real_consensus(case, run, alive_only=False):
    cfg, checked, cross_certificates = c.read(run / "config.json"), set(), 0
    for node in c.read(run / "manifest.json")["nodes"]:
        if alive_only and not c.is_our_process(node):
            continue
        for entry in journal(run, node["shard"], node["replica"]):
            certificate = entry["certificate"]
            proposal = certificate["proposal"]["body"]
            case.assertEqual(proposal["shard"], node["shard"])
            case.assertEqual(proposal["seq"], entry["seq"])
            assert_signature(case, run, cfg, certificate["proposal"], checked)
            if certificate.get("sharper"):
                cross_certificates += 1
                # The signed original client envelopes remain intact, with
                # only the transport target remapped before client signing.
                value = proposal["value"]
                original_envelope, requests = value["sharper_propose"], value["requests"]
                original = original_envelope["body"]
                participants = requests[0]["body"]["txs"][0]["participants"]
                case.assertEqual(original["type"], "SH_SUPER_PROPOSE")
                case.assertNotIn("requests", original)
                case.assertNotIn("requests", original_envelope)
                case.assertEqual(original["request_digest"], digest(packed(requests)))
                assert_signature(case, run, cfg, original_envelope, checked)
                for request in requests:
                    case.assertEqual(request["body"]["target"], min(participants))
                    case.assertTrue(all(tx["participants"] == participants for tx in request["body"]["txs"]))
                    assert_signature(case, run, cfg, request, checked, cfg["client_public_key"])
                for field, message_type in (("prepares", "SH_ACCEPT"), ("commits", "SH_COMMIT")):
                    votes = certificate[field]
                    case.assertTrue(all(vote["body"]["type"] == message_type for vote in votes))
                    case.assertEqual({vote["body"]["shard"] for vote in votes}, set(participants))
                    for sid in participants:
                        signers = {vote["body"]["from"] for vote in votes if vote["body"]["shard"] == sid}
                        case.assertGreaterEqual(len(signers), 3)
                        case.assertTrue(signers.issubset(set(range(4))))
                    for vote in votes:
                        assert_signature(case, run, cfg, vote, checked)
                for field in ("seqs", "claims"):
                    commits = certificate["commits"]
                    case.assertTrue(all(field in vote["body"] for vote in commits), commits)
                    case.assertEqual(len({packed(vote["body"][field]) for vote in commits}), 1,
                                     "every shard must certify the same sequence vector and claims")
                for sid in participants:
                    records = [vote["body"]["record"] for vote in certificate["commits"]
                               if vote["body"]["shard"] == sid]
                    case.assertEqual(len({packed(record) for record in records}), 1,
                                     "each shard must have three matching votes for its local read/write record")
            else:
                # Intra-shard transactions still use the existing local PBFT.
                case.assertGreaterEqual(len({vote["body"]["from"] for vote in certificate["prepares"]}), 2)
                case.assertGreaterEqual(len({vote["body"]["from"] for vote in certificate["commits"]}), 3)
                for vote in certificate["prepares"] + certificate["commits"]:
                    case.assertEqual(vote["body"]["shard"], node["shard"])
                    case.assertEqual(vote["body"]["seq"], entry["seq"])
                    case.assertEqual(vote["body"]["digest"], proposal["digest"])
                    assert_signature(case, run, cfg, vote, checked)
    case.assertGreater(cross_certificates, 0, "cross-shard slots must carry real participant certificates")


def expected_serial_cross_kv(workload):
    """Independent business replay without protocol proofs or C++ execution."""
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
        raise AssertionError("failed to stop selected replica")


def receive_frame(connection):
    def exact(size):
        data = b""
        while len(data) < size:
            part = connection.recv(size - len(data))
            if not part:
                raise ConnectionError("end of frame stream")
            data += part
        return data
    size = struct.unpack("!I", exact(4))[0]
    if not 0 < size <= 16 * 1024 * 1024:
        raise AssertionError("invalid frame length")
    return json.loads(exact(size))


def capture_sync(run, cfg):
    """Obtain a real stable checkpoint and certificates through recovery."""
    stop_node(run, 1, 3)
    receiver, source = node_for(cfg, 1, 3), node_for(cfg, 1, 0)
    query = c.signed({"type": "SYNC_REQUEST", "run": cfg["run_id"], "shard": 1,
                      "from": 3, "view": 0, "after": 0, "stable_seq": 0}, receiver["private_key"], run)
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((receiver["host"], receiver["port"]))
        listener.listen(8)
        listener.settimeout(.2)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            # A retry also replaces the stopped replica's previous persistent
            # TCP connection; the test does not manufacture a checkpoint.
            c.send_frame(source["host"], source["port"], query)
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(2)
                try:
                    message = receive_frame(connection)
                except (ConnectionError, socket.timeout):
                    continue
                if message["body"]["type"] == "SYNC" and message["body"]["certificates"]:
                    return message["body"]
    raise AssertionError("live replica did not provide its real recovery certificate")


class FakeReplies:
    """Observe client-signed routing and send controlled authenticated replies."""
    def __init__(self, run, cfg, pattern):
        self.run, self.cfg, self.pattern = run, cfg, pattern
        self.stopped = threading.Event()
        self.listeners, self.threads, self.frames, self.errors = [], [], [], []

    def __enter__(self):
        try:
            for replica in range(4):
                node = node_for(self.cfg, 1, replica)
                listener = socket.socket()
                self.listeners.append(listener)
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((node["host"], node["port"]))
                listener.listen(8)
                listener.settimeout(.05)
                thread = threading.Thread(target=self.capture, args=(replica, listener))
                self.threads.append(thread)
                thread.start()
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def capture(self, replica, listener):
        while not self.stopped.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError as error:
                if not self.stopped.is_set():
                    self.errors.append(str(error))
                return
            with connection:
                connection.settimeout(.15)
                while not self.stopped.is_set():
                    try:
                        envelope = receive_frame(connection)
                    except socket.timeout:
                        continue
                    except ConnectionError:
                        break
                    except (OSError, ValueError, AssertionError) as error:
                        if not self.stopped.is_set():
                            self.errors.append(str(error))
                        break
                    self.frames.append((replica, envelope))
                    if replica >= 2 or (self.pattern == "one" and replica != 0):
                        continue
                    body = envelope["body"]
                    sender_shard = 2 if self.pattern == "wrong-shard" else 1
                    result = {"id": body["txs"][0]["id"], "kind": "executed", "value": 1}
                    if self.pattern == "different":
                        result["value"] += replica
                    reply = {"type": "REPLY", "run": self.cfg["run_id"], "shard": sender_shard,
                             "from": replica, "view": 0, "request": body["id"], "results": [result]}
                    signed = c.signed(reply, node_for(self.cfg, sender_shard, replica)["private_key"], self.run)
                    # Repeated messages from one signer cannot stand in for
                    # two independent matching replicas.
                    for _ in range(2 if self.pattern == "one" else 1):
                        try:
                            c.send_frame(body["reply"]["host"], body["reply"]["port"], signed)
                        except OSError:
                            break

    def __exit__(self, *_):
        self.stopped.set()
        for listener in self.listeners:
            listener.close()
        for thread in self.threads:
            thread.join(timeout=3)
            if thread.is_alive():
                self.errors.append("capture thread did not stop")


class SharPerIntegration(unittest.TestCase):
    def test_first_cross_transaction_after_idle_keeps_the_initial_view(self):
        with running("idle-first-cross") as run:
            time.sleep(3)
            workload = job(run, "idle-first", [1, 2], count=4, batch=4, shared=True)
            assert_complete(self, *client(run, "load", workload), 4)
            rows = settled(run, [workload])
            self.assertTrue(all(row["view"] == 0 and row["view_changes"] == 0 for row in rows), rows)
            assert_real_consensus(self, run)

    def test_cross_batch_size_respects_the_shared_configuration_contract(self):
        with self.assertRaisesRegex(ValueError, "cross_shard_batch_size"):
            c.validate(raw_config(consensus={"batch_size": 2, "cross_shard_batch_size": 8}))
        cfg = c.validate(raw_config(consensus={"batch_size": 8, "cross_shard_batch_size": 2}))
        self.assertEqual(cfg["consensus"]["cross_shard_batch_size"], 2)

    def test_two_leaf_direct_quorums_same_keys_and_no_ancestor(self):
        with running("two-leaf", trace=True) as run:
            workload = job(run, "two-leaf", [1, 2], count=24, shared=True)
            self.assertTrue(all(request["target"] == 5 for request in workload["requests"]))
            assert_complete(self, *client(run, "load", workload), 24)
            rows = settled(run, [workload])
            assert_real_consensus(self, run)
            self.assertTrue(all(row["applied_batches"] == 0 for row in rows if row["shard"] == 5))
            cfg = c.read(run / "config.json")
            # Heartbeats keep appending to network.jsonl after settlement.
            # Stop the run so the parser sees flushed, complete trace records.
            c.stop_run(run)
            port_owner = {node["port"]: node["shard"] for node in cfg["nodes"]}
            for sid in (1, 2):
                for replica in range(4):
                    records = [json.loads(line) for line in
                               (run / f"shard{sid}" / f"node{replica}" / "network.jsonl").read_text().splitlines()]
                    node_ports = {row["dst_port"] for row in records if row["dst_port"] in port_owner}
                    self.assertTrue(all(port_owner[port] in (1, 2) for port in node_ports), records)
                    remote_ports = {node["port"] for node in cfg["nodes"] if node["shard"] == 3 - sid}
                    self.assertTrue(remote_ports.issubset(node_ports), (sid, replica, node_ports))
            self.assertEqual(journal(run, 5), [])

    def test_two_and_three_participant_transactions_leave_other_shards_idle(self):
        with running("three-layer", THREE_LAYER) as run:
            workloads = []
            for index, participants in enumerate(([1, 2], [1, 3], [2, 3], [1, 2, 3])):
                workload = job(run, f"route-{index}", participants, count=8, shared=True)
                self.assertEqual(workload["requests"][0]["target"], 5 if index == 0 else 7)
                workloads.append(workload)
                assert_complete(self, *client(run, f"route-{index}", workload), 8)
                rows = settled(run, workloads)
            for sid in (4, 5, 6, 7):
                self.assertTrue(all(row["executed_transactions"] == 0 and row["applied_batches"] == 0
                                    for row in rows if row["shard"] == sid))
                self.assertEqual(journal(run, sid), [])
            assert_real_consensus(self, run)

    def test_serial_shared_key_values_match_independent_replay_and_arbor(self):
        shared_workload, observed = None, {}
        for method in ("arbor", "sharper"):
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
        self.assertEqual(observed["arbor"], observed["sharper"])

    def test_intra_and_cross_aliases_execute_each_transaction_once(self):
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
            self.assertEqual(rc, 0, result)
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
            assert_real_consensus(self, run)

    def test_one_offline_replica_per_participant_still_completes(self):
        with running("one-offline") as run:
            stop_node(run, 1, 3)
            stop_node(run, 2, 3)
            workload = job(run, "offline", [1, 2], count=8, shared=True)
            assert_complete(self, *client(run, "load", workload), 8)
            settled(run, [workload], alive_only=True)
            assert_real_consensus(self, run, alive_only=True)

    def test_primary_failure_recovers_without_ancestor_coordinator(self):
        with running("primary-failure") as run:
            before = job(run, "before", [1, 2], count=4, shared=True)
            assert_complete(self, *client(run, "before", before), 4)
            settled(run, [before])
            stop_node(run, 1, 0)
            stop_node(run, 2, 0)
            after = job(run, "after", [1, 2], count=8, shared=True)
            assert_complete(self, *client(run, "after", after), 8)
            rows = settled(run, [before, after], alive_only=True)
            self.assertTrue(all(row["view"] >= 1 for row in rows
                                if row["alive"] and row["shard"] in (1, 2)))
            self.assertEqual(journal(run, 5), [])
            assert_real_consensus(self, run, alive_only=True)

    def test_missing_participant_quorum_cannot_execute_partially(self):
        with running("missing-quorum", consensus={"view_timeout_ms": 5000}) as run:
            paused = [node for node in c.read(run / "manifest.json")["nodes"]
                      if node["shard"] == 2 and node["replica"] in (2, 3)]
            process, resumed = None, False
            workload = job(run, "quorum", [1, 2], count=4, batch=4, shared=True)
            try:
                for node in paused:
                    os.kill(node["pid"], signal.SIGSTOP)
                process, output = launch_client(run, "load", workload)
                wait_status(run, lambda rows: any(row.get("sharper_active_batches", 0) > 0
                            for row in rows if row["shard"] == 1), timeout=10)
                # Both local primaries are healthy. Missing two remote votes
                # must prevent every participant from reaching execution.
                time.sleep(1)
                rows = c.statuses(run)
                self.assertTrue(all(row["executed_transactions"] == 0
                                    for row in rows if row["shard"] in (1, 2)), rows)
                self.assertIsNone(process.poll(), "client must not confirm a partially executed CST")
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                resumed = True
                assert_complete(self, process.wait(timeout=60), c.read(output), 4)
                settled(run, [workload])
                assert_real_consensus(self, run)
            finally:
                if not resumed:
                    for node in paused:
                        if c.is_our_process(node):
                            os.kill(node["pid"], signal.SIGCONT)
                stop_client(process)

    def test_origin_primary_failure_while_waiting_for_remote_quorum(self):
        with running("midphase-primary-failure") as run:
            paused = [node for node in c.read(run / "manifest.json")["nodes"] if node["shard"] == 2]
            process, resumed = None, False
            workload = job(run, "midphase", [1, 2], count=4, batch=4, shared=True)
            try:
                for node in paused:
                    os.kill(node["pid"], signal.SIGSTOP)
                process, output = launch_client(run, "load", workload)
                wait_status(run, lambda rows: all(row.get("sharper_active_batches", 0) > 0
                            and row["executed_transactions"] == 0
                            for row in rows if row["shard"] == 1), timeout=10)
                stop_node(run, 1, 0)
                # Keep the remote shard paused until recovery is observed.
                # Otherwise the three remaining old-view origin replicas can
                # finish directly, which legitimately needs no view change.
                wait_status(run, lambda rows: all(row["view"] >= 1
                            and row["executed_transactions"] == 0
                            for row in rows if row["shard"] == 1 and row["alive"]), timeout=15)
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                resumed = True
                assert_complete(self, process.wait(timeout=60), c.read(output), 4)
                rows = settled(run, [workload], alive_only=True)
                self.assertTrue(all(row["view"] >= 1 for row in rows
                                    if row["alive"] and row["shard"] == 1))
                self.assertEqual(journal(run, 5), [])
                assert_real_consensus(self, run, alive_only=True)
            finally:
                if not resumed:
                    for node in paused:
                        if c.is_our_process(node):
                            os.kill(node["pid"], signal.SIGCONT)
                stop_client(process)

    def test_client_remaps_before_signing_and_requires_two_matching_leaf_replies(self):
        with running("client-replies") as run:
            cfg = c.read(run / "config.json")
            for replica in range(4):
                stop_node(run, 1, replica)
            for pattern in ("one", "different", "wrong-shard", "matching"):
                with self.subTest(pattern=pattern):
                    workload = job(run, "reply-" + pattern, [1, 2], count=1, batch=1)
                    workload["timeout_s"] = .8
                    self.assertEqual(workload["requests"][0]["target"], 5)
                    with FakeReplies(run, cfg, pattern) as capture:
                        rc, result = client(run, "reply-" + pattern, workload)
                    self.assertFalse(capture.errors, capture.errors)
                    self.assertTrue(capture.frames, "client did not reach a participating leaf")
                    envelopes = [envelope for _, envelope in capture.frames]
                    self.assertTrue(all(envelope == envelopes[0] for envelope in envelopes), envelopes)
                    self.assertEqual(envelopes[0]["body"]["target"], 1)
                    self.assertEqual(envelopes[0]["body"]["txs"], workload["requests"][0]["txs"])
                    self.assertEqual(c.signed(envelopes[0]["body"], cfg["client_private_key"], run), envelopes[0])
                    self.assertEqual((rc, result["completed_requests"]),
                                     (0, 1) if pattern == "matching" else (2, 0), result)

    def test_duplicate_and_malicious_participant_certificates_do_not_apply(self):
        with running("participant-proof-validation") as run:
            workload = job(run, "proof-validation", [1, 2], count=4, batch=4, shared=True)
            assert_complete(self, *client(run, "load", workload), 4)
            settled(run, [workload])
            cfg = c.read(run / "config.json")
            sync = capture_sync(run, cfg)
            proof = next(certificate for certificate in sync["certificates"] if certificate.get("sharper"))
            proof = copy.deepcopy(proof)
            # Keep exactly the required honest quorum from each participant,
            # so replacing one vote truly leaves fewer than three matches.
            for field in ("prepares", "commits"):
                proof[field] = [vote for sid in (1, 2)
                                for vote in [vote for vote in proof[field] if vote["body"]["shard"] == sid][:3]]
            destination, source = node_for(cfg, 1, 1), node_for(cfg, 1, 0)
            fields = ("executed_transactions", "kv_digest", "state_digest", "applied_batches", "chain_digest")

            def selected(rows):
                return next(row for row in rows if (row["shard"], row["replica"]) == (1, 1))

            def send(certificate):
                before = selected(c.statuses(run))
                body = {"type": "SYNC", "run": cfg["run_id"], "shard": 1, "from": 0,
                        "view": before["view"], "stable": sync["stable"],
                        "certificates": [certificate], "execution_witnesses": []}
                c.send_frame(destination["host"], destination["port"], c.signed(body, source["private_key"], run))
                return before

            # A real certificate may be retransmitted without changing state.
            before = send(proof)
            send(proof)
            time.sleep(.3)
            after = selected(c.statuses(run))
            self.assertEqual(tuple(after[field] for field in fields), tuple(before[field] for field in fields))
            self.assertEqual(after["rejected_messages"], before["rejected_messages"])

            variants = []
            for field in ("prepares", "commits"):
                short = copy.deepcopy(proof)
                short[field] = [vote for vote in short[field] if vote["body"]["shard"] == 1]
                short[field] += [vote for vote in proof[field] if vote["body"]["shard"] == 2][:2]
                variants.append(("two-" + field, short))
                repeated = copy.deepcopy(proof)
                remote = next(vote for vote in repeated[field] if vote["body"]["shard"] == 2)
                repeated[field] = [vote for vote in repeated[field] if vote["body"]["shard"] == 1] + [remote] * 3
                variants.append(("repeated-" + field, repeated))
            changed = copy.deepcopy(proof)
            vote = next(vote for vote in changed["commits"] if vote["body"]["shard"] == 2)
            body = copy.deepcopy(vote["body"])

            def change_read(value):
                if isinstance(value, dict):
                    if type(value.get("value")) is int:
                        value["value"] = (value["value"] + 1) & ((1 << 64) - 1)
                        return True
                    return any(change_read(child) for child in value.values())
                if isinstance(value, list):
                    return any(change_read(child) for child in value)
                return False

            self.assertTrue(change_read(body["record"].get("reads", body["record"])),
                            "the malicious record must change a real application input")
            if "record_digest" in body:
                body["record_digest"] = digest(packed(body["record"]))
            changed["commits"][changed["commits"].index(vote)] = c.signed(
                body, node_for(cfg, 2, body["from"])["private_key"], run)
            variants.append(("honestly-signed-different-record", changed))
            outsider = copy.deepcopy(proof)
            remote = next(vote for vote in outsider["commits"] if vote["body"]["shard"] == 2)
            body = copy.deepcopy(remote["body"])
            body["shard"], body["from"] = 5, 0
            outsider["commits"][outsider["commits"].index(remote)] = c.signed(
                body, node_for(cfg, 5, 0)["private_key"], run)
            variants.append(("nonparticipant-cannot-replace-leaf-vote", outsider))
            forged = copy.deepcopy(proof)
            forged["commits"][0]["signature"] = "00" * 64
            variants.append(("forged-signature", forged))
            for name, bad in variants:
                with self.subTest(variant=name):
                    before = send(bad)
                    after = selected(wait_status(run, lambda rows:
                        selected(rows)["rejected_messages"] > before["rejected_messages"], timeout=5))
                    self.assertEqual(tuple(after[field] for field in fields), tuple(before[field] for field in fields))
            settled(run, [workload], alive_only=True)

    def test_arbor_comparator_reuses_one_identical_unsigned_workload(self):
        config, output = TEST_ROOT / "comparison.config.json", TEST_ROOT / "comparison"
        c.write(config, raw_config(consensus={"view_timeout_ms": 3000}))
        result = subprocess.run([sys.executable, str(ROOT / "baseline/compare.py"), "--baseline", "sharper",
                                 "--config", str(config), "--participants", "1,2", "--count", "16",
                                 "--rate", "1000", "--batch", "4", "--repeat", "1", "--timeout", "30",
                                 "--drain-timeout", "20", "--output-dir", str(output), "--skip-build"], timeout=100)
        self.assertEqual(result.returncode, 0)
        report = c.read(output / "summary.json")
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["baseline"], "sharper")
        self.assertEqual({row["method"] for row in report["cases"]}, {"arbor", "sharper"})
        self.assertTrue(all(row["status"] == "PASS" for row in report["cases"]), report)
        self.assertTrue(report["method_comparisons"][0]["comparable"])
        self.assertIn("arbor_over_sharper_tps", report["method_comparisons"][0])
        workload_paths = list(output.rglob("workload.json"))
        self.assertGreaterEqual(len(workload_paths), 2)
        workloads = [c.read(path) for path in workload_paths]
        self.assertTrue(all(workload == workloads[0] for workload in workloads[1:]), workload_paths)
        self.assertTrue(all(request["target"] == 5 for request in workloads[0]["requests"]))

    def test_original_90_10_mixed_workload_matches_arbor_and_participant_counts(self):
        config, output = TEST_ROOT / "mixed-comparison.config.json", TEST_ROOT / "mixed-comparison"
        c.write(config, raw_config(THREE_LAYER, consensus={"view_timeout_ms": 3000}))
        # This regression intentionally replays the original three-leaf 90/10
        # workload, independently of the new locality-aware default generator.
        result = subprocess.run([sys.executable, str(ROOT / "baseline/compare_mixed.py"), "--baseline", "sharper", "--uniform",
                                 "--config", str(config), "--count", "30", "--rate", "1000", "--batch", "3",
                                 "--repeat", "1", "--timeout", "45", "--drain-timeout", "20",
                                 "--output-dir", str(output), "--skip-build"], timeout=150)
        self.assertEqual(result.returncode, 0)
        report = c.read(output / "summary.json")
        self.assertEqual(report["status"], "PASS", report)
        self.assertEqual(report["participants_per_transaction"], {"2": 27, "3": 3})
        self.assertEqual(report["groups"], {"1,2": 9, "1,3": 9, "2,3": 9, "1,2,3": 3})
        self.assertEqual({row["method"] for row in report["cases"]}, {"arbor", "sharper"})
        self.assertEqual(len({row["workload_sha256"] for row in report["cases"]}), 1)
        for row in report["cases"]:
            self.assertEqual(row["client"]["executed_transactions"], 30)
            self.assertEqual(row["client"]["errors"], 0)
            self.assertEqual(row["client"]["ordered_only_transactions"], 0)
            after = c.read(Path(row["run_dir"]).parent / "status-after.json")
            self.assertTrue(all(peer["executed_transactions"] == (21 if peer["shard"] in (1, 2, 3) else 0)
                                for peer in after), after)
            if row["method"] == "sharper":
                self.assertTrue(all(peer["ordered_cst_transactions"] == 0
                                    and peer["completed_cst_transactions"] == 0 for peer in after), after)
                self.assertTrue(all(peer["applied_batches"] == 0 for peer in after
                                    if peer["shard"] in (4, 5, 6, 7)), after)
        workload_paths = list(output.rglob("workload.json"))
        self.assertEqual(len(workload_paths), 2)
        self.assertEqual(c.read(workload_paths[0]), c.read(workload_paths[1]))


if __name__ == "__main__":
    print("SharPer integration logs:", TEST_ROOT, flush=True)
    unittest.main(verbosity=2)
