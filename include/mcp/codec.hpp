// SPDX-License-Identifier: Apache-2.0
//
// mcp/codec.hpp — codecs as a category.
//
//   A Codec<T> witnesses a partial isomorphism   T ⇄ Json   with the law
//   decode ∘ encode = id_T.  Codecs compose:
//
//     codec<T>()             primitive / atomic encodings
//     list_codec(c)          List functor
//     maybe_codec(c)         Maybe functor
//     newtype_codec<P,A>()   Newtype<P,A> inherits A's codec
//     variant_codec(c…)      structural union (try each arm, first that decodes)
//     record(field…)         product Π fᵢ : Tᵢ  — names are wire keys
//     sum_tagged(key, arm…)  coproduct tagged by a string discriminator
//     enum_codec(…)          string-valued enum
//
//   Every MCP type downstream is a closed expression in this algebra.
//   Hand-written to_json/from_json never appear at call sites.
//
#pragma once

#include <mcp/core.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mcp {

//==============================================================================
//  CodecError — the failure mode of decode.
//==============================================================================
struct CodecError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

//==============================================================================
//  Codec<T> — an erased pair of (encode, decode).
//==============================================================================
template <class T>
struct Codec {
    std::function<Json(const T&)> encode;
    std::function<T(const Json&)> decode;
};

template <class T> struct CodecOf;        // user supplies ::get() for spec types

namespace detail {
    template <class T> struct is_maybe           : std::false_type {};
    template <class A> struct is_maybe<Maybe<A>> : std::true_type  {};

    template <class T> struct is_list            : std::false_type {};
    template <class A> struct is_list<List<A>>   : std::true_type  {};

    template <class T> struct is_newtype         : std::false_type {};
    template <class P, class A>
    struct is_newtype<Newtype<P, A>>             : std::true_type  {
        using Phantom = P;
        using Carrier = A;
    };
} // namespace detail

template <class A> Codec<List<A>>  list_codec(Codec<A> inner);
template <class A> Codec<Maybe<A>> maybe_codec(Codec<A> inner);
template <class P, class A> Codec<Newtype<P, A>> newtype_codec();

template <class T> const Codec<T>& codec();   // cached accessor (fwd decl)

template <class T>
inline Codec<T> build_codec() {
    if constexpr (detail::is_maybe<T>::value) {
        using A = typename T::value_type;
        return maybe_codec<A>(codec<A>());
    } else if constexpr (detail::is_list<T>::value) {
        using A = typename T::value_type;
        return list_codec<A>(codec<A>());
    } else if constexpr (detail::is_newtype<T>::value) {
        using P = typename detail::is_newtype<T>::Phantom;
        using A = typename detail::is_newtype<T>::Carrier;
        return newtype_codec<P, A>();
    } else {
        return CodecOf<T>::get();
    }
}

// One lazily-built, immutable codec per type (Meyers singleton). Building a
// codec allocates a tree of std::functions; doing that per-message was the hot
// path. Thread-safe: static init is once; the codec is const thereafter.
template <class T>
inline const Codec<T>& codec() {
    static const Codec<T> instance = build_codec<T>();
    return instance;
}

template <class T> inline Json to_json(const T& v)      { return codec<T>().encode(v); }
template <class T> inline T    from_json(const Json& j) { return codec<T>().decode(j); }

//==============================================================================
//  Primitive codecs — the generators of the category.
//==============================================================================
template <> struct CodecOf<bool> {
    static Codec<bool> get() {
        return {[](const bool& v) -> Json { return v; },
                [](const Json& j) -> bool {
                    if (!j.is_boolean()) throw CodecError("expected bool");
                    return j.get<bool>();
                }};
    }
};
template <> struct CodecOf<std::string> {
    static Codec<std::string> get() {
        return {[](const std::string& v) -> Json { return v; },
                [](const Json& j) -> std::string {
                    if (!j.is_string()) throw CodecError("expected string");
                    return j.get<std::string>();
                }};
    }
};
template <> struct CodecOf<std::int64_t> {
    static Codec<std::int64_t> get() {
        return {[](const std::int64_t& v) -> Json { return v; },
                [](const Json& j) -> std::int64_t {
                    if (!j.is_number_integer()) throw CodecError("expected integer");
                    return j.get<std::int64_t>();
                }};
    }
};
template <> struct CodecOf<int> {
    static Codec<int> get() {
        auto inner = CodecOf<std::int64_t>::get();
        return {[enc = inner.encode](const int& v) { return enc(static_cast<std::int64_t>(v)); },
                [dec = inner.decode](const Json& j) { return static_cast<int>(dec(j)); }};
    }
};
template <> struct CodecOf<double> {
    static Codec<double> get() {
        return {[](const double& v) -> Json { return v; },
                [](const Json& j) -> double {
                    if (!j.is_number()) throw CodecError("expected number");
                    return j.get<double>();
                }};
    }
};
template <> struct CodecOf<Unit> {
    static Codec<Unit> get() {
        return {[](const Unit&) -> Json { return Json::object(); },
                [](const Json&) -> Unit { return Unit{}; }};
    }
};
// Json itself — identity codec, for opaque _meta blobs and structuredContent.
template <> struct CodecOf<Json> {
    static Codec<Json> get() {
        return {[](const Json& v) { return v; }, [](const Json& j) { return j; }};
    }
};

//==============================================================================
//  Functorial lifts — List and Maybe.
//==============================================================================
template <class A>
inline Codec<List<A>> list_codec(Codec<A> inner) {
    return {[enc = inner.encode](const List<A>& xs) -> Json {
                Json out = Json::array();
                for (const auto& x : xs) out.push_back(enc(x));
                return out;
            },
            [dec = inner.decode](const Json& j) -> List<A> {
                if (!j.is_array()) throw CodecError("expected array");
                List<A> out;
                out.reserve(j.size());
                for (const auto& el : j) out.push_back(dec(el));
                return out;
            }};
}

template <class A>
inline Codec<Maybe<A>> maybe_codec(Codec<A> inner) {
    return {[enc = inner.encode](const Maybe<A>& v) -> Json {
                return v.has_value() ? enc(*v) : Json(nullptr);
            },
            [dec = inner.decode](const Json& j) -> Maybe<A> {
                if (j.is_null()) return Nothing;
                return Just(dec(j));
            }};
}

//==============================================================================
//  Newtype<P, A> — inherits A's codec; nominal in C++, structural on wire.
//==============================================================================
template <class P, class A>
inline Codec<Newtype<P, A>> newtype_codec() {
    auto inner = codec<A>();
    return {[enc = inner.encode](const Newtype<P, A>& n) -> Json { return enc(n.value); },
            [dec = inner.decode](const Json& j) -> Newtype<P, A> { return Newtype<P, A>{dec(j)}; }};
}

//==============================================================================
//  Enums — bijection between a closed inhabitant set and string labels.
//==============================================================================
template <class E>
struct EnumMapping {
    E value;
    std::string_view label;
};

template <class E, std::size_t N>
inline Codec<E> enum_codec(std::array<EnumMapping<E>, N> table) {
    return {[table](const E& v) -> Json {
                for (const auto& m : table) if (m.value == v) return std::string(m.label);
                throw CodecError("enum value outside declared mapping");
            },
            [table](const Json& j) -> E {
                if (!j.is_string()) throw CodecError("expected string for enum");
                const auto& s = j.get_ref<const std::string&>();
                for (const auto& m : table) if (m.label == s) return m.value;
                throw CodecError("unknown enum label: '" + s + "'");
            }};
}

template <class E, class... Rest>
inline Codec<E> enum_codec(EnumMapping<E> first, Rest... rest) {
    constexpr std::size_t N = 1 + sizeof...(Rest);
    std::array<EnumMapping<E>, N> table{first, rest...};
    return enum_codec<E, N>(table);
}

//==============================================================================
//  Records as products  —  T  ≅  Π fᵢ : Tᵢ
//
//    required(key, &T::f)              — key MUST be present
//    optional(key, &T::f)              — &T::f has type Maybe<U>; omit if Nothing
//    defaulted(key, &T::f, dflt)       — absent ⇒ dflt
//    meta(key, &T::f)                  — _meta extension blob; omit when empty
//    constant(key, "value")            — phantom discriminant emitted verbatim
//==============================================================================
template <class T, class U>
struct Field {
    std::string_view key;
    U T::*lens;
    Codec<U> codec_;
    enum class Kind : unsigned char { Required, Optional, Defaulted, Meta, Constant } kind;
    U default_value{};
    std::string_view constant_value{};   // for Kind::Constant (lens unused)
};

template <class T, class U>
constexpr Field<T, U> required(std::string_view key, U T::*lens) {
    return Field<T, U>{key, lens, codec<U>(), Field<T, U>::Kind::Required, U{}, {}};
}
template <class T, class U>
constexpr Field<T, U> required(std::string_view key, U T::*lens, Codec<U> c) {
    return Field<T, U>{key, lens, std::move(c), Field<T, U>::Kind::Required, U{}, {}};
}
template <class T, class U>
constexpr Field<T, Maybe<U>> optional(std::string_view key, Maybe<U> T::*lens) {
    return Field<T, Maybe<U>>{key, lens, maybe_codec(codec<U>()),
                              Field<T, Maybe<U>>::Kind::Optional, Maybe<U>{}, {}};
}
template <class T, class U>
constexpr Field<T, Maybe<U>> optional(std::string_view key, Maybe<U> T::*lens, Codec<U> c) {
    return Field<T, Maybe<U>>{key, lens, maybe_codec(std::move(c)),
                              Field<T, Maybe<U>>::Kind::Optional, Maybe<U>{}, {}};
}
template <class T, class U>
Field<T, U> defaulted(std::string_view key, U T::*lens, U dflt) {
    return Field<T, U>{key, lens, codec<U>(), Field<T, U>::Kind::Defaulted, std::move(dflt), {}};
}

// _meta extension field: omit on encode when null/empty-object; default to {}
// on decode. Matches the spec exactly (_meta is OPTIONAL).
template <class T>
Field<T, Json> meta(std::string_view key, Json T::*lens) {
    return Field<T, Json>{key, lens, CodecOf<Json>::get(),
                          Field<T, Json>::Kind::Meta, Json::object(), {}};
}

template <class T, class... Us>
inline Codec<T> record(Field<T, Us>... fields_) {
    auto fields = std::make_tuple(std::move(fields_)...);
    return {
        // ---- encode ----
        [fields](const T& v) -> Json {
            Json out = Json::object();
            std::apply([&](const auto&... f) {
                ((/* per-field */ [&] {
                    using FT = std::decay_t<decltype(v.*(f.lens))>;
                    using K  = typename std::decay_t<decltype(f)>::Kind;
                    if constexpr (std::is_same_v<FT, Json>) {
                        if (f.kind == K::Meta) {
                            const Json& m = v.*(f.lens);
                            if (!m.is_null() && !(m.is_object() && m.empty()))
                                out[std::string(f.key)] = m;
                            return;
                        }
                    }
                    if constexpr (detail::is_maybe<FT>::value) {
                        if ((v.*(f.lens)).has_value())
                            out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
                    } else {
                        out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
                    }
                }()), ...);
            }, fields);
            return out;
        },
        // ---- decode ----
        [fields](const Json& j) -> T {
            if (!j.is_object()) throw CodecError("expected object for record");
            T out{};
            std::apply([&](const auto&... f) {
                ((/* per-field */ [&] {
                    auto it = j.find(std::string(f.key));
                    const bool absent = (it == j.end()) || it->is_null();
                    using K = typename std::decay_t<decltype(f)>::Kind;
                    if (absent) {
                        if (f.kind == K::Required)
                            throw CodecError("missing required field: " + std::string(f.key));
                        if (f.kind == K::Defaulted || f.kind == K::Meta)
                            (out.*(f.lens)) = f.default_value;
                        // Optional: leave Nothing
                    } else {
                        (out.*(f.lens)) = f.codec_.decode(*it);
                    }
                }()), ...);
            }, fields);
            return out;
        }};
}

//==============================================================================
//  Tagged sums  —  Sum<A₀, …, Aₙ>  discriminated by a string key.
//
//      Wire form:   { "<key>": "<label>", …rest from the matching arm… }
//      e.g.         { "type": "text", "text": "hello" }
//==============================================================================
template <class Sum_, class Alt>
struct Arm {
    using SumT = Sum_;
    using AltT = Alt;
    std::string_view label;
    Codec<Alt> inner;
};

template <class Sum_, class Alt>
constexpr Arm<Sum_, Alt> arm(std::string_view label) {
    return Arm<Sum_, Alt>{label, codec<Alt>()};
}
template <class Sum_, class Alt>
constexpr Arm<Sum_, Alt> arm(std::string_view label, Codec<Alt> c) {
    return Arm<Sum_, Alt>{label, std::move(c)};
}

template <class Sum_, class... Alts>
inline Codec<Sum_> sum_tagged(std::string_view tag_key, Arm<Sum_, Alts>... arms_) {
    auto arms = std::make_tuple(std::move(arms_)...);
    std::string key{tag_key};
    return {
        // ---- encode ----
        [arms, key](const Sum_& s) -> Json {
            return std::visit([&](const auto& x) -> Json {
                using X = std::decay_t<decltype(x)>;
                Json out; bool matched = false;
                std::apply([&](const auto&... a) {
                    ((/* per arm */ [&] {
                        if (matched) return;
                        using A = std::decay_t<decltype(a)>;
                        if constexpr (std::is_same_v<X, typename A::AltT>) {
                            out = a.inner.encode(x);
                            if (!out.is_object())
                                throw CodecError("sum_tagged arm '" + std::string(a.label) +
                                                 "' must encode to a JSON object");
                            out[key] = std::string(a.label);
                            matched = true;
                        }
                    }()), ...);
                }, arms);
                if (!matched) throw CodecError("sum_tagged: no arm for active alternative");
                return out;
            }, s);
        },
        // ---- decode ----
        [arms, key](const Json& j) -> Sum_ {
            if (!j.is_object()) throw CodecError("sum_tagged: expected object");
            auto it = j.find(key);
            if (it == j.end() || !it->is_string())
                throw CodecError("sum_tagged: missing tag '" + key + "'");
            const std::string label = it->get<std::string>();
            Maybe<Sum_> out;
            std::apply([&](const auto&... a) {
                ((/* per arm */ [&] {
                    if (out.has_value()) return;
                    if (a.label == label) out = Sum_{a.inner.decode(j)};
                }()), ...);
            }, arms);
            if (!out.has_value()) throw CodecError("sum_tagged: unknown label '" + label + "'");
            return std::move(*out);
        }};
}

//==============================================================================
//  Untagged structural unions  —  Sum<A₀, …, Aₙ>  with NO discriminator key.
//
//      Used where the spec models a union by SHAPE rather than a tag, e.g.
//      RequestId = string | number, or the elicitation enum schemas. Encode
//      dispatches on the active alternative; decode tries each arm in order
//      and accepts the first that decodes without throwing.
//==============================================================================
template <class Sum_, class... Alts>
inline Codec<Sum_> variant_codec(Codec<Alts>... arms_) {
    auto arms = std::make_shared<std::tuple<Codec<Alts>...>>(std::move(arms_)...);
    return {
        // encode: the active alternative type uniquely selects its codec.
        [arms](const Sum_& s) -> Json {
            Json out; bool matched = false;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((/* per arm */ [&] {
                    if (matched) return;
                    using A = std::tuple_element_t<I, std::tuple<Alts...>>;
                    if (const A* p = std::get_if<A>(&s)) {
                        out = std::get<I>(*arms).encode(*p);
                        matched = true;
                    }
                }()), ...);
            }(std::index_sequence_for<Alts...>{});
            if (!matched) throw CodecError("variant_codec: no arm for active alternative");
            return out;
        },
        // decode: try each arm in declaration order; first success wins.
        [arms](const Json& j) -> Sum_ {
            Maybe<Sum_> out;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((/* per arm */ [&] {
                    if (out.has_value()) return;
                    try { out = Sum_{std::get<I>(*arms).decode(j)}; }
                    catch (const CodecError&) {}
                }()), ...);
            }(std::index_sequence_for<Alts...>{});
            if (!out.has_value()) throw CodecError("variant_codec: no arm decoded the value");
            return std::move(*out);
        }};
}

} // namespace mcp
