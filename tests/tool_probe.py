#!/usr/bin/env python3
"""Throw hostile and edge-case arguments at every mcp-cpp tool and flag what
breaks: crashes, hangs, non-JSON replies, internal errors, leaked exception
text, and writes or reads that escape the workspace.

    tool_probe.py <eval_server> [filter]

Each probe runs in a fresh workspace (and a fresh server, so one crash can't
hide the next). A finding is printed as `TOOL | probe | what`.
"""
import json, os, shutil, subprocess, sys, tempfile, time

SERVER = sys.argv[1]
ONLY = sys.argv[2] if len(sys.argv) > 2 else ""

def ws_setup(root):
    os.makedirs(f"{root}/src/sub", exist_ok=True)
    os.makedirs(f"{root}/empty", exist_ok=True)
    with open(f"{root}/src/a.cpp", "w") as f:
        f.write("int main() {\n    return 0;\n}\n" + "".join(f"int f{i}() {{ return {i}; }}\n" for i in range(50)))
    with open(f"{root}/src/sub/b.py", "w") as f: f.write("def g():\n    return 1\n")
    with open(f"{root}/crlf.txt", "wb") as f: f.write(b"one\r\ntwo\r\nthree\r\n")
    with open(f"{root}/nonl.txt", "w") as f: f.write("no newline at end")
    with open(f"{root}/empty.txt", "w") as f: pass
    with open(f"{root}/bin.dat", "wb") as f: f.write(bytes(range(256)) * 4)
    with open(f"{root}/latin1.txt", "wb") as f: f.write(b"caf\xe9 \xff\xfe bad utf8\n")
    with open(f"{root}/bom.txt", "wb") as f: f.write(b"\xef\xbb\xbfhello bom\n")
    with open(f"{root}/long.txt", "w") as f: f.write("x" * 300000 + "\n")
    with open(f"{root}/many.txt", "w") as f: f.write("".join(f"line {i}\n" for i in range(20000)))
    with open(f"{root}/sp ace.txt", "w") as f: f.write("space name\n")
    with open(f"{root}/data.json", "w") as f: json.dump({"a": [1, 2, {"b": "c"}], "k": None}, f)
    os.symlink("/etc/passwd", f"{root}/link_out")
    os.symlink("src", f"{root}/link_dir")
    os.symlink("nowhere", f"{root}/dangling")
    subprocess.run(["git", "init", "-q", root])
    subprocess.run(["git", "-C", root, "add", "-A"], capture_output=True)
    subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init"], capture_output=True)

OUTSIDE = tempfile.mkdtemp(prefix="probe_outside_")
OUT_FILE = f"{OUTSIDE}/secret.txt"
open(OUT_FILE, "w").write("TOP SECRET OUTSIDE\n")

def call(root, tool, args, timeout=25):
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": tool, "arguments": args}},
    ]
    t0 = time.time()
    try:
        p = subprocess.run([SERVER, "-w", root], input="\n".join(json.dumps(r) for r in reqs) + "\n",
                           capture_output=True, text=True, timeout=timeout, errors="replace")
    except subprocess.TimeoutExpired:
        return {"hang": True, "secs": timeout}
    dt = time.time() - t0
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    out = {"rc": p.returncode, "secs": dt, "stderr": p.stderr[-600:]}
    for l in lines:
        try:
            j = json.loads(l)
        except Exception:
            out["badjson"] = l[:200]
            continue
        if j.get("id") == 2: out["resp"] = j
    return out

findings = []
def flag(tool, probe, what):
    findings.append((tool, probe, what))
    print(f"FINDING  {tool:18} | {probe:34} | {what}", flush=True)

def text_of(resp):
    r = resp.get("result") or {}
    return "".join(c.get("text", "") for c in r.get("content", []) if isinstance(c, dict))

def check(root, tool, probe, args, expect_error=None, forbid=(), timeout=25):
    r = call(root, tool, args, timeout)
    if r.get("hang"):
        flag(tool, probe, f"HANG > {timeout}s"); return None
    if r["rc"] not in (0,) and r["rc"] is not None:
        flag(tool, probe, f"server exit {r['rc']} stderr={r['stderr'][-200:]!r}"); return None
    if "badjson" in r:
        flag(tool, probe, f"non-JSON reply: {r['badjson']!r}")
    resp = r.get("resp")
    if resp is None:
        flag(tool, probe, f"no reply (rc={r['rc']}) stderr={r['stderr'][-200:]!r}"); return None
    if "error" in resp:
        flag(tool, probe, f"JSON-RPC error {resp['error']}"); return None
    txt = text_of(resp)
    is_err = bool((resp.get("result") or {}).get("isError"))
    low = txt.lower()
    for bad in ("std::", "basic_string", "what():", "terminate", "nlohmann", "json.exception",
                "type_error", "out_of_range", "bad_alloc", "invalid_argument", "stoi", "stoul",
                "filesystem error", "[json.exception"):
        if bad in low:
            flag(tool, probe, f"leaks internal error text ({bad}): {txt[:160]!r}")
            break
    if "TOP SECRET OUTSIDE" in txt:
        flag(tool, probe, "READ OUTSIDE WORKSPACE")
    for f in forbid:
        if f in txt:
            flag(tool, probe, f"output contains forbidden {f!r}: {txt[:160]!r}")
    if expect_error is True and not is_err:
        flag(tool, probe, f"expected an error, got success: {txt[:160]!r}")
    if expect_error is False and is_err:
        flag(tool, probe, f"unexpected error: {txt[:200]!r}")
    if r["secs"] > 10:
        flag(tool, probe, f"slow: {r['secs']:.1f}s")
    if len(txt) > 200_000:
        flag(tool, probe, f"huge output {len(txt)} bytes")
    return txt, is_err

# Generic hostile values for any string / int / bool / array argument.
HOSTILE = {
    "missing":  None,
    "null":     None,
    "empty":    "",
    "number":   12345,
    "bool":     True,
    "array":    ["a", "b"],
    "object":   {"x": 1},
    "huge":     "A" * 200000,
    "nul":      "a\u0000b",
    "newline":  "a\nb",
    "ctrl":     "a\u001b[31mb\u0007",
    "rtl":      "a\u202eb",
    "neg":      -1,
    "bigint":   2**62,
    "float":    1.5,
}

def schema_props(tool_schema):
    return (tool_schema.get("inputSchema") or {}).get("properties", {}), \
           (tool_schema.get("inputSchema") or {}).get("required", [])

def tools_list():
    root = tempfile.mkdtemp(prefix="probe_ws_")
    reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}]
    p = subprocess.run([SERVER, "-w", root], input="\n".join(map(json.dumps, reqs)) + "\n",
                       capture_output=True, text=True)
    for l in p.stdout.splitlines():
        j = json.loads(l)
        if j.get("id") == 2: return j["result"]["tools"]

# A known-good base call per tool, so a probe mutates ONE argument at a time.
BASE = {
    "read": {"path": "src/a.cpp"}, "write": {"path": "new.txt", "content": "hi\n"},
    "list_dir": {"path": "."}, "outline": {"path": "src/a.cpp"},
    "move": {"source": "nonl.txt", "destination": "moved.txt"},
    "remove": {"path": "nonl.txt"},
    "edit": {"path": "src/a.cpp", "edits": [{"old_text": "return 0;", "new_text": "return 1;"}]},
    "apply_patch": {"path": "src/a.cpp", "patch": "@@ -1,2 +1,2 @@\n int main() {\n-    return 0;\n+    return 2;\n"},
    "shell": {"command": "echo hi"}, "process_start": {"command": "sleep 1"},
    "process_poll": {"id": "proc-1"}, "process_stop": {"id": "proc-1"},
    "grep": {"pattern": "main"}, "glob": {"pattern": "*.cpp"},
    "find_definition": {"symbol": "main"}, "search_structural": {"pattern": "return $X;"},
    "rewrite_structural": {"pattern": "return $X;", "rewrite": "return ($X);"},
    "repo_map": {}, "extract": {"pattern": "f(\\d+)", "group": 1},
    "aggregate": {"pattern": "return"}, "replace": {"find": "main", "replacement": "main2", "glob": "*.cpp"},
    "read_filter": {"path": "src/a.cpp", "pattern": "return"}, "json_query": {"path": "data.json", "query": ".a"},
    "diagnostics": {"command": "true"}, "test": {"command": "true"},
    "git_status": {}, "git_diff": {}, "git_log": {}, "git_show": {}, "git_blame": {"path": "src/a.cpp"},
    "git_commit": {"message": "x", "stage_all": True}, "git_branch": {}, "git_stash": {},
    "git_rebase": {"action": "abort"}, "git_cherry_pick": {"action": "abort"},
    "todo": {"todos": [{"content": "a", "status": "pending"}]},
}

_made = []
def fresh():
    root = tempfile.mkdtemp(prefix="probe_ws_")
    _made.append(root)
    if len(_made) > 8:                      # each has a 300 KB + 20k-line file
        shutil.rmtree(_made.pop(0), ignore_errors=True)
    ws_setup(root)
    return root

def run():
    tools = tools_list()
    for t in tools:
        name = t["name"]
        if ONLY and ONLY not in name: continue
        props, req = schema_props(t)
        base = BASE.get(name, {})
        root = fresh()
        # 1. the base call works
        check(root, name, "base", dict(base))
        # 2. empty arguments / wrong top-level type
        check(fresh(), name, "args={}", {})
        # 3. each property, each hostile value
        for pname in props:
            for hname, hval in HOSTILE.items():
                args = dict(base)
                if hname == "missing":
                    args.pop(pname, None)
                else:
                    args[pname] = hval
                check(fresh(), name, f"{pname}={hname}", args)
        # 4. unknown extra key
        a = dict(base); a["zzz_unknown"] = 1
        check(fresh(), name, "unknown key", a)
    path_probes()
    print(f"\n{len(findings)} finding(s)")

def path_probes():
    """Every path-taking argument against escape and odd-file shapes."""
    PATHS = {
        "abs_outside": OUT_FILE, "dotdot": f"../{os.path.basename(OUTSIDE)}/secret.txt",
        "deep_dotdot": "src/../../../../../../etc/passwd", "symlink_out": "link_out",
        "symlink_dir": "link_dir/a.cpp", "dangling": "dangling", "dir_as_file": "src",
        "missing": "nope/none.txt", "binary": "bin.dat", "latin1": "latin1.txt", "bom": "bom.txt",
        "crlf": "crlf.txt", "empty": "empty.txt", "longline": "long.txt", "many": "many.txt",
        "space": "sp ace.txt", "tilde": "~/.bashrc", "proc": "/proc/self/environ",
        "devzero": "/dev/zero", "fifo_like": "/dev/stdin", "trailing_slash": "src/a.cpp/",
        "dot": ".", "nul_in": "src/a\u0000.cpp", "url": "file:///etc/passwd",
    }
    path_args = {
        "read": "path", "outline": "path", "read_filter": "path", "json_query": "path",
        "list_dir": "path", "grep": "path", "glob": "path", "git_blame": "path",
        "write": "path", "edit": "path", "apply_patch": "path", "remove": "path",
    }
    writes = {"write", "edit", "apply_patch", "remove"}
    for tool, arg in path_args.items():
        if ONLY and ONLY not in tool: continue
        for pname, pval in PATHS.items():
            root = fresh()
            args = dict(BASE.get(tool, {})); args[arg] = pval
            if tool == "read_filter": args.setdefault("pattern", ".")
            before = open(OUT_FILE).read()
            r = check(root, tool, f"path:{pname}", args)
            if tool in writes:
                if not os.path.exists(OUT_FILE) or open(OUT_FILE).read() != before:
                    flag(tool, f"path:{pname}", "WROTE/REMOVED OUTSIDE WORKSPACE")
                    open(OUT_FILE, "w").write("TOP SECRET OUTSIDE\n")
                if os.path.exists("/etc/passwd") is False:
                    flag(tool, f"path:{pname}", "/etc/passwd gone?!")
            if pname in ("abs_outside", "dotdot", "deep_dotdot", "symlink_out", "proc") and r and not r[1]:
                flag(tool, f"path:{pname}", f"outside path ACCEPTED: {r[0][:120]!r}")

if __name__ == "__main__":
    try:
        run()
    finally:
        for d in _made: shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(OUTSIDE, ignore_errors=True)
