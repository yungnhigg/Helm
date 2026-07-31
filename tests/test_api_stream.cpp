// The Anthropic SSE parser must be indifferent to how the socket chunks the
// bytes, so every case here is fed twice: whole, and one byte at a time.
#include "api/anthropic_stream.h"
#include <iostream>
#include <string>

using namespace lar;
using nlohmann::json;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

static ApiTurnResult run(const std::string& stream, size_t chunk,
                         std::string* streamed_text = nullptr) {
    AnthropicStreamParser parser;
    if (streamed_text)
        parser.on_text = [streamed_text](const std::string& piece) { *streamed_text += piece; };
    for (size_t i = 0; i < stream.size(); i += chunk)
        parser.feed(stream.data() + i, std::min(chunk, stream.size() - i));
    return parser.finish();
}

static std::string frame(const std::string& event, const json& data) {
    return "event: " + event + "\ndata: " + data.dump() + "\n\n";
}

int main() {
    // ---- plain text reply ----
    std::string text_stream =
        frame("message_start", {{"type", "message_start"}, {"message", {{"id", "msg_1"}}}}) +
        frame("content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                      {"content_block", {{"type", "text"}, {"text", ""}}}}) +
        frame("ping", {{"type", "ping"}}) +
        frame("content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                      {"delta", {{"type", "text_delta"}, {"text", "Hello "}}}}) +
        frame("content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                      {"delta", {{"type", "text_delta"}, {"text", "world"}}}}) +
        frame("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}}) +
        frame("message_delta", {{"type", "message_delta"},
                                {"delta", {{"stop_reason", "end_turn"}}}, {"usage", {{"output_tokens", 5}}}}) +
        frame("message_stop", {{"type", "message_stop"}});

    for (size_t chunk : {text_stream.size(), size_t(1), size_t(7)}) {
        std::string streamed;
        const auto r = run(text_stream, chunk, &streamed);
        expect("text: ok", r.ok);
        expect("text: assembled", r.text == "Hello world");
        expect("text: streamed matches", streamed == "Hello world");
        expect("text: stop reason", r.stop_reason == "end_turn");
        expect("text: no tool calls", r.tool_calls.empty());
        expect("text: one content block", r.content.size() == 1);
    }

    // ---- tool use with the input JSON split mid-token ----
    std::string tool_stream =
        frame("message_start", {{"type", "message_start"}}) +
        frame("content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                      {"content_block", {{"type", "text"}, {"text", ""}}}}) +
        frame("content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                      {"delta", {{"type", "text_delta"}, {"text", "Searching."}}}}) +
        frame("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}}) +
        frame("content_block_start", {{"type", "content_block_start"}, {"index", 1},
                                      {"content_block", {{"type", "tool_use"}, {"id", "toolu_9"},
                                                         {"name", "search_web"}, {"input", json::object()}}}}) +
        frame("content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                                      {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"query\": \"tru"}}}}) +
        frame("content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                                      {"delta", {{"type", "input_json_delta"}, {"partial_json", "mp news\", \"max_results\": 5}"}}}}) +
        frame("content_block_stop", {{"type", "content_block_stop"}, {"index", 1}}) +
        frame("message_delta", {{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}}) +
        frame("message_stop", {{"type", "message_stop"}});

    for (size_t chunk : {tool_stream.size(), size_t(1), size_t(13)}) {
        const auto r = run(tool_stream, chunk);
        expect("tool: ok", r.ok);
        expect("tool: stop reason", r.stop_reason == "tool_use");
        expect("tool: one call", r.tool_calls.size() == 1);
        if (!r.tool_calls.empty()) {
            expect("tool: id", r.tool_calls[0].id == "toolu_9");
            expect("tool: name", r.tool_calls[0].name == "search_web");
            expect("tool: input reassembled",
                   r.tool_calls[0].input.value("query", "") == "trump news" &&
                   r.tool_calls[0].input.value("max_results", 0) == 5);
        }
        expect("tool: replay content preserved", r.content.size() == 2 &&
               r.content[1].value("type", "") == "tool_use");
        expect("tool: preamble text kept", r.text == "Searching.");
    }

    // ---- zero-argument tool: no input deltas at all ----
    std::string zero_arg =
        frame("content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                      {"content_block", {{"type", "tool_use"}, {"id", "toolu_0"},
                                                         {"name", "get_time"}, {"input", json::object()}}}}) +
        frame("content_block_stop", {{"type", "content_block_stop"}, {"index", 0}}) +
        frame("message_delta", {{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}}) +
        frame("message_stop", {{"type", "message_stop"}});
    {
        const auto r = run(zero_arg, 3);
        expect("zeroarg: ok", r.ok);
        expect("zeroarg: empty object input",
               r.tool_calls.size() == 1 && r.tool_calls[0].input.is_object() && r.tool_calls[0].input.empty());
    }

    // ---- refusal stop reason passes through ----
    std::string refusal =
        frame("message_delta", {{"type", "message_delta"}, {"delta", {{"stop_reason", "refusal"}}}}) +
        frame("message_stop", {{"type", "message_stop"}});
    {
        const auto r = run(refusal, refusal.size());
        expect("refusal: ok at transport level", r.ok);
        expect("refusal: stop reason surfaced", r.stop_reason == "refusal");
        expect("refusal: no content", r.text.empty() && r.tool_calls.empty());
    }

    // ---- API error event ----
    std::string errored =
        frame("content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                      {"content_block", {{"type", "text"}, {"text", ""}}}}) +
        frame("error", {{"type", "error"},
                        {"error", {{"type", "overloaded_error"}, {"message", "try again"}}}});
    {
        const auto r = run(errored, 5);
        expect("error: not ok", !r.ok);
        expect("error: message carried",
               r.error.find("overloaded_error") != std::string::npos &&
               r.error.find("try again") != std::string::npos);
    }

    // ---- truncated stream (connection drop before message_stop) ----
    {
        const auto r = run(text_stream.substr(0, text_stream.size() - 40), 9);
        expect("drop: not ok", !r.ok);
        expect("drop: says dropped", r.error.find("message_stop") != std::string::npos);
    }

    // ---- CRLF framing and [DONE] tolerance ----
    std::string crlf =
        "event: content_block_start\r\ndata: " +
        json{{"type", "content_block_start"}, {"index", 0},
             {"content_block", {{"type", "text"}, {"text", ""}}}}.dump() + "\r\n\r\n" +
        "data: " + json{{"type", "content_block_delta"}, {"index", 0},
                        {"delta", {{"type", "text_delta"}, {"text", "ok"}}}}.dump() + "\r\n\r\n" +
        "data: " + json{{"type", "message_delta"}, {"delta", {{"stop_reason", "end_turn"}}}}.dump() + "\r\n\r\n" +
        "data: " + json{{"type", "message_stop"}}.dump() + "\r\n\r\n" +
        "data: [DONE]\r\n\r\n";
    for (size_t chunk : {crlf.size(), size_t(1)}) {
        const auto r = run(crlf, chunk);
        expect("crlf: ok", r.ok);
        expect("crlf: text", r.text == "ok");
    }

    // ---- malformed frame does not take the stream down silently ----
    std::string malformed =
        "data: {not json}\n\n" + text_stream;
    {
        const auto r = run(malformed, malformed.size());
        expect("malformed: surfaces as error", !r.ok &&
               r.error.find("unparseable") != std::string::npos);
    }

    if (failures) return 1;
    std::cout << "all api-stream tests passed\n";
    return 0;
}
