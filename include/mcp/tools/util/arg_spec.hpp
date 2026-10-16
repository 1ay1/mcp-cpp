// SPDX-License-Identifier: Apache-2.0
#pragma once
// A tool's arguments, declared once.
//
//   struct ListDirArgs { std::string path = "."; bool recursive = false; int max_depth = 3; };
//   inline const ArgSpec<ListDirArgs> kListDirArgs{
//       arg(&ListDirArgs::path,      "path",      "Directory to list (default: cwd)"),
//       arg(&ListDirArgs::recursive, "recursive", "List recursively"),
//       arg(&ListDirArgs::max_depth, "max_depth", "Max depth").range(1, 16),
//   };
//   kListDirArgs.schema()   -> the JSON schema advertised to the model
//   kListDirArgs.decode(j)  -> expected<ListDirArgs, ToolError>
//
// Both come from the same declaration, so the schema a model sees and the
// parser that reads its call can't disagree. Defaults are the struct's own
// member initialisers. Decoding is lenient the way ArgReader is (aliases a
// weak model uses, "42" for 42, a string array joined for a string), because
// the alternative is failing calls a human would read fine.

#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp::tools::util {

template <class T>
concept ArgValue =
    std::same_as<T, std::string> || std::same_as<T, int> || std::same_as<T, bool>
    || std::same_as<T, std::optional<std::string>> || std::same_as<T, std::optional<int>>
    || std::same_as<T, std::vector<std::string>> || std::same_as<T, nlohmann::json>;

/// One argument: where it lands, its key, and what the model is told.
template <class A, ArgValue V>
class Arg {
public:
    Arg(V A::*member, std::string key, std::string description)
        : member_(member), key_(std::move(key)), desc_(std::move(description)) {}

    /// The call fails without it (and an empty string counts as missing).
    /// `why` replaces the default "<key> is required" message; `{keys}` in it
    /// expands to the keys the call did send, so a model can tell a typo in
    /// the key from arguments that never arrived.
    Arg& required(std::string why = {}) { required_ = true; why_ = std::move(why); return *this; }
    /// The key must be present, but an empty string is a real value (a
    /// `replacement` of "" deletes the matches).
    Arg& required_present(std::string why = {}) requires std::same_as<V, std::string> {
        required_ = true; empty_ok_ = true; why_ = std::move(why); return *this;
    }
    /// Listed as required in the schema, but not enforced by the decoder: the
    /// tool's check recovers it some other way (a body under another key, a
    /// path folded out of a `paths` list) and words its own error.
    Arg& advertise_required() { advertised_ = true; return *this; }
    /// For int: clamp into [lo, hi] and advertise the bounds.
    Arg& range(int lo, int hi) requires (std::same_as<V, int> || std::same_as<V, std::optional<int>>) {
        lo_ = lo; hi_ = hi; return *this;
    }
    /// For string: only these values; anything else is an invalid_args error.
    Arg& one_of(std::vector<std::string> values)
        requires (std::same_as<V, std::string> || std::same_as<V, std::optional<std::string>>) {
        enum_ = std::move(values); return *this;
    }
    /// For string: advertise these values, but let anything else through
    /// (the tool falls back to its default for an unknown one).
    Arg& suggest(std::vector<std::string> values)
        requires (std::same_as<V, std::string> || std::same_as<V, std::optional<std::string>>) {
        enum_ = std::move(values); lenient_enum_ = true; return *this;
    }
    /// The schema of a structured (json) argument: an array of edits, etc.
    Arg& shape(nlohmann::json schema) requires std::same_as<V, nlohmann::json> {
        shape_ = std::move(schema); return *this;
    }

    [[nodiscard]] const std::string& key() const { return key_; }
    [[nodiscard]] bool is_required() const { return required_ || advertised_; }

    void describe(nlohmann::json& props, const A& defaults) const {
        nlohmann::json p = shape_.is_object() ? shape_ : nlohmann::json::object();
        if (!shape_.is_object()) p["type"] = type_name();
        if constexpr (std::same_as<V, std::vector<std::string>>)
            if (!shape_.is_object()) p["items"] = {{"type", "string"}};
        if (!desc_.empty()) p["description"] = desc_;
        if (!enum_.empty()) p["enum"] = enum_;
        if (lo_) p["minimum"] = *lo_;
        if (hi_) p["maximum"] = *hi_;
        if (!required_) {
            const V& d = defaults.*member_;
            if constexpr (std::same_as<V, bool> || std::same_as<V, int>) p["default"] = d;
            else if constexpr (std::same_as<V, std::string>) { if (!d.empty()) p["default"] = d; }
        }
        props[key_] = std::move(p);
    }

    [[nodiscard]] std::optional<ToolError> read(const ArgReader& r, A& out) const {
        V& dst = out.*member_;
        const auto missing = [&] { return ToolError::invalid_args(missing_text(r)); };
        if constexpr (std::same_as<V, std::string>) {
            if (required_ && empty_ok_) {
                const auto* v = r.raw(key_);
                if (!v || v->is_null()) return missing();
                dst = r.str(key_, "");
            } else if (required_) {
                auto v = r.require_str(key_);
                if (!v) return missing();
                dst = std::move(*v);
            } else {
                dst = r.str(key_, dst);
            }
            return check_enum(dst);
        } else if constexpr (std::same_as<V, std::optional<std::string>>) {
            if (auto v = r.require_str(key_)) dst = std::move(*v);
            else if (required_) return missing();
            if (dst) return check_enum(*dst);
            return std::nullopt;
        } else if constexpr (std::same_as<V, int>) {
            if (required_ && !r.has(key_)) return missing();
            dst = clamp(r.integer(key_, dst));
            return std::nullopt;
        } else if constexpr (std::same_as<V, std::optional<int>>) {
            if (r.has(key_)) dst = clamp(r.integer(key_, 0));
            else if (required_) return missing();
            return std::nullopt;
        } else if constexpr (std::same_as<V, bool>) {
            if (required_ && !r.has(key_)) return missing();
            dst = r.boolean(key_, dst);
            return std::nullopt;
        } else if constexpr (std::same_as<V, std::vector<std::string>>) {
            const auto* v = r.raw(key_);
            if (!v || v->is_null()) {
                if (required_) return missing();
                return std::nullopt;
            }
            dst.clear();
            // A list of strings: drop non-strings and empties (a model's
            // trailing "" or a stray null shouldn't become an argument).
            if (v->is_array()) {
                for (const auto& e : *v)
                    if (e.is_string() && !e.get_ref<const std::string&>().empty())
                        dst.push_back(e.get<std::string>());
            } else if (v->is_string()) {
                if (!v->get_ref<const std::string&>().empty())
                    dst.push_back(v->get<std::string>());   // one value where a list was expected
            }
            if (required_ && dst.empty()) return missing();
            return std::nullopt;
        } else {   // nlohmann::json: passed through; the tool validates its shape
            const auto* v = r.raw(key_);
            if (!v || v->is_null()) {
                if (required_) return missing();
                return std::nullopt;
            }
            dst = *v;
            // Models sometimes send an array/object as a JSON string
            // ("edits": "[{...}]"). Parse it, like zed does.
            if (v->is_string()) {
                const auto& s = v->get_ref<const std::string&>();
                const auto first = s.find_first_not_of(" \t\r\n");
                if (first != std::string::npos && (s[first] == '[' || s[first] == '{')) {
                    auto parsed = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
                    if (!parsed.is_discarded()) dst = std::move(parsed);
                }
            }
            return std::nullopt;
        }
    }

private:
    std::string missing_text(const ArgReader& r) const {
        if (why_.empty()) return key_ + " is required";
        std::string out = why_;
        if (auto at = out.find("{keys}"); at != std::string::npos)
            out.replace(at, 6, r.received_keys());
        return out;
    }
    static constexpr const char* type_name() {
        if constexpr (std::same_as<V, std::string> || std::same_as<V, std::optional<std::string>>) return "string";
        else if constexpr (std::same_as<V, int> || std::same_as<V, std::optional<int>>) return "integer";
        else if constexpr (std::same_as<V, bool>) return "boolean";
        else if constexpr (std::same_as<V, std::vector<std::string>>) return "array";
        else return "object";
    }
    int clamp(int v) const {
        return std::clamp(v, lo_.value_or(INT_MIN), hi_.value_or(INT_MAX));
    }
    std::optional<ToolError> check_enum(const std::string& v) const {
        if (lenient_enum_ || enum_.empty()
            || std::find(enum_.begin(), enum_.end(), v) != enum_.end())
            return std::nullopt;
        std::string allowed;
        for (const auto& e : enum_) allowed += (allowed.empty() ? "" : ", ") + e;
        return ToolError::invalid_args(key_ + " must be one of: " + allowed + " (got '" + v + "')");
    }

    V A::*                     member_;
    std::string                key_;
    std::string                desc_;
    std::string                why_;
    bool                       required_ = false;
    bool                       empty_ok_ = false;
    bool                       advertised_ = false;
    std::optional<int>         lo_, hi_;
    std::vector<std::string>   enum_;
    bool                       lenient_enum_ = false;
    nlohmann::json             shape_;
};

template <class A, ArgValue V>
[[nodiscard]] Arg<A, V> arg(V A::*member, std::string key, std::string description = {}) {
    return Arg<A, V>(member, std::move(key), std::move(description));
}

/// Every argument of one tool. A must be default-constructible: its member
/// initialisers are the defaults.
template <class A>
    requires std::default_initializable<A>
class ArgSpec {
public:
    template <class... V>
    explicit ArgSpec(Arg<A, V>... args) {
        (add(std::move(args)), ...);
    }

    /// A pass over the raw call before the fields are read, for tools that
    /// fold shapes a model sends (`file:10-20`, a `paths` list) into the
    /// declared keys. Returns the object to decode.
    using Tidy = nlohmann::json (*)(const nlohmann::json&, const Bounds&);
    ArgSpec& tidy(Tidy t) { tidy_ = t; return *this; }
    [[nodiscard]] bool has_tidy() const noexcept { return tidy_ != nullptr; }

    /// Keep the (tidied) input object in `member`, for a check that needs to
    /// know which keys arrived, not just their values.
    ArgSpec& raw(nlohmann::json A::* member) { raw_ = member; return *this; }

    /// Decode a call's arguments. Lenient on shape, strict on what's required.
    /// `b` is the call's bounds, for a tidy pass that resolves paths.
    [[nodiscard]] std::expected<A, ToolError> decode(const nlohmann::json& j,
                                                     const Bounds* b = nullptr) const {
        A out{};
        nlohmann::json tidied;
        if (tidy_ && b) tidied = tidy_(j, *b);
        const nlohmann::json& in = (tidy_ && b) ? tidied : j;
        ArgReader r(in);
        for (const auto& f : readers_)
            if (auto err = f(r, out)) return std::unexpected(std::move(*err));
        if (raw_) out.*raw_ = in;
        return out;
    }

    /// The JSON schema for the tool's inputSchema.
    [[nodiscard]] nlohmann::json schema() const {
        const A defaults{};
        nlohmann::json props = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();
        for (const auto& d : describers_) d(props, defaults);
        for (const auto& k : required_) required.push_back(k);
        nlohmann::json s{{"type", "object"}, {"properties", std::move(props)}};
        if (!required.empty()) s["required"] = std::move(required);
        return s;
    }

private:
    template <class V>
    void add(Arg<A, V> a) {
        if (a.is_required()) required_.push_back(a.key());
        describers_.push_back([a](nlohmann::json& props, const A& d) { a.describe(props, d); });
        readers_.push_back([a](const ArgReader& r, A& out) { return a.read(r, out); });
    }

    std::vector<std::function<void(nlohmann::json&, const A&)>>                      describers_;
    std::vector<std::function<std::optional<ToolError>(const ArgReader&, A&)>>       readers_;
    std::vector<std::string>                                                         required_;
    Tidy                                                                             tidy_ = nullptr;
    nlohmann::json A::*                                                              raw_ = nullptr;
};

}  // namespace mcp::tools::util
