#include "api/anthropic_stream.h"

using nlohmann::json;

namespace lar {

void AnthropicStreamParser::feed(const char* data, size_t len) {
    buffer_.append(data, len);

    // SSE frames end with a blank line. The data: line carries JSON whose
    // own "type" field duplicates the event: line, so the event name line is
    // informational and the JSON is authoritative.
    for (;;) {
        size_t frame_end = buffer_.find("\n\n");
        size_t skip = 2;
        const size_t crlf = buffer_.find("\r\n\r\n");
        if (crlf != std::string::npos && (frame_end == std::string::npos || crlf < frame_end)) {
            frame_end = crlf;
            skip = 4;
        }
        if (frame_end == std::string::npos) return;

        const std::string frame = buffer_.substr(0, frame_end);
        buffer_.erase(0, frame_end + skip);

        size_t pos = 0;
        while (pos < frame.size()) {
            size_t eol = frame.find('\n', pos);
            if (eol == std::string::npos) eol = frame.size();
            std::string line = frame.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue;   // event:/ping lines
            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
            if (payload.empty() || payload == "[DONE]") continue;
            try {
                handle_event(json::parse(payload));
            } catch (const std::exception& e) {
                // One malformed frame must not poison the stream; record and
                // keep reading - the terminal check in finish() decides.
                if (error_.empty()) error_ = std::string("unparseable stream frame: ") + e.what();
            }
        }
    }
}

void AnthropicStreamParser::handle_event(const json& event) {
    const std::string type = event.value("type", "");

    if (type == "content_block_start") {
        const size_t index = event.value("index", blocks_.size());
        if (blocks_.size() <= index) {
            blocks_.resize(index + 1, json::object());
            partial_inputs_.resize(index + 1);
        }
        blocks_[index] = event.value("content_block", json::object());
        // tool_use inputs arrive as string deltas, not in the start event;
        // begin from empty rather than the placeholder {} the start carries.
        if (blocks_[index].value("type", "") == "tool_use") partial_inputs_[index].clear();
        return;
    }

    if (type == "content_block_delta") {
        const size_t index = event.value("index", 0);
        if (blocks_.size() <= index) {
            blocks_.resize(index + 1, json::object());
            partial_inputs_.resize(index + 1);
        }
        const json& delta = event.value("delta", json::object());
        const std::string kind = delta.value("type", "");
        if (kind == "text_delta") {
            const std::string piece = delta.value("text", "");
            blocks_[index]["text"] = blocks_[index].value("text", "") + piece;
            if (on_text && !piece.empty()) on_text(piece);
        } else if (kind == "thinking_delta") {
            const std::string piece = delta.value("thinking", "");
            blocks_[index]["thinking"] = blocks_[index].value("thinking", "") + piece;
            if (on_thinking && !piece.empty()) on_thinking(piece);
        } else if (kind == "input_json_delta") {
            partial_inputs_[index] += delta.value("partial_json", "");
        } else if (kind == "signature_delta") {
            blocks_[index]["signature"] = blocks_[index].value("signature", "") +
                                          delta.value("signature", "");
        }
        return;
    }

    if (type == "content_block_stop") {
        const size_t index = event.value("index", 0);
        if (index < blocks_.size() && blocks_[index].value("type", "") == "tool_use") {
            // Empty accumulation means a zero-argument tool.
            const std::string& raw = partial_inputs_[index];
            try { blocks_[index]["input"] = raw.empty() ? json::object() : json::parse(raw); }
            catch (...) {
                blocks_[index]["input"] = json::object();
                if (error_.empty()) error_ = "tool_use input was not valid JSON: " + raw;
            }
        }
        return;
    }

    if (type == "message_delta") {
        const json& delta = event.value("delta", json::object());
        if (delta.contains("stop_reason") && !delta["stop_reason"].is_null())
            stop_reason_ = delta["stop_reason"].get<std::string>();
        return;
    }

    if (type == "message_stop") { message_stopped_ = true; return; }

    if (type == "error") {
        const json& err = event.value("error", json::object());
        error_ = err.value("type", "api_error") + ": " + err.value("message", "unknown error");
        return;
    }

    // message_start, ping, and anything newer are fine to ignore.
}

ApiTurnResult AnthropicStreamParser::finish() {
    ApiTurnResult result;
    result.stop_reason = stop_reason_;
    result.content = json::array();
    for (auto& block : blocks_) {
        if (block.empty()) continue;
        const std::string type = block.value("type", "");
        if (type == "text") result.text += block.value("text", "");
        if (type == "tool_use") {
            ApiToolCall call;
            call.id = block.value("id", "");
            call.name = block.value("name", "");
            call.input = block.contains("input") ? block["input"] : json::object();
            result.tool_calls.push_back(std::move(call));
        }
        result.content.push_back(std::move(block));
    }

    if (!error_.empty()) { result.error = error_; return result; }
    if (!message_stopped_) {
        result.error = "stream ended before message_stop (connection dropped?)";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace lar
