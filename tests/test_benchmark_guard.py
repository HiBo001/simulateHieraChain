"""Fault-free benchmark guards must reject unhealthy or stalled samples."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import benchmark_guard as g
import benchmark_mixed as mixed


class Guards(unittest.TestCase):
    def guard(self):
        return g.RunGuard(overall_deadline=200, case_seconds=60, no_progress_seconds=15)

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_budget_includes_startup(self, clock):
        guard = self.guard()
        clock.return_value = 160
        with self.assertRaisesRegex(g.GuardAbort, "budget"):
            guard.check()

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_only_completed_progress_resets_stall_timer(self, clock):
        guard = self.guard(); guard.begin_client()
        clock.return_value = 110
        guard.progress("progress submitted=500 completed=0 requests=100")
        clock.return_value = 115
        with self.assertRaisesRegex(g.GuardAbort, "no_completion_progress"):
            guard.check()

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_finished_client_does_not_timeout_drain(self, clock):
        guard = self.guard(); guard.begin_client()
        clock.return_value = 110
        guard.progress("progress submitted=500 completed=30 requests=100")
        clock.return_value = 124; guard.check()
        guard.finish_client()
        clock.return_value = 130; guard.check()

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_transient_view_change_is_detected_from_events(self, clock):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); node = root / "node"; node.mkdir()
            g.c.write(root / "manifest.json", {"nodes": [{"directory": str(node)}]})
            (node / "events.jsonl").write_text('{"event":"view_change_started","target_view":1}\n')
            with self.assertRaisesRegex(g.GuardAbort, "unexpected_view_change"):
                self.guard().check(root, [])

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_error_counter_and_exited_node_abort(self, clock):
        with tempfile.TemporaryDirectory() as folder:
            for field in g.RunGuard.HARD_ERRORS:
                with self.subTest(field=field), self.assertRaises(g.GuardAbort):
                    self.guard().check(folder, [{field: 1}])
            dead = mock.Mock(); dead.poll.return_value = 1
            with mock.patch.dict(g.c.RUN_PROCESSES, {str(Path(folder).resolve()): [dead]}), \
                    self.assertRaisesRegex(g.GuardAbort, "node_exited"):
                self.guard().check(folder)

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_startup_transport_errors_are_separate_and_new_errors_abort(self, clock):
        with tempfile.TemporaryDirectory() as folder:
            row = {"shard": 5, "replica": 0, "network_connect_errors": 3}
            guard = self.guard();guard.begin_client([row])
            guard.check(folder, [row])
            with self.assertRaisesRegex(g.GuardAbort, "node_counter_error"):
                guard.check(folder, [dict(row, network_connect_errors=4)])
            self.assertEqual(guard.evidence["before_value"], 3)
            self.assertEqual(guard.evidence["new_errors"], 1)

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_startup_hard_errors_are_still_fatal(self, clock):
        with tempfile.TemporaryDirectory() as folder:
            row = {"shard": 5, "replica": 0, "network_parse_errors": 1}
            guard = self.guard();guard.begin_client([row])
            with self.assertRaises(g.GuardAbort):
                guard.check(folder, [row])

    @mock.patch.object(g.time, "monotonic", return_value=100)
    def test_transport_counter_reset_is_not_treated_as_recovery(self, clock):
        with tempfile.TemporaryDirectory() as folder:
            row = {"shard": 5, "replica": 0, "network_connect_errors": 3}
            guard = self.guard();guard.begin_client([row])
            with self.assertRaisesRegex(g.GuardAbort, "node_counter_regressed"):
                guard.check(folder, [dict(row, network_connect_errors=0)])

    def test_profile_deltas_preserve_replica_identity(self):
        before = [{"shard": 1, "replica": 0, "leaf_profile": {"wait_root_ns": 10}}]
        after = [{"shard": 1, "replica": 0, "leaf_profile": {"wait_root_ns": 40,
                  "scheduler_elapsed_ns": 7, "mode": "stream"}}]
        self.assertEqual(mixed.profile_delta(before, after), [{"shard": 1, "replica": 0,
            "wait_root_ns": 30, "scheduler_elapsed_ns": 7}])


if __name__ == "__main__":
    unittest.main()
