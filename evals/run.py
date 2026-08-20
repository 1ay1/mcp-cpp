#!/usr/bin/env python3
"""Tier-1 deterministic tool eval runner.

Drives the toolset over the same MCP JSON-RPC surface the model uses (via
`<binary> mcp-serve`), then checks each response against declarative verifiers.
No model, no API key, no network — safe for CI.

Task format (evals/tasks/*.jsonl, one JSON object per line):
  name          human label
  tool          tool name to call
  args          arguments object (paths are resolved against the workspace)
  workspace     optional subdir under evals/ to run in (default: a temp copy of fixtures/)
  expect        verifiers: contains[], not_contains[], equals, regex, is_error

Exit code: 0 iff every task passes.
"""
import json, os, re, subprocess, sys, tempfile, shutil, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent


def find_binary():
    # The CTest integration builds a self-contained stdio server exposing the
    # full toolset and points us at it. This is the CI path.
    if srv := os.environ.get("AGENTTY_EVAL_SERVER"):
        return [srv]
    # Manual runs can point at a full agentty binary instead.
    if env := os.environ.get("AGENTTY_BIN"):
        return [env, "mcp-serve"]
    for cand in [
        ROOT.parent / "build" / "agentty",
        ROOT / "build-tests" / "tests" / "mcp_eval_server",
        ROOT / "build" / "tests" / "mcp_eval_server",
    ]:
        if cand.exists():
            return [str(cand), "mcp-serve"] if cand.name == "agentty" else [str(cand)]
    sys.exit("no server binary found; set AGENTTY_EVAL_SERVER or AGENTTY_BIN")


def rpc(binary, workspace, calls):
    """Send initialize + a batch of tools/call, return list of result texts."""
    lines = [json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "eval", "version": "1"}}})]
    for i, (tool, args) in enumerate(calls, start=2):
        lines.append(json.dumps({
            "jsonrpc": "2.0", "id": i, "method": "tools/call",
            "params": {"name": tool, "arguments": args}}))
    cmd = list(binary)
    if binary[-1] == "mcp-serve":
        # agentty: -w <dir> mcp-serve
        cmd = binary[:-1] + ["-w", str(workspace), "mcp-serve"]
    else:
        # eval_server: -w <dir>
        cmd = binary + ["-w", str(workspace)]
    p = subprocess.run(cmd, input="\n".join(lines) + "\n",
                       capture_output=True, text=True, timeout=60)
    out = {}
    for line in p.stdout.splitlines():
        try:
            o = json.loads(line)
        except Exception:
            continue
        if "id" in o and o["id"] >= 2:
            out[o["id"]] = o
    return out


def response_text(resp):
    if resp.get("error") or resp.get("result", {}).get("isError"):
        return None
    return "".join(c.get("text", "")
                   for c in resp.get("result", {}).get("content", []))


def check(resp, expect):
    """Return (ok, reason)."""
    is_err = bool(resp.get("error")) or bool(resp.get("result", {}).get("isError"))
    if expect.get("is_error"):
        return (is_err, "expected an error, got success" if not is_err else "")
    if is_err:
        return (False, f"unexpected error: {json.dumps(resp)[:200]}")
    text = response_text(resp) or ""
    for sub in expect.get("contains", []):
        if sub not in text:
            return (False, f"missing {sub!r} in output")
    for sub in expect.get("not_contains", []):
        if sub in text:
            return (False, f"unexpected {sub!r} in output")
    if "equals" in expect and text.strip() != expect["equals"]:
        return (False, f"expected == {expect['equals']!r}, got {text.strip()[:80]!r}")
    if "regex" in expect and not re.search(expect["regex"], text):
        return (False, f"output did not match /{expect['regex']}/")
    return (True, "")


# Volatile bits of tool output that would make a golden flaky: absolute temp
# paths (the workspace lives in a fresh mktemp dir each run) and match counts
# that depend on scan order. Normalise the workspace path to <WS>.
def normalise_golden(text, workspace):
    return text.replace(str(workspace), "<WS>")


def check_golden(resp, golden_path, workspace, update):
    """Compare the tool's FULL output to a stored golden file. Returns
    (ok, reason). With `update`, (re)writes the golden and always passes."""
    text = response_text(resp)
    if text is None:
        return (False, f"tool errored: {json.dumps(resp)[:200]}")
    norm = normalise_golden(text, workspace)
    if update:
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        golden_path.write_text(norm)
        return (True, "(updated)")
    if not golden_path.exists():
        return (False, f"no golden at {golden_path.name} — run with --update to create")
    want = golden_path.read_text()
    if norm == want:
        return (True, "")
    # Show a compact first-difference so a formatting drift is obvious.
    wl, gl = norm.splitlines(), want.splitlines()
    for i in range(max(len(wl), len(gl))):
        a = wl[i] if i < len(wl) else "<eof>"
        b = gl[i] if i < len(gl) else "<eof>"
        if a != b:
            return (False, f"golden drift at line {i+1}:\n      got:  {a!r}\n      want: {b!r}")
    return (False, "golden differs")


def main():
    update = "--update" in sys.argv
    binary = find_binary()
    tasks = []
    for f in sorted((HERE / "tasks").glob("*.jsonl")):
        for ln in f.read_text().splitlines():
            ln = ln.strip()
            if ln and not ln.startswith("//"):
                tasks.append(json.loads(ln))
    if not tasks:
        sys.exit("no tasks found under evals/tasks/*.jsonl")

    fixtures = HERE / "fixtures"
    passed = failed = 0
    for t in tasks:
        with tempfile.TemporaryDirectory() as ws:
            if fixtures.exists():
                shutil.copytree(fixtures, ws, dirs_exist_ok=True)
            args = dict(t.get("args", {}))
            resp = rpc(binary, ws, [(t["tool"], args)]).get(2, {})
            if "golden" in t:
                ok, why = check_golden(resp, HERE / "golden" / t["golden"],
                                       ws, update)
            else:
                ok, why = check(resp, t.get("expect", {}))
            if ok:
                passed += 1
                print(f"  ok   {t['name']}{(' ' + why) if why else ''}")
            else:
                failed += 1
                print(f"  FAIL {t['name']}: {why}")
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
