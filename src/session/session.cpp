#include "session/session.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;
using nlohmann::json;

namespace lar {

const char* role_name(Role r) {
    switch (r) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        default: return "tool";
    }
}

static Role role_from(const std::string& s) {
    if (s == "user") return Role::User;
    if (s == "assistant") return Role::Assistant;
    return Role::Tool;
}

static long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

LocalSessionStore::LocalSessionStore() : dir_(app_data_dir() + "sessions\\") {
    std::error_code ec;
    fs::create_directories(utf8_to_wide(dir_), ec);
    load_all();
    writer_ = std::thread([this] { writer_main(); });
    if (sessions_.empty()) create();
    else {
        auto metas = list();
        active_ = metas.front().id;
    }
}

LocalSessionStore::~LocalSessionStore() {
    writer_queue_.shutdown();
    if (writer_.joinable()) writer_.join();

    std::vector<SessionData> snapshots;
    std::vector<std::string> deleted_ids;
    {
        std::lock_guard lk(m_);
        for (const auto& [_, session] : sessions_) snapshots.push_back(session);
        deleted_ids.assign(deleted_ids_.begin(), deleted_ids_.end());
    }
    for (auto& session : snapshots) persist_snapshot(std::move(session));

    // A shutdown intentionally discards queued writer operations. Complete
    // deletions synchronously so removed conversations cannot reappear.
    for (const auto& id : deleted_ids) {
        std::error_code ec;
        fs::remove(utf8_to_wide(dir_ + id + ".json"), ec);
    }
}

void LocalSessionStore::writer_main() {
    std::function<void()> task;
    while (writer_queue_.pop(task)) {
        try { task(); }
        catch (const std::exception& e) { log(std::string("session writer exception: ") + e.what()); }
        catch (...) { log("session writer exception: unknown"); }
    }
}

void LocalSessionStore::load_all() {
    std::lock_guard lk(m_);
    sessions_.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(utf8_to_wide(dir_), ec)) {
        if (ec || e.path().extension() != L".json") continue;
        std::ifstream f(e.path(), std::ios::binary);
        try {
            json j = json::parse(f);
            SessionData data;
            data.meta.id = j.value("id", "");
            data.meta.title = j.value("title", "new conversation");
            data.meta.updated = j.value("updated", 0LL);
            data.meta.mode = j.value("mode", "chat");
            if (data.meta.mode.empty()) data.meta.mode = "chat";
            if (data.meta.id.empty()) continue;
            if (j.contains("messages") && j["messages"].is_array()) {
                for (const auto& jm : j["messages"])
                    data.messages.push_back({
                        role_from(jm.value("role", "user")),
                        jm.value("content", ""),
                        jm.value("tool_name", ""),
                        "" // legacy on-disk traces are deliberately ignored
                    });
            }
            sessions_[data.meta.id] = std::move(data);
        } catch (...) {
            log("skipping unreadable session file: " + wide_to_utf8(e.path().wstring()));
        }
    }
}

std::vector<SessionMeta> LocalSessionStore::list() const {
    std::lock_guard lk(m_);
    std::vector<SessionMeta> out;
    out.reserve(sessions_.size());
    for (const auto& [_, s] : sessions_) out.push_back(s.meta);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.updated > b.updated; });
    return out;
}

std::string LocalSessionStore::create(const std::string& mode) {
    SessionData snapshot;
    {
        std::lock_guard lk(m_);
        snapshot.meta = { new_uuid(), "new conversation", now_s(),
                          mode.empty() ? "chat" : mode };
        active_ = snapshot.meta.id;
        sessions_[snapshot.meta.id] = snapshot;
    }
    persist_async(snapshot.meta.id);
    return snapshot.meta.id;
}

bool LocalSessionStore::set_title(const std::string& id, const std::string& title) {
    std::string clean = title;
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\n' || clean.front() == '\r')) clean.erase(0, 1);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\n' || clean.back() == '\r')) clean.pop_back();
    if (clean.empty()) return false;
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        it->second.meta.title = utf8_prefix(clean, 60);
    }
    persist_async(id);
    return true;
}

std::string LocalSessionStore::title(const std::string& id) const {
    std::lock_guard lk(m_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? std::string() : it->second.meta.title;
}

bool LocalSessionStore::select(const std::string& id) {
    std::lock_guard lk(m_);
    if (!sessions_.contains(id)) return false;
    active_ = id;
    return true;
}

bool LocalSessionStore::remove(const std::string& id) {
    bool existed = false;
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        existed = true;
        sessions_.erase(it);
        deleted_ids_.insert(id);
        if (active_ == id) {
            active_.clear();
            if (!sessions_.empty()) {
                auto best = std::max_element(sessions_.begin(), sessions_.end(),
                    [](const auto& a, const auto& b) { return a.second.meta.updated < b.second.meta.updated; });
                active_ = best->first;
            }
        }
    }
    if (existed) delete_async(id);
    bool needs_session = false;
    {
        std::lock_guard lk(m_);
        needs_session = active_.empty();
    }
    if (needs_session) create();
    return true;
}

std::string LocalSessionStore::active_id() const {
    std::lock_guard lk(m_);
    return active_;
}

std::vector<Message> LocalSessionStore::messages(const std::string& session_id) const {
    std::lock_guard lk(m_);
    auto it = sessions_.find(session_id);
    return it == sessions_.end() ? std::vector<Message>{} : it->second.messages;
}

bool LocalSessionStore::append(const std::string& session_id, const Message& msg) {
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        it->second.messages.push_back(msg);
        it->second.meta.updated = now_s();
        if (it->second.meta.title == "new conversation" && msg.role == Role::User) {
            std::string title = utf8_prefix(msg.content, 64);
            auto newline = title.find_first_of("\r\n");
            if (newline != std::string::npos) title.resize(newline);
            if (!title.empty()) it->second.meta.title = title;
        }
    }
    persist_async(session_id);
    return true;
}

bool LocalSessionStore::replace(const std::string& session_id, const std::vector<Message>& msgs) {
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        it->second.messages = msgs;
        it->second.meta.updated = now_s();
    }
    persist_async(session_id);
    return true;
}

void LocalSessionStore::persist_async(const std::string& id) {
    SessionData snapshot;
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        snapshot = it->second;
    }
    writer_queue_.push([this, snapshot = std::move(snapshot)]() mutable {
        persist_snapshot(std::move(snapshot));
    });
}

void LocalSessionStore::persist_snapshot(SessionData s) const {
    json j;
    j["id"] = s.meta.id;
    j["title"] = s.meta.title;
    j["updated"] = s.meta.updated;
    j["mode"] = s.meta.mode;
    j["messages"] = json::array();
    for (const auto& m : s.messages) {
        json jm = {
            {"role", role_name(m.role)},
            {"content", m.content},
            {"tool_name", m.tool_name}
        };
        j["messages"].push_back(std::move(jm));
    }
    const fs::path path = utf8_to_wide(dir_ + s.meta.id + ".json");
    if (!atomic_write_text(path, j.dump(1))) log("failed to persist session " + s.meta.id);
}

void LocalSessionStore::delete_async(const std::string& id) {
    const fs::path path = utf8_to_wide(dir_ + id + ".json");
    writer_queue_.push([path] {
        std::error_code ec;
        fs::remove(path, ec);
    });
}

} // namespace lar
