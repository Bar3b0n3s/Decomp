#include "agent/tools.hpp"

#include "core/log.hpp"
#include "core/strings.hpp"

#include <cmath>
#include <exception>
#include <format>

namespace decomp::agent {
namespace {

std::string_view type_name(const Json& v) {
    switch (v.type()) {
    case Json::value_t::null: return "null";
    case Json::value_t::boolean: return "boolean";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned: return "integer";
    case Json::value_t::number_float: return "number";
    case Json::value_t::string: return "string";
    case Json::value_t::array: return "array";
    case Json::value_t::object: return "object";
    default: return "unsupported value";
    }
}

bool matches_type(const Json& v, std::string_view type) {
    if (type == "object") return v.is_object();
    if (type == "array") return v.is_array();
    if (type == "string") return v.is_string();
    if (type == "boolean") return v.is_boolean();
    if (type == "null") return v.is_null();
    if (type == "number") return v.is_number();
    if (type == "integer") {
        if (v.is_number_integer()) return true;
        if (v.is_number_float()) {
            const double d = v.get<double>();
            return std::isfinite(d) && std::floor(d) == d;
        }
    }
    return false;
}

const Json* member(const Json& schema, std::string_view key) {
    auto it = schema.find(key);
    return it == schema.end() ? nullptr : &*it;
}

std::size_t utf8_length(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

std::unexpected<Error> invalid(const std::string& path, std::string message) {
    return make_error(ErrorCode::invalid_argument, "{}: {}", path, message);
}

Result<void> validate_at(const Json& value, const Json& schema, const std::string& path) {
    if (schema.is_boolean()) {
        if (schema.get<bool>()) return {};
        return invalid(path, "no value is allowed here");
    }
    if (!schema.is_object()) return {};

    if (const Json* type = member(schema, "type")) {
        bool ok = false;
        std::string expected;
        if (type->is_string()) {
            expected = type->get<std::string>();
            ok = matches_type(value, expected);
        } else if (type->is_array()) {
            std::vector<std::string> names;
            for (const Json& t : *type) {
                if (!t.is_string()) continue;
                names.push_back(t.get<std::string>());
                ok = ok || matches_type(value, names.back());
            }
            expected = join(names, " or ");
        } else {
            ok = true;
        }
        if (!ok) return invalid(path, std::format("expected {}, got {}", expected, type_name(value)));
    }

    if (const Json* allowed = member(schema, "enum"); allowed && allowed->is_array()) {
        bool found = false;
        for (const Json& candidate : *allowed) found = found || candidate == value;
        if (!found) return invalid(path, std::format("{} is not one of {}", dump_compact(value), dump_compact(*allowed)));
    }
    if (const Json* constant = member(schema, "const"); constant && *constant != value)
        return invalid(path, std::format("must be {}", dump_compact(*constant)));

    if (const Json* any_of = member(schema, "anyOf"); any_of && any_of->is_array()) {
        std::string first_failure;
        bool matched = false;
        for (const Json& option : *any_of) {
            auto r = validate_at(value, option, path);
            if (r) {
                matched = true;
                break;
            }
            if (first_failure.empty()) first_failure = r.error().message;
        }
        if (!matched) return invalid(path, std::format("matches none of the allowed schemas (first: {})", first_failure));
    }

    if (value.is_number()) {
        const double d = value.get<double>();
        if (const Json* m = member(schema, "minimum"); m && m->is_number() && d < m->get<double>())
            return invalid(path, std::format("{} is less than the minimum {}", dump_compact(value), dump_compact(*m)));
        if (const Json* m = member(schema, "maximum"); m && m->is_number() && d > m->get<double>())
            return invalid(path, std::format("{} is greater than the maximum {}", dump_compact(value), dump_compact(*m)));
        if (const Json* m = member(schema, "exclusiveMinimum"); m && m->is_number() && d <= m->get<double>())
            return invalid(path, std::format("{} must be greater than {}", dump_compact(value), dump_compact(*m)));
        if (const Json* m = member(schema, "exclusiveMaximum"); m && m->is_number() && d >= m->get<double>())
            return invalid(path, std::format("{} must be less than {}", dump_compact(value), dump_compact(*m)));
    }

    if (value.is_string()) {
        const std::size_t length = utf8_length(value.get_ref<const std::string&>());
        if (const Json* m = member(schema, "minLength"); m && m->is_number_integer() && length < m->get<std::size_t>())
            return invalid(path, std::format("string is shorter than {} characters", m->get<std::size_t>()));
        if (const Json* m = member(schema, "maxLength"); m && m->is_number_integer() && length > m->get<std::size_t>())
            return invalid(path, std::format("string is longer than {} characters", m->get<std::size_t>()));
    }

    if (value.is_object()) {
        const Json* properties = member(schema, "properties");
        if (properties && !properties->is_object()) properties = nullptr;
        if (const Json* required = member(schema, "required"); required && required->is_array()) {
            for (const Json& name : *required) {
                if (name.is_string() && !value.contains(name.get<std::string>()))
                    return invalid(path, std::format("missing required property '{}'", name.get<std::string>()));
            }
        }
        const Json* additional = member(schema, "additionalProperties");
        for (const auto& [key, item] : value.items()) {
            const std::string item_path = path + "." + key;
            if (properties && properties->contains(key)) {
                TRY(validate_at(item, (*properties)[key], item_path));
            } else if (additional && additional->is_boolean() && !additional->get<bool>()) {
                return invalid(path, std::format("unexpected property '{}'", key));
            } else if (additional && additional->is_object()) {
                TRY(validate_at(item, *additional, item_path));
            }
        }
    }

    if (value.is_array()) {
        if (const Json* m = member(schema, "minItems"); m && m->is_number_integer() && value.size() < m->get<std::size_t>())
            return invalid(path, std::format("expected at least {} items, got {}", m->get<std::size_t>(), value.size()));
        if (const Json* m = member(schema, "maxItems"); m && m->is_number_integer() && value.size() > m->get<std::size_t>())
            return invalid(path, std::format("expected at most {} items, got {}", m->get<std::size_t>(), value.size()));
        if (const Json* items = member(schema, "items"); items && (items->is_object() || items->is_boolean())) {
            for (std::size_t i = 0; i < value.size(); ++i) TRY(validate_at(value[i], *items, std::format("{}[{}]", path, i)));
        }
    }
    return {};
}

bool declares_object(const Json& schema) {
    const Json* type = member(schema, "type");
    if (!type) return schema.contains("properties");
    if (type->is_string()) return type->get<std::string>() == "object";
    if (type->is_array()) {
        for (const Json& t : *type) {
            if (t.is_string() && t.get<std::string>() == "object") return true;
        }
    }
    return false;
}

Result<void> check_strict_at(const Json& schema, const std::string& path) {
    if (!schema.is_object()) return {};
    if (declares_object(schema)) {
        const Json* additional = member(schema, "additionalProperties");
        if (!additional || !additional->is_boolean() || additional->get<bool>())
            return invalid(path, "object schemas must set \"additionalProperties\": false");
        const Json* properties = member(schema, "properties");
        if (properties && properties->is_object() && !properties->empty()) {
            const Json* required = member(schema, "required");
            if (!required || !required->is_array()) return invalid(path, "object schemas must list \"required\" properties");
            for (const Json& name : *required) {
                if (!name.is_string() || !properties->contains(name.get<std::string>()))
                    return invalid(path, std::format("required property {} is not defined", dump_compact(name)));
            }
        }
        if (properties && properties->is_object()) {
            for (const auto& [key, sub] : properties->items()) TRY(check_strict_at(sub, path + ".properties." + key));
        }
    }
    if (const Json* items = member(schema, "items")) TRY(check_strict_at(*items, path + ".items"));
    if (const Json* any_of = member(schema, "anyOf"); any_of && any_of->is_array()) {
        for (std::size_t i = 0; i < any_of->size(); ++i) TRY(check_strict_at((*any_of)[i], std::format("{}.anyOf[{}]", path, i)));
    }
    return {};
}

Json normalize_content(Json content) {
    if (content.is_string() || content.is_array()) return content;
    if (content.is_null()) return "";
    return dump_compact(content);
}

} // namespace

Result<void> validate_json(const Json& value, const Json& schema) {
    try {
        return validate_at(value, schema, "input");
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::invalid_argument, "schema validation failed: {}", e.what());
    }
}

Result<void> check_strict_schema(const Json& schema) {
    if (!schema.is_object()) return make_error(ErrorCode::invalid_argument, "input_schema must be a JSON object");
    try {
        return check_strict_at(schema, "input_schema");
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::invalid_argument, "invalid schema: {}", e.what());
    }
}

std::string invalid_input_content(std::string_view raw_input, std::string_view reason) {
    Json content{{"INVALID_JSON", std::string(raw_input)}};
    if (!reason.empty()) content["error"] = std::string(reason);
    return dump_compact(content);
}

void ToolRegistry::add(Tool tool) {
    if (auto strict = check_strict_schema(tool.input_schema); !strict)
        log::warn("tool '{}' does not meet strict tool use requirements: {}", tool.name, strict.error().message);
    std::string name = tool.name;
    tools_.insert_or_assign(std::move(name), std::move(tool));
}

Json ToolRegistry::definitions() const {
    Json definitions = Json::array();
    for (const auto& [name, tool] : tools_) {
        Json schema = tool.input_schema.is_object()
                          ? tool.input_schema
                          : Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
        definitions.push_back(Json{{"name", name},
                                   {"description", tool.description},
                                   {"input_schema", std::move(schema)},
                                   {"strict", true},
                                   {"eager_input_streaming", true}});
    }
    return definitions;
}

const Tool* ToolRegistry::find(std::string_view name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [name, tool] : tools_) out.push_back(name);
    return out;
}

ToolResult ToolRegistry::dispatch(const ToolCall& call) const {
    const Tool* tool = find(call.name);
    if (!tool) {
        return ToolResult::error(std::format("Unknown tool '{}'. Available tools: {}.", call.name,
                                             tools_.empty() ? std::string("none") : join(names(), ", ")));
    }
    if (!call.input_valid) return ToolResult::error(invalid_input_content(call.raw_input));
    if (tool->input_schema.is_object()) {
        if (auto valid = validate_json(call.input, tool->input_schema); !valid) {
            const std::string raw = call.raw_input.empty() ? dump_compact(call.input) : call.raw_input;
            return ToolResult::error(invalid_input_content(raw, valid.error().message));
        }
    }
    if (!tool->handler) return ToolResult::error(std::format("Tool '{}' has no handler.", call.name));
    try {
        ToolResult result = tool->handler(call);
        result.content = normalize_content(std::move(result.content));
        return result;
    } catch (const std::exception& e) {
        return ToolResult::error(std::format("Tool '{}' failed: {}", call.name, e.what()));
    } catch (...) {
        return ToolResult::error(std::format("Tool '{}' failed with an unknown exception.", call.name));
    }
}

std::vector<ToolCall> extract_tool_calls(const Response& response) {
    std::vector<ToolCall> calls;
    for (std::size_t i : echoed_block_indices(response)) {
        const Json& block = response.content[i];
        if (json_string_or(block, "type", "") != "tool_use") continue;
        ToolCall call;
        call.id = json_string_or(block, "id", "");
        call.name = json_string_or(block, "name", "");
        if (auto it = block.find("input"); it != block.end()) call.input = *it;
        if (i < response.blocks.size()) {
            const BlockStatus& status = response.blocks[i];
            call.input_valid = status.input_valid && status.complete;
            call.raw_input = status.raw_input;
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

} // namespace decomp::agent
