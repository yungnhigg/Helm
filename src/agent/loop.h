#pragma once
#include "agent/jobs.h"
#include "agent/prompt_builder.h"
#include "agent/registry.h"
#include "engine/engine.h"
#include "session/session.h"
#include "session/memory.h"
#include "workspace/workspace.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <mutex>

namespace lar {

struct AgentEvents {
    std::function<void(const nlohmann::json&)> emit;
    void send(const char* type, nlohmann::json j = {}) const {
        j["type"] = type;
        if (emit) emit(j);
    }
};

struct TurnOptions {
    std::string mode = "chat";       // chat | agent
    std::string effort = "medium";   // low | medium | high
    std::string agent_id;
    std::vector<std::string> resource_ids;
    // Autonomous run. A plain reply is progress, not a finish line: the loop
    // keeps driving until the model calls task_complete or the iteration budget
    // is spent. Prompting a model not to stop does not work, because the C++
    // returns on a reply no matter what the model was told.
    bool autonomous = false;
};

class AgentLoop {
public:
    AgentLoop(const Config& cfg, Engine& eng, Registry& reg, SessionStore& store,
              WorkspaceStore& workspace, JobManager& jobs, MemoryStore& memory, AgentEvents ev);

    void user_turn(const std::string& session_id, const std::string& text, TurnOptions options);
    bool busy() const { return busy_.load(); }
    bool refresh_tools();
    bool has_in_flight_work() const {
        return busy_.load() || pending_followups_.load() > 0 || jobs_.active_count() > 0;
    }
    void cancel_job(int id) { jobs_.cancel(id); }

private:
    void run_turn(const std::string& session_id, const TurnOptions& options);
    std::vector<Message> trimmed_history(const std::string& session_id,
                                         const std::string& tool_docs,
                                         const std::string& extra_system,
                                         int generation_tokens,
                                         bool harmony,
                                         const std::string& effort);
    bool compress_history(const std::string& session_id, std::vector<Message>& msgs, bool harmony);
    // Cap a tool result so no single one can swallow the context window.
    std::string clamp_tool_result(const std::string& text, const std::string& tool_name) const;
    std::string workspace_prompt(const TurnOptions& options, const std::string& query) const;
    void send_for(const std::string& session_id, const char* type, nlohmann::json j = {}) const;
    void schedule_followup(const std::string& session_id, TurnOptions options);
    void enqueue_followup(const std::string& session_id, TurnOptions options);
    int generation_limit(const std::string& effort) const;

    const Config& cfg_;
    Engine& eng_;
    Registry& reg_;
    SessionStore& store_;
    WorkspaceStore& workspace_;
    MemoryStore& memory_;
    JobManager& jobs_;
    AgentEvents ev_;
    std::string agent_grammar_;
    std::string tool_docs_;
    std::string harmony_docs_;
    std::atomic<bool> busy_{false};
    std::atomic<int> pending_followups_{0};
};

} // namespace lar
