# date-mcp — a C++ MCP server example (built with mcp-cpp)

An MCP server in ~130 lines of C++, built with [mcp-cpp](../..). It gives an
MCP client (like [agentty](https://agentty.org)) something the model can't know
on its own — the current date/time — plus a small date-math helper.

Tools:

| tool | what it does |
|------|--------------|
| `current_date` | today's date & time (UTC + local + weekday) |
| `days_between` | signed whole days from date `a` to date `b` (YYYY-MM-DD) |

## Build

It builds automatically with the mcp-cpp examples (from the mcp-cpp root:
`cmake -B build && cmake --build build --target date_server` →
`build/examples/date-mcp/date_server`).

To build it **standalone** (copied out of the repo), the `CMakeLists.txt` here
fetches mcp-cpp via `FetchContent` — so from this directory:

```bash
cmake -B build
cmake --build build -j8 --target date_server
# → build/date_server
```

(No external deps — mcp-cpp is header-only.)

## Add it to agentty

Point agentty at the built binary (use its absolute path):

```bash
agentty plugin add date -- "/abs/path/to/date_server"
```

That writes a `date` server into `~/.agentty/mcp.json`. It connects immediately
(no restart). Its tools appear to the model as `mcp__date__current_date` and
`mcp__date__days_between`. You can also add it from the TUI: **Ctrl+K → Plugins
→ `a`**, then type `date -- /abs/path/to/date_server`.

Then ask agentty *"what day is it?"* or *"how many days until 2026-12-25?"* and
watch it call the tool. Full walkthrough: **[agentty.org/docs/plugins](https://agentty.org/docs/plugins)**
and **[agentty.org/docs/build-a-plugin](https://agentty.org/docs/build-a-plugin)**.

## Verify without a client

MCP is newline-delimited JSON-RPC on stdio, so you can drive it by hand:

```bash
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cli","version":"1"}}}' \
'{"jsonrpc":"2.0","method":"notifications/initialized"}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"current_date","arguments":{}}}' \
| ./build/date_server
```

## Use it as a template

`date_server.cpp` is the whole thing. To make your own: copy it,
`register_tool(spec, lambda)` for each tool (the `description` is what the model
reads — write it for the model; the `inputSchema` is its typed arguments), and
keep `transport.start(server.engine()); transport.join();` at the end to serve
until the client closes stdin.

Prefer a scripting language? See the sibling [`wordcount-mcp`](../wordcount-mcp)
— a stdlib-only Python server with no build step.
