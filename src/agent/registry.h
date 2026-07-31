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
#include <unordered_set>

namespace lar {

class JobHandle; // agent/jobs.h
class IMemoryStore; // session/memory.h
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
    // Optional semantic validation that cannot be expressed by the grammar
    // (numeric ranges, mutually exclusive flags, path policy, and so on).
    // Empty means valid; a non-empty string is returned to the model as an
    // error before any sync function or background job is started. Kept last so
    // existing aggregate tool declarations remain source-compatible.
    std::function<std::string(const nlohmann::json& args)> validate;
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

    // An empty allow-set means "all tools" (Chat mode and legacy agents). A
    // non-empty set restricts generation to exactly those names, so a permission
    // system controls what the model can even see and emit, not just what it is
    // told not to do. The three generators share one predicate so grammar,
    // prompt docs, and Harmony declarations can never expose different sets.
    using AllowSet = std::unordered_set<std::string>;
    std::vector<GrammarTool> grammar_specs(const AllowSet& allow = {}) const;
    std::string prompt_docs(const AllowSet& allow = {}) const;
    std::string harmony_docs(const AllowSet& allow = {}) const;

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
void register_tool_memory(Registry&, const Config&, IMemoryStore&);
void register_external_tools(Registry&, const Config&);

} // namespace lar
