// SPDX-License-Identifier: Apache-2.0
//
// mcp/codec.hpp — the codec algebra, from jsonrpc-cpp.
//
//   MCP's own types get their codecs from mcp::CodecOf<T>, specialised next
//   to each type. JSONRPC_PROTOCOL_CODECS lets jsonrpc's codec<T>() find that
//   table by argument-dependent lookup; the built-in types fall through to
//   jsonrpc::CodecOf.
#pragma once

#include <jsonrpc/codec.hpp>

#include <mcp/core.hpp>

namespace mcp {

using jsonrpc::Codec;
using jsonrpc::CodecError;

template <class T> struct CodecOf : jsonrpc::CodecOf<T> {};
JSONRPC_PROTOCOL_CODECS(CodecOf)

using jsonrpc::codec;
using jsonrpc::to_json;
using jsonrpc::from_json;
using jsonrpc::list_codec;
using jsonrpc::maybe_codec;
using jsonrpc::newtype_codec;
using jsonrpc::EnumMapping;
using jsonrpc::enum_codec;
using jsonrpc::Field;
using jsonrpc::required;
using jsonrpc::optional;
using jsonrpc::defaulted;
using jsonrpc::meta;
using jsonrpc::record;
using jsonrpc::Arm;
using jsonrpc::arm;
using jsonrpc::sum_tagged;
using jsonrpc::variant_codec;

}  // namespace mcp
