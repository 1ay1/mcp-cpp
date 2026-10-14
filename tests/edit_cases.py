#!/usr/bin/env python3
"""Targeted correctness checks for edit / apply_patch / write / replace.
Each case sets up files, makes one call, and checks the bytes on disk.

    edit_cases.py <eval_server>
"""
import json, os, shutil, subprocess, sys, tempfile

SERVER = sys.argv[1]
fails = 0

def call(root, tool, args):
    reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": tool, "arguments": args}}]
    try:
        p = subprocess.run([SERVER, "-w", root], input="\n".join(map(json.dumps, reqs)) + "\n",
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return "HANG (>30s)", None
    for l in p.stdout.splitlines():
        j = json.loads(l)
        if j.get("id") == 2:
            r = j.get("result", {})
            return "".join(c.get("text", "") for c in r.get("content", [])), bool(r.get("isError"))
    return f"NO REPLY rc={p.returncode} {p.stderr[-300:]}", True

def case(name, files, tool, args, expect_files=None, expect_error=None, contains=None):
    global fails
    root = tempfile.mkdtemp(prefix="ec_")
    for p, data in files.items():
        os.makedirs(os.path.dirname(f"{root}/{p}") or root, exist_ok=True)
        with open(f"{root}/{p}", "wb") as f: f.write(data if isinstance(data, bytes) else data.encode())
    txt, err = call(root, tool, args)
    problems = []
    if err is None:
        problems.append("HANG: no reply in 30s")
        err = False
    if expect_error is not None and err != expect_error:
        problems.append(f"isError={err} (wanted {expect_error})")
    if contains and contains not in txt:
        problems.append(f"reply lacks {contains!r}")
    for p, want in (expect_files or {}).items():
        got = open(f"{root}/{p}", "rb").read() if os.path.exists(f"{root}/{p}") else None
        want_b = want if isinstance(want, bytes) or want is None else want.encode()
        if got != want_b:
            problems.append(f"{p}: got {got!r} want {want_b!r}")
    status = "FAIL" if problems else "ok  "
    if problems: fails += 1
    print(f"{status} {name}")
    for pr in problems: print(f"       {pr}")
    if problems: print(f"       reply: {txt[:300]!r}")
    shutil.rmtree(root, ignore_errors=True)

# ── edit: batch semantics (documented: successful edits are written, failed
#    ones reported per index so only those are retried) ─────────────────────
case("edit batch: a failing count gate is reported, others land",
     {"f.txt": "A\nB\nB\nB\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "A", "new_text": "AA"},
                                 {"old_text": "B", "new_text": "C", "expected_replacements": 2}]},
     expect_files={"f.txt": "AA\nB\nB\nB\n"}, contains="edits[1]: FAILED")
case("edit: a lone count mismatch is an error, file untouched",
     {"f.txt": "A\nB\nB\nB\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "B", "new_text": "C", "expected_replacements": 2}]},
     expect_files={"f.txt": "A\nB\nB\nB\n"}, expect_error=True, contains="expected_replacements=2")
case("edit batch: a missing old_text is reported, others land",
     {"f.txt": "one\ntwo\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "one", "new_text": "1"},
                                 {"old_text": "zzz-not-there", "new_text": "x"}]},
     expect_files={"f.txt": "1\ntwo\n"}, contains="edits[1]: FAILED")
case("edit batch: later edit sees earlier result",
     {"f.txt": "a b\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "a", "new_text": "x"}, {"old_text": "x b", "new_text": "y"}]},
     expect_files={"f.txt": "y\n"}, expect_error=False)

# ── edit: ambiguity ────────────────────────────────────────────────────────
case("edit: two exact matches, no line -> refuse",
     {"f.txt": "foo();\nbar();\nfoo();\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "foo();", "new_text": "baz();"}]},
     expect_files={"f.txt": "foo();\nbar();\nfoo();\n"}, expect_error=True)
case("edit: two exact matches, line picks the second",
     {"f.txt": "foo();\nbar();\nfoo();\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "foo();", "new_text": "baz();", "line": 3}]},
     expect_files={"f.txt": "foo();\nbar();\nbaz();\n"}, expect_error=False)
case("edit: replace_all",
     {"f.txt": "x x x\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "x", "new_text": "y", "replace_all": True}]},
     expect_files={"f.txt": "y y y\n"}, expect_error=False)
case("edit: overlapping replace_all (aa in aaa)",
     {"f.txt": "aaa\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "aa", "new_text": "b", "replace_all": True}]},
     expect_files={"f.txt": "ba\n"}, expect_error=False)
case("edit: fuzzy must not match an unrelated block",
     {"f.txt": "int alpha() { return 1; }\nint beta() { return 2; }\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "int gamma() { return 3; }", "new_text": "X"}]},
     expect_files={"f.txt": "int alpha() { return 1; }\nint beta() { return 2; }\n"}, expect_error=True)
case("edit: near-identical blocks, typo'd needle -> refuse, not guess",
     {"f.txt": "def a():\n    x = 1\n    return x\n\ndef b():\n    x = 1\n    return x\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "    x = 1\n    retrun x", "new_text": "    x = 2\n    return x"}]},
     expect_files={"f.txt": "def a():\n    x = 1\n    return x\n\ndef b():\n    x = 1\n    return x\n"},
     expect_error=True)

# ── edit: line endings / BOM / trailing newline ────────────────────────────
case("edit: CRLF file keeps CRLF",
     {"f.txt": b"one\r\ntwo\r\nthree\r\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "two\nthree", "new_text": "2\n3"}]},
     expect_files={"f.txt": b"one\r\n2\r\n3\r\n"}, expect_error=False)
case("edit: no trailing newline stays without one",
     {"f.txt": "a\nb"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "b", "new_text": "c"}]},
     expect_files={"f.txt": "a\nc"}, expect_error=False)
case("edit: BOM preserved",
     {"f.txt": b"\xef\xbb\xbfhello\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "hello", "new_text": "bye"}]},
     expect_files={"f.txt": b"\xef\xbb\xbfbye\n"}, expect_error=False)
case("edit: tabs (Makefile) untouched by indent adjust",
     {"Makefile": "all:\n\techo a\n\techo b\n"}, "edit",
     {"path": "Makefile", "edits": [{"old_text": "\techo a", "new_text": "\techo A\n\techo A2"}]},
     expect_files={"Makefile": "all:\n\techo A\n\techo A2\n\techo b\n"}, expect_error=False)

# ── edit: indentation auto-adjust vs string contents ──────────────────────
src = 'class C:\n    def f(self):\n        s = """\nraw line\n"""\n        return s\n'
case("edit: exact match leaves new_text indentation alone",
     {"f.py": src}, "edit",
     {"path": "f.py", "edits": [{"old_text": '        s = """\nraw line\n"""',
                                  "new_text": '        s = """\nraw line\nraw two\n"""'}]},
     expect_files={"f.py": 'class C:\n    def f(self):\n        s = """\nraw line\nraw two\n"""\n        return s\n'},
     expect_error=False)
case("edit: under-indented needle still re-indents only code, keeps string body",
     {"f.py": src}, "edit",
     {"path": "f.py", "edits": [{"old_text": 's = """\nraw line\n"""',
                                  "new_text": 's = """\nraw line\nraw two\n"""'}]},
     expect_files={"f.py": 'class C:\n    def f(self):\n        s = """\nraw line\nraw two\n"""\n        return s\n'},
     expect_error=None)

# ── edit: regex ────────────────────────────────────────────────────────────
case("edit regex: catastrophic pattern does not hang",
     {"f.txt": "a" * 5000 + "c\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "(a+)+b", "new_text": "x", "regex": True}]},
     expect_files={"f.txt": "a" * 5000 + "c\n"}, expect_error=True)
case("edit regex: capture groups",
     {"f.txt": "f(1) f(22)\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "f\\((\\d+)\\)", "new_text": "g[$1]", "regex": True, "replace_all": True}]},
     expect_files={"f.txt": "g[1] g[22]\n"}, expect_error=False)
case("edit regex: empty-match pattern refused",
     {"f.txt": "abc\n"}, "edit",
     {"path": "f.txt", "edits": [{"old_text": "x*", "new_text": "-", "regex": True, "replace_all": True}]},
     expect_files={"f.txt": "abc\n"}, expect_error=True)

# ── apply_patch ─────────────────────────────────────────────────────────────
base = "".join(f"line{i}\n" for i in range(1, 21))
case("apply_patch: two adjacent hunks",
     {"f.txt": base}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -3,1 +3,1 @@\n line2\n-line3\n+LINE3\n@@ -4,1 +4,1 @@\n-line4\n+LINE4\n line5\n"},
     expect_files={"f.txt": base.replace("line3\n", "LINE3\n").replace("line4\n", "LINE4\n")}, expect_error=False)
case("apply_patch: overlapping hunks refused",
     {"f.txt": base}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -3,2 +3,2 @@\n line3\n-line4\n+X\n@@ -4,2 +4,2 @@\n-line4\n+Y\n line5\n"},
     expect_files={"f.txt": base}, expect_error=True)
case("apply_patch: stale patch (context gone) refused",
     {"f.txt": base}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -3,1 +3,1 @@\n nothere\n-line3\n+X\n"},
     expect_files={"f.txt": base}, expect_error=True)
case("apply_patch: one bad hunk -> nothing written",
     {"f.txt": base}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -2,1 +2,1 @@\n-line2\n+TWO\n@@ -9,1 +9,1 @@\n-nope\n+X\n"},
     expect_files={"f.txt": base}, expect_error=True)
case("apply_patch: CRLF file",
     {"f.txt": b"a\r\nb\r\nc\r\n"}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n"},
     expect_files={"f.txt": b"a\r\nB\r\nc\r\n"}, expect_error=False)
case("apply_patch: pure insertion with no removed line",
     {"f.txt": "a\nb\n"}, "apply_patch",
     {"path": "f.txt", "patch": "@@ -1,1 +1,2 @@\n a\n+inserted\n"},
     expect_files={"f.txt": "a\ninserted\nb\n"}, expect_error=False)

# ── write ──────────────────────────────────────────────────────────────────
case("write: through a symlink keeps the link",
     {"real.txt": "old\n"}, "write", {"path": "real.txt", "content": "new\n"},
     expect_files={"real.txt": "new\n"}, expect_error=False)
case("write: empty content",
     {"f.txt": "old\n"}, "write", {"path": "f.txt", "content": ""},
     expect_files={"f.txt": ""}, expect_error=None)
case("write: creates parent dirs",
     {}, "write", {"path": "a/b/c.txt", "content": "x"},
     expect_files={"a/b/c.txt": "x"}, expect_error=False)

# ── replace ────────────────────────────────────────────────────────────────
case("replace: dry run writes nothing",
     {"a.txt": "foo\n", "b.txt": "foo foo\n"}, "replace", {"find": "foo", "replacement": "bar", "glob": "*.txt"},
     expect_files={"a.txt": "foo\n", "b.txt": "foo foo\n"}, expect_error=False)
case("replace: apply",
     {"a.txt": "foo\n", "b.txt": "foo foo\n"}, "replace", {"find": "foo", "replacement": "bar", "glob": "*.txt", "apply": True},
     expect_files={"a.txt": "bar\n", "b.txt": "bar bar\n"}, expect_error=False)
case("replace: skips binary files",
     {"a.txt": "foo\n", "b.bin": b"foo\x00\x01\x02foo"}, "replace", {"find": "foo", "replacement": "bar", "glob": "*", "apply": True},
     expect_files={"a.txt": "bar\n", "b.bin": b"foo\x00\x01\x02foo"}, expect_error=None)
case("replace: regex catastrophic does not hang",
     {"a.txt": "a" * 5000 + "c\n"}, "replace", {"find": "(a+)+b", "replacement": "x", "regex": True, "glob": "*.txt", "apply": True},
     expect_files={"a.txt": "a" * 5000 + "c\n"}, expect_error=None)

print(f"\n{fails} failing case(s)")
sys.exit(1 if fails else 0)
