#pragma once
// Streaming-response parser for the Anthropic Messages API. Pure logic, no
// network and no Windows dependencies: the WinHTTP transport feeds raw bytes
// in whatever chunk sizes the socket produces, and this class reassembles
// SSE events and accumulates content blocks. Split out exactly so the
// protocol handling is testable under ctest without an API key.
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace lar {

struct ApiToolCall {
    std::string id;      // provider tool_use id, echoed back in tool_result
    std::string name;
    nlohmann::json input = nlohmann::json::object();
};

struct ApiTurnResult {
    bool ok = false;
    std::string error;
    std::string stop_reason;   // end_turn | tool_use | max_tokens | refusal | ...
    // The assistant content exactly as the API produced it, for verbatim
    // replay on the next request (the same role harmony_raw plays locally).
    nlohmann::json content = nlohmann::json::array();
    std::string text;          // concatenated text blocks
    std::vector<ApiToolCall> tool_calls;
};

class AnthropicStreamParser {
public:
    // Streaming callbacks fire as deltas arrive; both may be empty.
    std::function<void(const std::string&)> on_text;
    std::function<void(const std::string&)> on_thinking;

    // Feed raw response bytes in arrival order, any chunking.
    void feed(const char* data, size_t len);

    // Finalize and take the accumulated result. Call once, after the last
    // feed. Returns ok=false with an error when the stream carried an error
    // event or ended before message_stop.
    ApiTurnResult finish();

private:
    void handle_event(const nlohmann::json& event);

    std::string buffer_;
    std::vector<nlohmann::json> blocks_;
    std::vector<std::string> partial_inputs_;   // input_json_delta accumulation per block
    std::string stop_reason_;
    std::string error_;
    bool message_stopped_ = false;
};

} // namespace lar
