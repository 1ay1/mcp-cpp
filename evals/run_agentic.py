#!/usr/bin/env python3
"""Tier-2 AGENTIC tool eval runner.

Asks the real agent (via `agentty run`) to accomplish a task, captures WHICH
tools it invoked (AGENTTY_TRACE_TOOLS=1 → `TOOL <name> <ok|error>` on stderr),
and checks that against the task's expectations. This is the tool-SELECTION
question Anthropic's guide emphasises — does the model actually reach for the
right tool? — which Tier 1 (tool correctness) can't answer.

Unlike Tier 1 this needs a live model + credentials, so it is NOT wired into
CI. Run it deliberately:

    AGENTTY_BIN=/path/to/agentty python3 evals/run_agentic.py

Task format (tasks/agentic/*.jsonl, one JSON object per line):
  name            label
  workspace       subdir under evals/ to run in (a temp copy is made)
  prompt          the natural-language instruction given to the agent
  expect_tools    tool names that MUST appear in the trace
  reject_tools    tool names that must NOT appear (e.g. the slow alternative)
  answer_contains substrings the final stdout answer must contain
"""
import json, os, re, subprocess, sys, tempfile, shutil, pathlib

HERE = pathlib.Path(__file__).resolve().parent


def agentty_bin():
    b = os.environ.get("AGENTTY_BIN")
    if not b:
        sys.exit("set AGENTTY_BIN=/path/to/agentty (Tier 2 needs a live model)")
    return b


def run_task(binary, workspace, prompt):
    """Run one prompt headless; return (final_answer, tools_used[])."""
    env = dict(os.environ, AGENTTY_TRACE_TOOLS="1")
    p = subprocess.run(
        [binary, "-w", str(workspace), "run", prompt],
        capture_output=True, text=True, env=env, timeout=300)
    tools = []
    for line in p.stderr.splitlines():
        m = re.match(r"TOOL (\S+) (ok|error)", line)
        if m:
            tools.append(m.group(1))
    return p.stdout.strip(), tools


def main():
    binary = agentty_bin()
    taskdir = HERE / "tasks" / "agentic"
    tasks = []
    if taskdir.exists():
        for f in sorted(taskdir.glob("*.jsonl")):
            for ln in f.read_text().splitlines():
                ln = ln.strip()
                if ln and not ln.startswith("//"):
                    tasks.append(json.loads(ln))
    if not tasks:
        sys.exit("no agentic tasks under evals/tasks/agentic/*.jsonl")

    fixtures = HERE / "fixtures"
    passed = failed = 0
    for t in tasks:
        with tempfile.TemporaryDirectory() as ws:
            if fixtures.exists():
                shutil.copytree(fixtures, ws, dirs_exist_ok=True)
            answer, tools = run_task(binary, ws, t["prompt"])
            reasons = []
            for want in t.get("expect_tools", []):
                if want not in tools:
                    reasons.append(f"expected tool {want!r}, used {tools}")
            for bad in t.get("reject_tools", []):
                if bad in tools:
                    reasons.append(f"used rejected tool {bad!r}")
            for sub in t.get("answer_contains", []):
                if sub.lower() not in answer.lower():
                    reasons.append(f"answer missing {sub!r}")
            if reasons:
                failed += 1
                print(f"  FAIL {t['name']}")
                for r in reasons:
                    print(f"       - {r}")
                print(f"       tools: {tools}")
            else:
                passed += 1
                print(f"  ok   {t['name']}  (tools: {tools})")
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
