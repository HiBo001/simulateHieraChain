#!/usr/bin/env python3
"""Local multi-process cluster lifecycle, deterministic workloads and probes."""
import argparse
import contextlib
import datetime
import fcntl
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import random
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/bin/arbor_node"
RUN_PROCESSES = {}


def read(path):
    return json.loads(Path(path).read_text())


def write(path, data):
    path = Path(path)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temp.replace(path)


def integer(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} 必须为 {low}..{high} 范围内的整数")
    return value


def validate(raw):
    c = json.loads(json.dumps(raw))
    allowed = {"shards", "replicas_per_shard", "host", "base_port", "consensus", "execution", "network"}
    if set(c) - allowed:
        raise ValueError("未知配置项: " + str(sorted(set(c) - allowed)))
    if c.get("replicas_per_shard", 4) != 4:
        raise ValueError("第一阶段每个分片固定为 4 个 PBFT 副本")
    c["replicas_per_shard"] = 4
    shards = c.get("shards")
    if not isinstance(shards, list) or not shards:
        raise ValueError("shards 必须是非空数组")
    parent = {}
    for s in shards:
        if set(s) != {"id", "parent"}:
            raise ValueError("每个分片必须且只能包含 id、parent；根的 parent 为 null")
        sid = integer(s["id"], "shard.id", 1, 1000000)
        if sid in parent:
            raise ValueError(f"分片 ID 重复: {sid}")
        if s["parent"] is not None:
            integer(s["parent"], "parent", 1, 1000000)
        parent[sid] = s["parent"]
    roots = [sid for sid, p in parent.items() if p is None]
    if len(roots) != 1:
        raise ValueError("拓扑必须恰有一个根节点")
    for sid in parent:
        seen, cursor = set(), sid
        while cursor is not None:
            if cursor not in parent:
                raise ValueError(f"引用了不存在的父分片: {cursor}")
            if cursor in seen:
                raise ValueError(f"拓扑存在环，涉及分片 {cursor}")
            seen.add(cursor)
            cursor = parent[cursor]
    c["shards"] = sorted(shards, key=lambda s: s["id"])
    c.setdefault("host", "127.0.0.1")
    ipaddress.IPv4Address(c["host"])
    c.setdefault("base_port", 19000)
    integer(c["base_port"], "base_port", 1024, 65535 - 4 * len(shards) + 1)
    defaults = {
        "consensus": {"batch_size": 32, "batch_wait_ms": 10, "cross_shard_batch_size": None,
                      "cross_shard_batch_wait_ms": 200, "view_timeout_ms": 2000, "checkpoint_batches": 16},
        "execution": {"fib_iterations": 10000},
        "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 20, "shard_links": [], "trace": False},
    }
    for group, fields in defaults.items():
        current = c.setdefault(group, {})
        if not isinstance(current, dict) or set(current) - set(fields):
            raise ValueError(f"{group} 包含未知配置项")
        for name, default in fields.items():
            current.setdefault(name, default)
    for name, low, high in [("batch_size", 1, 1024), ("batch_wait_ms", 0, 10000), ("view_timeout_ms", 200, 300000), ("checkpoint_batches", 1, 32)]:
        integer(c["consensus"][name], name, low, high)
    if c["consensus"]["cross_shard_batch_size"] is None:
        c["consensus"]["cross_shard_batch_size"] = min(64, c["consensus"]["batch_size"])
    integer(c["consensus"]["cross_shard_batch_size"], "cross_shard_batch_size", 1, c["consensus"]["batch_size"])
    integer(c["consensus"]["cross_shard_batch_wait_ms"], "cross_shard_batch_wait_ms", 0, 10000)
    integer(c["execution"]["fib_iterations"], "fib_iterations", 1, 100000000)
    n = c["network"]
    for name in ["intra_shard_delay_ms", "default_inter_shard_delay_ms"]:
        integer(n[name], name, 0, 60000)
    if type(n["trace"]) is not bool:
        raise ValueError("network.trace 必须为布尔值")
    if not isinstance(n["shard_links"], list):
        raise ValueError("shard_links 必须为数组")
    resolved = {}
    for link in n["shard_links"]:
        if set(link) != {"shards", "delay_ms"}:
            raise ValueError("每条链路必须包含 shards 和 delay_ms")
        pair = link["shards"]
        if not isinstance(pair, list) or len(pair) != 2:
            raise ValueError("shards 必须是长度为 2 的分片 ID 数组")
        for sid in pair:
            integer(sid, "link.shards", 1, 1000000)
            if sid not in parent:
                raise ValueError(f"链路引用了不存在的分片: {sid}")
        a, b = pair
        if a == b:
            raise ValueError("片内延迟请使用 intra_shard_delay_ms")
        d = integer(link["delay_ms"], "link.delay_ms", 0, 60000)
        if f"{a}:{b}" in resolved:
            raise ValueError(f"重复的分片链路: {a}, {b}")
        resolved[f"{a}:{b}"] = resolved[f"{b}:{a}"] = d
    n["resolved_links"] = resolved
    if c["consensus"]["view_timeout_ms"] <= 4 * n["intra_shard_delay_ms"] + c["consensus"]["batch_wait_ms"]:
        raise ValueError("view_timeout_ms 太短，应大于 4 倍片内单向延迟加组批等待时间")
    return c


def topology(c):
    parent = {s["id"]: s["parent"] for s in c["shards"]}
    leaves = sorted(set(parent) - set(parent.values()))
    return parent, leaves


def format_topology(c):
    parent, _ = topology(c)
    root = next(sid for sid, ancestor in parent.items() if ancestor is None)
    children = {sid: [] for sid in parent}
    for sid, ancestor in parent.items():
        if ancestor is not None:
            children[ancestor].append(sid)
    for siblings in children.values():
        siblings.sort()

    def label(sid):
        kind = "叶子" if not children[sid] else "协调者"
        if sid == root:
            kind = "根/" + kind
        return f"分片 {sid}（{kind}）"

    lines = [f"分片拓扑（{len(parent)} 个分片，每片 {c.get('replicas_per_shard', 4)} 个 PBFT 副本）", label(root)]

    def visit(sid, prefix):
        for index, child in enumerate(children[sid]):
            last = index == len(children[sid]) - 1
            lines.append(prefix + ("└── " if last else "├── ") + label(child))
            visit(child, prefix + ("    " if last else "│   "))

    visit(root, "")
    return "\n".join(lines)


def lca(c, participants):
    parent, leaves = topology(c)
    if not participants or len(set(participants)) != len(participants) or not set(participants) <= set(leaves):
        raise ValueError(f"参与者必须是不重复的叶子分片 ID；当前叶子为 {leaves}")
    paths = []
    for sid in participants:
        path = []
        while sid is not None:
            path.append(sid)
            sid = parent[sid]
        paths.append(path)
    return next(sid for sid in paths[0] if all(sid in p for p in paths[1:]))


def openssl():
    candidates = [os.environ.get("OPENSSL_BIN"), "/opt/homebrew/opt/openssl@3/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl", shutil.which("openssl")]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise ValueError("未找到 OpenSSL，请安装 OpenSSL 3")


def keypair(directory, name):
    secret, public = directory / (name + ".pem"), directory / (name + ".pub.pem")
    subprocess.run([openssl(), "genpkey", "-algorithm", "ED25519", "-out", str(secret)], check=True, capture_output=True)
    secret.chmod(0o600)
    subprocess.run([openssl(), "pkey", "-in", str(secret), "-pubout", "-out", str(public)], check=True, capture_output=True)
    return str(secret), str(public)


def is_our_process(n):
    pid = n.get("pid")
    if not isinstance(pid, int) or pid <= 1:
        return False
    r = subprocess.run(["ps", "-p", str(pid), "-o", "command="], capture_output=True, text=True)
    command = r.stdout.strip()
    return r.returncode == 0 and str(BIN) in command and n["directory"] in command and " node " in command


def stop_run(run):
    run = Path(run).resolve()
    manifest_path = run / "manifest.json"
    if not manifest_path.exists():
        raise ValueError(f"找不到运行记录: {manifest_path}")
    m = read(manifest_path)
    for n in m["nodes"]:
        if is_our_process(n):
            try:
                os.kill(n["pid"], signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and any(is_our_process(n) for n in m["nodes"]):
        time.sleep(.1)
    for n in m["nodes"]:
        if is_our_process(n):
            os.kill(n["pid"], signal.SIGKILL)
    for process in RUN_PROCESSES.pop(str(run), []):
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    m["stopped_at"] = datetime.datetime.now().isoformat()
    write(manifest_path, m)
    print(f"已停止本次运行的全部节点: {run}")


def statuses(run):
    m = read(Path(run) / "manifest.json")
    result = []
    for n in m["nodes"]:
        path = Path(n["directory"]) / "status.json"
        s = read(path) if path.exists() else {"shard": n["shard"], "replica": n["replica"], "ready": False}
        s["alive"] = is_our_process(n)
        s["ready"] = s.get("ready", False) and s["alive"]
        result.append(s)
    return result


def start(config, run=None):
    explicit_run = run is not None
    c = validate(read(config))
    if not BIN.exists():
        raise ValueError("请先运行 make")
    root_runtime = ROOT / "runtime"
    root_runtime.mkdir(exist_ok=True)
    # Prevent concurrent start commands from racing on the same ports/latest link.
    with (root_runtime / ".lifecycle.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        latest = root_runtime / "latest"
        if run is None and latest.exists():
            m = read(latest / "manifest.json")
            if any(is_our_process(n) for n in m["nodes"]):
                raise ValueError("已有运行中的集群，请先 ./stop_all.sh，或指定独立 --run-dir 和端口")
        run = Path(run).resolve() if run else root_runtime / (datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
        if run.exists():
            raise ValueError(f"运行目录已存在，为保留日志请指定新目录: {run}")
        # Check all ports before creating keys or launching any node.
        with contextlib.ExitStack() as stack:
            for i in range(4 * len(c["shards"])):
                sock = stack.enter_context(socket.socket())
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind((c["host"], c["base_port"] + i))
        run.mkdir(parents=True)
        keys = run / "keys"
        keys.mkdir(mode=0o700)
        c["run_id"] = uuid.uuid4().hex
        c["client_private_key"], c["client_public_key"] = keypair(keys, "client")
        c["nodes"] = []
        for s in c["shards"]:
            for r in range(4):
                sid = s["id"]
                secret, public = keypair(keys, f"s{sid}-n{r}")
                directory = run / f"shard{sid}" / f"node{r}"
                directory.mkdir(parents=True)
                c["nodes"].append({"shard": sid, "replica": r, "host": c["host"], "port": c["base_port"] + len(c["nodes"]), "private_key": secret, "public_key": public, "directory": str(directory)})
        write(run / "config.json", c)
        manifest = {"run_id": c["run_id"], "config_source": str(Path(config).resolve()), "nodes": []}
        write(run / "manifest.json", manifest)
        processes = []
        RUN_PROCESSES[str(run)] = processes
        try:
            for n in c["nodes"]:
                with (Path(n["directory"]) / "node.log").open("w") as log:
                    p = subprocess.Popen([str(BIN), "node", str(run / "config.json"), str(n["shard"]), str(n["replica"]), n["directory"]], stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                processes.append(p)
                manifest["nodes"].append(dict(n, pid=p.pid))
                write(run / "manifest.json", manifest)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if any(p.poll() is not None for p in processes):
                    raise RuntimeError("有节点启动失败，请检查对应 node.log")
                if all((Path(n["directory"]) / "status.json").exists() and read(Path(n["directory"]) / "status.json").get("ready") for n in manifest["nodes"]):
                    break
                time.sleep(.1)
            else:
                raise RuntimeError("等待节点就绪超时")
            if not explicit_run:
                if latest.is_symlink():
                    latest.unlink()
                elif latest.exists():
                    raise ValueError("runtime/latest 已存在且不是符号链接")
                latest.symlink_to(run, target_is_directory=True)
            parent, leaves = topology(c)
            print(f"已启动 {len(c['shards'])} 个分片、{len(c['nodes'])} 个节点；根 {next(s for s,p in parent.items() if p is None)}；叶子 {leaves}")
            print(format_topology(c))
            print(f"RUN_DIR={run}")
            return run
        except BaseException:
            stop_run(run)
            raise


def prepare_workload(c, count, rate, seed, prefix, shard=None, participants=None, batch=None, timeout=30):
    if count <= 0 or not math.isfinite(rate) or rate <= 0 or timeout <= 0:
        raise ValueError("count、rate、timeout 必须为正数")
    _, leaves = topology(c)
    if participants is not None:
        target = lca(c, participants)
    elif shard is not None:
        if shard not in leaves:
            raise ValueError("--shard 必须是叶子；测试协调者排序请用 --participants 叶子1,叶子2")
        target, participants = shard, [shard]
    else:
        target, participants = None, None
    cross_shard = participants is not None and len(participants) > 1
    batch_limit = c["consensus"]["cross_shard_batch_size" if cross_shard else "batch_size"]
    if batch is None:
        batch = batch_limit
    integer(batch, "跨片 --batch（cross_shard_batch_size）" if cross_shard else "--batch", 1, batch_limit)
    rng = random.Random(seed)
    requests = []
    remaining, index = count, 0
    while remaining:
        ps = participants or [leaves[len(requests) % len(leaves)]]
        dest = target if target is not None else ps[0]
        txs = []
        for _ in range(min(batch, remaining)):
            if len(ps) == 1:
                tx = {"id": f"{prefix}:tx:{index}", "key": f"account:{ps[0]}:{rng.randrange(1000)}", "value": rng.randrange(1, 1000000), "participants": sorted(ps)}
            else:
                accesses = [{"shard": sid, "key": f"account:{sid}:{rng.randrange(1000)}", "value": rng.randrange(1, 1000000)} for sid in sorted(ps)]
                tx = {"id": f"{prefix}:tx:{index}", "key": accesses[0]["key"], "value": accesses[0]["value"],
                      "participants": sorted(ps), "accesses": accesses}
            txs.append(tx)
            index += 1
        remaining -= len(txs)
        requests.append({"id": f"{prefix}:request:{len(requests)}", "target": dest, "txs": txs})
    return {"rate": rate, "timeout_s": timeout, "seed": seed, "requests": requests}


def load(run, count=100, rate=100, seed=1, prefix=None, shard=None, participants=None, batch=None, timeout=30, output=None):
    run = Path(run).resolve()
    c = read(run / "config.json")
    prefix = prefix or uuid.uuid4().hex
    workload = prepare_workload(c, count, rate, seed, prefix, shard, participants, batch, timeout)
    output = Path(output).resolve() if output else run / ("client-" + uuid.uuid4().hex[:8] + ".json")
    workload_path = output.with_suffix(".workload.json")
    write(workload_path, workload)
    rc = subprocess.run([str(BIN), "client", str(run / "config.json"), str(workload_path), str(output)]).returncode
    print(f"客户端结果: {output}")
    return rc, read(output)


def send_frame(host, port, env):
    payload = json.dumps(env, ensure_ascii=False, separators=(",", ":")).encode()
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(struct.pack("!I", len(payload)) + payload)


def signed(body, key, run):
    name = uuid.uuid4().hex
    input_path, output_path = Path(run) / (name + ".input.json"), Path(run) / (name + ".signed.json")
    try:
        write(input_path, body)
        subprocess.run([str(BIN), "sign", str(input_path), str(key), str(output_path)], check=True)
        return read(output_path)
    finally:
        input_path.unlink(missing_ok=True)
        output_path.unlink(missing_ok=True)


def probe(run, source, dest, samples=5):
    run = Path(run).resolve()
    c = read(run / "config.json")
    endpoints = {f"{n['shard']}:{n['replica']}": n for n in c["nodes"]}
    if source not in endpoints or dest not in endpoints:
        raise ValueError("source/to 格式为 分片ID:副本ID，且必须存在于配置中")
    src, dst = endpoints[source], endpoints[dest]
    rtts = []
    for _ in range(samples):
        nonce = uuid.uuid4().hex
        env = signed({"type": "PROBE", "run": c["run_id"], "source": source, "dst_shard": dst["shard"], "dst_replica": dst["replica"], "id": nonce}, c["client_private_key"], run)
        send_frame(src["host"], src["port"], env)
        deadline = time.monotonic() + 130
        while time.monotonic() < deadline:
            data = read(Path(src["directory"]) / "status.json").get("probes", {})
            if nonce in data:
                rtts.append(data[nonce]["rtt_ms"])
                break
            time.sleep(.025)
        else:
            raise RuntimeError("链路探测超时")
    n = c["network"]
    one_way = n["intra_shard_delay_ms"] if src["shard"] == dst["shard"] else n["resolved_links"].get(f"{src['shard']}:{dst['shard']}", n["default_inter_shard_delay_ms"])
    result = {"source": source, "destination": dest, "configured_one_way_ms": one_way, "expected_added_rtt_ms": 2 * one_way, "samples_ms": rtts, "mean_ms": sum(rtts) / len(rtts)}
    write(run / f"probe-{source.replace(':','-')}-{dest.replace(':','-')}.json", result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return result


class SingleUse(argparse.Action):
    """Reject repeated load options instead of silently keeping the last value."""

    def __call__(self, parser, namespace, values, option_string=None):
        seen = getattr(namespace, "_single_use_options", set())
        if self.dest in seen:
            parser.error(f"{option_string} 在一条 load 命令中不能重复；向多个分片发送交易请省略 --shard 或分别运行 load")
        seen.add(self.dest)
        namespace._single_use_options = seen
        setattr(namespace, self.dest, values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ["validate", "start"]:
        p = commands.add_parser(name)
        p.add_argument("--config", default=str(ROOT / "config/two_layer.json"))
        if name == "start":
            p.add_argument("--run-dir")
    p = commands.add_parser("topology", help="打印最近一次运行或指定配置的分片拓扑")
    source = p.add_mutually_exclusive_group()
    source.add_argument("--run-dir", help="运行目录；默认 runtime/latest")
    source.add_argument("--config", help="预览尚未启动的配置文件")
    for name in ["stop", "status", "load", "probe", "kill-node"]:
        p = commands.add_parser(name)
        p.add_argument("--run-dir", default=str(ROOT / "runtime/latest"))
        if name == "status":
            p.add_argument("--json", action="store_true")
        if name == "load":
            p.add_argument("--count", type=int, default=100, action=SingleUse)
            p.add_argument("--rate", type=float, default=100, action=SingleUse)
            p.add_argument("--seed", type=int, default=1)
            p.add_argument("--id-prefix")
            p.add_argument("--shard", type=int, action=SingleUse)
            p.add_argument("--participants")
            p.add_argument("--batch", type=int)
            p.add_argument("--timeout", type=float, default=30)
            p.add_argument("--output")
        if name == "probe":
            p.add_argument("--source", required=True)
            p.add_argument("--to", required=True)
            p.add_argument("--samples", type=int, default=5)
        if name == "kill-node":
            p.add_argument("--shard", type=int, required=True)
            p.add_argument("--replica", type=int, required=True)
    a = parser.parse_args()
    if a.command == "validate":
        c = validate(read(a.config))
        parent, leaves = topology(c)
        print(json.dumps({"shard_count": len(parent), "node_count": 4 * len(parent), "root": next(s for s,p in parent.items() if p is None), "leaves": leaves, "parents": parent, "network": c["network"]}, ensure_ascii=False, indent=2))
    elif a.command == "start":
        start(a.config, a.run_dir)
    elif a.command == "topology":
        if a.config:
            c = validate(read(a.config))
        else:
            config_path = Path(a.run_dir or ROOT / "runtime/latest") / "config.json"
            if not config_path.is_file():
                raise ValueError(f"找不到运行配置 {config_path}；请先启动集群，或使用 --config 预览")
            c = read(config_path)
        print(format_topology(c))
    elif a.command == "stop":
        stop_run(a.run_dir)
    elif a.command == "status":
        rows = statuses(a.run_dir)
        if a.json:
            print(json.dumps(rows, ensure_ascii=False, indent=2))
        else:
            for r in rows:
                print(f"shard={r['shard']} node={r['replica']} alive={r['alive']} view={r.get('view','?')} primary={r.get('primary','?')} forward={r.get('forward_replica','?')} batches={r.get('applied_batches',0)} executed={r.get('executed_transactions',0)} ordered_only={r.get('ordered_cst_transactions',0)} cst_batches={r.get('cst_order_index',0)} avg_cst_batch={r.get('avg_cst_batch_size',0):.1f} leaf_ordered_cst={r.get('leaf_ordered_cst_transactions',0)} staged_cst={r.get('staged_cst_batches',0)} finalized_cst={r.get('finalized_cst_batches',0)} completed_cst={r.get('completed_cst_transactions',0)}")
    elif a.command == "load":
        if a.participants and a.shard is not None:
            raise ValueError("--shard 和 --participants 不能同时指定")
        participants = [int(s) for s in a.participants.split(",")] if a.participants else None
        rc, _ = load(a.run_dir, a.count, a.rate, a.seed, a.id_prefix, a.shard, participants, a.batch, a.timeout, a.output)
        return rc
    elif a.command == "probe":
        integer(a.samples, "samples", 1, 100)
        probe(a.run_dir, a.source, a.to, a.samples)
    elif a.command == "kill-node":
        m = read(Path(a.run_dir) / "manifest.json")
        node = next((n for n in m["nodes"] if n["shard"] == a.shard and n["replica"] == a.replica), None)
        if not node or not is_our_process(node):
            raise ValueError("指定节点不在运行中")
        os.kill(node["pid"], signal.SIGTERM)
        print(f"已停止 shard={a.shard} replica={a.replica} PID={node['pid']}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)
