// SPDX-License-Identifier: Apache-2.0
//
// mcp/ids.hpp — scalar identifier types & closed enumerations.
//
//   The MCP schema has several "string | number" unions (RequestId,
//   ProgressToken) and pure-string opaque ids (Cursor, task ids). We model:
//
//     • string|number unions  →  Sum<std::string, std::int64_t> with a
//                                 structural variant_codec
//     • opaque string ids     →  Newtype<Tag, std::string> (nominal typing)
//     • closed string enums   →  scoped enum + enum_codec
//
#pragma once

#include <mcp/codec.hpp>

namespace mcp {

//==============================================================================
//  RequestId  =  string | number          (JSON-RPC 2.0 / schema.ts)
//  ProgressToken = string | number
//==============================================================================
using Scalar = Sum<std::string, std::int64_t>;

inline Codec<Scalar> scalar_codec() {
    return variant_codec<Scalar>(codec<std::string>(), codec<std::int64_t>());
}

using RequestId     = Scalar;
using ProgressToken = Scalar;

template <> struct CodecOf<Scalar> { static Codec<Scalar> get() { return scalar_codec(); } };

//==============================================================================
//  Opaque string ids.
//==============================================================================
struct CursorTag;        using Cursor        = Newtype<CursorTag,        std::string>;
struct TaskIdTag;        using TaskId        = Newtype<TaskIdTag,        std::string>;
struct ElicitationIdTag; using ElicitationId = Newtype<ElicitationIdTag, std::string>;

//==============================================================================
//  Role  =  "user" | "assistant"
//==============================================================================
enum class Role { User, Assistant };
template <> struct CodecOf<Role> {
    static Codec<Role> get() {
        return enum_codec<Role>(
            EnumMapping<Role>{Role::User,      "user"},
            EnumMapping<Role>{Role::Assistant, "assistant"});
    }
};

//==============================================================================
//  LoggingLevel — RFC 5424 syslog severities (schema.ts LoggingLevel).
//==============================================================================
enum class LoggingLevel {
    Debug, Info, Notice, Warning, Error, Critical, Alert, Emergency
};
template <> struct CodecOf<LoggingLevel> {
    static Codec<LoggingLevel> get() {
        return enum_codec<LoggingLevel>(
            EnumMapping<LoggingLevel>{LoggingLevel::Debug,     "debug"},
            EnumMapping<LoggingLevel>{LoggingLevel::Info,      "info"},
            EnumMapping<LoggingLevel>{LoggingLevel::Notice,    "notice"},
            EnumMapping<LoggingLevel>{LoggingLevel::Warning,   "warning"},
            EnumMapping<LoggingLevel>{LoggingLevel::Error,     "error"},
            EnumMapping<LoggingLevel>{LoggingLevel::Critical,  "critical"},
            EnumMapping<LoggingLevel>{LoggingLevel::Alert,     "alert"},
            EnumMapping<LoggingLevel>{LoggingLevel::Emergency, "emergency"});
    }
};

//==============================================================================
//  TaskStatus — durable-request lifecycle (schema.ts TaskStatus).
//==============================================================================
enum class TaskStatus { Working, InputRequired, Completed, Failed, Cancelled };
template <> struct CodecOf<TaskStatus> {
    static Codec<TaskStatus> get() {
        return enum_codec<TaskStatus>(
            EnumMapping<TaskStatus>{TaskStatus::Working,       "working"},
            EnumMapping<TaskStatus>{TaskStatus::InputRequired, "input_required"},
            EnumMapping<TaskStatus>{TaskStatus::Completed,     "completed"},
            EnumMapping<TaskStatus>{TaskStatus::Failed,        "failed"},
            EnumMapping<TaskStatus>{TaskStatus::Cancelled,     "cancelled"});
    }
};

//==============================================================================
//  Protocol revision pinned to this build.
//
//  MCP 2026-07-28 made the protocol core STATELESS: there is no initialize
//  handshake and no protocol-level session. Instead every request carries its
//  protocol version, client identity, and client capabilities as per-request
//  `_meta` fields (the "modern" era). 2025-11-25 and earlier are "legacy"
//  (session established via an initialize handshake). This SDK is DUAL-ERA:
//  it prefers modern per-request metadata and falls back to the legacy
//  handshake for servers that only speak the older revisions.
//==============================================================================

// The newest (modern, stateless) revision this build implements + advertises.
inline constexpr std::string_view kProtocolVersion = "2026-07-28";

// The last legacy (handshake/session) revision, retained for interop with
// servers that have not yet moved to the stateless core.
inline constexpr std::string_view kLegacyProtocolVersion = "2025-11-25";

// True iff `v` is a modern (>= 2026-07-28), per-request-metadata revision.
// The date-string format is lexicographically ordered, so a plain compare
// works for the foreseeable calendar-versioned future.
inline constexpr bool is_modern_protocol(std::string_view v) noexcept {
    return v >= std::string_view{"2026-07-28"};
}

// Every revision this build can serve/consume, newest first. Advertised in a
// server/discover response and used to answer an inline version mismatch.
inline constexpr std::string_view kSupportedProtocolVersions[] = {
    "2026-07-28",
    "2025-11-25",
    "2025-06-18",
};

// Reverse-DNS `_meta` keys that carry the modern per-request protocol metadata
// (schema.ts: io.modelcontextprotocol/{protocolVersion,clientInfo,
// clientCapabilities,serverInfo}). Single source of truth for the wire keys.
namespace meta_key {
inline constexpr std::string_view ProtocolVersion    = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view ClientInfo         = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view ClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view ServerInfo         = "io.modelcontextprotocol/serverInfo";
// MRTR (Multi Round-Trip Requests): the client echoes the server's opaque
// requestState and returns the fulfilled inputResponses under these keys on
// the retry request.
inline constexpr std::string_view InputResponses     = "io.modelcontextprotocol/inputResponses";
inline constexpr std::string_view RequestState       = "io.modelcontextprotocol/requestState";
// Tasks extension (2026-07-28, SEP-2663): Tasks left the experimental core for
// the io.modelcontextprotocol/tasks extension. A task-augmented request carries
// its task metadata under this key.
inline constexpr std::string_view Tasks              = "io.modelcontextprotocol/tasks";
} // namespace meta_key

} // namespace mcp
