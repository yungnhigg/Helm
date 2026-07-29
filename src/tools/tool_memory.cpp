#include "agent/registry.h"
#include "session/memory.h"
#include "common/config.h"
#include "common/util.h"

namespace lar {

// Agent-mode counterpart to the /remember and /forget operators. The operators
// are the deterministic path and work in chat mode too, where no tools exist;
// these let the model act on "add that to memory", where resolving "that"
// needs the conversation.
void register_tool_memory(Registry& r, const Config& cfg, MemoryStore& memory) {
    MemoryStore* m = &memory;
    const Config* c = &cfg;

    r.add({
        "remember",
        "Save a durable fact to long-term memory, available in every future conversation. "
        "Use only when the user asks you to remember something. Write one self-contained "
        "fact per call, phrased so it still makes sense with no surrounding context.",
        {{"fact", ParamType::String, "A single self-contained fact to store"}},
        ToolClass::Sync,
        [m, c](const nlohmann::json& a) {
            if (!c->enable_memory) return std::string("error: long-term memory is disabled in Settings");
            const auto st = m->append(a.at("fact").get<std::string>());
            log(std::string("memory append: ") + st.message);
            return st.ok ? ("remembered (" + std::to_string(st.bytes) + "/" +
                            std::to_string(st.budget) + " bytes used)")
                         : ("error: " + st.message);
        },
        {}
    });

    r.add({
        "recall_memory",
        "Read back everything currently in long-term memory. Memory is already included in "
        "your context each turn, so use this only to confirm what is stored before editing it.",
        {},
        ToolClass::Sync,
        [m, c](const nlohmann::json&) {
            if (!c->enable_memory) return std::string("error: long-term memory is disabled in Settings");
            const std::string text = m->text();
            return text.empty() ? std::string("long-term memory is empty") : text;
        },
        {}
    });

    r.add({
        "forget",
        "Remove long-term memory entries containing the given text. Confirm with the user "
        "before calling this; it cannot be undone from here.",
        {{"match", ParamType::String, "Text to match against stored entries"}},
        ToolClass::Sync,
        [m, c](const nlohmann::json& a) {
            if (!c->enable_memory) return std::string("error: long-term memory is disabled in Settings");
            const auto st = m->forget(a.at("match").get<std::string>());
            log(std::string("memory forget: ") + st.message);
            return st.ok ? st.message : ("error: " + st.message);
        },
        {}
    });
}

} // namespace lar
