#include "agent/work_ledger.h"
#include "agent/run_guard.h"
#include "common/util.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;
using nlohmann::json;

namespace lar {
namespace {

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool should_record(const std::string& tool_name) {
    return tool_name != "get_time" && tool_name != "roll_dice" &&
           tool_name != "demo_job" && tool_name != "task_complete";
}

std::string status_for(const std::string& result) {
    std::string head = concise_tool_result_summary(result, 96);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (head.empty()) return "empty";
    if (head.rfind("error:", 0) == 0 || head.rfind("failed", 0) == 0 ||
        head.find(" failed:") != std::string::npos) return "error";
    if (head.rfind("cancelled", 0) == 0 || head.find(" cancelled:") != std::string::npos)
        return "cancelled";
    return "ok";
}

} // namespace

AgentWorkLedger::AgentWorkLedger()
    : root_(app_data_dir() + "workspace\\agent-ledgers\\") {
    std::error_code ec;
    fs::create_directories(utf8_to_wide(root_), ec);
}

std::string AgentWorkLedger::safe_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
    }
    return out.empty() ? "unknown" : out;
}

std::string AgentWorkLedger::path_for(const std::string& agent_id) const {
    return root_ + safe_id(agent_id) + ".json";
}

void AgentWorkLedger::load_locked(const std::string& agent_id) const {
    if (loaded_.contains(agent_id)) return;
    loaded_.insert(agent_id);
    std::ifstream f(utf8_to_wide(path_for(agent_id)), std::ios::binary);
    if (!f) return;
    try {
        const json j = json::parse(f);
        auto& list = entries_[agent_id];
        for (const auto& item : j.value("entries", json::array())) {
            Entry e;
            e.task_key = item.value("task_key", "");
            e.tool_name = item.value("tool_name", "");
            e.signature = item.value("signature", "");
            e.resource_key = item.value("resource_key", "");
            e.result_fingerprint = item.value("result_fingerprint", "");
            e.status = item.value("status", "ok");
            e.summary = item.value("summary", "");
            e.updated = item.value("updated", 0LL);
            if (!e.signature.empty()) list.push_back(std::move(e));
        }
    } catch (const std::exception& e) {
        log(std::string("agent ledger load failed: ") + e.what());
    }
}

void AgentWorkLedger::persist_locked(const std::string& agent_id) const {
    json j;
    j["agent_id"] = agent_id;
    j["entries"] = json::array();
    auto it = entries_.find(agent_id);
    if (it != entries_.end()) {
        for (const auto& e : it->second) {
            j["entries"].push_back({
                {"task_key", e.task_key}, {"tool_name", e.tool_name},
                {"signature", e.signature}, {"resource_key", e.resource_key},
                {"result_fingerprint", e.result_fingerprint}, {"status", e.status},
                {"summary", e.summary}, {"updated", e.updated}
            });
        }
    }
    const std::string encoded = j.dump(1, ' ', false, json::error_handler_t::replace);
    if (!atomic_write_text(utf8_to_wide(path_for(agent_id)), encoded))
        log("failed to persist agent work ledger " + agent_id);
}

void AgentWorkLedger::record(const std::string& agent_id,
                             const std::string& task_key,
                             const std::string& tool_name,
                             const json& arguments,
                             const std::string& result,
                             std::size_t max_entries) {
    if (agent_id.empty() || !should_record(tool_name)) return;
    Entry incoming;
    incoming.task_key = task_key;
    incoming.tool_name = tool_name;
    incoming.signature = canonical_tool_call_signature(tool_name, arguments);
    incoming.resource_key = canonical_tool_resource_key(tool_name, arguments);
    incoming.result_fingerprint = stable_text_fingerprint(result);
    incoming.status = status_for(result);
    incoming.summary = concise_tool_result_summary(result);
    incoming.updated = now_seconds();

    std::lock_guard lk(m_);
    load_locked(agent_id);
    auto& list = entries_[agent_id];
    auto existing = std::find_if(list.begin(), list.end(), [&](const Entry& e) {
        return e.task_key == incoming.task_key && e.signature == incoming.signature;
    });
    if (existing != list.end()) *existing = std::move(incoming);
    else list.push_back(std::move(incoming));

    std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        return a.updated < b.updated;
    });
    if (max_entries == 0) list.clear();
    else if (list.size() > max_entries)
        list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(max_entries));
    persist_locked(agent_id);
}

std::string AgentWorkLedger::prompt_block(const std::string& agent_id,
                                          const std::string& task_key,
                                          std::size_t max_chars,
                                          std::size_t max_entries) const {
    if (agent_id.empty() || max_chars == 0 || max_entries == 0) return {};
    std::lock_guard lk(m_);
    load_locked(agent_id);
    auto it = entries_.find(agent_id);
    if (it == entries_.end()) return {};

    std::ostringstream body;
    std::size_t used = 0;
    std::size_t count = 0;
    for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry) {
        if (!task_key.empty() && entry->task_key != task_key) continue;
        const std::string identity = entry->resource_key.empty() ? entry->signature : entry->resource_key;
        std::string line = "- [" + entry->status + "] " + entry->tool_name + " | " +
                           identity + " | " + entry->summary + "\n";
        if (used + line.size() > max_chars || count >= max_entries) break;
        body << line;
        used += line.size();
        ++count;
    }
    if (!count) return {};

    return "Agent work ledger (recent verified progress for this task). Continue from it and avoid redundant work; revisit an item only when a refresh, a different range, or a materially different method is needed:\n" + body.str();
}

void AgentWorkLedger::seed_watchdog(const std::string& agent_id,
                                    const std::string& task_key,
                                    ProgressWatchdog& watchdog,
                                    std::size_t max_entries) const {
    if (agent_id.empty() || max_entries == 0) return;
    std::lock_guard lk(m_);
    load_locked(agent_id);
    auto it = entries_.find(agent_id);
    if (it == entries_.end()) return;

    std::size_t count = 0;
    std::unordered_set<std::string> seeded;
    for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry) {
        if (!task_key.empty() && entry->task_key != task_key) continue;
        if (entry->resource_key.empty() || !seeded.insert(entry->resource_key).second) continue;
        watchdog.seed_resource_observation(entry->resource_key, entry->result_fingerprint,
                                           entry->status != "ok");
        if (++count >= max_entries) break;
    }
}

void AgentWorkLedger::remove_agent(const std::string& agent_id) {
    if (agent_id.empty()) return;
    std::lock_guard lk(m_);
    entries_.erase(agent_id);
    loaded_.erase(agent_id);
    std::error_code ec;
    fs::remove(utf8_to_wide(path_for(agent_id)), ec);
}

} // namespace lar
