#include "agent/harmony.h"
#include "agent/prompt_builder.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <iostream>

int main() {
    nlohmann::json parsed;
    const std::string call =
        "<|start|>assistant<|channel|>analysis<|message|>Need weather.<|end|>"
        "<|start|>assistant<|channel|>commentary to=functions.search_web <|constrain|>json<|message|>"
        "{\"query\":\"current weather Jonesboro AR\",\"max_results\":3}<|call|>";
    assert(lar::parse_harmony_response(call, parsed));
    assert(parsed.at("type") == "tool_call");
    assert(parsed.at("name") == "search_web");
    assert(parsed.at("arguments").at("max_results") == 3);

    const std::string call_without_eog =
        "<|start|>assistant<|channel|>commentary to=functions.read_memory <|constrain|>json<|message|>{}";
    assert(lar::parse_harmony_response(call_without_eog, parsed));
    assert(parsed.at("name") == "read_memory");

    const std::string final =
        "<|start|>assistant<|channel|>final<|message|>It is 84 degrees and clear.<|return|>";
    assert(lar::parse_harmony_response(final, parsed));
    assert(parsed.at("type") == "reply");
    assert(parsed.at("content") == "It is 84 degrees and clear.");

    lar::Config cfg;
    cfg.system_prompt = "Use tools when needed.";
    lar::PromptBuilder builder(cfg);
    const std::string raw_trace =
        "<|channel|>analysis<|message|>Need current conditions.<|end|>"
        "<|start|>assistant<|channel|>commentary to=functions.search_web <|constrain|>json<|message|>"
        "{\"query\":\"weather Jonesboro AR\"}";
    std::vector<lar::Message> active_chain = {
        {lar::Role::User, "weather", "", ""},
        {lar::Role::Assistant, "{\"query\":\"weather Jonesboro AR\"}", "search_web", raw_trace},
        {lar::Role::Tool, "result", "search_web", ""}
    };
    const std::string continued = builder.build_harmony(active_chain, "# Tools", "", "medium");
    assert(continued.find("Need current conditions.") != std::string::npos);
    assert(continued.find("<|call|>") != std::string::npos);
    assert(continued.find("<|start|>assistant<|channel|>analysis") != std::string::npos);

    // Once a final answer closes the previous chain, its private analysis must
    // not be replayed in later turns.
    active_chain.push_back({lar::Role::Assistant, "It is clear.", "", ""});
    active_chain.push_back({lar::Role::User, "thanks", "", ""});
    const std::string after_final = builder.build_harmony(active_chain, "# Tools", "", "medium");
    assert(after_final.find("Need current conditions.") == std::string::npos);
    assert(after_final.find("It is clear.") != std::string::npos);

    std::cout << "harmony tests passed\n";
}
