#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lar {

class ProgressWatchdog;

// Persistent, bounded progress memory for each reusable agent. It records what
// resources were examined or changed without relying on the model to remember
// to call a special tool. Only entries relevant to the active agent/task are
// injected into the prompt.
class AgentWorkLedger {
public:
    AgentWorkLedger();

    void record(const std::string& agent_id,
                const std::string& task_key,
                const std::string& tool_name,
                const nlohmann::json& arguments,
                const std::string& result,
                std::size_t max_entries = 512);

    std::string prompt_block(const std::string& agent_id,
                             const std::string& task_key,
                             std::size_t max_chars = 4096,
                             std::size_t max_entries = 24) const;
    void seed_watchdog(const std::string& agent_id,
                       const std::string& task_key,
                       ProgressWatchdog& watchdog,
                       std::size_t max_entries = 24) const;

    void remove_agent(const std::string& agent_id);

private:
    struct Entry {
        std::string task_key;
        std::string tool_name;
        std::string signature;
        std::string resource_key;
        std::string result_fingerprint;
        std::string status;
        std::string summary;
        long long updated = 0;
    };

    static std::string safe_id(const std::string& id);
    std::string path_for(const std::string& agent_id) const;
    void load_locked(const std::string& agent_id) const;
    void persist_locked(const std::string& agent_id) const;

    std::string root_;
    mutable std::mutex m_;
    mutable std::unordered_map<std::string, std::vector<Entry>> entries_;
    mutable std::unordered_set<std::string> loaded_;
};

} // namespace lar
