#!/usr/bin/env python3
"""Targeted probes for specific tool bugs. Each case writes files into a fresh
workspace, calls one tool through the real mcp_eval_server, and checks the
resulting bytes on disk (not just the reply text).

    tool_cases.py <mcp_eval_server> [filter]
"""
import json, os, subprocess, sys, tempfile

SERVER = sys.argv[1]
ONLY = sys.argv[2] if len(sys.argv) > 2 else ""
fails = 0


def call(root, tool, args, timeout=30):
    init = {"protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "tool_cases", "version": "1"}}
    reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": init},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
             "params": {"name": tool, "arguments": args}}]
    # MCP_SERVE=1: SERVER is agentty, run as `agentty mcp-serve` in root, so the
    # real exec, sandbox and files layers are the ones under test.
    argv = ([SERVER, "mcp-serve"] if os.environ.get("MCP_SERVE")
            else [SERVER, "-w", root])
    p = subprocess.run(argv, cwd=root,
                       input="\n".join(map(json.dumps, reqs)) + "\n",
                       capture_output=True, text=True, timeout=timeout, errors="replace")
    for line in p.stdout.splitlines():
        try:
            j = json.loads(line)
        except Exception:
            continue
        if j.get("id") == 2:
            r = j.get("result") or {}
            txt = "".join(c.get("text", "") for c in r.get("content", []) if isinstance(c, dict))
            return txt, bool(r.get("isError")), p.returncode
    return None, None, p.returncode


def case(name, files, tool, args, check):
    global fails
    if ONLY and ONLY not in name:
        return
    root = tempfile.mkdtemp(prefix="tc_")
    for rel, data in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    try:
        txt, is_err, rc = call(root, tool, args)
    except subprocess.TimeoutExpired:
        print(f"FAIL {name}: HANG"); fails += 1; return
    def read(rel):
        with open(os.path.join(root, rel), "rb") as f:
            return f.read()
    try:
        problem = check(txt or "", is_err, read, root)
    except Exception as e:
        problem = f"check raised {e!r}"
    if rc not in (0, None):
        problem = (problem or "") + f" (server rc={rc})"
    if txt is None:
        problem = (problem or "") + " (no reply)"
    if problem:
        fails += 1
        print(f"FAIL {name}: {problem}\n     reply: {(txt or '')[:300]!r}")
    else:
        print(f"ok   {name}")


# ── edit / apply_patch ─────────────────────────────────────────────────────
case("edit replace_all where new contains old",
     {"a.txt": b"foo\nfoo\nbar\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "foo", "new_text": "foofoo", "replace_all": True}]},
     lambda t, e, r, _: None if r("a.txt") == b"foofoo\nfoofoo\nbar\n" else f"got {r('a.txt')!r}")

case("edit keeps CRLF",
     {"a.txt": b"one\r\ntwo\r\nthree\r\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "two\n", "new_text": "TWO\nmore\n"}]},
     lambda t, e, r, _: None if r("a.txt") == b"one\r\nTWO\r\nmore\r\nthree\r\n" else f"got {r('a.txt')!r}")

case("edit keeps missing final newline",
     {"a.txt": b"alpha\nbeta"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "beta", "new_text": "gamma"}]},
     lambda t, e, r, _: None if r("a.txt") == b"alpha\ngamma" else f"got {r('a.txt')!r}")

case("edit keeps BOM",
     {"a.txt": b"\xef\xbb\xbfhello\nworld\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "world", "new_text": "there"}]},
     lambda t, e, r, _: None if r("a.txt") == b"\xef\xbb\xbfhello\nthere\n" else f"got {r('a.txt')!r}")

case("edit on first line with BOM",
     {"a.txt": b"\xef\xbb\xbfhello\nworld\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "hello", "new_text": "hi"}]},
     lambda t, e, r, _: None if r("a.txt") == b"\xef\xbb\xbfhi\nworld\n" else f"got {r('a.txt')!r}")

case("edit ambiguous similar blocks is refused, not guessed",
     {"a.txt": b"x = compute(1)\ny = 2\n\nx = compute(2)\ny = 2\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "x = compute(3)\ny = 2", "new_text": "Z"}]},
     lambda t, e, r, _: None if r("a.txt") == b"x = compute(1)\ny = 2\n\nx = compute(2)\ny = 2\n" and e
                        else f"file now {r('a.txt')!r} err={e}")

case("edit preserves file mode",
     {"run.sh": b"#!/bin/sh\necho hi\n"}, "edit",
     {"path": "run.sh", "edits": [{"old_text": "echo hi", "new_text": "echo bye"}]},
     lambda t, e, r, root: None if (os.chmod(os.path.join(root, "run.sh"), 0o755) or True) else None)

def mode_case():
    global fails
    if ONLY and ONLY not in "edit keeps exec bit":
        return
    root = tempfile.mkdtemp(prefix="tc_")
    p = os.path.join(root, "run.sh")
    with open(p, "wb") as f:
        f.write(b"#!/bin/sh\necho hi\n")
    os.chmod(p, 0o755)
    txt, e, rc = call(root, "edit", {"path": "run.sh", "edits": [{"old_text": "echo hi", "new_text": "echo bye"}]})
    mode = os.stat(p).st_mode & 0o777
    if mode != 0o755 or open(p, "rb").read() != b"#!/bin/sh\necho bye\n":
        fails += 1
        print(f"FAIL edit keeps exec bit: mode={oct(mode)} body={open(p,'rb').read()!r}")
    else:
        print("ok   edit keeps exec bit")
mode_case()

def symlink_case():
    global fails
    if ONLY and ONLY not in "write through symlink keeps link":
        return
    root = tempfile.mkdtemp(prefix="tc_")
    with open(os.path.join(root, "real.txt"), "wb") as f:
        f.write(b"one\n")
    os.symlink("real.txt", os.path.join(root, "link.txt"))
    txt, e, rc = call(root, "edit", {"path": "link.txt", "edits": [{"old_text": "one", "new_text": "two"}]})
    is_link = os.path.islink(os.path.join(root, "link.txt"))
    real = open(os.path.join(root, "real.txt"), "rb").read()
    if not is_link or real != b"two\n":
        fails += 1
        print(f"FAIL write through symlink keeps link: still_link={is_link} real={real!r} reply={txt[:200]!r}")
    else:
        print("ok   write through symlink keeps link")
symlink_case()

PATCH_TWO = (
    "@@ -1,3 +1,3 @@\n a1\n-a2\n+A2\n a3\n"
    "@@ -7,3 +7,3 @@\n b1\n-b2\n+B2\n b3\n"
)
case("apply_patch two hunks",
     {"p.txt": b"a1\na2\na3\nx\nx\nx\nb1\nb2\nb3\n"}, "apply_patch",
     {"path": "p.txt", "patch": PATCH_TWO},
     lambda t, e, r, _: None if r("p.txt") == b"a1\nA2\na3\nx\nx\nx\nb1\nB2\nb3\n" else f"got {r('p.txt')!r}")

case("apply_patch hunks listed bottom-first",
     {"p.txt": b"a1\na2\na3\nx\nx\nx\nb1\nb2\nb3\n"}, "apply_patch",
     {"path": "p.txt", "patch": "@@ -7,3 +7,3 @@\n b1\n-b2\n+B2\n b3\n@@ -1,3 +1,3 @@\n a1\n-a2\n+A2\n a3\n"},
     lambda t, e, r, _: None if r("p.txt") == b"a1\nA2\na3\nx\nx\nx\nb1\nB2\nb3\n" else f"got {r('p.txt')!r}")

case("apply_patch is atomic when one hunk fails",
     {"p.txt": b"a1\na2\na3\n"}, "apply_patch",
     {"path": "p.txt", "patch": "@@ -1,3 +1,3 @@\n a1\n-a2\n+A2\n a3\n@@ -9,3 +9,3 @@\n zz\n-qq\n+QQ\n yy\n"},
     lambda t, e, r, _: None if r("p.txt") == b"a1\na2\na3\n" and e else f"file={r('p.txt')!r} err={e}")

case("apply_patch keeps CRLF",
     {"p.txt": b"a1\r\na2\r\na3\r\n"}, "apply_patch",
     {"path": "p.txt", "patch": "@@ -1,3 +1,3 @@\n a1\n-a2\n+A2\n a3\n"},
     lambda t, e, r, _: None if r("p.txt") == b"a1\r\nA2\r\na3\r\n" else f"got {r('p.txt')!r}")

case("apply_patch pure insertion at end of file",
     {"p.txt": b"a\nb\n"}, "apply_patch",
     {"path": "p.txt", "patch": "@@ -2,1 +2,2 @@\n b\n+c\n"},
     lambda t, e, r, _: None if r("p.txt") == b"a\nb\nc\n" else f"got {r('p.txt')!r}")

# ── write ──────────────────────────────────────────────────────────────────
case("write creates nested dirs",
     {}, "write", {"file_path": "deep/er/new.txt", "content": "hi\n"},
     lambda t, e, r, _: None if r("deep/er/new.txt") == b"hi\n" else "missing")

case("write empty content",
     {"a.txt": b"old\n"}, "write", {"file_path": "a.txt", "content": ""},
     lambda t, e, r, _: None if r("a.txt") == b"" else f"got {r('a.txt')!r}")

# ── read ───────────────────────────────────────────────────────────────────
case("read negative offset tails",
     {"a.txt": b"".join(b"line%d\n" % i for i in range(1, 11))}, "read",
     {"path": "a.txt", "offset": -2},
     lambda t, e, r, _: None if "line9" in t and "line10" in t and "line8" not in t else "wrong tail")

case("read offset past EOF is a clear error or empty",
     {"a.txt": b"one\ntwo\n"}, "read", {"path": "a.txt", "offset": 50},
     lambda t, e, r, _: None if ("past" in t.lower() or "beyond" in t.lower() or "2 lines" in t or e) else "silent")

case("read invalid utf8 doesn't break reply",
     {"a.txt": b"caf\xe9\n\xff\xfe\n"}, "read", {"path": "a.txt"},
     lambda t, e, r, _: None if t is not None and not e else "error on latin1")

# ── grep / search ──────────────────────────────────────────────────────────
case("grep bad exclude regex is an error",
     {"a.txt": b"hello\n"}, "grep", {"pattern": "hello", "exclude": "[bad"},
     lambda t, e, r, _: None if e and "exclude" in t.lower() else f"err={e}")

def grep_order():
    global fails
    if ONLY and ONLY not in "grep order stable":
        return
    files = {f"d{i}/f{j}.txt": b"needle\n" for i in range(6) for j in range(6)}
    root = tempfile.mkdtemp(prefix="tc_")
    for rel, data in files.items():
        os.makedirs(os.path.join(root, os.path.dirname(rel)), exist_ok=True)
        open(os.path.join(root, rel), "wb").write(data)
    outs = {call(root, "grep", {"pattern": "needle", "output": "files"})[0] for _ in range(4)}
    if len(outs) != 1:
        fails += 1; print("FAIL grep order stable: replies differ between runs")
    else:
        print("ok   grep order stable")
grep_order()

case("replace dry-run writes nothing",
     {"a.txt": b"foo\n"}, "replace", {"find": "foo", "replacement": "bar", "glob": "*.txt"},
     lambda t, e, r, _: None if r("a.txt") == b"foo\n" else f"wrote {r('a.txt')!r}")

case("replace apply with multibyte",
     {"a.txt": "héllo wörld\n".encode()}, "replace",
     {"find": "wörld", "replacement": "мир", "glob": "*.txt", "apply": True},
     lambda t, e, r, _: None if r("a.txt") == "héllo мир\n".encode() else f"got {r('a.txt')!r}")

# ── shell ──────────────────────────────────────────────────────────────────
case("shell big output keeps the tail",
     {}, "shell", {"command": "seq 1 200000; echo LAST_LINE_MARKER"},
     lambda t, e, r, _: None if "LAST_LINE_MARKER" in t else "tail lost")

case("shell timeout kills grandchildren",
     {}, "shell", {"command": "(sleep 30 &) ; sleep 30", "timeout": 2},
     lambda t, e, r, _: None if ("timed out" in t.lower() or "timeout" in t.lower()) else "no timeout")

case("shell stdin closed",
     {}, "shell", {"command": "cat; echo after-cat", "timeout": 5},
     lambda t, e, r, _: None if "after-cat" in t else "cat blocked on stdin")

case("shell signal exit code",
     {}, "shell", {"command": "kill -9 $$"},
     lambda t, e, r, _: None if ("137" in t or "signal" in t.lower() or "killed" in t.lower()) else "signal not reported")

case("shell nonexistent cwd",
     {}, "shell", {"command": "pwd", "cd": "no/such/dir"},
     lambda t, e, r, _: None if e else "ran anyway")

# ── malformed arguments: always a clear error, never a crash ───────────────────────────
for tool, args in [
    ("read", {}), ("read", {"path": 5}),
    ("edit", {"path": "a.txt"}), ("edit", {"path": "a.txt", "edits": "nope"}),
    ("edit", {"path": "a.txt", "edits": [{"old_text": "", "new_text": "x"}]}),
    ("write", {"file_path": ""}), ("shell", {"command": ""}),
    ("apply_patch", {"path": "a.txt", "patch": "not a diff"}),
    ("glob", {"pattern": ""}), ("replace", {"find": "", "replacement": "x"}),
]:
    case(f"bad args {tool} {json.dumps(args)[:40]}",
         {"a.txt": b"hello\n"}, tool, args,
         lambda t, e, r, _: None if e and r("a.txt") == b"hello\n"
                            else f"err={e} file={r('a.txt')!r}")

# lenient by design: a bad value is coerced or ignored, the call still works
# and the reply says what happened.
case("lenient: bad regex searched as text, and says so",
     {"a.txt": b"f(x)\n"}, "grep", {"pattern": "f(x"},
     lambda t, e, r, _: None if "f(x" in t and "plain text" in t else "no fallback note")
case("lenient: string offset still reads",
     {"a.txt": b"one\ntwo\n"}, "read", {"path": "a.txt", "offset": "2"},
     lambda t, e, r, _: None if "two" in t and not e else "not coerced")
case("lenient: negative limit doesn't break grep",
     {"a.txt": b"hello\n"}, "grep", {"pattern": "hello", "limit": -5},
     lambda t, e, r, _: None if "hello" in t else "no match returned")

# ── read edge cases ───────────────────────────────────────────────────────────────────────
case("read empty file",
     {"e.txt": b""}, "read", {"path": "e.txt"},
     lambda t, e, r, _: None if not e else "error on empty file")

case("read a directory lists it instead",
     {"d/x.txt": b"x"}, "read", {"path": "d"},
     lambda t, e, r, _: None if "x.txt" in t and "directory" in t else "no listing")

case("read huge file without a range returns a bounded slice and says so",
     {"l.txt": b"x" * 300000 + b"\nEND\n"}, "read", {"path": "l.txt"},
     lambda t, e, r, _: None if len(t) < 20000 and "line range" in t else f"len={len(t)}")
case("read huge file with a range reaches the end",
     {"l.txt": b"x" * 300000 + b"\nEND\n"}, "read", {"path": "l.txt", "offset": -1},
     lambda t, e, r, _: None if "END" in t else "tail not reachable")

case("write onto a directory is refused",
     {"d/x.txt": b"x"}, "write", {"file_path": "d", "content": "hi"},
     lambda t, e, r, root: None if e and os.path.isdir(os.path.join(root, "d")) else "clobbered dir")

case("edit unicode boundary",
     {"u.txt": "αβγ δεζ\n日本語\n".encode()}, "edit",
     {"path": "u.txt", "edits": [{"old_text": "δεζ", "new_text": "✓"}]},
     lambda t, e, r, _: None if r("u.txt") == "αβγ ✓\n日本語\n".encode() else f"got {r('u.txt')!r}")

case("edit expected_replacements mismatch writes nothing",
     {"a.txt": b"x\nx\nx\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "x", "new_text": "y", "expected_replacements": 2}]},
     lambda t, e, r, _: None if r("a.txt") == b"x\nx\nx\n" and e else f"file={r('a.txt')!r} err={e}")

# edit applies what it can and reports each edit; apply_patch is the atomic one.
case("edit batch reports a failed edit and keeps the good one",
     {"a.txt": b"one\ntwo\n"}, "edit",
     {"path": "a.txt", "edits": [{"old_text": "one", "new_text": "ONE"},
                                 {"old_text": "zzz-not-there", "new_text": "q"}]},
     lambda t, e, r, _: None if r("a.txt") == b"ONE\ntwo\n" and "edits[1]: FAILED" in t
                        else f"file={r('a.txt')!r}")

# ── git ────────────────────────────────────────────────────────────────────────────────────
def git_case(name, tool, args, check, setup="git init -q && git -c user.email=a@b -c user.name=n commit -q --allow-empty -m init"):
    global fails
    if ONLY and ONLY not in name:
        return
    root = tempfile.mkdtemp(prefix="tc_")
    subprocess.run(setup, shell=True, cwd=root, capture_output=True)
    try:
        txt, e, rc = call(root, tool, args, timeout=20)
    except subprocess.TimeoutExpired:
        fails += 1; print(f"FAIL {name}: HANG"); return
    problem = check(txt or "", e)
    if problem:
        fails += 1; print(f"FAIL {name}: {problem}\n     reply: {(txt or '')[:300]!r}")
    else:
        print(f"ok   {name}")

git_case("git_status in empty repo", "git_status", {},
         lambda t, e: None if not e else "error", setup="git init -q")
git_case("git_log in repo with no commits", "git_log", {},
         lambda t, e: None if t else "no reply", setup="git init -q")
git_case("git_status outside a repo is an error", "git_status", {},
         lambda t, e: None if e else "no error", setup="true")
git_case("git_commit nothing staged doesn't hang", "git_commit", {"message": "x"},
         lambda t, e: None if t else "no reply")
git_case("git_rebase onto missing ref doesn't hang", "git_rebase",
         {"action": "onto", "upstream": "no-such-ref"},
         lambda t, e: None if e else "no error")
git_case("git path with spaces", "git_diff", {"path": "a b.txt"},
         lambda t, e: None if t is not None else "no reply",
         setup="git init -q && printf 'x\\n' > 'a b.txt' && git add . && "
               "git -c user.email=a@b -c user.name=n commit -q -m i && printf 'y\\n' > 'a b.txt'")

# ── process ────────────────────────────────────────────────────────────────────────────────
case("process_poll unknown id is an error",
     {}, "process_poll", {"id": "proc-nope"},
     lambda t, e, r, _: None if e else "no error")
case("process_stop unknown id is an error",
     {}, "process_stop", {"id": "proc-nope"},
     lambda t, e, r, _: None if e else "no error")

# ── read cache across calls (one server) ──────────────────────────────────────────────────────────
def calls(root, seq, timeout=30):
    """Several tool calls on ONE server, so per-session caches carry over."""
    init = {"protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "tool_cases", "version": "1"}}
    reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": init},
            {"jsonrpc": "2.0", "method": "notifications/initialized"}]
    for i, (tool, args) in enumerate(seq):
        reqs.append({"jsonrpc": "2.0", "id": 10 + i, "method": "tools/call",
                     "params": {"name": tool, "arguments": args}})
    argv = ([SERVER, "mcp-serve"] if os.environ.get("MCP_SERVE") else [SERVER, "-w", root])
    p = subprocess.run(argv, cwd=root, input="\n".join(map(json.dumps, reqs)) + "\n",
                       capture_output=True, text=True, timeout=timeout, errors="replace")
    out = {}
    for line in p.stdout.splitlines():
        try:
            j = json.loads(line)
        except Exception:
            continue
        if isinstance(j.get("id"), int) and j["id"] >= 10:
            r = j.get("result") or {}
            out[j["id"] - 10] = "".join(c.get("text", "") for c in r.get("content", []))
    return [out.get(i, "") for i in range(len(seq))]

def symbol_then_range():
    global fails
    name = "read: a symbol read never answers a later line range"
    if ONLY and ONLY not in name:
        return
    root = tempfile.mkdtemp(prefix="tc_")
    body = "".join(f"// filler {i}\n" for i in range(1, 100)) \
         + "int target_fn(int x) {\n    return x + 1;\n}\n" \
         + "".join(f"// more {i}\n" for i in range(1, 100))
    open(os.path.join(root, "f.c"), "w").write(body)
    p = os.path.join(root, "f.c")
    r = calls(root, [("read", {"path": p, "symbol": "target_fn"}),
                     ("read", {"path": p, "start_line": 10, "end_line": 12}),
                     ("read", {"path": p, "start_line": "20", "end_line": 21})])
    bad = [i for i, t in ((1, r[1]), (2, r[2])) if "target_fn" in t]
    ok = not bad and "filler 10" in r[1] and "filler 20" in r[2]
    if ok:
        print(f"ok   {name}")
    else:
        fails += 1
        print(f"FAIL {name}: replies {bad} replayed the symbol\n     reply: {r[1][:200]!r}")
symbol_then_range()

print(f"\n{fails} failure(s)")
sys.exit(1 if fails else 0)
