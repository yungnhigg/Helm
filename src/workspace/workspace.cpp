#include "workspace/workspace.h"
#include "common/util.h"
#include "common/config.h"
#include "tools/external_tools.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <sstream>
#include <cctype>
#include <cwctype>

namespace fs = std::filesystem;
using nlohmann::json;

namespace lar {

static std::string safe_filename(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    return s.empty() ? "resource" : s;
}

WorkspaceStore::WorkspaceStore(const Config& cfg) : cfg_(cfg), root_(app_data_dir() + "workspace\\") {
    std::error_code ec;
    fs::create_directories(utf8_to_wide(root_ + "rag\\"), ec);
    fs::create_directories(utf8_to_wide(root_ + "attachments\\"), ec);
    fs::create_directories(utf8_to_wide(root_ + "agent-configs\\"), ec);
    fs::create_directories(utf8_to_wide(root_ + "tool-packs\\"), ec);
    load();
}

void WorkspaceStore::load() {
    std::lock_guard lk(m_);
    std::ifstream f(utf8_to_wide(root_ + "workspace.json"), std::ios::binary);
    if (!f) return;
    try {
        json j = json::parse(f);
        for (const auto& r : j.value("resources", json::array())) {
            resources_.push_back({
                r.value("id", ""), r.value("name", ""), r.value("path", ""),
                r.value("kind", ""), r.value("size", 0LL)
            });
        }
        for (const auto& a : j.value("agents", json::array())) {
            AgentProfile p;
            p.id = a.value("id", "");
            p.name = a.value("name", "Agent");
            p.type = a.value("type", "local");
            p.model_id = a.value("model_id", "");
            p.model_ids = a.value("model_ids", std::vector<std::string>{});
            if (p.model_ids.empty() && !p.model_id.empty()) p.model_ids.push_back(p.model_id);
            p.coordinator = a.value("coordinator", "single");
            p.config_resource_id = a.value("config_resource_id", "");
            p.site_url = a.value("site_url", "");
            p.filesystem_root = a.value("filesystem_root", "");
            p.rag_ids = a.value("rag_ids", std::vector<std::string>{});
            p.allowed_tools = a.value("allowed_tools", std::vector<std::string>{});
            p.permissions_configured = a.value("permissions_configured", false);
            if (!p.id.empty()) agents_.push_back(std::move(p));
        }
    } catch (const std::exception& e) {
        log(std::string("workspace load failed: ") + e.what());
    }
}

void WorkspaceStore::persist_locked() const {
    json j;
    j["resources"] = json::array();
    for (const auto& r : resources_) j["resources"].push_back({
        {"id", r.id}, {"name", r.name}, {"path", r.path}, {"kind", r.kind}, {"size", r.size}
    });
    j["agents"] = json::array();
    for (const auto& a : agents_) j["agents"].push_back({
        {"id", a.id}, {"name", a.name}, {"type", a.type}, {"model_id", a.model_id},
        {"model_ids", a.model_ids}, {"coordinator", a.coordinator}, {"config_resource_id", a.config_resource_id}, {"site_url", a.site_url},
        {"filesystem_root", a.filesystem_root}, {"rag_ids", a.rag_ids},
        {"allowed_tools", a.allowed_tools}, {"permissions_configured", a.permissions_configured}
    });
    if (!atomic_write_text(utf8_to_wide(root_ + "workspace.json"), j.dump(1)))
        log("failed to persist workspace metadata");
}

std::vector<WorkspaceResource> WorkspaceStore::resources(const std::string& kind) const {
    std::lock_guard lk(m_);
    std::vector<WorkspaceResource> out;
    for (const auto& r : resources_) if (kind.empty() || r.kind == kind) out.push_back(r);
    return out;
}

std::vector<WorkspaceResource> WorkspaceStore::import_files(const std::vector<std::wstring>& paths, const std::string& kind) {
    std::vector<WorkspaceResource> added;
    std::lock_guard lk(m_);
    const std::string folder = kind == "rag" ? "rag" : kind == "agent_config" ? "agent-configs" : kind == "tool_pack" ? "tool-packs" : "attachments";
    for (const auto& src_w : paths) {
        fs::path src(src_w);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) continue;
        WorkspaceResource r;
        r.id = new_uuid();
        r.name = wide_to_utf8(src.filename().wstring());
        r.kind = kind;
        const std::string dest_name = r.id + "-" + safe_filename(r.name);
        fs::path dest = utf8_to_wide(root_ + folder + "\\" + dest_name);
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) { log("failed to import resource: " + wide_to_utf8(src.wstring())); continue; }
        r.path = wide_to_utf8(dest.wstring());
        r.size = static_cast<long long>(fs::file_size(dest, ec));
        resources_.push_back(r);
        added.push_back(r);
    }
    persist_locked();
    return added;
}

bool WorkspaceStore::remove_resource(const std::string& id) {
    std::lock_guard lk(m_);
    auto it = std::find_if(resources_.begin(), resources_.end(), [&](const auto& r) { return r.id == id; });
    if (it == resources_.end()) return false;
    std::error_code ec;
    fs::remove(utf8_to_wide(it->path), ec);
    resources_.erase(it);
    for (auto& a : agents_) {
        a.rag_ids.erase(std::remove(a.rag_ids.begin(), a.rag_ids.end(), id), a.rag_ids.end());
        if (a.config_resource_id == id) a.config_resource_id.clear();
    }
    persist_locked();
    return true;
}

std::vector<AgentProfile> WorkspaceStore::agents() const {
    std::lock_guard lk(m_);
    return agents_;
}

AgentProfile WorkspaceStore::create_agent(const json& j) {
    AgentProfile a;
    a.id = new_uuid();
    a.name = j.value("name", "New agent");
    a.type = j.value("type", "local");
    a.model_id = j.value("model_id", "");
    a.model_ids = j.value("model_ids", std::vector<std::string>{});
    if (a.model_ids.empty() && !a.model_id.empty()) a.model_ids.push_back(a.model_id);
    a.coordinator = j.value("coordinator", "single");
    a.config_resource_id = j.value("config_resource_id", "");
    a.site_url = j.value("site_url", "");
    a.filesystem_root = j.value("filesystem_root", "");
    a.rag_ids = j.value("rag_ids", std::vector<std::string>{});
    a.allowed_tools = j.value("allowed_tools", std::vector<std::string>{});
    a.permissions_configured = j.value("permissions_configured", false);
    if (a.type != "local" && a.type != "task" && a.type != "webscraper") a.type = "local";
    std::lock_guard lk(m_);
    agents_.push_back(a);
    persist_locked();
    return a;
}

bool WorkspaceStore::remove_agent(const std::string& id) {
    std::lock_guard lk(m_);
    auto it = std::remove_if(agents_.begin(), agents_.end(), [&](const auto& a) { return a.id == id; });
    if (it == agents_.end()) return false;
    agents_.erase(it, agents_.end());
    persist_locked();
    std::error_code ec;
    fs::remove(utf8_to_wide(root_ + "agent-ledgers\\" + id + ".json"), ec);
    return true;
}

bool WorkspaceStore::rename_agent(const std::string& id, const std::string& new_name) {
    const std::string trimmed = new_name.substr(0, 80);
    if (trimmed.empty()) return false;
    std::lock_guard lk(m_);
    for (auto& a : agents_) {
        if (a.id != id) continue;
        a.name = trimmed;
        persist_locked();
        return true;
    }
    return false;
}

bool WorkspaceStore::get_agent(const std::string& id, AgentProfile& out) const {
    std::lock_guard lk(m_);
    auto it = std::find_if(agents_.begin(), agents_.end(), [&](const auto& a) { return a.id == id; });
    if (it == agents_.end()) return false;
    out = *it;
    return true;
}

bool WorkspaceStore::text_like(const fs::path& p) {
    static const std::unordered_set<std::wstring> exts = {
        L".txt", L".md", L".json", L".jsonl", L".csv", L".tsv", L".xml", L".html", L".htm",
        L".cpp", L".cc", L".c", L".h", L".hpp", L".py", L".js", L".ts", L".css", L".yaml", L".yml",
        L".toml", L".ini", L".log", L".sql", L".rs", L".go", L".java", L".cs", L".sh", L".ps1"
    };
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return exts.contains(ext);
}

std::string WorkspaceStore::read_text(const std::string& path, size_t max_bytes) const {
    fs::path p = utf8_to_wide(path);
    if (text_like(p)) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::string out(max_bytes, '\0');
        f.read(out.data(), static_cast<std::streamsize>(max_bytes));
        out.resize(static_cast<size_t>(f.gcount()));
        return out;
    }
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".pdf" || ext == L".docx" || ext == L".xlsx" || ext == L".pptx")
        return extract_document_text(cfg_, path, max_bytes);
    return {};
}

static std::vector<std::string> query_words(const std::string& q) {
    std::vector<std::string> out;
    std::string word;
    for (unsigned char c : q) {
        if (std::isalnum(c) || c == '_') word += static_cast<char>(std::tolower(c));
        else if (word.size() >= 3) { out.push_back(word); word.clear(); }
        else word.clear();
    }
    if (word.size() >= 3) out.push_back(word);
    return out;
}

std::string WorkspaceStore::context_for(const std::vector<std::string>& ids, const std::string& query, size_t max_chars) const {
    std::vector<WorkspaceResource> selected;
    {
        std::lock_guard lk(m_);
        for (const auto& id : ids) {
            auto it = std::find_if(resources_.begin(), resources_.end(), [&](const auto& r) { return r.id == id; });
            if (it != resources_.end()) selected.push_back(*it);
        }
    }
    const auto words = query_words(query);
    struct Scored { int score; WorkspaceResource r; std::string text; };
    std::vector<Scored> scored;
    std::vector<WorkspaceResource> binary;
    for (const auto& r : selected) {
        std::string text = read_text(r.path, std::min<size_t>(max_chars, 128000));
        if (text.empty()) { binary.push_back(r); continue; }
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int score = 0;
        for (const auto& w : words) {
            size_t pos = 0;
            while ((pos = lower.find(w, pos)) != std::string::npos && score < 1000) { ++score; pos += w.size(); }
        }
        scored.push_back({score, r, std::move(text)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    std::ostringstream out;
    size_t used = 0;
    for (const auto& r : binary) {
        const std::string line = "\nATTACHED NON-TEXT FILE: " + r.name +
            " (content extraction is not configured for this format; use its metadata only unless a multimodal/tool adapter is available).";
        if (used + line.size() > max_chars) break;
        out << line;
        used += line.size();
    }
    for (const auto& s : scored) {
        const std::string header = "\n\n--- RAG FILE: " + s.r.name + " ---\n";
        if (used + header.size() >= max_chars) break;
        out << header;
        used += header.size();
        const size_t take = std::min(max_chars - used, s.text.size());
        out << s.text.substr(0, take);
        used += take;
        if (used >= max_chars) break;
    }
    return out.str();
}

json WorkspaceStore::snapshot() const {
    std::lock_guard lk(m_);
    json r = json::array();
    for (const auto& x : resources_) r.push_back({
        {"id", x.id}, {"name", x.name}, {"kind", x.kind}, {"size", x.size}
    });
    json a = json::array();
    for (const auto& x : agents_) a.push_back({
        {"id", x.id}, {"name", x.name}, {"type", x.type}, {"model_id", x.model_id},
        {"model_ids", x.model_ids}, {"coordinator", x.coordinator}, {"config_resource_id", x.config_resource_id}, {"site_url", x.site_url},
        {"filesystem_root", x.filesystem_root}, {"rag_ids", x.rag_ids},
        {"allowed_tools", x.allowed_tools}, {"permissions_configured", x.permissions_configured}
    });
    return {{"resources", r}, {"agents", a}};
}

} // namespace lar
