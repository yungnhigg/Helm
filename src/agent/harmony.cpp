#include "agent/harmony.h"
#include <nlohmann/json.hpp>
#include <cctype>

using nlohmann::json;

namespace lar {
namespace {

std::string trim_copy(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

} // namespace

bool parse_harmony_response(const std::string& raw, json& output) {
    // Some conversions still emit Helm's JSON envelope even when the filename
    // says GPT-OSS, so keep that as a compatibility fallback.
    try {
        output = json::parse(trim_copy(raw));
        if (output.is_object()) return true;
    } catch (...) {}

    const std::string recipient = "to=functions.";
    const size_t tool_at = raw.rfind(recipient);
    if (tool_at != std::string::npos) {
        const size_t name_begin = tool_at + recipient.size();
        size_t name_end = name_begin;
        while (name_end < raw.size()) {
            const unsigned char c = static_cast<unsigned char>(raw[name_end]);
            if (!(std::isalnum(c) || c == '_' || c == '-')) break;
            ++name_end;
        }
        const size_t message = raw.find("<|message|>", name_end);
        if (name_end > name_begin && message != std::string::npos) {
            const size_t args_begin = message + std::string("<|message|>").size();
            size_t args_end = raw.find("<|call|>", args_begin);
            if (args_end == std::string::npos) args_end = raw.find("<|end|>", args_begin);
            if (args_end == std::string::npos) args_end = raw.size();
            const std::string args_text = trim_copy(raw.substr(args_begin, args_end - args_begin));
            try {
                output = {{"type", "tool_call"},
                          {"name", raw.substr(name_begin, name_end - name_begin)},
                          {"arguments", args_text.empty() ? json::object() : json::parse(args_text)}};
                return true;
            } catch (...) {}
        }
    }

    const size_t final_channel = raw.rfind("<|channel|>final");
    if (final_channel != std::string::npos) {
        const size_t message = raw.find("<|message|>", final_channel);
        if (message != std::string::npos) {
            const size_t begin = message + std::string("<|message|>").size();
            size_t end = raw.find("<|return|>", begin);
            const size_t normal_end = raw.find("<|end|>", begin);
            if (end == std::string::npos || (normal_end != std::string::npos && normal_end < end)) end = normal_end;
            if (end == std::string::npos) end = raw.size();
            output = {{"type", "reply"}, {"content", trim_copy(raw.substr(begin, end - begin))}};
            return true;
        }
    }
    return false;
}

} // namespace lar
