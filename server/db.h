#pragma once
// Server-side state. SQLite rather than JSON files because several clients
// write concurrently and a half-written JSON document is an unrecoverable
// corruption; SQLite gives us atomic transactions and WAL concurrency for
// free.
//
// One connection guarded by one mutex. httplib serves each request on its own
// thread, so every entry point here must be callable concurrently. A single
// serialized connection is the simplest correct answer at this scale - the
// workload is a handful of instances heartbeating, not a web service.
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace helm {

struct InstanceState {
    std::string id;
    std::string name;
    long long last_heartbeat = 0;
    std::string model_id;
    std::string state;        // idle | busy | unloaded
    long long vram_used = 0;
    long long vram_total = 0;
};

struct SessionMeta {
    std::string id;
    std::string title;
    long long updated = 0;
    std::string mode;
};

// Mirrors the instance-side compare-and-swap contract: a write carrying a
// stale version is refused rather than applied.
struct MemoryDoc {
    std::string text;
    std::string version;
};

class Db {
public:
    // Opens (creating if needed) the database and applies the schema.
    // Throws std::runtime_error when the file cannot be opened.
    explicit Db(const std::string& path);
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // ---- instances ----
    void register_instance(const std::string& id, const std::string& name);
    void heartbeat(const InstanceState& s);
    std::vector<InstanceState> instances() const;
    // Commands ride the heartbeat response, so instances never listen on a
    // port: the server can reach a machine behind NAT without an inbound path.
    void queue_command(const std::string& instance_id, const std::string& command);
    std::vector<std::string> take_commands(const std::string& instance_id);

    // ---- sessions ----
    std::vector<SessionMeta> list_sessions() const;
    // Returns null when the session does not exist.
    nlohmann::json get_session(const std::string& id) const;
    void put_session(const nlohmann::json& session);
    bool delete_session(const std::string& id);

    // ---- memory ----
    MemoryDoc get_memory() const;
    // Refuses when expected_version is non-empty and does not match the
    // stored version. Returns false on that conflict; the caller answers 409.
    bool put_memory(const std::string& text, const std::string& expected_version,
                    MemoryDoc& out);

    static std::string content_version(const std::string& text);

private:
    void exec(const char* sql);

    sqlite3* db_ = nullptr;
    mutable std::mutex m_;
};

} // namespace helm
