#pragma once
// In-memory session store with asynchronous, atomic persistence under
// %LOCALAPPDATA%\Helm\sessions. Every operation is explicitly session-bound.
#include "common/util.h"
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>

namespace lar {

enum class Role { User, Assistant, Tool };

struct Message {
    Role role;
    std::string content;
    std::string tool_name;
    // Native GPT-OSS Harmony output for an in-progress tool chain. Persisted so
    // the model receives its own analysis and tool-call trace on the next tool
    // continuation. Never sent to the UI, and dropped from prompts once a final
    // answer closes the chain.
    std::string harmony_raw;
};

struct SessionMeta {
    std::string id;
    std::string title;
    long long updated = 0;
};

class SessionStore {
public:
    SessionStore();
    ~SessionStore();

    std::vector<SessionMeta> list() const;
    std::string create();
    bool select(const std::string& id);
    bool remove(const std::string& id);

    std::string active_id() const;
    std::vector<Message> messages() const;
    std::vector<Message> messages(const std::string& session_id) const;
    bool append(const std::string& session_id, const Message& m);
    bool replace(const std::string& session_id, const std::vector<Message>& msgs);

private:
    struct SessionData {
        SessionMeta meta;
        std::vector<Message> messages;
    };

    void load_all();
    void persist_async(const std::string& id);
    void persist_snapshot(SessionData snapshot) const;
    void delete_async(const std::string& id);
    void writer_main();

    std::string dir_;
    mutable std::mutex m_;
    std::unordered_map<std::string, SessionData> sessions_;
    std::unordered_set<std::string> deleted_ids_;
    std::string active_;
    TaskQueue writer_queue_;
    std::thread writer_;
};

const char* role_name(Role r);

} // namespace lar
