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

Run:

```bash
python3 evals/run.py                    # against a built ./build*/… mcp-serve
AGENTTY_BIN=/path/to/agentty python3 evals/run.py   # via agentty mcp-serve
```

Exit non-zero on any failure — wire it into CI next to the unit tests.

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
*correctness*). It needs a live model and a way to observe tool calls. The
observation hook is the **remaining wiring**: `agentty run` currently prints only
the final answer, so a `--trace-tools` flag (or parsing `AGENTTY_DEBUG_API`
events) is required before `run_agentic.py` can assert on `expect_tools`. Until
then this tier is a documented design + fixtures, not yet executable.

## Why this shape

- **No LLM in Tier 1** → deterministic, fast, CI-friendly; regressions in tool
  behaviour (a broken slice, a dropped location tag) fail the build.
- **Real JSON-RPC surface**, not internal C++ calls → catches schema/dispatch
  bugs the unit tests (which call `run_*` directly) can miss.
- **Tasks are data** → adding coverage is a JSONL line, not a C++ recompile;
  the file doubles as executable documentation of each tool's contract.
