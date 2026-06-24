// SPDX-License-Identifier: Apache-2.0
//
// mcp/tools/util/progress.hpp — host-wireable live-progress sink for the
// subprocess runners. The bash/diagnostics/git tools stream captured
// stdout to the host UI as it arrives; rather than couple the util layer
// to any host's event system, the host installs a thread-local Sink that
// the runners call. No sink ⇒ a cheap no-op (idle tools never allocate).
//
// Mirrors the agentty tools::progress seam verbatim, so a host wires it in
// one line:  mcp::tools::util::progress::set([](auto s){ my_emit(s); });

#pragma once

#include <functional>
#include <string_view>

namespace mcp::tools::util::progress {

using Sink = std::function<void(std::string_view snapshot)>;

// Install the thread-local sink (call on the worker thread the tool runs on).
void set(Sink s);
void clear();

// No-op when no sink is installed — cheap enough to call per pipe read.
void emit(std::string_view snapshot);

// RAII guard: `set` on construction, `clear` on destruction.
struct Scope {
    explicit Scope(Sink s) { set(std::move(s)); }
    ~Scope()               { clear(); }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace mcp::tools::util::progress
