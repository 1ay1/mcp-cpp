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
    if env := os.environ.get("AGENTTY_BIN"):
        return [env, "mcp-serve"]
    # Prefer an agentty superproject binary if present (full toolset), else a
    # mcp-cpp example server. Fall back to a built mcp-serve-capable target.
    for cand in [
        ROOT.parent / "build" / "agentty",
        ROOT / "build-tests" / "examples" / "mcp_server_example",
        ROOT / "build" / "examples" / "mcp_server_example",
    ]:
        if cand.exists():
            # agentty needs the `mcp-serve` subcommand; the example server does not.
            return [str(cand), "mcp-serve"] if cand.name == "agentty" else [str(cand)]
    sys.exit("no server binary found; set AGENTTY_BIN=/path/to/agentty")


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
        cmd = binary[:-1] + ["-w", str(workspace), "mcp-serve"]
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


def check(resp, expect):
    """Return (ok, reason)."""
    is_err = bool(resp.get("error")) or bool(resp.get("result", {}).get("isError"))
    if expect.get("is_error"):
        return (is_err, "expected an error, got success" if not is_err else "")
    if is_err:
        return (False, f"unexpected error: {json.dumps(resp)[:200]}")
    text = ""
    for c in resp.get("result", {}).get("content", []):
        text += c.get("text", "")
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


def main():
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
            ok, why = check(resp, t.get("expect", {}))
            if ok:
                passed += 1
                print(f"  ok   {t['name']}")
            else:
                failed += 1
                print(f"  FAIL {t['name']}: {why}")
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
