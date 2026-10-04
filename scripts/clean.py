#!/usr/bin/env python3
"""Stop repository runs, then remove generated output without following links."""
import contextlib
import fcntl
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import cluster as c


def process_commands():
    result = subprocess.run(["ps", "-ax", "-o", "pid=,command="],
                            capture_output=True, text=True, check=True)
    processes = {}
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) == 2 and fields[0].isdigit():
            processes[int(fields[0])] = fields[1]
    return processes


def process_cwd(pid):
    proc = Path(f"/proc/{pid}/cwd")
    if proc.exists():
        return proc.resolve()
    if shutil.which("lsof"):
        result = subprocess.run(["lsof", "-a", "-p", str(pid), "-d", "cwd", "-Fn"],
                                capture_output=True, text=True, timeout=5)
        for line in result.stdout.splitlines():
            if line.startswith("n"):
                return Path(line[1:]).resolve()
    return None


def within(path, parent):
    try:
        Path(path).resolve().relative_to(parent.resolve())
        return True
    except (ValueError, OSError):
        return False


def assert_no_writers(root, processes):
    for pid, command in processes.items():
        if pid == os.getpid():
            continue
        binaries = [str(root / "build/bin" / (method + "_node")) for method in c.METHODS]
        if any(command.startswith(binary + " client ") for binary in binaries) and any(
                str(root / name) + os.sep in command for name in ("runtime", "test-results")):
            raise ValueError(f"客户端仍在运行（PID {pid}），请先结束负载，再执行 make clean")
        executable = Path(command.split(None, 1)[0]).name.lower()
        if not executable.startswith("python"):
            continue
        # Absolute project paths may contain spaces. Match those before the
        # token-based fallback for commands started from a relative path.
        project_script = next((str(root / relative) for relative in (
            "scripts/benchmark.py", "scripts/cluster.py", "baseline/compare.py")
            if str(root / relative) in command), None)
        if project_script:
            suffix = command.split(project_script, 1)[1]
            if project_script.endswith("cluster.py") and not re.search(r"\b(start|restart|load|probe)\b", suffix):
                continue
            raise ValueError(f"实验脚本仍在运行（PID {pid}），请先结束负载/benchmark/测试，再执行 make clean")
        match = re.search(r"(?:^|\s)(\S*(?:scripts/(?:benchmark|cluster)\.py|baseline/compare\.py|tests/test_[^/\s]+\.py))(?:\s|$)", command)
        if not match:
            continue
        script = match.group(1)
        if script.endswith("cluster.py") and not re.search(r"\b(start|restart|load|probe)\b", command[match.end():]):
            continue
        # Absolute commands and relative commands from this checkout are both common.
        belongs_here = str(root) + os.sep in script
        if not belongs_here and not Path(script).is_absolute():
            cwd = process_cwd(pid)
            belongs_here = cwd is not None and within(cwd / script, root)
        if belongs_here:
            raise ValueError(f"实验脚本仍在运行（PID {pid}），请先结束负载/benchmark/测试，再执行 make clean")


def manifests(directory):
    if directory.is_symlink() or not directory.is_dir():
        return
    for current, dirs, files in os.walk(directory, followlinks=False):
        dirs[:] = [name for name in dirs if not (Path(current) / name).is_symlink()]
        if "manifest.json" in files:
            path = Path(current) / "manifest.json"
            if not path.is_symlink():
                yield path


def stop_managed_runs(root):
    processes = process_commands()
    assert_no_writers(root, processes)
    binaries = [str(root / "build/bin" / (method + "_node")) for method in c.METHODS]
    directories = [root / "runtime", root / "test-results"]
    active = {pid for pid, command in processes.items()
              if any(command.startswith(binary + " node ") for binary in binaries) and any(
                  str(directory) + os.sep in command for directory in directories)}
    if not active:
        return
    runs, registered = [], set()
    for directory in directories:
        for manifest in manifests(directory):
            try:
                nodes = c.read(manifest)["nodes"]
                matching = [n for n in nodes if n.get("pid") in active and
                            str(n.get("directory", "")) in processes[n["pid"]]]
                if not matching:
                    continue
                # stop_run reads all entries: none may point outside this run.
                if any(not n.get("directory") or not within(n["directory"], manifest.parent)
                       for n in nodes):
                    raise ValueError(f"运行记录含越界节点目录: {manifest}")
                runs.append(manifest.parent)
                registered.update(n["pid"] for n in matching)
            except (ValueError, KeyError, TypeError, AttributeError):
                # Corrupt inactive records may still be deleted. A live node
                # without another valid matching record blocks cleanup below.
                continue
    if active - registered:
        raise ValueError("发现没有匹配 manifest 的活动节点，请先停止这些节点，再执行 make clean")
    for run in runs:
        c.stop_run(run)
    deadline = time.monotonic() + 3
    while active:
        remaining = process_commands()
        active = {pid for pid in active if any(remaining.get(pid, "").startswith(binary + " node ") for binary in binaries)}
        if not active:
            break
        if time.monotonic() >= deadline:
            raise ValueError("仍有节点未退出，保留记录，未执行清理")
        time.sleep(.1)


def remove(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def remove_caches(directory):
    if directory.is_symlink() or not directory.is_dir():
        return
    for current, dirs, files in os.walk(directory, topdown=True, followlinks=False):
        for name in list(dirs):
            path = Path(current) / name
            if name == "__pycache__":
                remove(path)
                dirs.remove(name)
            elif path.is_symlink():
                dirs.remove(name)
        for name in files:
            if name.endswith((".pyc", ".pyo")):
                remove(Path(current) / name)


def clean():
    root = c.ROOT.resolve()
    runtime = root / "runtime"
    # Keep the lock inode so a concurrent cluster.start cannot bypass this lock.
    with contextlib.ExitStack() as stack:
        if not runtime.is_symlink():
            runtime.mkdir(exist_ok=True)
            lock_path = runtime / ".lifecycle.lock"
            if lock_path.is_symlink():
                raise ValueError("runtime/.lifecycle.lock 不能是符号链接，未执行清理")
            lock = stack.enter_context(lock_path.open("a"))
            fcntl.flock(lock, fcntl.LOCK_EX)
        stop_managed_runs(root)
        if runtime.is_symlink():
            remove(runtime)
        else:
            for path in runtime.iterdir():
                if path.name != ".lifecycle.lock":
                    remove(path)
        for name in ("build", "test-results", "__pycache__"):
            remove(root / name)
        for name in ("scripts", "tests", "baseline"):
            remove_caches(root / name)
        for pattern in ("*.pyc", "*.pyo"):
            for path in root.glob(pattern):
                remove(path)
        for directory in root.glob("shard*"):
            if directory.is_dir() and not directory.is_symlink():
                remove(directory / "node.log")
        print("已清理编译产物、运行日志/记录、测试与性能结果、Python 缓存和旧分片日志。")


if __name__ == "__main__":
    try:
        clean()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"清理失败: {error}", file=sys.stderr)
        sys.exit(1)
