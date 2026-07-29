#include "agent/prompt_builder.h"
#include <nlohmann/json.hpp>
#include <ctime>
#include <iomanip>
#include <sstream>

using nlohmann::json;

namespace lar {

std::string PromptBuilder::render(const Message& m) const {
    const auto& t = cfg_.tmpl;
    switch (m.role) {
        case Role::User:
            return t.user_prefix + m.content + t.user_suffix;
        case Role::Assistant: {
            // History must look exactly like what the model emits, so the
            // clean stored content is re-wrapped into the output envelope.
            std::string envelope;
            if (m.tool_name.empty()) {
                envelope = "{\"type\":\"reply\",\"content\":" + json(m.content).dump() + "}";
            } else {
                std::string args = "{}";
                try { args = json::parse(m.content).dump(); } catch (...) {}
                envelope = "{\"type\":\"tool_call\",\"name\":" + json(m.tool_name).dump() +
                           ",\"arguments\":" + args + "}";
            }
            return t.assistant_prefix + envelope + t.assistant_suffix;
        }
        case Role::Tool: {
            // Tool results ride in on a configured role, wrapped so the model
            // can distinguish them from user text.
            std::string body = t.tool_result_open + "[" + m.tool_name + "] " + m.content + t.tool_result_close;
            if (t.tool_result_role == "assistant")
                return t.assistant_prefix + body + t.assistant_suffix;
            return t.user_prefix + body + t.user_suffix;
        }
    }
    return {};
}

std::string PromptBuilder::build(const std::vector<Message>& msgs, const std::string& tool_docs,
                                 const std::string& extra_system) const {
    const auto& t = cfg_.tmpl;
    std::string system = cfg_.system_prompt + tool_docs;
    if (!extra_system.empty()) system += "\n\n## Current workspace\n" + extra_system;
    std::string out = t.system_prefix + system + t.system_suffix;
    for (const auto& m : msgs) out += render(m);
    out += t.assistant_prefix; // generation point
    return out;
}

// ---------------------------------------------------------------- Harmony
// GPT-OSS speaks its own envelope. Rather than bending it into the ChatML
// template, this renders it natively: private reasoning on the analysis
// channel, tool calls on commentary, the user-visible answer on final.

std::string PromptBuilder::harmony_escape(std::string text) {
    // Stop user, tool, or retrieved text from closing a Harmony boundary and
    // injecting a channel of its own.
    size_t pos = 0;
    while ((pos = text.find("<|", pos)) != std::string::npos) {
        text.insert(pos + 1, "\xE2\x80\x8B"); // zero-width space
        pos += 4;
    }
    return text;
}

std::string PromptBuilder::normalize_harmony_trace(std::string raw) {
    // llama.cpp generation begins immediately after `<|start|>assistant`, so
    // converted GGUFs commonly return a trace beginning at `<|channel|>`.
    // Restore the missing prefix and the tool-call stop token before replaying
    // the trace on the next continuation.
    if (raw.empty()) return raw;
    if (!raw.starts_with("<|start|>assistant")) raw.insert(0, "<|start|>assistant");
    if (raw.find("to=functions.") != std::string::npos &&
        raw.find("<|call|>") == std::string::npos &&
        raw.find("<|return|>") == std::string::npos) {
        raw += "<|call|>";
    }
    return raw;
}

std::string PromptBuilder::render_harmony(const Message& m, bool preserve_trace) {
    const std::string content = harmony_escape(m.content);
    if (m.role == Role::User)
        return "<|start|>user<|message|>" + content + "<|end|>";
    if (m.role == Role::Assistant) {
        if (m.tool_name.empty())
            return "<|start|>assistant<|channel|>final<|message|>" + content + "<|end|>";
        if (preserve_trace && !m.harmony_raw.empty())
            return normalize_harmony_trace(m.harmony_raw);
        std::string args = "{}";
        try { args = json::parse(m.content).dump(); } catch (...) {}
        return "<|start|>assistant<|channel|>commentary to=functions." + m.tool_name +
               " <|constrain|>json<|message|>" + args + "<|call|>";
    }
    // Compression writes its summary on the tool rail, but no function by that
    // name exists. Rendering it as a function return would advertise a tool the
    // model cannot call, so it goes in as user-supplied context instead.
    if (m.tool_name == "conversation_summary")
        return "<|start|>user<|message|>[summary of earlier conversation] " + content + "<|end|>";
    return "<|start|>functions." + m.tool_name +
           " to=assistant<|channel|>commentary<|message|>" + content + "<|end|>";
}

static std::string current_date_string() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d");
    return out.str();
}

std::string PromptBuilder::build_harmony(const std::vector<Message>& msgs, const std::string& harmony_tools,
                                         const std::string& extra_system, const std::string& effort) const {
    const std::string reasoning = effort == "low" || effort == "high" ? effort : "medium";
    std::string out;
    out += "<|start|>system<|message|>You are ChatGPT, a large language model trained by OpenAI.\n";
    out += "Knowledge cutoff: 2024-06\n";
    out += "Current date: " + current_date_string() + "\n\n";
    out += "Reasoning: " + reasoning + "\n\n";
    out += "# Valid channels: analysis, commentary, final. Channel must be included for every message.\n";
    out += "Calls to these tools must go to the commentary channel: 'functions'.<|end|>";

    out += "<|start|>developer<|message|># Instructions\n\n";
    out += harmony_escape(cfg_.system_prompt);
    if (!extra_system.empty()) out += "\n\n## Current workspace\n" + harmony_escape(extra_system);
    out += "\n\n" + harmony_escape(harmony_tools) + "<|end|>";

    // GPT-OSS needs its own analysis messages preserved between tool calls,
    // but prior chains must not be replayed after a final answer. Find the most
    // recent final and preserve raw traces only in the active chain after it.
    size_t last_final = msgs.size();
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role == Role::Assistant && msgs[i].tool_name.empty()) last_final = i;
    }
    for (size_t i = 0; i < msgs.size(); ++i) {
        const bool active_chain = last_final == msgs.size() || i > last_final;
        out += render_harmony(msgs[i], active_chain);
    }
    out += "<|start|>assistant";
    return out;
}

} // namespace lar
