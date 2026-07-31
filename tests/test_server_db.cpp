// Exercises the server's state layer directly: the compare-and-swap on
// memory, exactly-once command delivery, and session round-tripping. The HTTP
// layer above it is thin route wiring, so this is where the logic worth
// testing lives - and it needs no socket, so it runs under ctest anywhere.
#include "db.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace helm;
using nlohmann::json;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    // A fresh database per run; the temp path keeps this off the real data dir.
    const fs::path dir = fs::temp_directory_path() / "helm-server-tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / "test.db").string();
    fs::remove(path, ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);

    {
        Db db(path);

        // ---- memory compare-and-swap ----
        MemoryDoc doc = db.get_memory();
        expect("fresh memory is empty", doc.text.empty());

        MemoryDoc out;
        expect("first write with empty expectation succeeds",
               db.put_memory("first entry", "", out));
        expect("write returns a version", !out.version.empty());
        const std::string v1 = out.version;

        expect("write with correct version succeeds",
               db.put_memory("second entry", v1, out));
        const std::string v2 = out.version;
        expect("version changes with content", v1 != v2);

        expect("write with stale version is refused",
               !db.put_memory("clobbering entry", v1, out));
        expect("refusal returns the current content", out.text == "second entry");
        expect("refusal returns the current version", out.version == v2);
        expect("refused write did not modify storage", db.get_memory().text == "second entry");

        expect("empty expectation still forces through",
               db.put_memory("forced", "", out));
        expect("forced write applied", db.get_memory().text == "forced");

        // Identical content must produce an identical version, or a no-op
        // save would spuriously invalidate every other reader's token.
        db.put_memory("stable", "", out);
        const std::string a = out.version;
        db.put_memory("stable", a, out);
        expect("identical content keeps the same version", a == out.version);

        // ---- commands: queued, delivered exactly once ----
        db.queue_command("box-1", "halt");
        db.queue_command("box-1", "evict");
        db.queue_command("box-2", "halt");

        auto taken = db.take_commands("box-1");
        expect("commands delivered in order",
               taken.size() == 2 && taken[0] == "halt" && taken[1] == "evict");
        expect("commands are not redelivered", db.take_commands("box-1").empty());
        expect("another instance's queue is untouched", db.take_commands("box-2").size() == 1);
        expect("unknown instance yields nothing", db.take_commands("box-404").empty());

        // ---- instances ----
        db.register_instance("box-1", "workstation");
        InstanceState st;
        st.id = "box-1";
        st.model_id = "qwen";
        st.state = "busy";
        st.vram_used = 20;
        st.vram_total = 24;
        db.heartbeat(st);

        auto list = db.instances();
        expect("instance recorded once", list.size() == 1);
        if (!list.empty()) {
            expect("heartbeat updates model", list[0].model_id == "qwen");
            expect("heartbeat updates state", list[0].state == "busy");
            expect("heartbeat stamps a time", list[0].last_heartbeat > 0);
            // A heartbeat carries no display name, so it must not erase the
            // one registration supplied.
            expect("heartbeat preserves the registered name", list[0].name == "workstation");
        }

        // An instance that heartbeats without registering is still tracked:
        // losing sight of a live machine is worse than a missing label.
        InstanceState unknown;
        unknown.id = "box-9";
        unknown.state = "idle";
        db.heartbeat(unknown);
        expect("unregistered instance is still tracked", db.instances().size() == 2);

        // ---- sessions ----
        expect("no sessions initially", db.list_sessions().empty());
        json session = {
            {"id", "s1"}, {"title", "A title with 'quotes' and \"doubles\""},
            {"updated", 1234}, {"mode", "agent"},
            {"messages", json::array({ {{"role", "user"}, {"content", "hi"}} })}
        };
        db.put_session(session);

        auto metas = db.list_sessions();
        expect("session listed", metas.size() == 1);
        if (!metas.empty()) {
            expect("title survives quoting", metas[0].title == session["title"].get<std::string>());
            expect("mode round-trips", metas[0].mode == "agent");
        }

        const json fetched = db.get_session("s1");
        expect("session fetched", !fetched.is_null());
        expect("messages round-trip",
               fetched["messages"].is_array() && fetched["messages"].size() == 1 &&
               fetched["messages"][0]["content"] == "hi");

        // Upsert, not duplicate.
        session["title"] = "renamed";
        db.put_session(session);
        expect("upsert does not duplicate", db.list_sessions().size() == 1);
        expect("upsert updates the title", db.get_session("s1")["title"] == "renamed");

        expect("missing session reads null", db.get_session("nope").is_null());
        expect("delete reports success", db.delete_session("s1"));
        expect("delete of a missing session reports failure", !db.delete_session("s1"));
        expect("session is gone", db.list_sessions().empty());

        bool threw = false;
        try { db.put_session(json{{"title", "no id"}}); } catch (...) { threw = true; }
        expect("a session without an id is rejected", threw);
    }

    // Reopening must see the persisted state - the whole point of choosing
    // SQLite over an in-process structure.
    {
        Db db(path);
        expect("memory persisted across reopen", db.get_memory().text == "stable");
        expect("instances persisted across reopen", db.instances().size() == 2);
    }

    fs::remove(path, ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);

    if (failures) return 1;
    std::cout << "all server-db tests passed\n";
    return 0;
}
