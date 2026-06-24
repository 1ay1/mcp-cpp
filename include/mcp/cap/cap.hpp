// SPDX-License-Identifier: Apache-2.0
//
// mcp/cap/cap.hpp — the capability layer in one include.
//
//   The capability layer sits ABOVE the wire protocol. It gives a host
//   application a single uniform abstraction — CapabilityProvider — over every
//   source of "things the agent can do":
//
//       LocalProvider         in-process C++ closures (the host's own tools)
//       StdioServerProvider   an external MCP server spawned over stdio
//       (your provider here)  HTTP/SSE MCP, RPC, a database, …
//
//   Register them into a Registry; ask the Registry for tools() and call
//   dispatch(). MCP becomes one implementation detail among many.
//
#pragma once

#include <mcp/cap/capability.hpp>
#include <mcp/cap/local.hpp>
#include <mcp/cap/registry.hpp>
#include <mcp/cap/process.hpp>
#include <mcp/cap/stdio_server.hpp>
