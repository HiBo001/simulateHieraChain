#!/usr/bin/env python3
"""Exercise cleanup only in temporary checkouts, with all processes mocked."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "scripts"))
spec = importlib.util.spec_from_file_location("clean_under_test", PROJECT / "scripts/clean.py")
cleaner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleaner)
PROCESS_COMMANDS = cleaner.process_commands


class CleanTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="arbor-clean-tests-")
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name).resolve()
        self.root = self.temp / "checkout"
        self.root.mkdir()
        self.binary = self.root / "build/bin/arbor_node"
        self.patch(cleaner.c, "ROOT", new=self.root)
        self.patch(cleaner.c, "BIN", new=self.binary)
        self.processes = self.patch(cleaner, "process_commands", return_value={})
        self.cwd = self.patch(cleaner, "process_cwd", return_value=None)
        # No test may fall through to stop_run, ps, or a real signal operation.
        self.stop = self.patch(cleaner.c, "stop_run", side_effect=AssertionError("unexpected stop_run"))
        self.kill = self.patch(os, "kill", side_effect=AssertionError("real process signals are forbidden"))
        self.saved = {}
        for name in ("source/main.cpp", "config/two_layer.json", "config/accessControlList",
                     "docs/CURRENT_IMPLEMENTATION_DESIGN.md", "docs/archived-note.log",
                     "shard1/shardId", "shard2/config.txt", "README.md", "Makefile",
                     ".git/HEAD"):
            self.saved[name] = f"preserve {name}\n"
            self.file(name, self.saved[name])

    def patch(self, target, name, **arguments):
        patcher = mock.patch.object(target, name, **arguments)
        result = patcher.start()
        self.addCleanup(patcher.stop)
        return result

    def file(self, name, content="generated\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def run_clean(self):
        self.assertNotEqual(cleaner.c.ROOT.resolve(), PROJECT.resolve())
        self.assertTrue(cleaner.c.ROOT.resolve().is_relative_to(self.temp.resolve()))
        with contextlib.redirect_stdout(io.StringIO()):
            cleaner.clean()
        self.kill.assert_not_called()

    def assert_saved(self):
        for name, content in self.saved.items():
            with self.subTest(preserved=name):
                self.assertEqual((self.root / name).read_text(), content)

    def make_outputs(self):
        for name in ("build/bin/arbor_node", "runtime/run-old/node-1/node.log",
                     "runtime/run-old/node-1/commits.jsonl", "runtime/run-old/node-1/status.json",
                     "runtime/run-old/keys/replica.pem", "runtime/run-old/resolved.json",
                     "test-results/benchmark-old/summary.json", "test-results/test-old/client.json",
                     "__pycache__/module.pyc", "scripts/__pycache__/cluster.pyc",
                     "scripts/helpers/cache.pyo", "tests/nested/__pycache__/test.pyc",
                     "tests/cache.pyc", "module.pyc", "module.pyo", "shard1/node.log",
                     "shard2/node.log"):
            self.file(name)
        (self.root / "runtime/latest").symlink_to("run-old", target_is_directory=True)

    def managed_run(self, name, pids):
        run = self.root / name
        nodes, processes = [], {}
        for index, pid in enumerate(pids):
            directory = run / f"node-{index}"
            self.file(str(directory.relative_to(self.root) / "commits.jsonl"))
            nodes.append({"pid": pid, "directory": str(directory), "shard": 1, "replica": index})
            processes[pid] = f"{self.binary} node --directory {directory} --config {run / 'resolved.json'}"
        self.file(str((run / "manifest.json").relative_to(self.root)), json.dumps({"nodes": nodes}))
        return run, processes

    def test_generated_outputs_deleted_but_project_inputs_preserved(self):
        self.make_outputs()
        self.run_clean()
        self.assert_saved()
        for name in ("build", "test-results", "__pycache__", "runtime/latest",
                     "runtime/run-old", "scripts/__pycache__", "scripts/helpers/cache.pyo",
                     "tests/nested/__pycache__", "tests/cache.pyc", "module.pyc", "module.pyo",
                     "shard1/node.log", "shard2/node.log"):
            with self.subTest(deleted=name):
                self.assertFalse((self.root / name).exists())
                self.assertFalse((self.root / name).is_symlink())
        self.assertEqual([p.name for p in (self.root / "runtime").iterdir()], [".lifecycle.lock"])
        self.stop.assert_not_called()

    def test_repeated_cleanup_is_idempotent_and_retains_lock_inode(self):
        self.make_outputs()
        self.run_clean()
        lock = self.root / "runtime/.lifecycle.lock"
        inode = lock.stat().st_ino
        self.run_clean()
        self.assertEqual(lock.stat().st_ino, inode)
        self.assert_saved()

    def test_output_symlinks_unlinked_without_deleting_external_targets(self):
        outside = self.temp / "outside"
        outside.mkdir()
        marker = outside / "precious.txt"
        marker.write_text("external data")
        for name in ("runtime", "build", "test-results", "__pycache__"):
            (self.root / name).symlink_to(outside, target_is_directory=True)
        (self.root / "scripts").mkdir()
        (self.root / "scripts/__pycache__").symlink_to(outside, target_is_directory=True)
        (self.root / "scripts/linked-helper").symlink_to(outside, target_is_directory=True)
        (self.root / "shard3").symlink_to(outside, target_is_directory=True)
        self.run_clean()
        self.assertEqual(marker.read_text(), "external data")
        for name in ("runtime", "build", "test-results", "__pycache__", "scripts/__pycache__"):
            self.assertFalse((self.root / name).is_symlink())
        self.assertTrue((self.root / "scripts/linked-helper").is_symlink())
        self.assertTrue((self.root / "shard3").is_symlink())
        self.assert_saved()

    def test_manifest_search_does_not_follow_directory_or_file_links(self):
        self.file("runtime/valid/manifest.json", '{"nodes": []}')
        outside = self.temp / "external-run"
        outside.mkdir()
        manifest = outside / "manifest.json"
        manifest.write_text('{"nodes": []}')
        (self.root / "runtime/external-run").symlink_to(outside, target_is_directory=True)
        (self.root / "runtime/file-linked").mkdir()
        (self.root / "runtime/file-linked/manifest.json").symlink_to(manifest)
        found = list(cleaner.manifests(self.root / "runtime"))
        self.assertEqual(found, [self.root / "runtime/valid/manifest.json"])
        self.assertEqual(list(cleaner.manifests(self.root / "runtime/external-run")), [])

    def test_symlinked_lifecycle_lock_rejected_without_touching_external_file(self):
        self.make_outputs()
        external_lock = self.temp / "external-lock"
        external_lock.write_text("external lock contents")
        (self.root / "runtime/.lifecycle.lock").symlink_to(external_lock)
        with self.assertRaisesRegex(ValueError, "lifecycle.lock"):
            self.run_clean()
        self.assertEqual(external_lock.read_text(), "external lock contents")
        self.assertTrue(self.binary.exists())
        self.assertTrue((self.root / "runtime/run-old/node-1/commits.jsonl").exists())
        self.stop.assert_not_called()

    def test_multiple_active_runs_are_all_stopped_before_any_output_is_deleted(self):
        run_a, state_a = self.managed_run("runtime/run-a", [910001, 910002])
        run_b, state_b = self.managed_run("test-results/suite/run-b", [910003, 910004])
        self.file("build/bin/arbor_node")
        state = {**state_a, **state_b}
        self.processes.side_effect = lambda: dict(state)
        stopped = []

        def stop(run):
            for directory in (run_a, run_b):
                self.assertTrue((directory / "manifest.json").exists())
            self.assertTrue(self.binary.exists())
            stopped.append(run)
            for pid in list(state):
                if str(run) + os.sep in state[pid]:
                    del state[pid]

        self.stop.side_effect = stop
        self.run_clean()
        self.assertCountEqual(stopped, [run_a, run_b])
        self.assertFalse(run_a.exists())
        self.assertFalse(run_b.exists())
        self.assert_saved()

    def test_reused_pid_with_unrelated_command_is_never_stopped(self):
        run, _ = self.managed_run("runtime/old-run", [910005])
        self.processes.return_value = {910005: "/usr/bin/some-unrelated-program --data another-place"}
        self.run_clean()
        self.stop.assert_not_called()
        self.assertFalse(run.exists())

    def test_matching_binary_with_old_pid_in_different_run_blocks_and_does_not_kill(self):
        old_run, _ = self.managed_run("runtime/old-run", [910006])
        orphan = self.root / "runtime/new-run/node-0"
        self.file("runtime/new-run/node-0/commits.jsonl")
        self.processes.return_value = {910006: f"{self.binary} node --directory {orphan}"}
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.run_clean()
        self.stop.assert_not_called()
        self.kill.assert_not_called()
        self.assertTrue((old_run / "manifest.json").exists())
        self.assertTrue((orphan / "commits.jsonl").exists())

    def test_unregistered_active_node_preserves_all_output_and_fails(self):
        self.make_outputs()
        self.processes.return_value = {910007: f"{self.binary} node --directory {self.root / 'runtime/run-old/node-1'}"}
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.run_clean()
        for name in ("build/bin/arbor_node", "runtime/run-old/node-1/commits.jsonl",
                     "test-results/benchmark-old/summary.json"):
            self.assertTrue((self.root / name).exists())
        self.stop.assert_not_called()
        self.kill.assert_not_called()

    def test_invalid_live_manifest_cannot_stop_external_directory(self):
        run, state = self.managed_run("runtime/run-unsafe", [910008])
        data = json.loads((run / "manifest.json").read_text())
        data["nodes"].append({"pid": 910009, "directory": str(self.temp / "outside")})
        (run / "manifest.json").write_text(json.dumps(data))
        self.processes.return_value = state
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.run_clean()
        self.stop.assert_not_called()
        self.assertTrue((run / "manifest.json").exists())

    def test_stopping_failure_leaves_records_and_build_intact(self):
        run, state = self.managed_run("runtime/run-failed-stop", [910010])
        self.file("build/bin/arbor_node")
        self.processes.return_value = state
        self.stop.side_effect = OSError("simulated stop failure")
        with self.assertRaisesRegex(OSError, "simulated stop failure"):
            self.run_clean()
        self.assertTrue((run / "manifest.json").exists())
        self.assertTrue(self.binary.exists())

    def test_node_that_does_not_exit_prevents_deletion(self):
        run, state = self.managed_run("runtime/run-stuck", [910011])
        self.file("build/bin/arbor_node")
        self.processes.return_value = state
        self.stop.side_effect = None
        with mock.patch.object(cleaner.time, "monotonic", side_effect=[100.0, 104.0]):
            with self.assertRaisesRegex(ValueError, "未退出"):
                self.run_clean()
        self.assertTrue((run / "manifest.json").exists())
        self.assertTrue(self.binary.exists())

    def test_active_client_blocks_before_stopping_nodes_or_deleting_outputs(self):
        self.make_outputs()
        self.processes.return_value = {910012: f"{self.binary} client --config {self.root / 'runtime/run-old/resolved.json'}"}
        with self.assertRaisesRegex(ValueError, "客户端"):
            self.run_clean()
        self.stop.assert_not_called()
        self.assertTrue(self.binary.exists())
        self.assertTrue((self.root / "runtime/run-old/node-1/commits.jsonl").exists())

    def test_absolute_benchmark_and_test_scripts_block_cleanup(self):
        self.make_outputs()
        for script in ("scripts/benchmark.py", "tests/test_stage2b.py"):
            with self.subTest(script=script):
                self.processes.return_value = {910013: f"python3 {self.root / script} --count 4000"}
                with self.assertRaisesRegex(ValueError, "实验脚本"):
                    self.run_clean()
                self.assertTrue(self.binary.exists())
        self.stop.assert_not_called()

    def test_relative_cluster_start_load_and_probe_block_from_same_checkout(self):
        self.make_outputs()
        self.cwd.return_value = self.root
        for command in ("start --config config/two_layer.json", "load --count 4000", "probe 1 2"):
            with self.subTest(command=command):
                self.processes.return_value = {910014: f"python3 scripts/cluster.py {command}"}
                with self.assertRaisesRegex(ValueError, "实验脚本"):
                    self.run_clean()
                self.assertTrue(self.binary.exists())
        self.stop.assert_not_called()

    def test_relative_experiment_from_another_checkout_does_not_block(self):
        self.make_outputs()
        other = self.temp / "another-checkout"
        other.mkdir()
        self.cwd.return_value = other
        self.processes.return_value = {910015: "python3 scripts/benchmark.py --count 1000"}
        self.run_clean()
        self.stop.assert_not_called()
        self.assert_saved()

    def test_mixed_benchmark_writer_blocks_cleanup(self):
        self.make_outputs()
        self.cwd.return_value = self.root
        for command in ("python3 scripts/benchmark_mixed.py --method saguaro",
                        f"python3 {self.root / 'scripts/benchmark_mixed.py'} --method arbor"):
            with self.subTest(command=command):
                self.processes.return_value = {910018: command}
                with self.assertRaisesRegex(ValueError, "实验脚本"):
                    self.run_clean()
                self.assertTrue(self.binary.exists())
        self.stop.assert_not_called()

    def test_read_only_cluster_commands_do_not_block_cleanup(self):
        self.make_outputs()
        self.cwd.return_value = self.root
        self.processes.return_value = {910016: "python3 scripts/cluster.py status",
                                       910017: f"python3 {self.root / 'scripts/cluster.py'} topology"}
        self.run_clean()
        self.stop.assert_not_called()
        self.assert_saved()

    def test_process_inventory_parses_commands_without_running_real_ps(self):
        fake = mock.Mock(stdout="  123 python3 scripts/cluster.py load\n 456 /a/path/node node --arg value\n 789\n junk\n")
        with mock.patch.object(cleaner.subprocess, "run", return_value=fake) as command:
            result = PROCESS_COMMANDS()
        self.assertEqual(result, {123: "python3 scripts/cluster.py load", 456: "/a/path/node node --arg value"})
        command.assert_called_once_with(["ps", "-ax", "-o", "pid=,command="],
                                        capture_output=True, text=True, check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
