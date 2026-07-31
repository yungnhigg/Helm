#pragma once
// Extensible workspace metadata: RAG library, transient attachments, agent
// definitions and imported open-source tool manifests.
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <filesystem>

namespace lar {

class Registry;
struct Config;

struct WorkspaceResource {
    std::string id;
    std::string name;
    std::string path;
    std::string kind; // rag | attachment | agent_config | tool_pack
    long long size = 0;
};

struct AgentProfile {
    std::string id;
    std::string name;
    std::string type; // local | task | webscraper
    std::string model_id;
    std::vector<std::string> model_ids; // future council/member models
    std::string coordinator = "single"; // single | council | router
    std::string config_resource_id;
    std::string site_url;
    // Optional narrower filesystem boundary for this reusable worker/task.
    // When set, file tools cannot read or write outside it; global write_root
    // remains the outer write boundary.
    std::string filesystem_root;
    std::vector<std::string> rag_ids;
    // Per-agent tool permissions. allowed_tools holds exact registered tool
    // names; permissions_configured distinguishes a deliberately-empty allow-set
    // from a legacy agent that predates permissions (which stays full-access).
    std::vector<std::string> allowed_tools;
    bool permissions_configured = false;
};

class WorkspaceStore {
public:
    explicit WorkspaceStore(const Config& cfg);

    std::vector<WorkspaceResource> resources(const std::string& kind = "") const;
    std::vector<WorkspaceResource> import_files(const std::vector<std::wstring>& paths, const std::string& kind);
    bool remove_resource(const std::string& id);

    std::vector<AgentProfile> agents() const;
    AgentProfile create_agent(const nlohmann::json& j);
    bool remove_agent(const std::string& id);
    // Renames in place; returns false if the agent id does not exist.
    bool rename_agent(const std::string& id, const std::string& new_name);
    bool get_agent(const std::string& id, AgentProfile& out) const;

    std::string context_for(const std::vector<std::string>& ids, const std::string& query,
                            size_t max_chars = 48000) const;
    nlohmann::json snapshot() const;
    size_t register_tool_packs(Registry& registry) const;

private:
    void load();
    void persist_locked() const;
    static bool text_like(const std::filesystem::path& p);
    std::string read_text(const std::string& path, size_t max_bytes) const;

    const Config& cfg_;
    std::string root_;
    mutable std::mutex m_;
    std::vector<WorkspaceResource> resources_;
    std::vector<AgentProfile> agents_;
};

} // namespace lar
