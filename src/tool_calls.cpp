#include "ryzenai/tool_calls.h"
#include <iostream>
#include <ctime>
#include <optional>

namespace ryzenai {

namespace {

std::string trimWhitespace(const std::string& input) {
    size_t start = input.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = input.find_last_not_of(" \t\n\r");
    return input.substr(start, end - start + 1);
}

bool isValidJsonEscapeChar(char c) {
    return c == '"' || c == '\\' || c == '/' || c == 'b' ||
           c == 'f' || c == 'n' || c == 'r' || c == 't' || c == 'u';
}

// LLMs often emit literal newlines or Windows paths with single backslashes inside
// JSON string values. Repair those before json::parse.
std::string sanitizeJsonStringLiterals(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 32);

    bool in_string = false;
    bool escape_next = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escape_next) {
            result += c;
            escape_next = false;
            continue;
        }

        if (!in_string) {
            if (c == '"') {
                in_string = true;
            }
            result += c;
            continue;
        }

        // Inside a JSON string value
        if (c == '\\') {
            if (i + 1 < input.size() && isValidJsonEscapeChar(input[i + 1])) {
                result += c;
                escape_next = true;
            } else {
                result += "\\\\";
            }
        } else if (c == '"') {
            in_string = false;
            result += c;
        } else if (c == '\n') {
            result += "\\n";
        } else if (c == '\r') {
            result += "\\r";
        } else if (c == '\t') {
            result += "\\t";
        } else {
            result += c;
        }
    }

    return result;
}

std::string balanceJsonBrackets(const std::string& input) {
    int brace_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    bool escape_next = false;

    for (char c : input) {
        if (escape_next) {
            escape_next = false;
            continue;
        }

        if (in_string) {
            if (c == '\\') {
                escape_next = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++brace_depth;
        } else if (c == '}') {
            --brace_depth;
        } else if (c == '[') {
            ++bracket_depth;
        } else if (c == ']') {
            --bracket_depth;
        }
    }

    std::string result = input;
    while (bracket_depth > 0) {
        result += ']';
        --bracket_depth;
    }
    while (brace_depth > 0) {
        result += '}';
        --brace_depth;
    }
    return result;
}

json tryParseJson(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const json::exception&) {
        return json();
    }
}

json parseToolCallJsonObject(const std::string& raw) {
    const std::string trimmed = trimWhitespace(raw);
    if (trimmed.empty()) {
        return json();
    }

    json result = tryParseJson(trimmed);
    if (!result.is_null()) {
        return result;
    }

    const std::string sanitized = sanitizeJsonStringLiterals(trimmed);
    result = tryParseJson(sanitized);
    if (!result.is_null()) {
        std::cout << "[ToolCalls DEBUG] Parsed after sanitizing JSON string literals" << std::endl;
        return result;
    }

    const std::string balanced = balanceJsonBrackets(sanitized);
    result = tryParseJson(balanced);
    if (!result.is_null()) {
        std::cout << "[ToolCalls DEBUG] Parsed after balancing JSON brackets" << std::endl;
        return result;
    }

    return json();
}

std::optional<ToolCall> toolCallFromJsonObject(const json& tool_call_obj) {
    if (!tool_call_obj.is_object()) {
        return std::nullopt;
    }

    ToolCall tool_call;

    if (tool_call_obj.contains("name") && tool_call_obj["name"].is_string()) {
        tool_call.name = tool_call_obj["name"];
    } else {
        std::cerr << "[WARNING] Tool call missing 'name' field, skipping" << std::endl;
        return std::nullopt;
    }

    if (tool_call_obj.contains("arguments")) {
        tool_call.arguments = tool_call_obj["arguments"];
    } else if (tool_call_obj.contains("parameters")) {
        tool_call.arguments = tool_call_obj["parameters"];
    } else {
        std::cerr << "[WARNING] Tool call missing 'arguments' or 'parameters' field, skipping" << std::endl;
        return std::nullopt;
    }

    return tool_call;
}

std::optional<ToolCall> parseToolCallPayload(const std::string& tool_call_json) {
    json tool_call_obj = parseToolCallJsonObject(tool_call_json);
    if (tool_call_obj.is_null()) {
        return std::nullopt;
    }
    return toolCallFromJsonObject(tool_call_obj);
}

void eraseMatch(std::string& cleaned_text, size_t match_pos, size_t match_len) {
    if (match_pos < cleaned_text.size()) {
        cleaned_text.erase(match_pos, std::min(match_len, cleaned_text.size() - match_pos));
    }
}

}  // namespace

std::pair<std::vector<ToolCall>, std::string> extractToolCalls(const std::string& text) {
    std::vector<ToolCall> tool_calls;
    std::string cleaned_text = text;

    std::cout << "[ToolCalls DEBUG] Extracting tool calls from text (" << text.length() << " chars)" << std::endl;
    std::cout << "[ToolCalls DEBUG] Text: " << text.substr(0, std::min(size_t(300), text.length())) << std::endl;

    // Qwen-style tool calls: <tool_call>...</tool_call>
    const std::regex closed_tool_call_pattern(
        R"(<tool_call>([\s\S]*?)</tool_call>)",
        std::regex::icase | std::regex::ECMAScript);

    std::smatch match;
    std::string search_text = text;
    size_t offset = 0;

    while (std::regex_search(search_text, match, closed_tool_call_pattern)) {
        const std::string tool_call_json = match[1].str();

        std::cout << "[ToolCalls DEBUG] Found Qwen-style match, JSON: " << tool_call_json << std::endl;

        if (auto tool_call = parseToolCallPayload(tool_call_json)) {
            tool_calls.push_back(*tool_call);
            eraseMatch(cleaned_text, offset + match.position(), match.length());
        } else {
            std::cerr << "[WARNING] Failed to parse tool call JSON after repair attempts" << std::endl;
        }

        search_text = match.suffix();
        offset += match.position() + match.length();
    }

    // Handle truncated generations that open <tool_call> but never emit </tool_call>.
    const std::regex open_tool_call_pattern(
        R"(<tool_call>([\s\S]+)$)",
        std::regex::icase | std::regex::ECMAScript);

    search_text = cleaned_text;
    offset = 0;
    while (std::regex_search(search_text, match, open_tool_call_pattern)) {
        const std::string tool_call_json = match[1].str();

        std::cout << "[ToolCalls DEBUG] Found unclosed Qwen-style match, JSON: " << tool_call_json << std::endl;

        if (auto tool_call = parseToolCallPayload(tool_call_json)) {
            tool_calls.push_back(*tool_call);
            eraseMatch(cleaned_text, offset + match.position(), match.length());
        } else {
            std::cerr << "[WARNING] Failed to parse unclosed tool call JSON after repair attempts" << std::endl;
        }

        search_text = match.suffix();
        offset += match.position() + match.length();
    }

    // Mistral-style: [TOOL_CALLS] [...]
    const std::regex mistral_pattern(
        R"(\[TOOL_CALLS\]\s*\[([\s\S]*?)\])",
        std::regex::icase | std::regex::ECMAScript);

    search_text = cleaned_text;
    offset = 0;

    while (std::regex_search(search_text, match, mistral_pattern)) {
        const std::string tool_calls_array_json = "[" + match[1].str() + "]";

        json tool_calls_array = parseToolCallJsonObject(tool_calls_array_json);
        if (tool_calls_array.is_array()) {
            for (const auto& tool_call_obj : tool_calls_array) {
                if (auto tool_call = toolCallFromJsonObject(tool_call_obj)) {
                    tool_calls.push_back(*tool_call);
                }
            }
            eraseMatch(cleaned_text, offset + match.position(), match.length());
        } else {
            std::cerr << "[WARNING] Failed to parse [TOOL_CALLS] JSON after repair attempts" << std::endl;
        }

        search_text = match.suffix();
        offset += match.position() + match.length();
    }

    cleaned_text = trimWhitespace(cleaned_text);

    std::cout << "[ToolCalls DEBUG] Extracted " << tool_calls.size() << " tool call(s)" << std::endl;

    return {tool_calls, cleaned_text};
}

json formatToolCallsForOpenAI(const std::vector<ToolCall>& tool_calls) {
    json openai_tool_calls = json::array();

    int index = 0;
    for (const auto& tool_call : tool_calls) {
        std::string tool_call_id = "call_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(index++);

        json openai_tool_call = {
            {"id", tool_call_id},
            {"type", "function"},
            {"function", {
                {"name", tool_call.name},
                {"arguments", tool_call.arguments.dump()}
            }}
        };
        openai_tool_calls.push_back(openai_tool_call);
    }

    return openai_tool_calls;
}

}  // namespace ryzenai
