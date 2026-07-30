#pragma once
// Layer 8 — tool registry. Single source of truth per tool: name, description,
// parameter schema, class (sync | job), and the C++ function. From this one
// declaration the registry generates both the prompt-side documentation and
// the output grammar, so prompt, grammar, and dispatch can never disagree.
#include "engine/grammar.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace lar {

class JobHandle; // agent/jobs.h
class MemoryStore; // session/memory.h
struct Config;   // common/config.h

enum class ToolClass { Sync, Job };

struct ToolParamDef {
    std::string name;
    ParamType   type;
    std::string description;
};

struct Tool {
    std::string name;
    std::string description;
    std::vector<ToolParamDef> params;
    ToolClass cls = ToolClass::Sync;
    // Sync: runs inline on the inference worker; must be fast. Returns result text.
    std::function<std::string(const nlohmann::json& args)> run_sync;
    // Job: runs on the tool pool; reports progress through the handle; may run
    // for most of an hour. Returns final result text.
    std::function<std::string(const nlohmann::json& args, JobHandle& job)> run_job;
};

class Registry {
public:
    bool add(Tool t) {
        if (find(t.name)) return false;
        tools_.push_back(std::move(t));
        return true;
    }
    const Tool* find(const std::string& name) const;
    const std::vector<Tool>& all() const { return tools_; }

    std::vector<GrammarTool> grammar_specs() const;
    // Markdown tool documentation appended to the system prompt.
    std::string prompt_docs() const;
    // GPT-OSS/Harmony TypeScript-style function declarations.
    std::string harmony_docs() const;

private:
    std::vector<Tool> tools_;
};

// Each tool file exports one registration function; wired up in main.cpp.
// The two that can touch the machine take the config so their limits come from
// one place instead of being baked into the tool body.
void register_tool_time(Registry&);
// Always registered. task_complete must exist in the registry or the grammar
// forbids the token sequence and an autonomous run has no reachable exit.
void register_run_control(Registry&);
void register_tool_dice(Registry&);
void register_tool_demo_job(Registry&);
void register_tool_files(Registry&, const Config&);
void register_tool_process(Registry&, const Config&);
void register_tool_web_crawl(Registry&);
void register_tool_memory(Registry&, const Config&, MemoryStore&);
void register_external_tools(Registry&, const Config&);

} // namespace lar
