#!/usr/bin/env python3
"""Real PBFT execution across NCA coordinators, certified frontiers and recovery."""
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
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_multilayer", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("multilayer-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)

THREE_LAYER = [(1, 5), (2, 5), (3, 6), (4, 6), (5, 7), (6, 7), (7, None)]
UNBALANCED = [(1, 5), (2, 7), (5, 7), (7, None)]
FOUR_LAYER = [(1, 5), (2, 5), (3, 6), (5, 8), (6, 9), (8, 9), (9, None)]
FLAT_FOUR_LEAVES = [(1, 7), (2, 7), (3, 7), (4, 7), (7, None)]
UINT64 = (1 << 64) - 1


def packed(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def free_range(count):
    for base in range(30000, 59000, 41):
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
def running(name, shards=THREE_LAYER, consensus=None, links=None):
    raw = {"replicas_per_shard": 4, "host": "127.0.0.1",
           "base_port": free_range(4 * len(shards)),
           "shards": [{"id": sid, "parent": parent} for sid, parent in shards],
           "consensus": {"batch_size": 8, "batch_wait_ms": 5,
                         "cross_shard_batch_size": 4, "cross_shard_batch_wait_ms": 30,
                         "view_timeout_ms": 3000, "checkpoint_batches": 4},
           "execution": {"fib_iterations": 1},
           "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 5,
                       "shard_links": links or [], "trace": False}}
    raw["consensus"].update(consensus or {})
    path = TEST_ROOT / (name + ".config.json")
    c.write(path, raw)
    run = c.start(path, TEST_ROOT / name)
    try:
        yield run
    finally:
        c.stop_run(run)


def wait_status(run, predicate, timeout=25):
    deadline = time.monotonic() + timeout
    rows = []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        if predicate(rows):
            return rows
        time.sleep(.1)
    raise AssertionError("cluster did not reach expected state: " + json.dumps(rows))


def node_for(cfg, shard, replica):
    return next(n for n in cfg["nodes"] if n["shard"] == shard and n["replica"] == replica)


def entries(run, shard, replica=0):
    path = run / f"shard{shard}" / f"node{replica}" / "commits.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()]


def events(run, shard, replica=0):
    path = run / f"shard{shard}" / f"node{replica}" / "events.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()]


def run_client(run, name, workload):
    job, output = run / (name + ".workload.json"), run / (name + ".result.json")
    c.write(job, workload)
    result = subprocess.run([str(c.BIN), "client", str(run / "config.json"), str(job), str(output)],
                            timeout=workload["timeout_s"] + 10)
    return result.returncode, c.read(output)


def workload(run, name, participants=None, count=4, batch=2, shared=True, shard=None):
    cfg = c.read(run / "config.json")
    job = c.prepare_workload(cfg, count, 1000, 42, name, participants=participants,
                             shard=shard, batch=batch, timeout=40)
    if shared:
        for request in job["requests"]:
            for tx in request["txs"]:
                if len(tx["participants"]) > 1:
                    for access in tx["accesses"]:
                        access["key"] = f"account:{access['shard']}:shared"
                    tx["key"] = tx["accesses"][0]["key"]
                else:
                    tx["key"] = f"account:{tx['participants'][0]}:shared"
    return job


def expected_counts(cfg, jobs):
    parent, leaves = c.topology(cfg)
    executed, ordered = {s: 0 for s in leaves}, {s: 0 for s in parent if s not in leaves}
    for job in jobs:
        for request in job["requests"]:
            for tx in request["txs"]:
                for leaf in tx["participants"]:
                    executed[leaf] += 1
                if len(tx["participants"]) > 1:
                    ordered[request["target"]] += 1
    return executed, ordered


def converged(run, jobs, alive_only=False, timeout=30):
    cfg = c.read(run / "config.json")
    executed, ordered = expected_counts(cfg, jobs)
    parent, leaves = c.topology(cfg)

    def settled(rows):
        selected = [r for r in rows if r["alive"] or not alive_only]
        for row in selected:
            sid = row["shard"]
            if not row["alive"] or row["executed_transactions"] != executed.get(sid, 0):
                return False
            if row["ordered_cst_transactions"] != ordered.get(sid, 0):
                return False
            if row.get("staged_cst_batches", 0) or row.get("pending_cst_batches", 0):
                return False
            if row.get("pending_requests", 0) or row.get("dedup_waiting_requests", 0):
                return False
            if sid not in leaves and row["completed_cst_transactions"] != ordered[sid]:
                return False
        for sid in parent:
            peers = [r for r in selected if r["shard"] == sid]
            if len(peers) < 3 or (not alive_only and len(peers) != 4):
                return False
            for field in ("state_digest", "chain_digest", "kv_digest"):
                if len({r[field] for r in peers}) != 1:
                    return False
        return True
    return wait_status(run, settled, timeout)


def assert_full_result(case, rc, result, count):
    case.assertEqual(rc, 0)
    case.assertEqual(result["executed_transactions"], count)
    case.assertEqual(result["ordered_only_transactions"], 0)
    case.assertEqual(result["errors"], 0)
    case.assertEqual(result["completed_requests"], result["requests"])


def assert_certificates_and_forwarding(case, run, alive_only=False):
    cfg = c.read(run / "config.json")
    nodes = c.read(run / "manifest.json")["nodes"]
    observed = set()
    for node in nodes:
        if alive_only and not c.is_our_process(node):
            continue
        for entry in entries(run, node["shard"], node["replica"]):
            cert = entry["certificate"]
            body = cert["proposal"]["body"]
            signers = {vote["body"]["from"] for vote in cert["commits"]}
            case.assertGreaterEqual(len(signers), 3)
            case.assertTrue(all(vote["body"]["shard"] == node["shard"]
                                and vote["body"]["seq"] == entry["seq"]
                                and vote["body"]["digest"] == body["digest"]
                                for vote in cert["commits"]))
            value = body["value"]
            case.assertNotIn("cst_decisions", value)
            case.assertNotIn("cst_finalizations", value)
            case.assertNotIn("pipeline_window", value)
            if "execution_witness" in entry:
                witness = entry["execution_witness"]
                for proof in witness["proofs"]:
                    case.assertEqual(len(proof["votes"]), 3)
                    case.assertEqual(len({vote["body"]["from"] for vote in proof["votes"]}), 3)
        for event in events(run, node["shard"], node["replica"]):
            if event["event"].startswith("cst_") and event["event"].endswith("_forwarded"):
                observed.add(event["event"])
                case.assertEqual(event["forward"], node["replica"])
                case.assertEqual(event["view"] % 4, node["replica"])
                if event["event"] in ("cst_prepared_forwarded", "cst_ack_forwarded"):
                    case.assertGreaterEqual(event["signers"], 3)
    case.assertTrue({"cst_order_forwarded", "cst_prepared_forwarded", "cst_ack_forwarded"} <= observed)
    case.assertTrue({"cst_ready_forwarded", "cst_decision_forwarded", "cst_done_forwarded"}.isdisjoint(observed))
    case.assertNotIn("pipeline_window", cfg["consensus"])


def assert_reference_state(case, run, alive_only=False):
    """Replay each certified leaf journal and independently evaluate its writes."""
    cfg = c.read(run / "config.json")
    _, leaves = c.topology(cfg)
    rows = c.statuses(run)
    initial = {"version": 0, "value": 0, "digest": digest("initial")}
    for leaf in leaves:
        peers = [r for r in rows if r["shard"] == leaf and (r["alive"] or not alive_only)]
        if not peers:
            continue
        replica = peers[0]["replica"]
        kv, seen, cross = {}, set(), 0
        for entry in entries(run, leaf, replica):
            value = entry["certificate"]["proposal"]["body"]["value"]
            for request in value["requests"]:
                for tx in request["body"]["txs"]:
                    if tx["id"] in seen:
                        continue
                    previous = kv.get(tx["key"], {"version": 0, "digest": digest("initial")})
                    kv[tx["key"]] = {"version": previous["version"] + 1, "value": tx["value"],
                                     "fib": 1, "digest": digest(packed(previous) + packed(tx) + "1")}
                    seen.add(tx["id"])
            if "cst_orders" not in value:
                continue
            witness = entry["execution_witness"]
            working = {proof["record"]["shard"]: copy.deepcopy(proof["record"]["reads"])
                       for proof in witness["proofs"]}
            case.assertIn(leaf, working)
            for key, state in working[leaf].items():
                case.assertEqual(state, kv.get(key, initial), "prepared local read must follow the committed journal")
            cert = value["cst_orders"][0]
            requests = cert["proposal"]["body"]["value"]["requests"]
            for request in requests:
                for tx in request["body"]["txs"]:
                    owners = sorted(tx["participants"])
                    if tx["id"] in seen:
                        continue
                    accesses = {a["shard"]: a for a in tx["accesses"]}
                    old = {owner: copy.deepcopy(working[owner][accesses[owner]["key"]]) for owner in owners}
                    next_states = {}
                    for owner in owners:
                        remote = [old[other] for other in owners if other != owner]
                        next_states[owner] = {"version": old[owner]["version"] + 1,
                            "value": (accesses[owner]["value"] + sum(state["value"] for state in remote) + 1) & UINT64,
                            "fib": 1, "digest": digest(packed(old[owner]) + "".join(packed(state) for state in remote)
                                                       + packed(tx) + str(owner))}
                    for owner in owners:
                        working[owner][accesses[owner]["key"]] = next_states[owner]
                    if leaf in owners:
                        seen.add(tx["id"])
                        cross += 1
            kv.update(working[leaf])
        for row in peers:
            case.assertEqual(row["kv_entries"], len(kv))
            case.assertEqual(row["kv_digest"], digest(packed(kv)), f"independent replay differs in shard {leaf}")
            case.assertEqual(row["executed_transactions"], len(seen))
            case.assertEqual(row["leaf_ordered_cst_transactions"], cross)


def stop_node(run, shard, replica):
    node = node_for(c.read(run / "manifest.json"), shard, replica)
    if c.is_our_process(node):
        os.kill(node["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline and c.is_our_process(node):
        time.sleep(.05)
    if c.is_our_process(node):
        raise AssertionError("node did not stop")
    return node


def receive_frame(connection):
    def exact(size):
        data = bytearray()
        while len(data) < size:
            chunk = connection.recv(size - len(data))
            if not chunk:
                raise ConnectionError("peer closed before delivering the reply")
            data.extend(chunk)
        return bytes(data)
    size = struct.unpack("!I", exact(4))[0]
    if not 0 < size <= 16 * 1024 * 1024:
        raise AssertionError("invalid response frame length")
    return json.loads(exact(size))


def reply_from_replica(run, cfg, request, replica):
    """Inspect the recovered replica directly, rather than hiding it behind f+1."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(8)
        listener.settimeout(.2)
        body = copy.deepcopy(request)
        body.update(type="CLIENT", run=cfg["run_id"],
                    reply={"host": "127.0.0.1", "port": listener.getsockname()[1]})
        envelope = c.signed(body, cfg["client_private_key"], run)
        node = node_for(cfg, body["target"], replica)
        deadline = time.monotonic() + 10
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
                if response["type"] == "REPLY" and response["from"] == replica and response["request"] == body["id"]:
                    return response["results"]
    raise AssertionError("recovered replica did not return cached results")


def signed_fixture_certificate(run, cfg, origin, value, seq=1):
    """Authentic signatures isolate frontier rules from signature rejection."""
    value_digest = digest(packed(value))
    common = {"run": cfg["run_id"], "shard": origin, "view": 0, "seq": seq, "digest": value_digest}
    proposal = c.signed(dict(common, type="PREPREPARE", **{"from": 0}, value=value),
                        node_for(cfg, origin, 0)["private_key"], run)
    prepares = [c.signed(dict(common, type="PREPARE", **{"from": replica}),
                         node_for(cfg, origin, replica)["private_key"], run) for replica in (1, 2)]
    commits = [c.signed(dict(common, type="COMMIT", **{"from": replica}),
                        node_for(cfg, origin, replica)["private_key"], run) for replica in (0, 1, 2)]
    return {"proposal": proposal, "prepares": prepares, "commits": commits}


def signed_fixture_request(run, cfg, name, participants):
    request = workload(run, name, participants, count=2)["requests"][0]
    request.update(type="CLIENT", run=cfg["run_id"], reply={"host": "127.0.0.1", "port": 9})
    return c.signed(request, cfg["client_private_key"], run)


def fixture_execution_witness(run, cfg, order):
    body = order["proposal"]["body"]
    requests = body["value"]["requests"]
    owners = sorted({owner for request in requests for tx in request["body"]["txs"]
                     for owner in tx["participants"]})
    key = f"{body['shard']}:{body['seq']}"
    initial = {"version": 0, "value": 0, "digest": digest("initial")}
    proofs = []
    for owner in owners:
        record = {"shard": owner, "batch_key": key, "order_digest": body["digest"],
                  "order_certificate": order, "reads": {}, "writes": []}
        for request in requests:
            for tx in request["body"]["txs"]:
                if owner not in tx["participants"]:
                    continue
                access = next(access for access in tx["accesses"] if access["shard"] == owner)
                record["reads"][access["key"]] = copy.deepcopy(initial)
                record["writes"].append({"id": tx["id"], "tx_digest": digest(packed(tx)), "key": access["key"],
                                         "value": access["value"], "fib": 1, "duplicate": False})
        canonical = {field: record[field] for field in ("shard", "batch_key", "order_digest", "reads", "writes")}
        votes = [c.signed({"type": "CST_PREPARED", "run": cfg["run_id"], "shard": owner,
                           "from": replica, "view": 0, "batch_key": key, "record_digest": digest(packed(canonical))},
                          node_for(cfg, owner, replica)["private_key"], run) for replica in (0, 1, 2)]
        proofs.append({"record": record, "votes": votes})
    return {"batch_key": key, "proofs": proofs}


def fixture_genesis():
    return {"seq": 0, "chain": digest("arbor-genesis"), "kv": {}, "seen": {}, "requests": {}, "executed": 0,
            "ordered_cst": 0, "cst_batches": {}, "cst_seen": {}, "leaf_ordered_cst": 0, "last_cst_seq": 0,
            "cst_finalized": {}, "cst_orders": {}, "cst_order_index": 0, "cst_round": 0, "cst_indices": {},
            "participant_indices": {}, "cst_rounds": {}}


def assert_bad_dependency_proofs(case, run):
    cfg = c.read(run / "config.json")
    entry = next(entry for entry in reversed(entries(run, 1)) if "execution_witness" in entry)
    witness = entry["execution_witness"]
    proof = next(proof for proof in witness["proofs"] if proof["record"]["shard"] == 3)
    source, destination = node_for(cfg, 3, 0), node_for(cfg, 1, 0)
    variants = []
    short = copy.deepcopy(proof); short["votes"] = short["votes"][:2]; variants.append(short)
    repeated = copy.deepcopy(proof); repeated["votes"] = [repeated["votes"][0]] * 3; variants.append(repeated)
    changed = copy.deepcopy(proof)
    changed["record"]["reads"][next(iter(changed["record"]["reads"]))]["value"] += 1
    variants.append(changed)
    before = c.read(Path(destination["directory"]) / "status.json")
    fields = ("state_digest", "kv_digest", "executed_transactions", "applied_batches")
    for bad in variants:
        rejected = c.read(Path(destination["directory"]) / "status.json")["rejected_messages"]
        envelope = c.signed({"type": "CST_PREPARED_QC", "run": cfg["run_id"], "shard": 3,
                             "from": 0, "view": 0, "target": 1, "proof": bad}, source["private_key"], run)
        c.send_frame(destination["host"], destination["port"], envelope)
        rows = wait_status(run, lambda rows: next(r for r in rows if (r["shard"], r["replica"]) == (1, 0))
                           ["rejected_messages"] > rejected, timeout=5)
        after = next(r for r in rows if (r["shard"], r["replica"]) == (1, 0))
        case.assertEqual(tuple(after[field] for field in fields), tuple(before[field] for field in fields))


def assert_dependencies_bound_to_participants(case, run):
    """Valid certificates must not authorize unrelated senders or destinations."""
    cfg = c.read(run / "config.json")
    entry = next(entry for entry in reversed(entries(run, 1)) if "execution_witness" in entry)
    witness = entry["execution_witness"]
    case.assertEqual([proof["record"]["shard"] for proof in witness["proofs"]], [1, 2])
    local_proof = next(proof for proof in witness["proofs"] if proof["record"]["shard"] == 1)
    redirects = ((1, 3, "CST_PREPARED_QC", {"proof": local_proof}),
                 (1, 3, "CST_DEPENDENCY_PROOFS", {"witnesses": [witness]}),
                 (3, 1, "CST_DEPENDENCY_PROOFS", {"witnesses": [witness]}),
                 (3, 1, "CST_DEPENDENCY_QUERY", {"batch_keys": [witness["batch_key"]]}))
    fields = ("state_digest", "kv_digest", "executed_transactions", "applied_batches", "staged_cst_batches")
    for source_shard, destination_shard, kind, details in redirects:
        source, destination = node_for(cfg, source_shard, 0), node_for(cfg, destination_shard, 0)
        before = c.read(Path(destination["directory"]) / "status.json")
        body = {"type": kind, "run": cfg["run_id"], "shard": source_shard, "from": 0,
                "view": 0, "target": destination_shard, **details}
        envelope = c.signed(body, source["private_key"], run)
        c.send_frame(destination["host"], destination["port"], envelope)
        rows = wait_status(run, lambda rows: next(row for row in rows if
                           (row["shard"], row["replica"]) == (destination_shard, 0))
                           ["rejected_messages"] > before["rejected_messages"], timeout=2)
        after = next(row for row in rows if (row["shard"], row["replica"]) == (destination_shard, 0))
        case.assertEqual(tuple(after[field] for field in fields), tuple(before[field] for field in fields))


class MultiLayerExecution(unittest.TestCase):
    def test_three_layers_local_root_and_three_four_participant_execution(self):
        with running("routing-and-participants") as run:
            cfg = c.read(run / "config.json")
            jobs = []
            for index, participants in enumerate(([1, 2], [1, 3], [3, 4], [1, 2, 3], [1, 2, 3, 4])):
                job = workload(run, f"routing-{index}", participants, count=4)
                self.assertEqual(job["requests"][0]["target"], c.lca(cfg, participants))
                jobs.append(job)
                assert_full_result(self, *run_client(run, f"routing-{index}", job), 4)
                converged(run, jobs)
                if index == 0:
                    assert_dependencies_bound_to_participants(self, run)
            # One intact signed request can contain several participant groups
            # with the same NCA; it must not be split into fabricated requests.
            mixed = workload(run, "mixed-participants", [1, 2, 3], count=4, batch=4)
            groups = ([1, 3], [2, 4], [1, 2, 3], [2, 3, 4])
            for index, (tx, owners) in enumerate(zip(mixed["requests"][0]["txs"], groups)):
                tx["participants"] = owners
                tx["accesses"] = [{"shard": owner, "key": f"account:{owner}:shared", "value": 100 + index * 10 + owner}
                                  for owner in owners]
                tx["key"], tx["value"] = tx["accesses"][0]["key"], tx["accesses"][0]["value"]
            jobs.append(mixed)
            assert_full_result(self, *run_client(run, "mixed-participants", mixed), 4)
            converged(run, jobs)
            root_requests = [request for entry in entries(run, 7)
                             for request in entry["certificate"]["proposal"]["body"]["value"]["requests"]
                             if request["body"]["id"] == mixed["requests"][0]["id"]]
            self.assertEqual(len(root_requests), 1)
            self.assertEqual(root_requests[0]["body"]["txs"], mixed["requests"][0]["txs"])
            assert_certificates_and_forwarding(self, run)
            assert_reference_state(self, run)
            assert_bad_dependency_proofs(self, run)

    def test_coordinator_indices_skip_uninvolved_participants_without_holes(self):
        cases = (("participant-holes", THREE_LAYER, ([1, 3], [2, 4], [1, 4], [2, 3], [1, 3])),
                 ("flat-four-leaf-holes", FLAT_FOUR_LEAVES, ([1, 2], [2, 3], [1, 3])))
        for name, shards, groups in cases:
            with self.subTest(topology=name), running(name, shards=shards) as run:
                jobs = []
                for index, participants in enumerate(groups):
                    job = workload(run, f"holes-{index}", participants, count=2)
                    jobs.append(job)
                    assert_full_result(self, *run_client(run, f"holes-{index}", job), 2)
                    converged(run, jobs)
                assert_reference_state(self, run)
                assert_certificates_and_forwarding(self, run)
                if shards == FLAT_FOUR_LEAVES:
                    for sid, _ in shards:
                        for entry in entries(run, sid):
                            value = entry["certificate"]["proposal"]["body"]["value"]
                            self.assertNotIn("cst_round", value)
                            self.assertNotIn("cst_frontier", value)

    def test_parallel_overlapping_coordinators_and_intra_transactions(self):
        with running("parallel-coordinators") as run:
            jobs = [workload(run, "parallel-local", [1, 2], count=8),
                    workload(run, "parallel-triple", [1, 2, 3], count=8),
                    workload(run, "parallel-root", [1, 3], count=8),
                    workload(run, "parallel-intra", shard=1, count=4)]
            clients = []
            try:
                for index, job in enumerate(jobs):
                    source, output = run / f"parallel-{index}.workload.json", run / f"parallel-{index}.result.json"
                    c.write(source, job)
                    process = subprocess.Popen([str(c.BIN), "client", str(run / "config.json"), str(source), str(output)])
                    clients.append((process, output, sum(len(request["txs"]) for request in job["requests"])))
                for process, output, count in clients:
                    assert_full_result(self, process.wait(timeout=50), c.read(output), count)
                converged(run, jobs, timeout=35)
                assert_reference_state(self, run)
                assert_certificates_and_forwarding(self, run)
                # Shared transactions must have the same relative order in every participant journal.
                sequences = {}
                for leaf in (1, 2, 3):
                    sequence = []
                    for entry in entries(run, leaf):
                        value = entry["certificate"]["proposal"]["body"]["value"]
                        for cert in value.get("cst_orders", []):
                            for request in cert["proposal"]["body"]["value"]["requests"]:
                                sequence.extend(tx["id"] for tx in request["body"]["txs"] if leaf in tx["participants"])
                    sequences[leaf] = sequence
                for left, right in ((1, 2), (1, 3), (2, 3)):
                    common = set(sequences[left]) & set(sequences[right])
                    self.assertEqual([tx for tx in sequences[left] if tx in common],
                                     [tx for tx in sequences[right] if tx in common])
            finally:
                for process, _, _ in clients:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)

    def test_unbalanced_and_four_layer_topologies(self):
        for name, shards, participants in (("unbalanced", UNBALANCED, [1, 2]),
                                           ("four-layer", FOUR_LAYER, [1, 3])):
            with self.subTest(name=name), running(name, shards=shards) as run:
                job = workload(run, name, participants, count=6)
                assert_full_result(self, *run_client(run, name, job), 6)
                converged(run, [job])
                assert_reference_state(self, run)
                assert_certificates_and_forwarding(self, run)

    def test_coordinator_and_leaf_forward_failure_retains_quorum(self):
        with running("forward-failure") as run:
            before = workload(run, "before-failure", [1, 2], count=2)
            assert_full_result(self, *run_client(run, "before-failure", before), 2)
            converged(run, [before])
            stop_node(run, 5, 0)
            stop_node(run, 1, 0)
            after = workload(run, "after-failure", [1, 2], count=4)
            assert_full_result(self, *run_client(run, "after-failure", after), 4)
            rows = converged(run, [before, after], alive_only=True)
            self.assertTrue(all(r["view"] >= 1 and r["forward_replica"] == r["primary"]
                                for r in rows if r["alive"] and r["shard"] in (1, 5)))
            assert_reference_state(self, run, alive_only=True)
            assert_certificates_and_forwarding(self, run, alive_only=True)

    def test_checkpoint_catchup_restores_multicoordinator_frontier_and_execution(self):
        with running("checkpoint-catchup", consensus={"checkpoint_batches": 2}) as run:
            cfg = c.read(run / "config.json")
            paused = [node_for(c.read(run / "manifest.json"), shard, 3) for shard in (1, 5)]
            for node in paused:
                os.kill(node["pid"], signal.SIGSTOP)
            jobs = []
            results = []
            try:
                for index, participants in enumerate(([1, 2], [1, 3], [1, 2, 3])):
                    job = workload(run, f"catchup-{index}", participants, count=4)
                    jobs.append(job)
                    rc, result = run_client(run, f"catchup-{index}", job)
                    results.append(result)
                    assert_full_result(self, rc, result, 4)
                wait_status(run, lambda rows: all(r["executed_transactions"] == 12 and r["stable_seq"] >= 2
                            for r in rows if r["shard"] == 1 and r["replica"] != 3))
                wait_status(run, lambda rows: all(r["ordered_cst_transactions"] == 4 and r["stable_seq"] >= 2
                            for r in rows if r["shard"] == 5 and r["replica"] != 3))
                for node in paused:
                    source = node_for(cfg, node["shard"], 0)
                    query = c.signed({"type": "SYNC_REQUEST", "run": cfg["run_id"], "shard": node["shard"],
                                      "from": 3, "view": 0, "after": 0, "stable_seq": 0},
                                     node["private_key"], run)
                    c.send_frame(source["host"], source["port"], query)
                time.sleep(.25)
                for node in paused:
                    os.kill(node["pid"], signal.SIGCONT)
                recovered_rows = converged(run, jobs)
                for node in paused:
                    recovery_events = events(run, node["shard"], 3)
                    installed = any(event["event"] == "state_sync" and event["seq"] >= 2
                                    for event in recovery_events)
                    if node["shard"] == 5:
                        self.assertTrue(installed, "the coordinator must exercise checkpoint state installation")
                    elif not installed:
                        # Retained PREPREPAREs and queued signed votes can
                        # replay the whole leaf prefix before SYNC arrives.
                        recovered = next(row for row in recovered_rows
                                         if (row["shard"], row["replica"]) == (1, 3))
                        journal = entries(run, 1, 3)
                        self.assertEqual([entry["seq"] for entry in journal],
                                         list(range(1, recovered["applied_batches"] + 1)))
                        cached = {event["digest"] for event in recovery_events
                                  if event["event"] == "future_preprepare_cached"}
                        accepted = {event["digest"] for event in recovery_events
                                    if event["event"] == "preprepare"}
                        self.assertTrue(cached & accepted, "the leaf must demonstrate replay of a retained proposal")
                        self.assertTrue(all("execution_witness" in entry for entry in journal))
                assert_reference_state(self, run)
                assert_certificates_and_forwarding(self, run)
                alias = copy.deepcopy(jobs[0]["requests"][0])
                alias["id"] += ":recovered-alias"
                old_results = {timing["id"]: timing["result"] for timing in results[0]["timings"]}
                cached = reply_from_replica(run, cfg, alias, 3)
                self.assertEqual(len(cached), len(alias["txs"]))
                for value in cached:
                    self.assertEqual(value["kind"], "duplicate")
                    self.assertEqual(value["digest"], old_results[value["id"]]["digest"])
                more = workload(run, "after-catchup", [1, 2], count=2)
                assert_full_result(self, *run_client(run, "after-catchup", more), 2)
                converged(run, jobs + [more])
                assert_reference_state(self, run)
            finally:
                for node in paused:
                    if c.is_our_process(node):
                        os.kill(node["pid"], signal.SIGCONT)

    def test_missing_nested_participant_cannot_apply_partial_writes(self):
        with running("missing-participant") as run:
            for replica in range(4):
                stop_node(run, 3, replica)
            job = workload(run, "missing-participant", [1, 3], count=2)
            job["timeout_s"] = 3
            rc, result = run_client(run, "missing-participant", job)
            self.assertEqual(rc, 2)
            self.assertEqual(result["executed_transactions"], 0)
            self.assertEqual(result["ordered_only_transactions"], 0)
            rows = wait_status(run, lambda rows: all(r["staged_cst_batches"] == 1
                               for r in rows if r["shard"] == 1), timeout=10)
            self.assertTrue(all(r["executed_transactions"] == 0 and r["kv_entries"] == 0
                                for r in rows if r["shard"] == 1))
            self.assertTrue(all(r["completed_cst_transactions"] == 0
                                for r in rows if r["shard"] == 7))

    def test_idle_backup_signed_future_slot_requests_and_recovers_missing_head(self):
        with running("idle-future-slot", consensus={"checkpoint_batches": 16}) as run:
            cfg = c.read(run / "config.json")
            head_request = signed_fixture_request(run, cfg, "missing-head", [1, 3])
            root_value = {"requests": [head_request], "cst_order_index": 1, "cst_round": 1,
                          "cst_watermarks": {"1": 1, "2": 0, "3": 1, "4": 0}}
            order = signed_fixture_certificate(run, cfg, 7, root_value)
            empty = signed_fixture_certificate(run, cfg, 5,
                        {"requests": [], "cst_round": 1, "cst_watermarks": {"1": 0, "2": 0}})
            head_value = {"requests": [], "cst_orders": [order], "cst_frontier": [empty, order]}
            head_certificate = signed_fixture_certificate(run, cfg, 1, head_value)
            witness = fixture_execution_witness(run, cfg, order)
            # Seed one honest source with certified history. The lagging
            # replica receives neither a head CLIENT nor a head CST_ORDER.
            source, backup = node_for(cfg, 1, 0), node_for(cfg, 1, 1)
            seed = c.signed({"type": "SYNC", "run": cfg["run_id"], "shard": 1, "from": 2, "view": 0,
                             "stable": {"seq": 0, "state": fixture_genesis(), "proof": []},
                             "certificates": [head_certificate],
                             "execution_witnesses": [{"seq": 1, "witness": witness}]},
                            node_for(cfg, 1, 2)["private_key"], run)
            c.send_frame(source["host"], source["port"], seed)
            wait_status(run, lambda rows: next(r for r in rows if (r["shard"], r["replica"]) == (1, 0))
                        ["executed_transactions"] == 2)
            idle = c.read(Path(backup["directory"]) / "status.json")
            self.assertEqual((idle["applied_batches"], idle["pending_requests"], idle["pending_cst_batches"],
                              idle["staged_cst_batches"]), (0, 0, 0, 0))
            future_request = signed_fixture_request(run, cfg, "future-slot", [1, 3])
            future_order = signed_fixture_certificate(run, cfg, 7,
                           {"requests": [future_request], "cst_order_index": 2, "cst_round": 2,
                            "cst_watermarks": {"1": 2, "2": 0, "3": 2, "4": 0}}, seq=2)
            future_empty = signed_fixture_certificate(run, cfg, 5,
                           {"requests": [], "cst_round": 2, "cst_watermarks": {"1": 0, "2": 0}}, seq=2)
            future_value = {"requests": [], "cst_orders": [future_order], "cst_frontier": [future_empty, future_order]}
            future_digest = digest(packed(future_value))
            future = c.signed({"type": "PREPREPARE", "run": cfg["run_id"], "shard": 1, "from": 0,
                               "view": 0, "seq": 2, "digest": future_digest, "value": future_value},
                              source["private_key"], run)
            c.send_frame(backup["host"], backup["port"], future)
            rows = wait_status(run, lambda rows: any(event["event"] == "future_preprepare_cached" and
                               event["digest"] == future_digest for event in events(run, 1, 1)), timeout=5)
            waiting = next(r for r in rows if (r["shard"], r["replica"]) == (1, 1))
            self.assertEqual((waiting["applied_batches"], waiting["executed_transactions"]), (0, 0))
            self.assertFalse(any(event["event"] in ("preprepare", "prepared", "committed_local") and
                                 event.get("digest") == future_digest for event in events(run, 1, 1)),
                             "the future proposal cannot earn a vote before its missing head executes")
            rows = wait_status(run, lambda rows: next(r for r in rows if (r["shard"], r["replica"]) == (1, 1))
                               ["executed_transactions"] == 2 and next(r for r in rows
                               if (r["shard"], r["replica"]) == (1, 1))["future_preprepare_cache_entries"] == 0,
                               timeout=8)
            recovered = next(r for r in rows if (r["shard"], r["replica"]) == (1, 1))
            honest = next(r for r in rows if (r["shard"], r["replica"]) == (1, 0))
            for field in ("state_digest", "chain_digest", "kv_digest", "applied_batches", "executed_transactions"):
                self.assertEqual(recovered[field], honest[field])
            self.assertEqual((recovered["applied_batches"], recovered["executed_transactions"], recovered["view_changes"]), (1, 2, 0))
            history = events(run, 1, 1)
            retained = next(event for event in history if event["event"] == "future_preprepare_cached" and
                            event["digest"] == future_digest)
            admitted = next(event for event in history if event["event"] == "preprepare" and
                            event["digest"] == future_digest)
            head_applied = next(event for event in history if event["event"] == "cst_ordered_at_leaf" and
                                event["coordinator_batch"] == "7:1")
            self.assertLess(retained["steady_ms"], head_applied["steady_ms"])
            self.assertLess(head_applied["steady_ms"], admitted["steady_ms"],
                            "the one transmitted future proposal must be admitted after recovering the head")
            self.assertTrue(any(event["event"] == "sync_requested" and event["after"] == 0 and event["target"] >= 1
                                for event in events(run, 1, 1)))
            self.assertTrue(any(event["event"] == "sync_proofs_sent" and event["destination_replica"] == 1 and
                                event["certificates"] >= 1 and event["execution_witnesses"] >= 1
                                for event in events(run, 1, 0)))
            journal = entries(run, 1, 1)
            self.assertEqual([entry["seq"] for entry in journal], [1])
            self.assertEqual(journal[0]["certificate"], head_certificate)
            self.assertEqual(journal[0]["execution_witness"], witness)

    def test_future_round_request_is_bounded_and_leaf_query_advances_missing_round(self):
        with running("future-round-query", shards=UNBALANCED,
                     consensus={"checkpoint_batches": 32, "batch_wait_ms": 0,
                                "cross_shard_batch_wait_ms": 0, "view_timeout_ms": 5000}) as run:
            cfg = c.read(run / "config.json")
            coordinator = node_for(cfg, 5, 0)
            request = c.signed({"type": "CST_ROUND_REQUEST", "run": cfg["run_id"], "shard": 7,
                                "from": 0, "view": 0, "target": 5, "round": 65},
                               node_for(cfg, 7, 0)["private_key"], run)
            c.send_frame(coordinator["host"], coordinator["port"], request)
            rows = wait_status(run, lambda rows: all(row["cst_round"] == 64
                               for row in rows if row["shard"] in (5, 7)), timeout=45)
            self.assertTrue(all(row["rounds_requested"] == 64 for row in rows if row["shard"] == 5))
            self.assertTrue(all(row["executed_transactions"] == 0 and row["ordered_cst_transactions"] == 0
                                for row in rows))
            # A leaf and the destination forward both have replica index 0.
            # The source shard still differs, so a proof reply must be sent.
            query = c.signed({"type": "CST_ROUND_QUERY", "run": cfg["run_id"], "shard": 1,
                              "from": 0, "view": 0, "target": 5, "rounds": [65], "leaf": 1, "after_index": 0},
                             node_for(cfg, 1, 0)["private_key"], run)
            c.send_frame(coordinator["host"], coordinator["port"], query)
            rows = wait_status(run, lambda rows: all(row["cst_round"] == 65 for row in rows if row["shard"] in (5, 7))
                               and all(row["round_certificate_count"] >= 65 for row in rows if row["shard"] == 1), timeout=15)
            previous = sum(event["event"] == "round_proofs_sent" and event["destination"] == 1
                           for event in events(run, 5, 0))
            c.send_frame(coordinator["host"], coordinator["port"], query)
            wait_status(run, lambda rows: sum(event["event"] == "round_proofs_sent" and event["destination"] == 1
                        and event["batches"] >= 1 for event in events(run, 5, 0)) > previous, timeout=5)
            self.assertTrue(all(row["executed_transactions"] == 0 and row["ordered_cst_transactions"] == 0
                                for row in rows))
            for shard in (5, 7):
                values = [entry["certificate"]["proposal"]["body"]["value"] for entry in entries(run, shard)]
                self.assertEqual([value["cst_round"] for value in values], list(range(1, 66)))
                self.assertTrue(all(value["requests"] == [] for value in values))
                self.assertTrue(all(value["cst_watermarks"] == {str(leaf): 0 for leaf in c.topology(cfg)[1]
                                                               if leaf == 1 or shard == 7} for value in values))

    def test_prepared_votes_coalesce_different_valid_order_certificate_signer_subsets(self):
        with running("prepared-certificate-subsets", shards=UNBALANCED) as run:
            cfg = c.read(run / "config.json")
            request = signed_fixture_request(run, cfg, "same-prepared-payload", [1, 2])
            order = signed_fixture_certificate(run, cfg, 7,
                        {"requests": [request], "cst_order_index": 1, "cst_round": 1,
                         "cst_watermarks": {"1": 1, "2": 1}})
            body = order["proposal"]["body"]
            common = {"run": cfg["run_id"], "shard": 7, "view": 0, "seq": 1, "digest": body["digest"]}
            prepare3 = c.signed(dict(common, type="PREPARE", **{"from": 3}),
                                node_for(cfg, 7, 3)["private_key"], run)
            commit3 = c.signed(dict(common, type="COMMIT", **{"from": 3}),
                               node_for(cfg, 7, 3)["private_key"], run)
            orders = [order, dict(order, prepares=[order["prepares"][1], prepare3],
                                 commits=[order["commits"][1], order["commits"][2], commit3]),
                      dict(order, prepares=[order["prepares"][0], prepare3],
                           commits=[order["commits"][0], order["commits"][2], commit3])]
            self.assertEqual(len({packed(cert) for cert in orders}), 3)
            witness = fixture_execution_witness(run, cfg, order)
            proof = next(proof for proof in witness["proofs"] if proof["record"]["shard"] == 1)
            forward = node_for(cfg, 1, 0)
            for vote, certificate in zip(proof["votes"], orders):
                envelope = copy.deepcopy(vote)
                envelope["record"] = copy.deepcopy(proof["record"])
                envelope["record"]["order_certificate"] = certificate
                c.send_frame(forward["host"], forward["port"], envelope)
            rows = wait_status(run, lambda rows: any(event["event"] == "cst_prepared_forwarded" and
                               event["batch_key"] == "7:1" and event["signers"] == 3
                               for event in events(run, 1, 0)), timeout=8)
            self.assertTrue(all(row["rejected_messages"] == 0 for row in rows))
            self.assertTrue(all(row["applied_batches"] == 0 and row["executed_transactions"] == 0
                                and row["kv_entries"] == 0 for row in rows))
            self.assertTrue(all(not entries(run, shard, replica)
                                for shard, _ in UNBALANCED for replica in range(4)))

    def test_certified_frontier_rejects_missing_ancestor_wrong_round_and_precedence(self):
        with running("frontier-forgery") as run:
            cfg = c.read(run / "config.json")
            root_request = signed_fixture_request(run, cfg, "frontier-root", [1, 3])
            root_value = {"requests": [root_request], "cst_order_index": 1, "cst_round": 1,
                          "cst_watermarks": {"1": 1, "2": 0, "3": 1, "4": 0}}
            root_cert = signed_fixture_certificate(run, cfg, 7, root_value)
            empty_value = {"requests": [], "cst_round": 1, "cst_watermarks": {"1": 0, "2": 0}}
            empty_cert = signed_fixture_certificate(run, cfg, 5, empty_value)
            next_round = signed_fixture_certificate(run, cfg, 5, dict(empty_value, cst_round=2))
            wrong_index = signed_fixture_certificate(run, cfg, 5, dict(empty_value, cst_watermarks={"1": 1, "2": 0}))
            lower_request = signed_fixture_request(run, cfg, "frontier-lower", [1, 2])
            lower_cert = signed_fixture_certificate(run, cfg, 5, {"requests": [lower_request], "cst_order_index": 1,
                        "cst_round": 1, "cst_watermarks": {"1": 1, "2": 1}})
            frontiers = ([root_cert], [root_cert, root_cert], [next_round, root_cert],
                         [wrong_index, root_cert], [lower_cert, root_cert])
            destination = node_for(cfg, 1, 1)
            def isolate_view(view):
                if view == 0:
                    return
                # Every fixture has a fresh certified view, rather than
                # equivocating with six digests for one view/sequence. No
                # earlier fixture has obtained a PREPARE or commit proof.
                changes = [c.signed({"type": "VIEW_CHANGE", "run": cfg["run_id"], "shard": 1,
                                     "from": replica, "view": view,
                                     "stable": {"seq": 0, "state": fixture_genesis(), "proof": []},
                                     "prepared": []}, node_for(cfg, 1, replica)["private_key"], run)
                           for replica in (0, 1, 2)]
                new_view = c.signed({"type": "NEW_VIEW", "run": cfg["run_id"], "shard": 1,
                                     "from": 0, "view": view, "changes": changes, "proposals": []},
                                    node_for(cfg, 1, 0)["private_key"], run)
                c.send_frame(destination["host"], destination["port"], new_view)
                wait_status(run, lambda rows: any(row["shard"] == 1 and row["replica"] == 1 and
                            row["view"] == view and not row["changing_view"] and row["applied_batches"] == 0
                            for row in rows), timeout=5)
            for index, frontier in enumerate(frontiers):
                view = index * 4
                isolate_view(view)
                value = {"requests": [], "cst_orders": [root_cert], "cst_frontier": frontier}
                value_digest = digest(packed(value))
                proposal = c.signed({"type": "PREPREPARE", "run": cfg["run_id"], "shard": 1,
                                     "from": 0, "view": view, "seq": 1, "digest": value_digest, "value": value},
                                    node_for(cfg, 1, 0)["private_key"], run)
                c.send_frame(destination["host"], destination["port"], proposal)
                wait_status(run, lambda rows: any(event["event"] == "invalid_preprepare_value" and
                            event["digest"] == value_digest for event in events(run, 1, 1)), timeout=5)
                self.assertTrue(all(row["applied_batches"] == 0 and row["executed_transactions"] == 0
                                    for row in c.statuses(run)), f"false frontier {index} changed state")
                self.assertFalse(any(event["event"] in ("preprepare", "prepared", "committed_local") and
                                     event.get("digest") == value_digest for event in events(run, 1, 1)),
                                 f"false frontier {index} obtained a PBFT vote")
            # Control: identical real QC/client signatures and the complete
            # same-round frontier must pass validation. One backup's PREPARE
            # alone cannot commit a batch or create any partial writes.
            value = {"requests": [], "cst_orders": [root_cert], "cst_frontier": [empty_cert, root_cert]}
            value_digest = digest(packed(value))
            view = len(frontiers) * 4
            isolate_view(view)
            proposal = c.signed({"type": "PREPREPARE", "run": cfg["run_id"], "shard": 1,
                                 "from": 0, "view": view, "seq": 1, "digest": value_digest, "value": value},
                                node_for(cfg, 1, 0)["private_key"], run)
            c.send_frame(destination["host"], destination["port"], proposal)
            wait_status(run, lambda rows: any(event["event"] == "preprepare" and event["digest"] == value_digest
                        for event in events(run, 1, 1)), timeout=5)
            self.assertTrue(all(row["applied_batches"] == 0 and row["executed_transactions"] == 0
                                for row in c.statuses(run)))


if __name__ == "__main__":
    print(f"Multi-layer test logs: {TEST_ROOT}", flush=True)
    unittest.main(verbosity=2)
