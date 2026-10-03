#!/usr/bin/env python3
"""Black-box regressions for f+1 initial delivery and authenticated retries."""
import hashlib
import socket
import threading
import unittest

import test_engineering as engineering


class ReplicaCapture:
    """Capture every frame, including retries on the same TCP connection."""

    def __init__(self, cfg, shard):
        self.cfg = cfg
        self.shard = shard
        self.frames = {replica: [] for replica in range(4)}
        self.errors = []
        self.stopped = threading.Event()
        self.listeners = []
        self.threads = []

    def __enter__(self):
        try:
            for replica in range(4):
                node = engineering.node_for(self.cfg, self.shard, replica)
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
                    self.errors.append((replica, str(error)))
                return
            with connection:
                connection.settimeout(2)
                while not self.stopped.is_set():
                    try:
                        frame = engineering.receive_frame(connection)
                    except ConnectionError:
                        break
                    except (OSError, ValueError, AssertionError) as error:
                        self.errors.append((replica, str(error)))
                        break
                    self.frames[replica].append(frame)

    def __exit__(self, *_):
        self.stopped.set()
        for listener in self.listeners:
            listener.close()
        for thread in self.threads:
            thread.join(timeout=3)
            if thread.is_alive():
                self.errors.append(("thread", "capture did not stop"))


class ClientFanout(unittest.TestCase):
    def capture_request(self, name, timeout):
        with engineering.running(name) as run:
            cfg = engineering.c.read(run / "config.json")
            for replica in range(4):
                engineering.stop_node(run, 1, replica)
            workload = engineering.c.prepare_workload(
                cfg, 1, 1000, 51, name, shard=1, batch=1, timeout=timeout)
            with ReplicaCapture(cfg, 1) as capture:
                rc, result = engineering.run_client(run, name, workload)
            self.assertEqual((rc, result["completed_requests"]), (2, 0))
            self.assertFalse(capture.errors)
            frames = [frame for replica_frames in capture.frames.values()
                      for frame in replica_frames]
            self.assertTrue(frames)
            self.assertTrue(all(frame == frames[0] for frame in frames))
            self.assertEqual(frames[0]["body"]["type"], "CLIENT")
            self.assertEqual(frames[0]["body"]["target"], 1)
            self.assertEqual(frames[0]["body"]["txs"], workload["requests"][0]["txs"])
            return [len(capture.frames[replica]) for replica in range(4)]

    def test_initial_request_has_two_distinct_recipients(self):
        # End before the 500 ms application retry, without any fake REPLY.
        self.assertEqual(self.capture_request("client-initial-pair", .25), [1, 1, 0, 0])

    def test_first_retry_expands_to_all_replicas_without_changing_signature(self):
        # First retry is after 500 ms; the next is another 1,000 ms later.
        self.assertEqual(self.capture_request("client-first-retry", .8), [2, 2, 1, 1])

    def test_backup_relays_to_primary_outside_initial_pair(self):
        with engineering.running("client-primary-outside-pair") as run:
            cfg = engineering.c.read(run / "config.json")
            # A fresh shard has no prepared slots, so three signed genesis
            # VIEW_CHANGE proofs authorize an empty NEW_VIEW at view 2.
            genesis = {
                "seq": 0, "chain": hashlib.sha256(b"arbor-genesis").hexdigest(),
                "kv": {}, "seen": {}, "requests": {}, "executed": 0,
                "ordered_cst": 0, "cst_batches": {}, "cst_seen": {},
                "leaf_ordered_cst": 0, "last_cst_seq": 0,
                "cst_finalized": {}, "cst_orders": {}, "cst_order_index": 0,
            }
            stable = {"seq": 0, "state": genesis, "proof": []}
            changes = [engineering.replica_message(
                run, cfg, 1, replica, "VIEW_CHANGE",
                {"view": 2, "stable": stable, "prepared": []})
                for replica in (1, 2, 3)]
            new_view = engineering.replica_message(
                run, cfg, 1, 2, "NEW_VIEW",
                {"view": 2, "changes": changes, "proposals": []})
            for replica in range(4):
                node = engineering.node_for(cfg, 1, replica)
                engineering.c.send_frame(node["host"], node["port"], new_view)

            def view_ready(rows):
                return all(row["view"] == 2 and row["primary"] == 2
                           for row in rows if row["shard"] == 1)

            engineering.wait_status(run, view_ready)
            # Finish before the first client retry can address primary 2
            # directly. Success therefore requires the initial backup relay.
            workload = engineering.c.prepare_workload(
                cfg, 1, 1000, 52, "outside-pair", shard=1, batch=1, timeout=.45)
            rc, result = engineering.run_client(run, "outside-pair", workload)
            self.assertEqual((rc, result["completed_requests"],
                              result["executed_transactions"]), (0, 1, 1))
            rows = engineering.wait_status(run, lambda rows: all(
                row["executed_transactions"] == 1 and row["applied_batches"] == 1
                and row["pending_requests"] == 0 and row["dedup_waiting_requests"] == 0
                and row["primary"] == 2 and row["view"] == 2
                for row in rows if row["shard"] == 1))
            self.assertEqual(len({row["state_digest"] for row in rows
                                  if row["shard"] == 1}), 1)


if __name__ == "__main__":
    print("Client fanout regression logs:", engineering.TEST_ROOT, flush=True)
    unittest.main(verbosity=2)
