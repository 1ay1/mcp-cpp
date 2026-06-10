// SPDX-License-Identifier: Apache-2.0
//
// mcp/core.hpp — the algebraic kernel.
//
//   The Model Context Protocol is, mathematically, a closed family of inductive
//   types built from a few constructors:
//
//       1            — the unit type (no information)
//       A × B        — products (records with named fields)
//       A + B        — coproducts / tagged sums
//       List A       — finite sequences
//       Maybe A      — A + 1, i.e. optional values
//       Json         — the free, untyped term language we (de)serialise into
//
//   Every spec type downstream is a closed expression in this algebra;
//   serialization is a fold over that expression, never a hand-written ritual.
//
#pragma once

#include <mcp/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

//==============================================================================
//  1 — the unit type.  Encoded as the empty JSON object {}.
//==============================================================================
struct Unit {
    friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
};

//==============================================================================
//  Maybe A  ≅  A + 1   (optionality is a sum, not a sentinel).
//==============================================================================
template <class A> using Maybe = std::optional<A>;

template <class A>
constexpr Maybe<A> Just(A x) { return Maybe<A>{std::move(x)}; }

inline constexpr std::nullopt_t Nothing = std::nullopt;

//==============================================================================
//  List A  ≅  μX. 1 + A × X      (finite sequences)
//==============================================================================
template <class A> using List = std::vector<A>;

//==============================================================================
//  RpcId — a JSON-RPC 2.0 id. Per the spec it is string OR number, so we keep
//  it as raw Json rather than a Newtype. (Distinct from the typed RequestId in
//  ids.hpp, which is used inside protocol params.)
//==============================================================================
using RpcId = Json;

//==============================================================================
//  Sum<A₀, …, Aₙ>  ≅  A₀ + … + Aₙ.   Discriminator is the position; the
//  sum_tagged codec maps positions ↔ wire labels.
//==============================================================================
template <class... As> using Sum = std::variant<As...>;

// match : the eliminator — a total, exhaustive case analysis over a sum.
template <class S, class... Fs>
constexpr decltype(auto) match(S&& s, Fs&&... fs) {
    struct overload : std::remove_reference_t<Fs>... {
        using std::remove_reference_t<Fs>::operator()...;
    };
    return std::visit(overload{std::forward<Fs>(fs)...}, std::forward<S>(s));
}

//==============================================================================
//  Compile-time string literal as a type — used as discriminator tags and
//  capability indices via non-type template parameters.
//==============================================================================
template <std::size_t N>
struct StaticString {
    char data[N]{};
    constexpr StaticString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string_view view() const noexcept { return {data, N - 1}; }
    constexpr std::size_t size() const noexcept { return N - 1; }
    constexpr bool operator==(const StaticString&) const = default;
};

template <StaticString S>
struct Tag {
    static constexpr std::string_view name = S.view();
};

//==============================================================================
//  Newtype<Phantom, A>  — opaque type alias, no runtime overhead.
//
//      Cursor, ProgressToken-string, task ids etc. are all "strings" on the
//      wire but should not be confused. Newtype gives nominal typing for free.
//==============================================================================
template <class Phantom, class A>
struct Newtype {
    A value{};
    constexpr Newtype() = default;
    constexpr explicit Newtype(A v) : value(std::move(v)) {}
    constexpr operator const A&() const noexcept { return value; }
    constexpr const A& get() const noexcept { return value; }
    friend constexpr bool operator==(const Newtype&, const Newtype&) = default;
    friend constexpr auto operator<=>(const Newtype&, const Newtype&) = default;
};

} // namespace mcp

// std::hash for any Newtype<P, A> when A is hashable.
template <class P, class A>
struct std::hash<mcp::Newtype<P, A>> {
    std::size_t operator()(const mcp::Newtype<P, A>& n) const
        noexcept(noexcept(std::hash<A>{}(n.value))) {
        return std::hash<A>{}(n.value);
    }
};
