// Standalone regression test for tool call JSON repair.
// Build: c++ -std=c++17 -I../include -I../external/json test_tool_calls_repair.cpp ../src/tool_calls.cpp -o test_tool_calls_repair
#include "ryzenai/tool_calls.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <iostream>

using json = nlohmann::json;

int main() {
    // Reproduce colleague report: literal newlines in file_text + Windows path backslashes + missing closing brace.
    const std::string text =
        "I'll create the file.\n"
        "<tool_call>\n"
        "{\"name\": \"create\", \"arguments\": {\"file_text\": \"#include <iostream>\n"
        "\n"
        "int main() {\n"
        "    std::cout << \\\"Hello, World!\\\" << std::endl;\n"
        "    return 0;\n"
        "}\n"
        "\", \"path\": \"C:\\Work\\copilot\\hello_world.cpp\"}\n"
        "</tool_call>\n";

    auto [tool_calls, cleaned] = ryzenai::extractToolCalls(text);

    if (tool_calls.size() != 1) {
        std::cerr << "Expected 1 tool call, got " << tool_calls.size() << std::endl;
        return 1;
    }

    if (tool_calls[0].name != "create") {
        std::cerr << "Unexpected tool name: " << tool_calls[0].name << std::endl;
        return 1;
    }

    if (!tool_calls[0].arguments.contains("path") ||
        tool_calls[0].arguments["path"] != "C:\\Work\\copilot\\hello_world.cpp") {
        std::cerr << "Unexpected path argument: " << tool_calls[0].arguments.dump() << std::endl;
        return 1;
    }

    if (!tool_calls[0].arguments.contains("file_text")) {
        std::cerr << "Missing file_text argument" << std::endl;
        return 1;
    }

    const std::string file_text = tool_calls[0].arguments["file_text"];
    if (file_text.find("#include <iostream>") == std::string::npos ||
        file_text.find("Hello, World!") == std::string::npos) {
        std::cerr << "Unexpected file_text: " << file_text << std::endl;
        return 1;
    }

    // Truncated tool call without closing tag.
    const std::string truncated =
        "Creating file now\n"
        "<tool_call>\n"
        "{\"name\": \"create\", \"arguments\": {\"file_text\": \"print('hi')\", \"path\": \"C:\\tmp\\a.py\"}";

    auto [trunc_calls, trunc_cleaned] = ryzenai::extractToolCalls(truncated);
    if (trunc_calls.size() != 1 || trunc_calls[0].name != "create") {
        std::cerr << "Failed truncated tool call extraction" << std::endl;
        return 1;
    }

    // arguments provided as a JSON string (common with some models)
    const std::string string_args_text =
        "Creating file\n"
        "<tool_call>\n"
        "{\"name\": \"create\", \"arguments\": \"{\\\"path\\\": \\\"C:\\\\tmp\\\\a.py\\\", \\\"file_text\\\": \\\"print('hi')\\\"}\"}\n"
        "</tool_call>\n";

    auto [string_arg_calls, string_arg_cleaned] = ryzenai::extractToolCalls(string_args_text);
    if (string_arg_calls.size() != 1 ||
        !string_arg_calls[0].arguments.is_object() ||
        string_arg_calls[0].arguments["path"] != "C:\\tmp\\a.py") {
        std::cerr << "Failed string-arguments tool call extraction" << std::endl;
        return 1;
    }

    json openai_tool_calls = ryzenai::formatToolCallsForOpenAI(string_arg_calls);
    if (!openai_tool_calls.is_array() || openai_tool_calls.size() != 1) {
        std::cerr << "Unexpected OpenAI tool call formatting" << std::endl;
        return 1;
    }
    if (!openai_tool_calls[0]["function"]["arguments"].is_string()) {
        std::cerr << "OpenAI arguments must be a JSON string" << std::endl;
        return 1;
    }

    auto stream_chunks = ryzenai::buildToolCallStreamChunks(openai_tool_calls, "test-model");
    if (stream_chunks.size() != 1) {
        std::cerr << "Expected one stream chunk" << std::endl;
        return 1;
    }
    json stream_chunk = json::parse(stream_chunks[0]);
    if (stream_chunk["choices"][0]["delta"]["tool_calls"][0]["index"] != 0 ||
        stream_chunk["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] != "create") {
        std::cerr << "Unexpected stream chunk payload: " << stream_chunks[0] << std::endl;
        return 1;
    }

    std::cout << "tool call repair tests passed" << std::endl;
    return 0;
}
