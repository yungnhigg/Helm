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
    // Native GPT-OSS Harmony output for the active in-memory tool chain. The
    // model needs it on the next continuation, but it is never written to disk,
    // is discarded on restart, and is not replayed after a final answer.
    std::string harmony_raw;
};

struct SessionMeta {
    std::string id;
    std::string title;
    long long updated = 0;
    // Which workspace surface owns this conversation: chat | agent | api | agora.
    // Fixed at creation; the session picker filters on it. Kept LAST so the
    // positional aggregate init in create() stays valid for older fields.
    std::string mode = "chat";
};

// The store seam for Phase E's server backend: everything that consumes
// sessions talks to this interface. LocalSessionStore owns today's on-disk
// JSON format; a RemoteSessionStore will speak the server protocol behind
// the same ten methods.
class ISessionStore {
public:
    virtual ~ISessionStore() = default;

    virtual std::vector<SessionMeta> list() const = 0;
    virtual std::string create(const std::string& mode = "chat") = 0;
    virtual bool select(const std::string& id) = 0;
    virtual bool remove(const std::string& id) = 0;
    // Explicit titles: agent sessions carry their agent's name, and chat
    // sessions get a model-written summary after the first exchange. A title
    // set here wins over append()'s first-message placeholder derivation.
    virtual bool set_title(const std::string& id, const std::string& title) = 0;
    virtual std::string title(const std::string& id) const = 0;

    virtual std::string active_id() const = 0;
    virtual std::vector<Message> messages(const std::string& session_id) const = 0;
    virtual bool append(const std::string& session_id, const Message& m) = 0;
    virtual bool replace(const std::string& session_id, const std::vector<Message>& msgs) = 0;
};

class LocalSessionStore final : public ISessionStore {
public:
    LocalSessionStore();
    ~LocalSessionStore() override;

    std::vector<SessionMeta> list() const override;
    std::string create(const std::string& mode = "chat") override;
    bool select(const std::string& id) override;
    bool remove(const std::string& id) override;
    bool set_title(const std::string& id, const std::string& title) override;
    std::string title(const std::string& id) const override;

    std::string active_id() const override;
    std::vector<Message> messages(const std::string& session_id) const override;
    bool append(const std::string& session_id, const Message& m) override;
    bool replace(const std::string& session_id, const std::vector<Message>& msgs) override;

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
