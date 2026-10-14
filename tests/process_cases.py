#!/usr/bin/env python3
"""process_start / poll / stop lifecycle, in one server so sessions persist.
    process_cases.py <eval_server>
"""
import json, subprocess, sys, tempfile, time

SERVER = sys.argv[1]
root = tempfile.mkdtemp(prefix="pc_")
p = subprocess.Popen([SERVER, "-w", root], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
nid = [0]
def call(tool, args):
    nid[0] += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid[0], "method": "tools/call",
                              "params": {"name": tool, "arguments": args}}) + "\n"); p.stdin.flush()
    t0 = time.time()
    line = p.stdout.readline()
    j = json.loads(line)
    r = j.get("result", {})
    return "".join(c.get("text", "") for c in r.get("content", [])), bool(r.get("isError")), time.time() - t0

p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}}) + "\n"); p.stdin.flush(); p.stdout.readline()
fails = 0
def expect(name, cond, info=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"   {info[:300]!r}"))
    if not cond: fails += 1

t, e, _ = call("process_start", {"command": "echo started; sleep 30"})
pid = t.split("proc-")[1].split()[0].strip(".,:)'\"`") if "proc-" in t else None
expect("start returns an id", pid is not None, t)
expect("start returns the first output", "started" in t, t)
pid = "proc-" + (pid or "x")
t, e, dt = call("process_poll", {"id": pid, "wait_ms": 300})
expect("poll after start reports running", "running" in t, t)
t, e, dt = call("process_poll", {"id": pid, "wait_ms": 10_000_000})
expect("huge wait_ms is bounded", dt < 70, f"took {dt:.0f}s")
t, e, _ = call("process_stop", {"id": pid})
expect("stop works", not e, t)
t, e, _ = call("process_stop", {"id": pid})
expect("second stop is clean (not an error, or a clear one)", "stopped" in t.lower() or "no such" in t.lower() or "unknown" in t.lower() or "not found" in t.lower(), t)
t, e, _ = call("process_poll", {"id": pid})
expect("poll after stop is clear", e or "exit" in t.lower() or "stopped" in t.lower(), t)
for bad in ["proc-abc", "proc-99999999999999999999999", "proc--1", "", "../x", "proc-1\n"]:
    t, e, _ = call("process_poll", {"id": bad})
    expect(f"poll bad id {bad!r} is an error", e, t)
t, e, _ = call("process_start", {"command": "exit 7"})
expect("a quick exit is reported by start, with its code", "exit 7" in t and "exited immediately" in t, t)
# output flood is bounded
t, e, _ = call("process_start", {"command": "yes 0123456789 | head -c 50000000"})
q = "proc-" + t.split("proc-")[1].split()[0].strip(".,:)'\"`") if "proc-" in t else ""
time.sleep(2)
t, e, _ = call("process_poll", {"id": q, "wait_ms": 1000})
expect("flood poll is bounded", len(t) < 300_000, f"{len(t)} bytes")
call("process_stop", {"id": q})
# session cap
ids = []
for i in range(40):
    t, e, _ = call("process_start", {"command": "sleep 30"})
    if e: break
    ids.append(t)
expect("session cap stops runaway starts", len(ids) < 40, f"{len(ids)} started")
p.stdin.close(); p.wait(timeout=30)
print(f"\n{fails} failing case(s)"); sys.exit(1 if fails else 0)
