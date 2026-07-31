#pragma once
// Instance side of the server control plane. Registers this machine with
// helm-server, heartbeats its model state, and executes the commands that
// ride back on the heartbeat response.
//
// Connectivity is strictly outbound: this class dials the server, never
// listens. That is what lets an instance sit behind NAT or a firewall with
// no inbound path and still be reachable by the control plane.
//
// The split of responsibilities is deliberate and matches Bridge's halt
// comment: the server owns the registry and the AUTHORITY to stop work
// anywhere; only this process owns the MECHANISM to free this GPU. Commands
// therefore land on the same Bridge entry points the local buttons use -
// the Engine cannot tell who asked.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace lar {

class AgentLoop;
class Bridge;
class Engine;
struct Config;

class ServerLink {
public:
    // Starts the background thread when server_url and server_token are both
    // configured; otherwise constructs inert. References must outlive this
    // object - main.cpp tears the link down before the bridge.
    ServerLink(const Config& cfg, Engine& eng, AgentLoop& loop, Bridge& bridge);
    ~ServerLink();

    ServerLink(const ServerLink&) = delete;
    ServerLink& operator=(const ServerLink&) = delete;

    bool active() const { return worker_.joinable(); }

private:
    void run();
    bool heartbeat_once(bool& registered);
    // POST a JSON body; returns the HTTP status (0 on transport failure) and
    // fills response_body on 2xx.
    int post_json(const std::wstring& path, const std::string& body, std::string& response_body);

    const Config& cfg_;
    Engine& eng_;
    AgentLoop& loop_;
    Bridge& bridge_;

    std::string instance_id_;
    std::string instance_name_;

    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
};

} // namespace lar
