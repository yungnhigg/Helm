// helm-server: the registry and authority half of Helm's multi-instance
// design. Instances own the mechanism - only the process holding a model can
// free that GPU - while this server owns the registry and the authority to
// ask any of them to stop.
//
// Connectivity is outbound-only from the instance side: instances register and
// heartbeat, and commands ride back on the heartbeat response. Nothing here
// dials into an instance, so no machine needs an inbound path.
//
// Transport is plain HTTP by design. This is meant to run inside an existing
// WireGuard tunnel, which already provides encryption and peer authentication;
// terminating TLS here would add a certificate lifecycle for no security gain.
// The bearer token is what stops a device that reaches the tunnel from issuing
// a kill to somebody else's GPU.
#include "db.h"
#include <httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace helm;

namespace {

constexpr int kDefaultPort = 8733;
// An instance that has not checked in for this long is reported offline. Two
// missed 5s heartbeats plus slack: long enough to ride out a GC pause or a
// blocked message pump, short enough that a dead box does not look alive.
constexpr long long kOfflineAfterSeconds = 20;

long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string data_dir() {
    char* base = nullptr;
    size_t len = 0;
    std::string root;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") == 0 && base) {
        root = base;
        free(base);
    }
    if (root.empty()) root = ".";
    const std::string dir = root + "\\Helm\\server\\";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// 32 bytes from the OS CSPRNG, hex-encoded. rand() would be a plausible-looking
// way to make the whole auth story worthless.
std::string generate_token() {
    unsigned char bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("BCryptGenRandom failed - refusing to run with a weak token");
    std::string out;
    out.reserve(sizeof(bytes) * 2);
    for (unsigned char b : bytes) {
        char pair[3];
        std::snprintf(pair, sizeof(pair), "%02x", b);
        out += pair;
    }
    return out;
}

std::string load_or_create_token(const std::string& dir) {
    const std::string path = dir + "token.txt";
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string token((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' '))
                token.pop_back();
            if (!token.empty()) return token;
        }
    }
    const std::string token = generate_token();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << token;
    out.close();
    std::cout << "Generated a new access token at " << path << std::endl;
    return token;
}

// Length-independent comparison. A byte-at-a-time early return leaks how much
// of a guessed token was correct, which is enough to walk one byte at a time.
bool token_matches(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

bool authorized(const httplib::Request& req, const std::string& token) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return false;
    const std::string& value = it->second;
    constexpr const char* prefix = "Bearer ";
    if (value.rfind(prefix, 0) != 0) return false;
    return token_matches(value.substr(std::char_traits<char>::length(prefix)), token);
}

void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& message) {
    send_json(res, status, json{{"error", message}});
}

// Every mutating body is model- or network-supplied. Parse defensively and
// answer 400 rather than letting an exception escape into a 500.
bool parse_body(const httplib::Request& req, httplib::Response& res, json& out) {
    try {
        out = json::parse(req.body);
        if (!out.is_object()) { send_error(res, 400, "body must be a JSON object"); return false; }
        return true;
    } catch (const std::exception& e) {
        send_error(res, 400, std::string("invalid JSON: ") + e.what());
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string bind = "127.0.0.1";
    int port = kDefaultPort;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--bind" || arg == "-b") && i + 1 < argc) bind = argv[++i];
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "helm-server - registry and control plane for Helm instances\n\n"
                "  --bind <address>   interface to listen on (default 127.0.0.1)\n"
                "  --port <number>    port to listen on (default " << kDefaultPort << ")\n\n"
                "Bind to the WireGuard interface address to reach it from your\n"
                "other devices. Do NOT port-forward this to the internet: the\n"
                "tunnel is the encryption layer, and the bearer token assumes\n"
                "the transport is already private.\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    std::string dir, token;
    try {
        dir = data_dir();
        token = load_or_create_token(dir);
    } catch (const std::exception& e) {
        std::cerr << "startup failed: " << e.what() << "\n";
        return 1;
    }

    std::unique_ptr<Db> db;
    try {
        db = std::make_unique<Db>(dir + "helm-server.db");
    } catch (const std::exception& e) {
        std::cerr << "database failed: " << e.what() << "\n";
        return 1;
    }

    httplib::Server server;

    // Liveness only, deliberately unauthenticated and deliberately empty: it
    // says the process is up without telling an unauthenticated caller
    // anything about the instances or their models.
    server.Get("/v1/health", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, json{{"ok", true}});
    });

    server.set_pre_routing_handler([&token](const httplib::Request& req, httplib::Response& res) {
        if (req.path == "/v1/health") return httplib::Server::HandlerResponse::Unhandled;
        if (authorized(req, token)) return httplib::Server::HandlerResponse::Unhandled;
        send_error(res, 401, "missing or invalid bearer token");
        return httplib::Server::HandlerResponse::Handled;
    });

    // ---------------------------------------------------------- instances
    server.Post("/v1/instances/register", [&db](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) return;
        const std::string id = body.value("instance_id", std::string());
        if (id.empty()) { send_error(res, 400, "instance_id is required"); return; }
        db->register_instance(id, body.value("name", std::string()));
        send_json(res, 200, json{{"ok", true}});
    });

    server.Post("/v1/instances/heartbeat", [&db](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) return;
        InstanceState st;
        st.id = body.value("instance_id", std::string());
        if (st.id.empty()) { send_error(res, 400, "instance_id is required"); return; }
        st.name = body.value("name", std::string());
        st.model_id = body.value("model_id", std::string());
        st.state = body.value("state", std::string("unknown"));
        st.vram_used = body.value("vram_used", 0LL);
        st.vram_total = body.value("vram_total", 0LL);
        db->heartbeat(st);
        // The response is the only channel back to the instance.
        send_json(res, 200, json{{"ok", true}, {"commands", db->take_commands(st.id)}});
    });

    server.Get("/v1/instances", [&db](const httplib::Request&, httplib::Response& res) {
        const long long cutoff = now_s() - kOfflineAfterSeconds;
        json list = json::array();
        for (const auto& s : db->instances()) {
            list.push_back({{"id", s.id}, {"name", s.name},
                            {"last_heartbeat", s.last_heartbeat},
                            {"model_id", s.model_id},
                            {"state", s.state},
                            {"online", s.last_heartbeat >= cutoff},
                            {"vram_used", s.vram_used}, {"vram_total", s.vram_total}});
        }
        send_json(res, 200, json{{"instances", list}});
    });

    // halt stops generation and ends agent loops; evict additionally frees the
    // VRAM. Distinct because the server wants both: stop what you are doing,
    // versus give me the card back.
    auto command_route = [&db](const char* command) {
        return [&db, command](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.path_params.at("id");
            if (id.empty()) { send_error(res, 400, "instance id is required"); return; }
            db->queue_command(id, command);
            // Queued, not performed: the instance executes it on its next
            // heartbeat. Saying "ok" here means "accepted", and the caller
            // confirms the effect by polling /v1/instances.
            send_json(res, 202, json{{"queued", command}, {"instance_id", id}});
        };
    };
    // httplib's own ":name" path-parameter syntax. A regex with named capture
    // groups is not an option: patterns without "/:" go to RegexMatcher, and
    // MSVC's std::regex has no named groups - it throws at registration, which
    // kills the process before it ever listens.
    server.Post("/v1/instances/:id/halt", command_route("halt"));
    server.Post("/v1/instances/:id/evict", command_route("evict"));

    // ---------------------------------------------------------- sessions
    server.Get("/v1/sessions", [&db](const httplib::Request&, httplib::Response& res) {
        json list = json::array();
        for (const auto& m : db->list_sessions())
            list.push_back({{"id", m.id}, {"title", m.title}, {"updated", m.updated}, {"mode", m.mode}});
        send_json(res, 200, json{{"sessions", list}});
    });

    server.Get("/v1/sessions/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        const json session = db->get_session(req.path_params.at("id"));
        if (session.is_null()) { send_error(res, 404, "no such session"); return; }
        send_json(res, 200, session);
    });

    server.Put("/v1/sessions/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) return;
        // The path is authoritative, so a body disagreeing with it cannot
        // write to a different session than the URL names.
        body["id"] = req.path_params.at("id");
        try { db->put_session(body); }
        catch (const std::exception& e) { send_error(res, 400, e.what()); return; }
        send_json(res, 200, json{{"ok", true}});
    });

    server.Delete("/v1/sessions/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        if (!db->delete_session(req.path_params.at("id"))) { send_error(res, 404, "no such session"); return; }
        send_json(res, 200, json{{"ok", true}});
    });

    // ---------------------------------------------------------- memory
    server.Get("/v1/memory", [&db](const httplib::Request&, httplib::Response& res) {
        const MemoryDoc doc = db->get_memory();
        send_json(res, 200, json{{"text", doc.text}, {"version", doc.version}});
    });

    server.Put("/v1/memory", [&db](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_body(req, res, body)) return;
        MemoryDoc doc;
        const bool ok = db->put_memory(body.value("text", std::string()),
                                       body.value("expected_version", std::string()), doc);
        if (!ok) {
            // 409 with the current state, so the caller can merge rather than
            // guess what it collided with.
            send_json(res, 409, json{{"error", "memory changed since it was read"},
                                     {"text", doc.text}, {"version", doc.version}});
            return;
        }
        send_json(res, 200, json{{"ok", true}, {"version", doc.version}});
    });

    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "internal error";
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { message = e.what(); }
        catch (...) {}
        res.status = 500;
        res.set_content(json{{"error", message}}.dump(), "application/json");
    });

    std::cout << "helm-server listening on http://" << bind << ":" << port << "\n"
              << "  data:  " << dir << "\n"
              << "  token: " << dir << "token.txt\n";
    if (bind != "127.0.0.1" && bind != "localhost") {
        std::cout << "  note:  bound to a non-loopback address - make sure this is your\n"
                     "         WireGuard interface and not an internet-facing one.\n";
    }
    // listen() blocks forever, so anything still sitting in the stdout buffer
    // would never appear in a pipe or a redirected log - including on an
    // abnormal exit, which is exactly when the operator needs to read it.
    std::cout << std::flush;

    if (!server.listen(bind, port)) {
        std::cerr << "failed to listen on " << bind << ":" << port
                  << " (port in use, or the address is not an interface on this machine)\n";
        return 1;
    }
    return 0;
}
