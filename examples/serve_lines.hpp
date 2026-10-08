// examples/serve_lines.hpp — serve an mcp::Server over stdin/stdout, one
// line at a time, on the calling thread.
//
// mcp-cpp has no transport or thread of its own; a real host drives the
// engine however it likes. This is the smallest such host: read a line, step
// the engine, write what comes back, until stdin closes. Calls are answered
// in order, so a slow tool holds up the next request; fine for an example.
#pragma once

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

namespace mcp::example {

inline int serve_lines(const Server& server, std::istream& in = std::cin,
                       std::ostream& out = std::cout) {
    Engine engine;
    std::string line;
    auto send = [&](const std::string& frame) { out << frame << '\n' << std::flush; };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        Effects fx = step(engine, Received{line});
        for (auto& f : fx.send) send(f);
        for (auto& c : fx.calls) send(server.handle(c));
        for (auto& n : fx.notifications) server.handle(n);
    }
    return 0;
}

}  // namespace mcp::example
