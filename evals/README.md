# Tool evals

A lightweight, **CI-safe** harness for the batteries-included toolset, grounded
in Anthropic's *"Writing effective tools for AI agents"* and OpenAI's o-series
function-calling guidance. It answers the two questions those guides say matter
most — and that unit tests alone can't:

1. **Do the tools produce the right result** for a realistic task? (deterministic)
2. **Does the model reach for the right tool**, with well-formed args? (agentic)

Unit tests prove a tool *works* in isolation; evals prove the tool is *usable* —
correct on real inputs, and ergonomic enough that the model picks it.

## Tier 1 — deterministic (runs anywhere, no model, no key)

`tasks/*.jsonl` — each line is one eval:

```json
{"name": "extract imports", "tool": "extract",
 "args": {"pattern": "import \\{ (\\w+) \\}", "group": 1, "unique": true, "glob": "*.ts"},
 "expect": {"contains": ["Foo", "Bar"], "not_contains": ["import"]}}
```

The runner drives the real toolset over the same `mcp-serve` JSON-RPC surface the
model uses, then checks the response against `expect` verifiers:

- `contains` / `not_contains` — substrings that must / must not appear
- `equals` — exact text match (after trim)
- `is_error` — the call must fail (bad-input tasks)
- `regex` — the output must match

### Golden-output tasks (formatting / ranking regressions)

A `contains` check can't catch a REORDERED aggregate table, a changed count
header, or a shifted outline indent — the output still "contains" the right
substrings. A golden task pins the tool's **entire output** against a stored
file so any format/ranking drift fails loudly:

```json
{"name": "golden: aggregate ranked table", "tool": "aggregate",
 "args": {"pattern": "TODO\\((\\w+)\\)", "by": "capture", "group": 1, "glob": "*.py"},
 "golden": "aggregate_todos.txt"}
```

The `golden` field names a file under `evals/golden/`. The workspace temp path
is normalised to `<WS>` so goldens are stable across runs. On drift the runner
prints the first differing line. After an INTENTIONAL output change, regenerate
and review the diff before committing:

```bash
AGENTTY_EVAL_SERVER=… python3 evals/run.py --update   # rewrites the goldens
git diff evals/golden/                                 # review, then commit
```

Run:

```bash
python3 evals/run.py                    # against the mcp_eval_server / a built mcp-serve
AGENTTY_BIN=/path/to/agentty python3 evals/run.py   # via agentty mcp-serve
```

Exit non-zero on any failure. Already wired into CTest as `tool_evals` (runs
whenever python3 is present) — the golden + substring tasks run together.

## Tier 2 — agentic (opt-in; needs a model + key)

`tasks/agentic/*.jsonl` — each is a natural-language prompt plus an assertion on
**which tools fired** and the final answer:

```json
{"name": "count TODO owners", "workspace": "fixtures/repo",
 "prompt": "How many TODOs does each author have? Use the fastest single tool.",
 "expect_tools": ["aggregate"], "reject_tools": ["grep"],
 "answer_contains": ["alice", "bob"]}
```

This is the tier Anthropic emphasises (tool *selection*, not just tool
*correctness*). It needs a live model + credentials, so it is NOT wired into
CI. Tool calls are observed via `AGENTTY_TRACE_TOOLS=1`, which makes the
headless agent loop emit one `TOOL <name> <ok|error>` line per executed tool
to stderr (the stdout answer stays clean). Run it deliberately:

```bash
AGENTTY_BIN=/path/to/agentty python3 evals/run_agentic.py
```

This loop already earned its keep: on the seed tasks it caught the model
reaching for `grep` instead of `aggregate` on a "count per author" task —
front-loading aggregate's "how many X per Y" use case in its description
flipped the selection (measure → fix → re-measure, exactly the workflow
Anthropic prescribes). All three seed tasks now pass.

## Why this shape

- **No LLM in Tier 1** → deterministic, fast, CI-friendly; regressions in tool
  behaviour (a broken slice, a dropped location tag) fail the build.
- **Real JSON-RPC surface**, not internal C++ calls → catches schema/dispatch
  bugs the unit tests (which call `run_*` directly) can miss.
- **Tasks are data** → adding coverage is a JSONL line, not a C++ recompile;
  the file doubles as executable documentation of each tool's contract.
