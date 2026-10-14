// arg_spec_test.cpp — a tool's schema and its decoder come from one declaration.

#include <mcp/tools/util/arg_spec.hpp>

#include "agtest.hpp"

#include <optional>
#include <string>
#include <vector>

using mcp::tools::util::arg;
using mcp::tools::util::ArgSpec;
using nlohmann::json;

namespace {
struct Args {
    std::string path = ".";
    std::string mode = "fast";
    int         depth = 3;
    bool        all = false;
    std::optional<int> limit;
    std::vector<std::string> tags;
};
const ArgSpec<Args> kSpec{
    arg(&Args::path,  "path",  "Where").required(),
    arg(&Args::mode,  "mode",  "How").one_of({"fast", "slow"}),
    arg(&Args::depth, "depth", "How deep").range(1, 8),
    arg(&Args::all,   "all",   "Everything"),
    arg(&Args::limit, "limit", "Cap"),
    arg(&Args::tags,  "tags",  "Labels"),
};
}  // namespace

TEST_CASE("arg_spec: the schema says what the decoder does") {
    const json s = kSpec.schema();
    CHECK(s["type"] == "object");
    CHECK(s["required"] == json::array({"path"}));
    CHECK(s["properties"]["path"]["type"] == "string");
    CHECK(s["properties"]["mode"]["enum"] == json::array({"fast", "slow"}));
    CHECK(s["properties"]["mode"]["default"] == "fast");      // the struct's own default
    CHECK(s["properties"]["depth"]["minimum"] == 1);
    CHECK(s["properties"]["depth"]["maximum"] == 8);
    CHECK(s["properties"]["depth"]["default"] == 3);
    CHECK(s["properties"]["all"]["type"] == "boolean");
    CHECK(s["properties"]["tags"]["type"] == "array");
}

TEST_CASE("arg_spec: decode applies defaults, bounds and requirements") {
    auto a = kSpec.decode(json{{"path", "src"}});
    REQUIRE(a.has_value());
    CHECK(a->path == "src");
    CHECK(a->mode == "fast");
    CHECK(a->depth == 3);
    CHECK(!a->all);
    CHECK(!a->limit.has_value());

    auto clamped = kSpec.decode(json{{"path", "x"}, {"depth", 99}, {"limit", "7"}});
    REQUIRE(clamped.has_value());
    CHECK(clamped->depth == 8);              // clamped into the advertised range
    CHECK(clamped->limit == 7);              // "7" read as 7, the lenient way

    CHECK(!kSpec.decode(json::object()).has_value());                       // required
    CHECK(!kSpec.decode(json{{"path", ""}}).has_value());                   // empty = missing
    CHECK(!kSpec.decode(json{{"path", "x"}, {"mode", "warp"}}).has_value()); // not in the enum

    // A model's synonym still lands: file_path is an alias of path.
    auto alias = kSpec.decode(json{{"file_path", "a.txt"}});
    REQUIRE(alias.has_value());
    CHECK(alias->path == "a.txt");

    // One string where a list was expected becomes a one-element list.
    auto one = kSpec.decode(json{{"path", "x"}, {"tags", "solo"}});
    REQUIRE(one.has_value());
    CHECK(one->tags == std::vector<std::string>{"solo"});
}
