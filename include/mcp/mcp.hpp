// SPDX-License-Identifier: Apache-2.0
//
// mcp/mcp.hpp — the one header to include. Pulls in the full type system, the
// JSON-RPC engine, transports, and the typed Client/Server peer surfaces.
//
#pragma once

#include <mcp/version.hpp>
#include <mcp/core.hpp>
#include <mcp/codec.hpp>
#include <mcp/ids.hpp>
#include <mcp/content.hpp>
#include <mcp/types.hpp>
#include <mcp/elicit.hpp>
#include <mcp/methods.hpp>
#include <mcp/rpc.hpp>
#include <mcp/protocol.hpp>
#include <mcp/stdio.hpp>
#include <mcp/client.hpp>
#include <mcp/server.hpp>

// The capability layer (a uniform CapabilityProvider abstraction over local
// closures, spawned MCP servers, and future transports). Sits above the wire
// protocol; opt-in via <mcp/cap/cap.hpp> too.
#include <mcp/cap/cap.hpp>
