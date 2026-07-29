#pragma once
#include "common/util.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <functional>
#include <string>
#include <atomic>

namespace lar {

class Engine;
class AgentLoop;
class JobManager;
class SessionStore;
class WorkspaceStore;
class Registry;
struct Config;

constexpr UINT WM_APP_EMIT = WM_APP + 1;

class Bridge {
public:
    Bridge(Config& cfg, Engine& eng, AgentLoop& loop, JobManager& jobs, SessionStore& store,
           MemoryStore& memory,
           WorkspaceStore& workspace, Registry& reg, HWND hwnd,
           std::function<void(const std::wstring&)> post_to_webview);

    void emit(const nlohmann::json& j) noexcept;
    void on_web_message(const std::wstring& raw);
    void on_wm_app_emit(LPARAM lp) noexcept;

private:
    void send_status();
    void send_models();
    void send_sessions();
    void send_history(const std::string& session_id = {});
    void send_workspace();
    void send_tools();
    void send_settings();
    void send_memory();
    void handle_slash(const std::string& name, const std::string& args, const std::string& session_id);
    void import_files(const std::string& kind, const std::wstring& title, bool multiple,
                      const std::vector<std::pair<std::wstring, std::wstring>>& filters);

    Config& cfg_;
    Engine& eng_;
    AgentLoop& loop_;
    JobManager& jobs_;
    SessionStore& store_;
    MemoryStore& memory_;
    WorkspaceStore& workspace_;
    Registry& reg_;
    HWND hwnd_;
    std::function<void(const std::wstring&)> post_;
    std::atomic<bool> model_transition_{false};
};

} // namespace lar
