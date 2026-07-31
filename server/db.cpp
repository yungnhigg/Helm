#include "db.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

using nlohmann::json;

namespace helm {
namespace {

long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// RAII for a prepared statement. Every query below goes through bound
// parameters - no string-built SQL anywhere, so a session title containing a
// quote is data, not syntax.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind(int i, const std::string& v) {
        sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind(int i, long long v) { sqlite3_bind_int64(s_, i, v); }

    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("step failed: ") + sqlite3_errstr(rc));
    }

    std::string text(int col) const {
        const unsigned char* p = sqlite3_column_text(s_, col);
        return p ? reinterpret_cast<const char*>(p) : std::string();
    }
    long long integer(int col) const { return sqlite3_column_int64(s_, col); }

    sqlite3_stmt* raw() { return s_; }

private:
    sqlite3_stmt* s_ = nullptr;
};

} // namespace

std::string Db::content_version(const std::string& text) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= static_cast<unsigned long long>(c);
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", hash);
    return out;
}

Db::Db(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string err = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open database " + path + ": " + err);
    }
    // WAL lets readers proceed during a write; busy_timeout replaces an
    // immediate SQLITE_BUSY with a bounded wait, which is what a
    // several-writer workload wants.
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    sqlite3_busy_timeout(db_, 5000);

    exec("CREATE TABLE IF NOT EXISTS instances ("
         " id TEXT PRIMARY KEY,"
         " name TEXT NOT NULL DEFAULT '',"
         " last_heartbeat INTEGER NOT NULL DEFAULT 0,"
         " model_id TEXT NOT NULL DEFAULT '',"
         " state TEXT NOT NULL DEFAULT 'unknown',"
         " vram_used INTEGER NOT NULL DEFAULT 0,"
         " vram_total INTEGER NOT NULL DEFAULT 0)");

    exec("CREATE TABLE IF NOT EXISTS sessions ("
         " id TEXT PRIMARY KEY,"
         " title TEXT NOT NULL DEFAULT '',"
         " updated INTEGER NOT NULL DEFAULT 0,"
         " mode TEXT NOT NULL DEFAULT 'chat',"
         " messages TEXT NOT NULL DEFAULT '[]')");

    exec("CREATE TABLE IF NOT EXISTS memory ("
         " id INTEGER PRIMARY KEY CHECK (id = 1),"
         " content TEXT NOT NULL DEFAULT '',"
         " version TEXT NOT NULL DEFAULT '')");

    exec("CREATE TABLE IF NOT EXISTS commands ("
         " seq INTEGER PRIMARY KEY AUTOINCREMENT,"
         " instance_id TEXT NOT NULL,"
         " command TEXT NOT NULL,"
         " created INTEGER NOT NULL)");
    exec("CREATE INDEX IF NOT EXISTS idx_commands_instance ON commands(instance_id, seq)");
}

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

void Db::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sql failed: " + message);
    }
}

void Db::register_instance(const std::string& id, const std::string& name) {
    std::lock_guard lk(m_);
    Stmt s(db_, "INSERT INTO instances (id, name, last_heartbeat) VALUES (?, ?, ?)"
                " ON CONFLICT(id) DO UPDATE SET name = excluded.name,"
                " last_heartbeat = excluded.last_heartbeat");
    s.bind(1, id);
    s.bind(2, name);
    s.bind(3, now_s());
    s.step();
}

void Db::heartbeat(const InstanceState& st) {
    std::lock_guard lk(m_);
    // An instance that heartbeats without having registered is still recorded:
    // losing sight of a live machine is worse than a missing display name.
    Stmt s(db_, "INSERT INTO instances (id, name, last_heartbeat, model_id, state, vram_used, vram_total)"
                " VALUES (?, ?, ?, ?, ?, ?, ?)"
                " ON CONFLICT(id) DO UPDATE SET last_heartbeat = excluded.last_heartbeat,"
                " model_id = excluded.model_id, state = excluded.state,"
                " vram_used = excluded.vram_used, vram_total = excluded.vram_total,"
                " name = CASE WHEN excluded.name = '' THEN instances.name ELSE excluded.name END");
    s.bind(1, st.id);
    s.bind(2, st.name);
    s.bind(3, now_s());
    s.bind(4, st.model_id);
    s.bind(5, st.state);
    s.bind(6, st.vram_used);
    s.bind(7, st.vram_total);
    s.step();
}

std::vector<InstanceState> Db::instances() const {
    std::lock_guard lk(m_);
    std::vector<InstanceState> out;
    Stmt s(db_, "SELECT id, name, last_heartbeat, model_id, state, vram_used, vram_total"
                " FROM instances ORDER BY name, id");
    while (s.step()) {
        InstanceState st;
        st.id = s.text(0);
        st.name = s.text(1);
        st.last_heartbeat = s.integer(2);
        st.model_id = s.text(3);
        st.state = s.text(4);
        st.vram_used = s.integer(5);
        st.vram_total = s.integer(6);
        out.push_back(std::move(st));
    }
    return out;
}

void Db::queue_command(const std::string& instance_id, const std::string& command) {
    std::lock_guard lk(m_);
    Stmt s(db_, "INSERT INTO commands (instance_id, command, created) VALUES (?, ?, ?)");
    s.bind(1, instance_id);
    s.bind(2, command);
    s.bind(3, now_s());
    s.step();
}

std::vector<std::string> Db::take_commands(const std::string& instance_id) {
    std::lock_guard lk(m_);
    std::vector<std::string> out;
    std::vector<long long> seqs;
    {
        Stmt s(db_, "SELECT seq, command FROM commands WHERE instance_id = ? ORDER BY seq");
        s.bind(1, instance_id);
        while (s.step()) {
            seqs.push_back(s.integer(0));
            out.push_back(s.text(1));
        }
    }
    // Delivered exactly once: a halt replayed on the next heartbeat would
    // cancel a turn the user started in the meantime.
    for (long long seq : seqs) {
        Stmt del(db_, "DELETE FROM commands WHERE seq = ?");
        del.bind(1, seq);
        del.step();
    }
    return out;
}

std::vector<SessionMeta> Db::list_sessions() const {
    std::lock_guard lk(m_);
    std::vector<SessionMeta> out;
    Stmt s(db_, "SELECT id, title, updated, mode FROM sessions ORDER BY updated DESC");
    while (s.step()) {
        SessionMeta m;
        m.id = s.text(0);
        m.title = s.text(1);
        m.updated = s.integer(2);
        m.mode = s.text(3);
        out.push_back(std::move(m));
    }
    return out;
}

json Db::get_session(const std::string& id) const {
    std::lock_guard lk(m_);
    Stmt s(db_, "SELECT id, title, updated, mode, messages FROM sessions WHERE id = ?");
    s.bind(1, id);
    if (!s.step()) return json();
    json out;
    out["id"] = s.text(0);
    out["title"] = s.text(1);
    out["updated"] = s.integer(2);
    out["mode"] = s.text(3);
    // Stored as text and parsed on read. A malformed blob must not take the
    // whole response down, so it degrades to an empty history.
    try { out["messages"] = json::parse(s.text(4)); }
    catch (...) { out["messages"] = json::array(); }
    return out;
}

void Db::put_session(const json& session) {
    const std::string id = session.value("id", std::string());
    if (id.empty()) throw std::runtime_error("session has no id");
    const json messages = session.contains("messages") && session["messages"].is_array()
        ? session["messages"] : json::array();

    std::lock_guard lk(m_);
    Stmt s(db_, "INSERT INTO sessions (id, title, updated, mode, messages) VALUES (?, ?, ?, ?, ?)"
                " ON CONFLICT(id) DO UPDATE SET title = excluded.title,"
                " updated = excluded.updated, mode = excluded.mode, messages = excluded.messages");
    s.bind(1, id);
    s.bind(2, session.value("title", std::string()));
    s.bind(3, static_cast<long long>(session.value("updated", 0LL)));
    s.bind(4, session.value("mode", std::string("chat")));
    s.bind(5, messages.dump());
    s.step();
}

bool Db::delete_session(const std::string& id) {
    std::lock_guard lk(m_);
    Stmt s(db_, "DELETE FROM sessions WHERE id = ?");
    s.bind(1, id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

MemoryDoc Db::get_memory() const {
    std::lock_guard lk(m_);
    Stmt s(db_, "SELECT content, version FROM memory WHERE id = 1");
    if (!s.step()) return { "", content_version("") };
    return { s.text(0), s.text(1) };
}

bool Db::put_memory(const std::string& text, const std::string& expected_version, MemoryDoc& out) {
    std::lock_guard lk(m_);
    std::string current, current_version;
    {
        Stmt s(db_, "SELECT content, version FROM memory WHERE id = 1");
        if (s.step()) { current = s.text(0); current_version = s.text(1); }
        else current_version = content_version("");
    }
    if (!expected_version.empty() && expected_version != current_version) {
        out = { current, current_version };
        return false;
    }
    const std::string version = content_version(text);
    Stmt s(db_, "INSERT INTO memory (id, content, version) VALUES (1, ?, ?)"
                " ON CONFLICT(id) DO UPDATE SET content = excluded.content,"
                " version = excluded.version");
    s.bind(1, text);
    s.bind(2, version);
    s.step();
    out = { text, version };
    return true;
}

} // namespace helm
