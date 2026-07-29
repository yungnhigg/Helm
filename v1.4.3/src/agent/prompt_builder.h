#pragma once
#include "common/config.h"
#include "session/session.h"
#include <string>
#include <vector>

namespace lar {

class PromptBuilder {
public:
    explicit PromptBuilder(const Config& cfg) : cfg_(cfg) {}
    std::string build(const std::vector<Message>& msgs, const std::string& tool_docs,
                      const std::string& extra_system = {}) const;
    // GPT-OSS native envelope. Selected by Engine::harmony_mode(), not by config:
    // the format is a property of the loaded weights, not a user preference.
    std::string build_harmony(const std::vector<Message>& msgs, const std::string& harmony_tools,
                              const std::string& extra_system, const std::string& effort) const;
    // Neutralises Harmony boundaries in untrusted text. Public because any code
    // that builds a Harmony prompt outside this class needs it too.
    static std::string harmony_escape(std::string text);
private:
    std::string render(const Message& m) const;
    static std::string normalize_harmony_trace(std::string raw);
    static std::string render_harmony(const Message& m, bool preserve_trace);
    const Config& cfg_;
};

} // namespace lar
