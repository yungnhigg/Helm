#pragma once
// The grammar forces every generation into one of two byte-exact envelopes:
//
//   {"type":"reply","thinking":"...","content":"..."}
//   {"type":"tool_call","thinking":"...","note":"...","name":"...","arguments":{...}}
//
// This filter watches the raw token stream and routes each JSON string value to
// its own channel, un-escaping incrementally so the window streams clean text
// live. "thinking" goes to a collapsible pane, "note" is the model narrating a
// tool call, "content" is the visible reply. Everything from "name" onward is
// suppressed — the bridge announces the call once loop.cpp parses the whole
// envelope.
//
// If the output does not match either envelope (grammar disabled, or a model
// ignoring it) the filter degrades to passthrough on the content channel rather
// than swallowing the response.
//
// Header-only; pure state machine, no allocation beyond the output strings.
#include <string>
#include <cstdint>
#include <cstdlib>

namespace lar {

struct FilterOut {
    std::string content;  // visible reply text
    std::string thinking; // reasoning channel
    std::string note;     // tool-call narration

    bool empty() const { return content.empty() && thinking.empty() && note.empty(); }
};

class StreamFilter {
public:
    enum class Kind { Undecided, Reply, ToolCall, Raw };

    // Feed a chunk, get back whatever became displayable.
    FilterOut feed(const std::string& chunk) {
        FilterOut out;
        for (char c : chunk) step(c, out);
        return out;
    }

    Kind kind() const { return kind_; }

private:
    static constexpr const char* REPLY_PREFIX = "{\"type\":\"reply\",";
    static constexpr const char* TOOL_PREFIX  = "{\"type\":\"tool_call\",";

    enum class Channel { None, Content, Thinking, Note };

    std::string* channel_target(FilterOut& out) {
        switch (channel_) {
            case Channel::Content:  return &out.content;
            case Channel::Thinking: return &out.thinking;
            case Channel::Note:     return &out.note;
            default:                return nullptr;
        }
    }

    void step(char c, FilterOut& out) {
        switch (state_) {
        case State::Prefix: {
            buf_ += c;
            const bool reply_ok = starts(REPLY_PREFIX);
            const bool tool_ok  = starts(TOOL_PREFIX);
            if (!reply_ok && !tool_ok) {      // grammar off or violated: pass raw
                kind_ = Kind::Raw;
                state_ = State::Passthrough;
                out.content += buf_;
                return;
            }
            if (reply_ok && buf_.size() == strlen_c(REPLY_PREFIX)) {
                kind_ = Kind::Reply;
                state_ = State::Key;
                key_.clear();
            } else if (tool_ok && buf_.size() == strlen_c(TOOL_PREFIX)) {
                kind_ = Kind::ToolCall;
                state_ = State::Key;
                key_.clear();
            }
            return;
        }

        case State::Key:
            // Accumulate the quoted key up to its colon: "thinking":
            if (c != ':') { key_ += c; return; }
            if      (key_ == "\"content\"")  channel_ = Channel::Content;
            else if (key_ == "\"thinking\"") channel_ = Channel::Thinking;
            else if (key_ == "\"note\"")     channel_ = Channel::Note;
            else { state_ = State::Suppress; return; } // "name" and beyond
            key_.clear();
            state_ = State::ValueOpen;
            return;

        case State::ValueOpen:
            // The grammar guarantees a string here; anything else is malformed.
            if (c != '"') { state_ = State::Suppress; return; }
            state_ = State::Value;
            return;

        case State::Value: {
            std::string* dst = channel_target(out);
            if (!dst) { state_ = State::Suppress; return; }
            if (esc_) { unescape(c, *dst); esc_ = false; return; }
            if (c == '\\') { esc_ = true; return; }
            if (high_) { append_utf8(0xFFFD, *dst); high_ = 0; }
            if (c == '"') {                       // value closed
                channel_ = Channel::None;
                state_ = State::AfterValue;
                return;
            }
            *dst += c;
            return;
        }

        case State::Hex: {
            std::string* dst = channel_target(out);
            hex_ += c;
            if (hex_.size() < 4) return;
            uint32_t cp = (uint32_t)strtoul(hex_.c_str(), nullptr, 16);
            hex_.clear();
            state_ = State::Value;
            if (!dst) return;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (high_) append_utf8(0xFFFD, *dst);
                high_ = cp;
                return;
            }
            if (cp >= 0xDC00 && cp <= 0xDFFF) {
                if (high_) {
                    cp = 0x10000 + ((high_ - 0xD800) << 10) + (cp - 0xDC00);
                    high_ = 0;
                    append_utf8(cp, *dst);
                } else {
                    append_utf8(0xFFFD, *dst);
                }
                return;
            }
            if (high_) { append_utf8(0xFFFD, *dst); high_ = 0; }
            append_utf8(cp, *dst);
            return;
        }

        case State::AfterValue:
            // Either another key follows, or the envelope closes.
            if (c == ',') { state_ = State::Key; key_.clear(); return; }
            state_ = State::Suppress;   // '}' or anything unexpected
            return;

        case State::Suppress:
            return;

        case State::Passthrough:
            out.content += c;
            return;
        }
    }

    void unescape(char c, std::string& dst) {
        if (high_ && c != 'u') { append_utf8(0xFFFD, dst); high_ = 0; }
        switch (c) {
            case 'n': dst += '\n'; break;
            case 't': dst += '\t'; break;
            case 'r': dst += '\r'; break;
            case 'b': dst += '\b'; break;
            case 'f': dst += '\f'; break;
            case 'u': state_ = State::Hex; break;
            default:  dst += c; break; // " \ /
        }
    }

    static void append_utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool starts(const char* prefix) const {
        const size_t n = strlen_c(prefix);
        const size_t m = buf_.size() < n ? buf_.size() : n;
        return buf_.compare(0, m, prefix, m) == 0;
    }
    static constexpr size_t strlen_c(const char* s) {
        size_t n = 0; while (s[n]) ++n; return n;
    }

    enum class State { Prefix, Key, ValueOpen, Value, Hex, AfterValue, Suppress, Passthrough };
    State state_ = State::Prefix;
    Kind kind_ = Kind::Undecided;
    Channel channel_ = Channel::None;
    std::string buf_, key_, hex_;
    uint32_t high_ = 0;
    bool esc_ = false;
};

} // namespace lar
