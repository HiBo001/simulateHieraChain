"""Fail-fast monitoring for a normal (fault-free) benchmark, never retries."""
import json
from pathlib import Path
import re
import time

import cluster as c


class GuardAbort(RuntimeError):
    pass


class RunGuard:
    # Transport failures after client completion may be late replies to its
    # closed listener. Parse/queue errors remain fatal throughout settlement.
    HARD_ERRORS = ("network_parse_errors", "network_oversized_errors",
                   "network_queue_errors", "inbox_dropped")
    TRANSPORT_ERRORS = ("network_socket_errors", "network_connect_errors",
                        "network_write_errors", "network_timeout_errors")

    def __init__(self, overall_deadline, case_seconds=60, no_progress_seconds=15):
        self.started = time.monotonic()
        self.deadline = min(overall_deadline, self.started + case_seconds)
        self.no_progress_seconds = no_progress_seconds
        self.last_progress = None
        self.completed = 0
        self.client_active = False
        self.transport_prior = {}
        self.nodes = []
        self.offsets = {}
        self.buffers = {}
        self.manifest_version = None
        self.evidence = {}

    def abort(self, reason, **details):
        self.evidence = {"reason": reason, "elapsed_wall_s": time.monotonic() - self.started,
                         "completed_requests": self.completed, **details}
        raise GuardAbort(reason)

    def begin_client(self, before_rows=()):
        self.transport_prior = {(row.get("shard"), row.get("replica")):
            {field: row.get(field, 0) for field in self.TRANSPORT_ERRORS} for row in before_rows}
        self.client_active = True
        self.last_progress = time.monotonic()

    def finish_client(self):
        self.client_active = False

    def progress(self, line):
        found = re.search(r"(?:^|\s)completed=(\d+)", line)
        if found and int(found[1]) > self.completed:
            self.completed = int(found[1])
            self.last_progress = time.monotonic()

    def new_lines(self, path):
        if not path.is_file():
            return []
        with path.open() as stream:
            stream.seek(self.offsets.get(path, 0))
            data = self.buffers.get(path, "") + stream.read()
            self.offsets[path] = stream.tell()
        parts = data.split("\n")
        self.buffers[path] = parts.pop()
        return parts

    def check(self, run=None, rows=None):
        now = time.monotonic()
        if now >= self.deadline:
            self.abort("overall_or_case_budget_exceeded")
        if run is not None:
            run = Path(run)
            manifest = run / "manifest.json"
            if manifest.is_file() and manifest.stat().st_mtime_ns != self.manifest_version:
                self.nodes = c.read(manifest)["nodes"]
                self.manifest_version = manifest.stat().st_mtime_ns
            # Popen handles belong to this exact run; polling does not invoke
            # ps for all 36 replicas every 250 ms.
            processes = c.RUN_PROCESSES.get(str(run.resolve()), [])
            if any(p.poll() is not None for p in processes):
                self.abort("node_exited")
            if rows is None:
                rows = []
                for n in self.nodes:
                    status = Path(n["directory"]) / "status.json"
                    if status.is_file():
                        rows.append(c.read(status))
            for n in self.nodes:
                directory = Path(n["directory"])
                for line in self.new_lines(directory / "node.log"):
                    if line.startswith("error:"):
                        self.abort("node_error", node=n["directory"], message=line)
                for line in self.new_lines(directory / "events.jsonl"):
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        self.abort("malformed_node_event", node=n["directory"])
                    if event.get("event") in ("view_change_started", "new_view_installed"):
                        self.abort("unexpected_view_change", node=n["directory"], event=event)
            for row in rows or []:
                identity = {"shard": row.get("shard"), "replica": row.get("replica")}
                if row.get("alive") is False:
                    self.abort("node_not_alive", **identity)
                if row.get("changing_view") or row.get("view_changes", 0) or row.get("view", 0):
                    self.abort("unexpected_view_change", **identity, view=row.get("view"))
                fields = self.HARD_ERRORS + (self.TRANSPORT_ERRORS if self.client_active else ())
                for field in fields:
                    prior = self.transport_prior.get((identity["shard"], identity["replica"]), {}).get(field, 0) if field in self.TRANSPORT_ERRORS else 0
                    if row.get(field, 0) < prior:
                        self.abort("node_counter_regressed", **identity, counter=field,
                                   value=row.get(field, 0), before_value=prior)
                    if row.get(field, 0) > prior:
                        self.abort("node_counter_error", **identity, counter=field, value=row[field],
                                   before_value=prior, new_errors=row[field]-prior)
        if self.client_active and now - self.last_progress >= self.no_progress_seconds:
            self.abort("no_completion_progress", idle_s=now - self.last_progress)
