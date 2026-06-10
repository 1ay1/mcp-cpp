// SPDX-License-Identifier: Apache-2.0
//
// mcp/elicit.hpp — elicitation request schemas (SEP-1330, SEP-1034, SEP-1036).
//
//   The 2025-11-25 revision overhauled elicitation schemas into a precise
//   algebra of primitive field types:
//
//     PrimitiveSchemaDefinition = String | Number | Boolean | Enum
//     EnumSchema = SingleSelect | MultiSelect | LegacyTitled
//     SingleSelect = Untitled | Titled
//     MultiSelect  = Untitled | Titled
//
//   Each is a closed sum we encode structurally (by which keys are present:
//   enum vs oneOf vs items.enum vs items.anyOf), exactly as the schema intends.
//
#pragma once

#include <mcp/types.hpp>

namespace mcp {

//==============================================================================
//  String / Number / Boolean schemas.
//==============================================================================
enum class StringFormat { Email, Uri, Date, DateTime };
template <> struct CodecOf<StringFormat> {
    static Codec<StringFormat> get() {
        return enum_codec<StringFormat>(
            EnumMapping<StringFormat>{StringFormat::Email,    "email"},
            EnumMapping<StringFormat>{StringFormat::Uri,      "uri"},
            EnumMapping<StringFormat>{StringFormat::Date,     "date"},
            EnumMapping<StringFormat>{StringFormat::DateTime, "date-time"});
    }
};

struct StringSchema {
    Maybe<std::string>  title;
    Maybe<std::string>  description;
    Maybe<std::int64_t> minLength;
    Maybe<std::int64_t> maxLength;
    Maybe<StringFormat> format;
    Maybe<std::string>  defaultValue;
};
template <> struct CodecOf<StringSchema> {
    static Codec<StringSchema> get() {
        return {
            [](const StringSchema& s) -> Json {
                Json j = {{"type", "string"}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.minLength)    j["minLength"]   = *s.minLength;
                if (s.maxLength)    j["maxLength"]   = *s.maxLength;
                if (s.format)       j["format"]      = to_json(*s.format);
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> StringSchema {
                if (j.value("type", "") != "string" || j.contains("enum") ||
                    j.contains("oneOf"))
                    throw CodecError("not a plain StringSchema");
                StringSchema s;
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("minLength");   it != j.end()) s.minLength = it->get<std::int64_t>();
                if (auto it = j.find("maxLength");   it != j.end()) s.maxLength = it->get<std::int64_t>();
                if (auto it = j.find("format");      it != j.end()) s.format = from_json<StringFormat>(*it);
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<std::string>();
                return s;
            }};
    }
};

struct NumberSchema {
    bool                integer = false;   // "integer" vs "number"
    Maybe<std::string>  title;
    Maybe<std::string>  description;
    Maybe<double>       minimum;
    Maybe<double>       maximum;
    Maybe<double>       defaultValue;
};
template <> struct CodecOf<NumberSchema> {
    static Codec<NumberSchema> get() {
        return {
            [](const NumberSchema& s) -> Json {
                Json j = {{"type", s.integer ? "integer" : "number"}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.minimum)      j["minimum"]     = *s.minimum;
                if (s.maximum)      j["maximum"]     = *s.maximum;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> NumberSchema {
                const auto t = j.value("type", "");
                if (t != "number" && t != "integer") throw CodecError("not a NumberSchema");
                NumberSchema s; s.integer = (t == "integer");
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("minimum");     it != j.end()) s.minimum = it->get<double>();
                if (auto it = j.find("maximum");     it != j.end()) s.maximum = it->get<double>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<double>();
                return s;
            }};
    }
};

struct BooleanSchema {
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<bool>        defaultValue;
};
template <> struct CodecOf<BooleanSchema> {
    static Codec<BooleanSchema> get() {
        return {
            [](const BooleanSchema& s) -> Json {
                Json j = {{"type", "boolean"}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> BooleanSchema {
                if (j.value("type", "") != "boolean") throw CodecError("not a BooleanSchema");
                BooleanSchema s;
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<bool>();
                return s;
            }};
    }
};

//==============================================================================
//  Enum schemas — the SEP-1330 family.
//==============================================================================
struct TitledOption { std::string constValue; std::string title; };  // {const,title}

// UntitledSingleSelect — { type:"string", enum:[…], default? }
struct UntitledSingleSelectEnum {
    List<std::string>  values;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<std::string> defaultValue;
};
// TitledSingleSelect — { type:"string", oneOf:[{const,title}], default? }
struct TitledSingleSelectEnum {
    List<TitledOption> options;
    Maybe<std::string> title;
    Maybe<std::string> description;
    Maybe<std::string> defaultValue;
};
// UntitledMultiSelect — { type:"array", items:{type:"string",enum:[…]} }
struct UntitledMultiSelectEnum {
    List<std::string>        values;
    Maybe<std::string>       title;
    Maybe<std::string>       description;
    Maybe<std::int64_t>      minItems;
    Maybe<std::int64_t>      maxItems;
    Maybe<List<std::string>> defaultValue;
};
// TitledMultiSelect — { type:"array", items:{anyOf:[{const,title}]} }
struct TitledMultiSelectEnum {
    List<TitledOption>       options;
    Maybe<std::string>       title;
    Maybe<std::string>       description;
    Maybe<std::int64_t>      minItems;
    Maybe<std::int64_t>      maxItems;
    Maybe<List<std::string>> defaultValue;
};

namespace detail {
inline Json enc_titled_options(const List<TitledOption>& opts) {
    Json a = Json::array();
    for (const auto& o : opts) a.push_back({{"const", o.constValue}, {"title", o.title}});
    return a;
}
inline List<TitledOption> dec_titled_options(const Json& a) {
    List<TitledOption> out;
    for (const auto& o : a)
        out.push_back({o.at("const").get<std::string>(), o.at("title").get<std::string>()});
    return out;
}
} // namespace detail

template <> struct CodecOf<UntitledSingleSelectEnum> {
    static Codec<UntitledSingleSelectEnum> get() {
        return {
            [](const UntitledSingleSelectEnum& s) -> Json {
                Json j = {{"type", "string"}, {"enum", s.values}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> UntitledSingleSelectEnum {
                if (j.value("type", "") != "string" || !j.contains("enum") ||
                    j.contains("enumNames"))
                    throw CodecError("not an UntitledSingleSelectEnum");
                UntitledSingleSelectEnum s;
                s.values = j.at("enum").get<List<std::string>>();
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<std::string>();
                return s;
            }};
    }
};

template <> struct CodecOf<TitledSingleSelectEnum> {
    static Codec<TitledSingleSelectEnum> get() {
        return {
            [](const TitledSingleSelectEnum& s) -> Json {
                Json j = {{"type", "string"}, {"oneOf", detail::enc_titled_options(s.options)}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> TitledSingleSelectEnum {
                if (j.value("type", "") != "string" || !j.contains("oneOf"))
                    throw CodecError("not a TitledSingleSelectEnum");
                TitledSingleSelectEnum s;
                s.options = detail::dec_titled_options(j.at("oneOf"));
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<std::string>();
                return s;
            }};
    }
};

template <> struct CodecOf<UntitledMultiSelectEnum> {
    static Codec<UntitledMultiSelectEnum> get() {
        return {
            [](const UntitledMultiSelectEnum& s) -> Json {
                Json j = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", s.values}}}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.minItems)     j["minItems"]    = *s.minItems;
                if (s.maxItems)     j["maxItems"]    = *s.maxItems;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> UntitledMultiSelectEnum {
                if (j.value("type", "") != "array") throw CodecError("not array enum");
                const Json& items = j.at("items");
                if (!items.contains("enum")) throw CodecError("not an UntitledMultiSelectEnum");
                UntitledMultiSelectEnum s;
                s.values = items.at("enum").get<List<std::string>>();
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("minItems");    it != j.end()) s.minItems = it->get<std::int64_t>();
                if (auto it = j.find("maxItems");    it != j.end()) s.maxItems = it->get<std::int64_t>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<List<std::string>>();
                return s;
            }};
    }
};

template <> struct CodecOf<TitledMultiSelectEnum> {
    static Codec<TitledMultiSelectEnum> get() {
        return {
            [](const TitledMultiSelectEnum& s) -> Json {
                Json j = {{"type", "array"},
                          {"items", {{"anyOf", detail::enc_titled_options(s.options)}}}};
                if (s.title)        j["title"]       = *s.title;
                if (s.description)  j["description"] = *s.description;
                if (s.minItems)     j["minItems"]    = *s.minItems;
                if (s.maxItems)     j["maxItems"]    = *s.maxItems;
                if (s.defaultValue) j["default"]     = *s.defaultValue;
                return j;
            },
            [](const Json& j) -> TitledMultiSelectEnum {
                if (j.value("type", "") != "array") throw CodecError("not array enum");
                const Json& items = j.at("items");
                if (!items.contains("anyOf")) throw CodecError("not a TitledMultiSelectEnum");
                TitledMultiSelectEnum s;
                s.options = detail::dec_titled_options(items.at("anyOf"));
                if (auto it = j.find("title");       it != j.end()) s.title = it->get<std::string>();
                if (auto it = j.find("description"); it != j.end()) s.description = it->get<std::string>();
                if (auto it = j.find("minItems");    it != j.end()) s.minItems = it->get<std::int64_t>();
                if (auto it = j.find("maxItems");    it != j.end()) s.maxItems = it->get<std::int64_t>();
                if (auto it = j.find("default");     it != j.end()) s.defaultValue = it->get<List<std::string>>();
                return s;
            }};
    }
};

//  PrimitiveSchemaDefinition — the union. Decode order matters: try the more
//  specific (enum) shapes before the plain String schema.
using PrimitiveSchema = Sum<
    UntitledSingleSelectEnum, TitledSingleSelectEnum,
    UntitledMultiSelectEnum,  TitledMultiSelectEnum,
    NumberSchema, BooleanSchema, StringSchema>;

template <> struct CodecOf<PrimitiveSchema> {
    static Codec<PrimitiveSchema> get() {
        return variant_codec<PrimitiveSchema>(
            codec<UntitledSingleSelectEnum>(),
            codec<TitledSingleSelectEnum>(),
            codec<UntitledMultiSelectEnum>(),
            codec<TitledMultiSelectEnum>(),
            codec<NumberSchema>(),
            codec<BooleanSchema>(),
            codec<StringSchema>());
    }
};

//  The elicitation result value: string | number | boolean | string[].
using ElicitValue = Sum<std::string, double, bool, List<std::string>>;
template <> struct CodecOf<ElicitValue> {
    static Codec<ElicitValue> get() {
        return variant_codec<ElicitValue>(
            codec<bool>(),               // bool before number (json bool isn't number)
            codec<double>(),
            codec<List<std::string>>(),
            codec<std::string>());
    }
};

enum class ElicitAction { Accept, Decline, Cancel };
template <> struct CodecOf<ElicitAction> {
    static Codec<ElicitAction> get() {
        return enum_codec<ElicitAction>(
            EnumMapping<ElicitAction>{ElicitAction::Accept,  "accept"},
            EnumMapping<ElicitAction>{ElicitAction::Decline, "decline"},
            EnumMapping<ElicitAction>{ElicitAction::Cancel,  "cancel"});
    }
};

} // namespace mcp
