// SPDX-License-Identifier: Apache-2.0
//
// mcp/core.hpp — the algebraic kernel, from jsonrpc-cpp.
//
//   Unit, Maybe, List, Sum, Newtype and match are jsonrpc-cpp's, named here
//   so protocol code can write them unqualified in namespace mcp.
#pragma once

#include <jsonrpc/core.hpp>

#include <mcp/json.hpp>

namespace mcp {

using jsonrpc::Unit;
using jsonrpc::Maybe;
using jsonrpc::Just;
using jsonrpc::Nothing;
using jsonrpc::List;
using jsonrpc::Sum;
using jsonrpc::match;
using jsonrpc::StaticString;
using jsonrpc::Tag;
using jsonrpc::Newtype;

// A JSON-RPC request id as it appears on the wire (string | number).
using RpcId = Json;

}  // namespace mcp
