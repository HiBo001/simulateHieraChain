#!/usr/bin/env python3
"""Authenticate real checkpoint snapshots, including dedup and unknown metadata."""
import copy
from pathlib import Path
import socket
import time
import unittest

import test_engineering as e


def capture_snapshot(run, cfg):
    """Capture an honest SYNC response on a stopped replica's actual endpoint."""
    e.stop_node(run, 1, 3)
    receiver = e.node_for(cfg, 1, 3)
    source = e.node_for(cfg, 1, 0)
    query = e.replica_message(run, cfg, 1, 3, "SYNC_REQUEST", {"after": 0, "stable_seq": 0})
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((receiver["host"], receiver["port"]))
        listener.listen(8)
        listener.settimeout(.2)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            # The old persistent connection may still be closing. Repeating
            # the query makes the sender reconnect to this temporary listener.
            e.c.send_frame(source["host"], source["port"], query)
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(.3)
                while time.monotonic() < deadline:
                    try:
                        envelope = e.receive_frame(connection)
                    except (ConnectionError, socket.timeout):
                        break
                    # Forward heartbeats can share the persistent TCP stream.
                    if envelope["body"]["type"] == "SYNC":
                        return envelope
    raise AssertionError("source did not return its honest checkpoint snapshot")


def leaf_rows(rows, alive_only=False):
    return [row for row in rows if row["shard"] == 1 and (row["alive"] or not alive_only)]


class SnapshotDigest(unittest.TestCase):
    def test_real_checkpoint_proof_rejects_tampered_state_and_accepts_original(self):
        with e.running("snapshot-digest", consensus={"checkpoint_batches": 4}) as run:
            cfg = e.c.read(run / "config.json")
            rc, result = e.c.load(run, count=64, rate=4000, seed=47,
                                  prefix="snapshot-before", shard=1, batch=8, timeout=15)
            self.assertEqual((rc, result["executed_transactions"]), (0, 64))
            e.wait_status(run, lambda rows: len(leaf_rows(rows, True)) == 4 and
                          all(row["applied_batches"] == 8 and row["stable_seq"] == 8 and
                              row["executed_transactions"] == 64 for row in leaf_rows(rows)) and
                          len({row["state_digest"] for row in leaf_rows(rows)}) == 1)

            original = capture_snapshot(run, cfg)
            stable = original["body"]["stable"]
            self.assertEqual((stable["seq"], stable["state"]["seq"]), (8, 8))
            self.assertGreaterEqual(len(stable["proof"]), 3)
            self.assertGreaterEqual(len({vote["body"]["from"] for vote in stable["proof"]}), 3)
            destination = e.node_for(cfg, 1, 1)
            fields = ("state_digest", "applied_batches", "executed_transactions", "kv_digest")
            before = e.c.read(Path(destination["directory"]) / "status.json")

            for name in ("seen", "requests", "kv", "unknown_metadata"):
                with self.subTest(field=name):
                    body = copy.deepcopy(original["body"])
                    state = body["stable"]["state"]
                    if name == "seen":
                        state[name][next(iter(state[name]))]["tx_digest"] = "0" * 64
                    elif name == "requests":
                        state[name][next(iter(state[name]))]["results"][0]["digest"] = "0" * 64
                    elif name == "kv":
                        state[name][next(iter(state[name]))]["value"] += 1
                    else:
                        state[name] = {"not_in_certified_snapshot": True}
                    self.assertEqual(body["stable"]["proof"], stable["proof"])
                    # The outer replica signature is valid. Rejection must
                    # come from binding the original honest quorum to state.
                    forged = e.c.signed(body, e.node_for(cfg, 1, 0)["private_key"], run)
                    rows = e.rejected_after_send(run, cfg, (1, 1), forged)
                    after = next(row for row in rows if (row["shard"], row["replica"]) == (1, 1))
                    self.assertEqual(tuple(after[field] for field in fields),
                                     tuple(before[field] for field in fields))

            rejected_before = e.c.read(Path(destination["directory"]) / "status.json")["rejected_messages"]
            e.c.send_frame(destination["host"], destination["port"], original)
            time.sleep(.3)  # Allow the handler and a fresh periodic status write.
            self.assertEqual(e.c.read(Path(destination["directory"]) / "status.json")["rejected_messages"],
                             rejected_before, "untouched honest snapshot was rejected")

            rc, result = e.c.load(run, count=8, rate=4000, seed=48,
                                  prefix="snapshot-after", shard=1, batch=8, timeout=15)
            self.assertEqual((rc, result["executed_transactions"]), (0, 8))
            rows = e.wait_status(run, lambda rows: len(leaf_rows(rows, True)) == 3 and
                                 all(row["executed_transactions"] == 72 and row["applied_batches"] == 9
                                     and row["pending_requests"] == 0 and row["dedup_waiting_requests"] == 0
                                     for row in leaf_rows(rows, True)) and
                                 len({row["state_digest"] for row in leaf_rows(rows, True)}) == 1 and
                                 len({row["kv_digest"] for row in leaf_rows(rows, True)}) == 1)
            self.assertNotEqual(leaf_rows(rows, True)[0]["state_digest"], before["state_digest"])


if __name__ == "__main__":
    print("Snapshot digest test logs:", e.TEST_ROOT, flush=True)
    unittest.main(verbosity=2)
