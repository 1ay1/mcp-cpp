# mcp-cpp

A modern, **type-theoretic** C++23 implementation of the
[Model Context Protocol](https://modelcontextprotocol.io/specification/2025-11-25)
(revision **2025-11-25**). Header-only, single dependency (nlohmann/json),
no codegen.

The protocol is encoded *in the type system*: every wire message is a closed
expression in a small algebra of constructors, and (de)serialisation is a
**fold over that expression** — there is not a single hand-written
`to_json`/`from_json` at any call site.

```
T  ⇄  Json          A Codec<T> witnesses a partial isomorphism with the law
encode   decode         decode ∘ encode = id_T
```

## Why type-theoretic?

The MCP schema (`schema.ts`) is a family of inductive types built from a handful
of constructors. We give those constructors names and compose them:

| Schema construct            | Algebra                       | Combinator           |
|-----------------------------|-------------------------------|----------------------|
| `interface { … }` (record)  | product  `Π fᵢ : Tᵢ`          | `record(field…)`     |
| `A \| B` tagged by `"type"` | coproduct `Σ Aᵢ`              | `sum_tagged(key, …)` |
| `A \| B` by shape           | structural union              | `variant_codec(…)`   |
| `"a" \| "b" \| "c"`         | closed enum                   | `enum_codec(…)`      |
| `T[]`                       | `List` functor                | `list_codec`         |
| `T?`                        | `Maybe ≅ T + 1`               | `maybe_codec`        |
| opaque string id            | `Newtype<Tag, string>`        | `newtype_codec`      |

A representative type — the elicitation field schema union (SEP-1330) — reads as
a single declarative expression:

```cpp
using PrimitiveSchema = Sum<
    UntitledSingleSelectEnum, TitledSingleSelectEnum,
    UntitledMultiSelectEnum,  TitledMultiSelectEnum,
    NumberSchema, BooleanSchema, StringSchema>;

template <> struct CodecOf<PrimitiveSchema> {
    static Codec<PrimitiveSchema> get() {
        return variant_codec<PrimitiveSchema>(
            codec<UntitledSingleSelectEnum>(), codec<TitledSingleSelectEnum>(),
            codec<UntitledMultiSelectEnum>(),  codec<TitledMultiSelectEnum>(),
            codec<NumberSchema>(), codec<BooleanSchema>(), codec<StringSchema>());
    }
};
```

The architecture is borrowed from
[`acp-cpp`](https://github.com/1ay1/acp-cpp) (the JSON-RPC kernel — `core`,
`codec`, `coro`, `rpc`, `stdio`) and rebuilt for MCP's message set.

## Coverage (2025-11-25)

Every `export interface` / `export type` in the authoritative
[`schema.ts`](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2025-11-25/schema.ts)
is represented and round-trips. Concretely:

- **Base protocol** — JSON-RPC 2.0 envelopes, `initialize` handshake +
  capability negotiation, `ping`, cancellation, progress, pagination, `_meta`.
- **Server features** — tools (input/output schemas, annotations, `execution`
  task-support), resources + resource templates + subscribe/updated, prompts,
  completion (autocomplete), logging.
- **Client features** — sampling (`createMessage` with `tools`/`toolChoice`,
  SEP-1577), roots, **elicitation** (form + URL mode, SEP-1036) with the full
  SEP-1330 enum-schema family.
- **Content** — text / image / audio / `resource_link` / embedded resource;
  sampling adds `tool_use` / `tool_result`.
- **Icons** (SEP-973) on tools, resources, prompts, implementation info.
- **Tasks** (durable requests, SEP-1686) — `tasks/get`, `tasks/result`,
  `tasks/cancel`, `tasks/list`, status notifications, task-augmented params.

The schema's seven closing discriminated unions (`JSONRPCMessage`,
`ClientRequest`, `ServerRequest`, `ClientNotification`, `ServerNotification`,
`ClientResult`, `ServerResult`) are modelled as real `Sum` types. On top of
them sits a **compile-time method dictionary**: each `dict::X` descriptor bakes
its wire-method literal (as an NTTP), its `Params`, and its `Result` into one
indivisible token, so

```cpp
auto fut = mcp::call<dict::CallTool>(engine, params);   // future<CallToolResult>
mcp::handle<dict::CallTool>(engine, [](const CallToolParams& p){ … });
mcp::send<dict::Progress>(engine, progressParams);
```

resolve the method string *and* the result type from a single name — a
mismatched (method, params, result) triple is unrepresentable.

## Effect-aware parallel tool scheduling

A differentiator no other MCP client ships. When a model emits a **batch** of
tool calls in one turn — and frontier models do this constantly (three `read`s,
a `grep`, a `glob`, then an `edit`) — almost every agent runtime runs them
**strictly sequentially**, because the bare wire gives no machine-readable
interference model. mcp-cpp has one: every tool declares an `EffectSet`
(Read / Write / Net / Exec) and most fs tools name their target **paths** right
in the args. From those two facts the scheduler derives a provably-safe
concurrent execution plan with **zero host annotation**:

```cpp
#include <mcp/cap/scheduler.hpp>

std::vector<cap::Request> batch = model_tool_calls();      // what the model asked for
auto results = cap::run(registry, batch,                  // 1:1 with batch, original order
                        mcp::tools::make_effect_fn());     // rich built-in effects
```

- A **wave** of pairwise non-conflicting calls runs concurrently; waves run in
  submission order, so a write is never reordered before a read the model
  intended first. The result vector comes back **1:1 with the input** — the
  agent loop is none the wiser anything ran in parallel.
- Conflict model: any `Exec` serialises against everything (a shell command's
  blast radius is unbounded); two pure reads **never** conflict; a write
  serialises only against calls whose paths **overlap** (prefix-aware, so
  writing `src/` orders against reading `src/a.c`); a write with no extractable
  path serialises against all fs peers.
- `plan_waves()` is a **pure** planner (no I/O, unit-tested in isolation);
  `run()` is a thin `std::async` executor behind the same one-call surface the
  host already uses. Works over **any** provider mix — a default `EffectFn`
  reads the standard MCP `readOnlyHint` / `openWorldHint` annotations so it does
  something safe even for a third-party server that never heard of `EffectSet`.

Four independent reads that each block 8 ms collapse from 32 ms serial to 8 ms
concurrent — and a write that touches one of those paths still serialises
exactly where it must.

## Layout

| Header               | Role                                                      |
|----------------------|----------------------------------------------------------|
| `mcp/core.hpp`       | the algebra: `Unit`, `Maybe`, `List`, `Sum`, `Newtype`   |
| `mcp/codec.hpp`      | `Codec<T>`, `record`, `sum_tagged`, `variant_codec`, …    |
| `mcp/ids.hpp`        | `RequestId`/`ProgressToken` unions, `Role`, enums         |
| `mcp/content.hpp`    | content blocks, annotations, resource contents            |
| `mcp/types.hpp`      | implementation, capabilities, tool, resource, prompt, task|
| `mcp/elicit.hpp`     | elicitation primitive/enum schemas                        |
| `mcp/methods.hpp`    | every request/result param record + `mcp::method::*`      |
| `mcp/protocol.hpp`   | JSON-RPC envelope algebra, message-level sums, method dict |
| `mcp/rpc.hpp`        | bidirectional JSON-RPC engine (sync + async + timeouts)   |
| `mcp/stdio.hpp`      | line-delimited stdio transport                            |
| `mcp/coro.hpp`       | optional `mcp::co::Task<T>` coroutine surface             |
| `mcp/client.hpp`     | typed host-side `Client`                                  |
| `mcp/server.hpp`     | typed `Server` with a tool/resource/prompt registry       |
| `mcp/cap/*.hpp`      | capability layer: `Registry` fan-in, providers, scheduler |
| `mcp/cap/scheduler.hpp` | effect-aware parallel tool scheduling (see above)      |
| `mcp/mcp.hpp`        | umbrella include                                          |

## Quick start — a server in ~10 lines

```cpp
#include <mcp/mcp.hpp>
using namespace mcp;

int main() {
    StdioTransport tx(std::cin, std::cout);
    Server server(tx.sink(), Implementation{"my-server", "1.0"});

    Tool add; add.name = "add";
    add.inputSchema.properties = Json{{"a",{{"type","integer"}}},{"b",{{"type","integer"}}}};
    server.register_tool(std::move(add), [](const Json& args) {
        CallToolResult r;
        r.content = { text("sum = " + std::to_string(args.value("a",0L)+args.value("b",0L))) };
        return r;
    });

    tx.start(server.engine());
    tx.join();
}
```

## Quick start — a client (coroutines)

```cpp
#include <mcp/mcp.hpp>
#include <mcp/coro.hpp>
using namespace mcp;
using mcp::co::Task; using mcp::co::operator co_await;

Task<void> run(Client& c) {
    auto init = co_await c.initialize(Implementation{"my-client","1.0"});
    c.initialized();
    auto tools = co_await c.list_tools();
    auto res   = co_await c.call_tool("add", Json{{"a",17},{"b",25}});
    co_return;
}
```

Or block synchronously: every call returns `std::future<T>`, so
`client.call_tool(...).get()` works without coroutines.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# end-to-end: client spawns the server over stdio
./build/examples/mcp_client_example ./build/examples/mcp_server_example
```

Requires a C++23 compiler (GCC 14+/Clang 17+). nlohmann/json v3.11.3 is fetched
automatically by CMake.

## Design notes

- **`std::expected`-free, exception-thin decode.** `CodecError` is thrown only on
  shape mismatch and is caught at the RPC boundary, mapped to `InvalidParams`.
- **One codec per type, built once.** `codec<T>()` is a Meyers singleton; nested
  codecs share cached nodes, so encode/decode are plain function-pointer calls.
- **`Newtype<Tag, std::string>`** gives nominal typing for opaque ids at zero
  runtime cost — you cannot pass a `Cursor` where a `TaskId` is expected.
- **Async handlers** (`on_*_async`) hand a one-shot `Responder` to a worker so a
  slow tool call or sampling request never blocks the single reader thread.
- The coroutine `Task<T>` lives in `mcp::co` to avoid colliding with the
  protocol's `mcp::Task` (a durable-request record).

## License

Apache-2.0.
