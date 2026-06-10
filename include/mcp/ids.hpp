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
//==============================================================================
inline constexpr std::string_view kProtocolVersion = "2025-11-25";

} // namespace mcp
