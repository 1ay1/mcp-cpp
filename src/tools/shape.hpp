// SPDX-License-Identifier: Apache-2.0
//
// shape.hpp — the code-shape matcher behind search_structural and
// rewrite_structural, from text alone (no grammar). A language-agnostic
// lexer folds a file into a bracket tree; a pattern with metavariables is
// matched against it. Comment and string bytes become their own tokens, so a
// pattern never matches inside them.
//
//   $X      one node: a token, or a whole balanced (…) […] {…} group
//   $$$X    zero or more sibling nodes
//   $_ $$$  the same, without binding
//
// A name bound twice must capture the same text ($A == $A matches x == x).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::tools::detail::shape {

// Comment and string rules differ per family; Unknown isn't scanned.
enum class Lang { CFamily, JsLike, Go, Rust, Python, Shell, Ruby, Lua, Unknown };

[[nodiscard]] Lang lang_of(const std::filesystem::path& p);
[[nodiscard]] bool is_source_ext(const std::filesystem::path& p);

enum class Tok : std::uint8_t { Ident, Number, String, Char, Comment, Punct };

struct Token {
    Tok         kind;
    std::string text;    // exact bytes; literals keep their quotes
    int         line;    // 1-based line where the token starts
    std::size_t pos = 0; // byte offset of its first byte
};

[[nodiscard]] std::vector<Token> tokenize(std::string_view s, Lang lang);

enum class MetaKind { None, One, Many };   // $X / $$$X

// Cheap byte-level gate: false when some literal the pattern needs is absent.
[[nodiscard]] bool passes_prefilter(std::string_view bytes, const std::vector<std::string>& gates);

enum class NodeKind : std::uint8_t { Atom, Group };

// A token (Atom) or a bracketed run (Group: opener + children).
struct Node {
    NodeKind          kind;
    Token             tok;          // Atom: the token. Group: its opener.
    char              open = 0;
    std::vector<Node> kids;
    int               line = 0;
    int               end_line = 0;
    std::size_t       beg = 0;      // byte span [beg, end) in the file
    std::size_t       end = 0;
};

[[nodiscard]] std::vector<Node> build_tree(const std::vector<Token>& toks);

// A pattern node: the same tree, but an Atom may be a metavariable.
struct PNode {
    NodeKind           kind;
    Token              tok;
    char               open = 0;
    MetaKind           meta = MetaKind::None;
    std::string        bind;
    std::vector<PNode> kids;
};

[[nodiscard]] std::vector<PNode> compile_pattern_tree(std::string_view pattern, Lang lang);

// A metavariable capture: `norm` (whitespace-normalised, for $X == $X) and
// the verbatim byte span (for rewrite). beg == end is an empty $$$ capture.
struct Bound {
    std::string norm;
    std::size_t beg = 0, end = 0;
};
using Binds = std::unordered_map<std::string, Bound>;

struct Hit {
    int         line = 0;
    std::size_t beg  = 0, end = 0;   // byte span of the whole match
    Binds       binds;
};

// Every match in the file, outermost first, one per start byte.
[[nodiscard]] std::vector<Hit> match_file(const std::vector<PNode>& pat,
                                          const std::vector<Node>& tree);

}  // namespace mcp::tools::detail::shape
