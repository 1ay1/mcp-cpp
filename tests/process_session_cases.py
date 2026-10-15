#!/usr/bin/env python3
"""Drive process_start / process_poll / process_stop through one long-lived
`agentty mcp-serve` and check output, ids, stop and reaping.

    process_cases.py AGENTTY
"""
import json, os, subprocess, sys, tempfile, time

A = sys.argv[1]
root = tempfile.mkdtemp(prefix="pc_")
p = subprocess.Popen([A, "mcp-serve"], cwd=root, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
nid = 0
def rpc(method, params=None, notify=False):
    global nid
    msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
    if not notify:
        nid += 1; msg["id"] = nid
    p.stdin.write(json.dumps(msg) + "\n"); p.stdin.flush()
    if notify:
        return None
    while True:
        line = p.stdout.readline()
        if not line:
            raise RuntimeError("server closed")
        j = json.loads(line)
        if j.get("id") == nid:
            return j

def tool(name, **args):
    r = rpc("tools/call", {"name": name, "arguments": args})["result"]
    return "".join(c.get("text", "") for c in r.get("content", [])), bool(r.get("isError"))

rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "pc", "version": "1"}})
rpc("notifications/initialized", notify=True)

fails = 0
def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
    fails += 0 if ok else 1

import re
def pid_of(text):
    m = re.search(r"\b(proc-\d+)\b", text)
    return m.group(1) if m else None

# 1. poll returns new output only, no duplicates, no gaps
t, e = tool("process_start", command="for i in $(seq 1 30); do echo line$i; sleep 0.05; done")
sid = pid_of(t)
check("start returns an id", sid is not None, t[:200])
seen = t   # start returns the first output too
for _ in range(40):
    t, e = tool("process_poll", id=sid, wait_ms=300)
    seen += t
    if "exited" in t:
        break
nums = [int(x) for x in re.findall(r"line(\d+)", seen)]
check("poll output has every line once, in order", nums == list(range(1, 31)),
      f"got {nums[:10]}...{nums[-5:]} len={len(nums)}")
check("poll reports the exit", "exited" in seen, seen[-200:])

# 2. poll after exit is still answerable, stop after exit is fine
t, e = tool("process_poll", id=sid)
check("poll after exit still answers", not e or "exited" in t, t[:200])

# 3. stop kills the tree and reaps
marker = f"pc_marker_{os.getpid()}"
t, e = tool("process_start", command=f"(exec -a {marker} sleep 300 &) ; exec -a {marker} sleep 300")
sid2 = pid_of(t)
time.sleep(0.8)
def alive():
    out = subprocess.run(["pgrep", "-f", marker], capture_output=True, text=True).stdout.split()
    return [x for x in out if x.strip()]
check("stop target is running", len(alive()) >= 1, "nothing running")
t, e = tool("process_stop", id=sid2)
for _ in range(20):
    if not alive():
        break
    time.sleep(0.2)
left = alive()
check("stop kills the whole tree", not left, f"{len(left)} left")
for x in left:
    subprocess.run(["kill", "-9", x])

# 4. ids are not reused after stop
t, e = tool("process_start", command="sleep 0.1")
sid3 = pid_of(t)
check("ids are not reused", sid3 not in (sid, sid2), f"{sid} {sid2} {sid3}")

# 5. a chatty process: output is bounded, latest kept
t, e = tool("process_start", command="yes 0123456789 | head -c 5000000; echo TAIL_MARK")
sid4 = pid_of(t)
last = ""
for _ in range(40):
    t, e = tool("process_poll", id=sid4, wait_ms=500)
    check_len = len(t)
    if check_len >= 200_000:
        break
    last += t
    if "exited" in t:
        break
check("chatty process output bounded per poll", check_len < 200_000, f"len={check_len}")
check("chatty process: the last line arrives", "TAIL_MARK" in last,
      "last poll head: " + repr(t[:300]) + " ... tail: " + repr(t[-300:]))

p.stdin.close()
p.wait(timeout=10)
print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
