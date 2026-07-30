#include "ui/bridge.h"
#include "ui/file_dialog.h"
#include "agent/loop.h"
#include "agent/jobs.h"
#include "agent/registry.h"
#include "engine/engine.h"
#include "session/session.h"
#include "workspace/workspace.h"
#include "common/config.h"
#include "session/memory.h"
#include "common/util.h"
#include "tools/external_tools.h"
#include <memory>
#include <utility>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <shellapi.h>

using nlohmann::json;

namespace lar {

Bridge::Bridge(Config& cfg, Engine& eng, AgentLoop& loop, JobManager& jobs, SessionStore& store,
               MemoryStore& memory,
               WorkspaceStore& workspace, Registry& reg, HWND hwnd,
               std::function<void(const std::wstring&)> post)
    : cfg_(cfg), eng_(eng), loop_(loop), jobs_(jobs), store_(store), memory_(memory),
      workspace_(workspace), reg_(reg), hwnd_(hwnd), post_(std::move(post)) {}

void Bridge::send_memory() {
    emit({{"type", "memory"},
          {"text", memory_.text()},
          {"bytes", memory_.bytes()},
          {"budget", memory_.budget()},
          {"enabled", cfg_.enable_memory},
          {"path", memory_.path()}});
}

// Operator commands. Handled here rather than in the agent loop so they are
// deterministic and work identically in chat mode, where the model has no tools
// at all. The model is never consulted; nothing can be misinterpreted.
void Bridge::handle_slash(const std::string& name, const std::string& args,
                          const std::string& session_id) {
    auto reply = [&](const std::string& text, bool ok = true) {
        emit({{"type", "operator_result"}, {"session_id", session_id},
              {"name", name}, {"ok", ok}, {"text", text}});
    };

    if (name == "remember") {
        if (!cfg_.enable_memory) { reply("Long-term memory is disabled in Settings.", false); return; }
        if (args.empty()) { reply("Usage: /remember <fact to keep>", false); return; }
        const auto st = memory_.append(args);
        reply(st.ok ? (st.message + " — " + std::to_string(st.bytes) + "/" +
                       std::to_string(st.budget) + " bytes used")
                    : st.message, st.ok);
        send_memory();
        return;
    }

    if (name == "forget") {
        if (!cfg_.enable_memory) { reply("Long-term memory is disabled in Settings.", false); return; }
        if (args.empty()) { reply("Usage: /forget <text to match>", false); return; }
        const auto st = memory_.forget(args);
        reply(st.message, st.ok);
        send_memory();
        return;
    }

    if (name == "memory") {
        const std::string text = memory_.text();
        reply(text.empty() ? "Long-term memory is empty."
                           : ("Long-term memory (" + std::to_string(memory_.bytes()) + "/" +
                              std::to_string(memory_.budget()) + " bytes):\n\n" + text));
        return;
    }

    if (name == "tools") {
        std::string out = "Registered tools:\n";
        for (const auto& t : reg_.all()) out += "  " + t.name + " — " + t.description.substr(0, 90) + "\n";
        out += "\nDisabled groups appear here but return an error when called.";
        out += std::string("\n  web: ")      + (cfg_.enable_web_tools ? "on" : "off");
        out += std::string("\n  image: ")    + (cfg_.enable_image_tools ? "on" : "off");
        out += std::string("\n  voice: ")    + (cfg_.enable_voice_tools ? "on" : "off");
        out += std::string("\n  document: ") + (cfg_.enable_document_tools ? "on" : "off");
        out += std::string("\n  desktop: ")  + (cfg_.enable_desktop_tools ? "on" : "off");
        out += std::string("\n  process: ")  + (cfg_.allow_process_tools ? "on" : "off");
        out += std::string("\n  memory: ")   + (cfg_.enable_memory ? "on" : "off");
        reply(out);
        return;
    }

    reply("Unknown command: /" + name, false);
}

void Bridge::emit(const json& j) noexcept {
    try {
        auto* payload = new std::string(j.dump());
        if (!PostMessageW(hwnd_, WM_APP_EMIT, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    } catch (const std::exception& e) {
        log(std::string("bridge emit failed: ") + e.what());
    } catch (...) {
        log("bridge emit failed: unknown");
    }
}

void Bridge::on_wm_app_emit(LPARAM lp) noexcept {
    try {
        std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lp));
        if (payload && post_) post_(utf8_to_wide(*payload));
    } catch (const std::exception& e) {
        log(std::string("bridge delivery failed: ") + e.what());
    } catch (...) {
        log("bridge delivery failed: unknown");
    }
}

void Bridge::send_status() {
    emit({{"type", "status"},
          {"model_loaded", eng_.loaded()},
          {"active_model_id", cfg_.active_model_id},
          {"model_path", cfg_.model_path},
          {"n_ctx", eng_.loaded() ? eng_.n_ctx() : cfg_.n_ctx},
          {"busy", loop_.busy()}});
}

void Bridge::send_models() {
    json list = json::array();
    for (const auto& m : cfg_.models)
        list.push_back({{"id", m.id}, {"name", m.name}, {"path", m.path}});
    emit({{"type", "models"}, {"list", list}, {"active", cfg_.active_model_id}});
}

void Bridge::send_sessions() {
    json list = json::array();
    for (const auto& m : store_.list())
        list.push_back({{"id", m.id}, {"title", m.title}, {"updated", m.updated}});
    emit({{"type", "sessions"}, {"list", list}, {"active", store_.active_id()}});
}

void Bridge::send_history(const std::string& requested) {
    const std::string id = requested.empty() ? store_.active_id() : requested;
    json messages = json::array();
    for (const auto& m : store_.messages(id))
        messages.push_back({{"role", role_name(m.role)}, {"content", m.content}, {"tool_name", m.tool_name}});
    emit({{"type", "history"}, {"session_id", id}, {"messages", messages}});
}

void Bridge::send_workspace() {
    json j = workspace_.snapshot();
    j["type"] = "workspace";
    emit(j);
}

void Bridge::send_tools() {
    json tools = json::array();
    for (const auto& t : reg_.all())
        tools.push_back({{"name", t.name}, {"description", t.description}, {"job", t.cls == ToolClass::Job}});
    emit({{"type", "tools"}, {"list", tools}});
}


void Bridge::send_settings() {
    namespace fs = std::filesystem;
    auto exists = [](const std::string& path) {
        std::error_code ec;
        return !path.empty() && fs::exists(utf8_to_wide(path), ec);
    };
    emit({{"type", "settings"},
          {"values", {
              {"n_ctx", cfg_.n_ctx}, {"n_gpu_layers", cfg_.n_gpu_layers},
              {"n_batch", cfg_.n_batch}, {"n_ubatch", cfg_.n_ubatch},
              {"n_threads", cfg_.n_threads}, {"n_threads_batch", cfg_.n_threads_batch},
              {"flash_attention", cfg_.flash_attention}, {"kv_cache_location", cfg_.kv_cache_location},
              {"kv_cache_type", cfg_.kv_cache_type}, {"tool_root", cfg_.tool_root},
              {"comfyui_url", cfg_.comfyui_url}, {"comfyui_workflow", cfg_.comfyui_workflow},
              {"archive_db", cfg_.archive_db}, {"archive_shards", cfg_.archive_shards},
              {"write_root", cfg_.write_root},
              {"enable_web_tools", cfg_.enable_web_tools}, {"enable_image_tools", cfg_.enable_image_tools},
              {"enable_voice_tools", cfg_.enable_voice_tools}, {"enable_document_tools", cfg_.enable_document_tools},
              {"enable_desktop_tools", cfg_.enable_desktop_tools}, {"enable_compression", cfg_.enable_compression},
              {"enable_archive_tools", cfg_.enable_archive_tools}
          }},
          {"detected", {
              {"python", exists(cfg_.resolved_tool_python())}, {"ffmpeg", exists(cfg_.resolved_ffmpeg())},
              {"whisper", exists(cfg_.resolved_whisper())}, {"whisper_model", exists(cfg_.resolved_whisper_model())},
              {"piper", exists(cfg_.resolved_piper())}, {"piper_voice", exists(cfg_.resolved_piper_voice())},
              {"comfy_workflow", exists(cfg_.resolved_comfyui_workflow())},
              // Headless Chromium is what lets tools read JavaScript-rendered
              // pages. Without it the web tools silently return empty pages,
              // so its absence needs to be visible rather than inferred.
              {"browser", exists(cfg_.tool_root + "\\Playwright-Browsers")},
              {"archive", exists(cfg_.archive_db)}
          }}});
}

static std::vector<unsigned char> decode_base64(const std::string& input) {
    static const signed char table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    std::vector<unsigned char> out;
    int value = 0, bits = -8;
    for (unsigned char c : input) {
        if (c >= 128) continue;
        const int d = table[c];
        if (d == -2) break;
        if (d < 0) continue;
        value = (value << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

void Bridge::import_files(const std::string& kind, const std::wstring& title, bool multiple,
                          const std::vector<std::pair<std::wstring, std::wstring>>& filters) {
    const auto paths = pick_files(hwnd_, multiple, title, filters);
    if (paths.empty()) return;
    const auto added = workspace_.import_files(paths, kind);
    json list = json::array();
    for (const auto& r : added)
        list.push_back({{"id", r.id}, {"name", r.name}, {"kind", r.kind}, {"size", r.size}});
    emit({{"type", "files_picked"}, {"kind", kind}, {"list", list}});
    send_workspace();
}

void Bridge::on_web_message(const std::wstring& raw) {
    json j;
    try { j = json::parse(wide_to_utf8(raw)); }
    catch (...) { log("bridge: bad inbound json"); return; }
    try {
        const std::string type = j.value("type", "");

    if (type == "ui_ready") {
        send_status(); send_models(); send_sessions(); send_history(); send_workspace(); send_tools(); send_settings();
        send_memory();
        // Reopen whatever model was last in use. Done here rather than at
        // startup so the window is already listening and shows "Loading model…"
        // instead of appearing frozen. Guarded on the file still existing —
        // GGUFs get moved.
        if (!eng_.loaded() && !model_transition_.load()) {
            const std::string path = cfg_.resolved_model_path();
            std::error_code ec;
            if (!path.empty() && std::filesystem::exists(std::filesystem::path(utf8_to_wide(path)), ec)) {
                model_transition_.store(true);
                emit({{"type", "model_loading"}, {"id", cfg_.active_model_id}});
                log("preloading last-used model: " + path);
                eng_.load([this](bool ok, const std::string& err) {
                    if (!ok) emit({{"type", "error"}, {"message", err}});
                    model_transition_.store(false);
                    send_status(); send_models();
                });
            } else if (!path.empty()) {
                log("skipping preload, model file missing: " + path);
            }
        }
        return;
    }

    if (type == "get_memory") { send_memory(); return; }

    if (type == "open_external") {
        // Markdown links. In-app navigation is pinned to the app origin, so a
        // link the model produced has to be handed to the shell instead.
        const std::string url = j.value("url", std::string());
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            ShellExecuteW(nullptr, L"open", utf8_to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            log("refused open_external for non-http URL: " + url);
        }
        return;
    }

    if (type == "save_memory") {
        const auto st = memory_.replace(j.value("text", std::string()));
        emit({{"type", "memory_saved"}, {"ok", st.ok}, {"message", st.message},
              {"bytes", st.bytes}, {"budget", st.budget}});
        send_memory();
        return;
    }

    if (type == "slash_command") {
        handle_slash(j.value("name", std::string()), j.value("args", std::string()),
                     j.value("session_id", std::string()));
        return;
    }
    if (type == "refresh_sessions") { send_sessions(); return; }
    if (type == "refresh_history") { send_history(j.value("session_id", store_.active_id())); return; }
    if (type == "get_settings") { send_settings(); return; }
    if (type == "open_tool_root") {
        std::filesystem::create_directories(utf8_to_wide(cfg_.tool_root));
        ShellExecuteW(hwnd_, L"open", utf8_to_wide(cfg_.tool_root).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (type == "pick_comfy_workflow") {
        const auto paths = pick_files(hwnd_, false, L"Choose ComfyUI API workflow",
            {{L"ComfyUI API workflows", L"*.json"}, {L"All files", L"*.*"}});
        if (!paths.empty()) {
            cfg_.comfyui_workflow = wide_to_utf8(paths.front());
            cfg_.persist_runtime_settings();
            send_settings();
        }
        return;
    }
    if (type == "save_settings") {
        if (model_transition_.load()) { emit({{"type", "error"}, {"message", "wait for the current model operation to finish"}}); return; }
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active work before changing runtime settings"}}); return; }
        const json v = j.value("values", json::object());
        cfg_.n_ctx = std::clamp(v.value("n_ctx", cfg_.n_ctx), 512, 1048576);
        cfg_.n_gpu_layers = std::clamp(v.value("n_gpu_layers", cfg_.n_gpu_layers), -1, 999);
        cfg_.n_batch = std::clamp(v.value("n_batch", cfg_.n_batch), 32, 8192);
        cfg_.n_ubatch = std::clamp(v.value("n_ubatch", cfg_.n_ubatch), 32, cfg_.n_batch);
        cfg_.n_threads = std::clamp(v.value("n_threads", cfg_.n_threads), 0, 256);
        cfg_.n_threads_batch = std::clamp(v.value("n_threads_batch", cfg_.n_threads_batch), 0, 256);
        cfg_.flash_attention = v.value("flash_attention", cfg_.flash_attention);
        if (cfg_.flash_attention != "auto" && cfg_.flash_attention != "on" && cfg_.flash_attention != "off") cfg_.flash_attention = "auto";
        cfg_.kv_cache_location = v.value("kv_cache_location", cfg_.kv_cache_location);
        if (cfg_.kv_cache_location != "vram" && cfg_.kv_cache_location != "ram") cfg_.kv_cache_location = "vram";
        cfg_.kv_cache_type = v.value("kv_cache_type", cfg_.kv_cache_type);
        if (cfg_.kv_cache_type != "f16" && cfg_.kv_cache_type != "q8_0" && cfg_.kv_cache_type != "q4_0") cfg_.kv_cache_type = "f16";
        cfg_.tool_root = v.value("tool_root", cfg_.tool_root);
        // Config is read from the inference and tool threads without a lock.
        // Rewriting strings like write_root or archive_db underneath a running
        // turn is a data race, so settings only apply between turns.
        if (loop_.busy()) {
            emit({{"type", "error"},
                  {"message", "Settings cannot change while a turn is running. Wait for it to finish, or stop it, then save again."}});
            return;
        }
        cfg_.write_root = v.value("write_root", cfg_.write_root);
        cfg_.archive_db = v.value("archive_db", cfg_.archive_db);
        cfg_.archive_shards = v.value("archive_shards", cfg_.archive_shards);
        cfg_.comfyui_url = v.value("comfyui_url", cfg_.comfyui_url);
        cfg_.comfyui_workflow = v.value("comfyui_workflow", cfg_.comfyui_workflow);
        cfg_.enable_web_tools = v.value("enable_web_tools", cfg_.enable_web_tools);
        cfg_.enable_image_tools = v.value("enable_image_tools", cfg_.enable_image_tools);
        cfg_.enable_voice_tools = v.value("enable_voice_tools", cfg_.enable_voice_tools);
        cfg_.enable_document_tools = v.value("enable_document_tools", cfg_.enable_document_tools);
        cfg_.enable_desktop_tools = v.value("enable_desktop_tools", cfg_.enable_desktop_tools);
        cfg_.enable_compression = v.value("enable_compression", cfg_.enable_compression);
        cfg_.enable_archive_tools = v.value("enable_archive_tools", cfg_.enable_archive_tools);
        cfg_.persist_runtime_settings();
        emit({{"type", "settings_saved"}, {"reloading", eng_.loaded()}});
        send_settings();
        if (eng_.loaded()) {
            if (model_transition_.exchange(true)) return;
            emit({{"type", "model_loading"}, {"path", cfg_.model_path}, {"reason", "runtime settings changed"}});
            eng_.unload([this] {
                eng_.load([this](bool ok, const std::string& err) {
                    if (!ok) emit({{"type", "error"}, {"message", err}});
                    model_transition_.store(false);
                    send_status();
                });
            });
        }
        return;
    }
    if (type == "transcribe_audio") {
        if (!cfg_.enable_voice_tools) { emit({{"type", "transcription_error"}, {"message", "voice tools are disabled in Settings"}}); return; }
        const std::string encoded = j.value("data", "");
        if (encoded.empty() || encoded.size() > 40 * 1024 * 1024) { emit({{"type", "transcription_error"}, {"message", "microphone recording is empty or too large"}}); return; }
        const auto bytes = decode_base64(encoded);
        if (bytes.empty()) { emit({{"type", "transcription_error"}, {"message", "could not decode microphone recording"}}); return; }
        namespace fs = std::filesystem;
        const fs::path dir = utf8_to_wide(app_data_dir() + "voice");
        fs::create_directories(dir);
        const std::string mime = j.value("mime", "audio/webm");
        const std::wstring ext = mime.find("ogg") != std::string::npos ? L".ogg" : mime.find("wav") != std::string::npos ? L".wav" : L".webm";
        const fs::path input = dir / (utf8_to_wide("mic-" + new_uuid()) + ext);
        std::ofstream out(input, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        const int id = jobs_.start_task("whisper_transcription",
            [this, input](JobHandle& job) { return transcribe_audio_file(cfg_, wide_to_utf8(input.wstring()), &job); },
            [this](int id, const std::string&, int pct, const std::string& note) {
                emit({{"type", "transcription_progress"}, {"id", id}, {"progress", pct}, {"note", note}});
            },
            [this, input](int id, const std::string&, JobStatus status, const std::string& result) {
                std::error_code ec; std::filesystem::remove(input, ec);
                if (status == JobStatus::Done && result.rfind("error:", 0) != 0)
                    emit({{"type", "transcription_result"}, {"id", id}, {"text", result}});
                else
                    emit({{"type", "transcription_error"}, {"id", id}, {"message", result.empty() ? "transcription failed" : result}});
            });
        emit({{"type", "transcription_started"}, {"id", id}});
        return;
    }

    if (type == "send") {
        TurnOptions options;
        options.mode = j.value("mode", "chat");
        if (options.mode != "chat" && options.mode != "agent") options.mode = "chat";
        options.effort = j.value("effort", "medium");
        if (options.effort != "low" && options.effort != "medium" && options.effort != "high") options.effort = "medium";
        options.agent_id = j.value("agent_id", "");
        options.resource_ids = j.value("resource_ids", std::vector<std::string>{});
        loop_.user_turn(j.value("session_id", store_.active_id()), j.value("text", ""), std::move(options));
        return;
    }
    if (type == "cancel") { eng_.cancel(); return; }
    if (type == "job_cancel") { loop_.cancel_job(j.value("id", 0)); return; }

    if (type == "load_model") {
        if (model_transition_.exchange(true)) { emit({{"type", "error"}, {"message", "a model operation is already running"}}); return; }
        if (loop_.has_in_flight_work()) { model_transition_.store(false); emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before changing models"}}); return; }
        const std::string id = j.value("id", cfg_.active_model_id);
        if (!cfg_.select_model(id)) { model_transition_.store(false); emit({{"type", "error"}, {"message", "unknown model"}}); return; }
        cfg_.persist_model_catalog();
        emit({{"type", "model_loading"}, {"id", id}});
        const bool was_loaded = eng_.loaded();
        auto load_selected = [this] {
            eng_.load([this](bool ok, const std::string& err) {
                if (!ok) emit({{"type", "error"}, {"message", err}});
                model_transition_.store(false);
                send_status(); send_models();
            });
        };
        if (was_loaded) eng_.unload(load_selected); else load_selected();
        return;
    }
    if (type == "unload_model") {
        if (model_transition_.exchange(true)) { emit({{"type", "error"}, {"message", "a model operation is already running"}}); return; }
        if (loop_.has_in_flight_work()) { model_transition_.store(false); emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before unloading"}}); return; }
        eng_.unload([this] { model_transition_.store(false); send_status(); });
        return;
    }
    if (type == "add_model") {
        if (model_transition_.load()) { emit({{"type", "error"}, {"message", "wait for the current model operation to finish"}}); return; }
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before adding a model"}}); return; }
        auto paths = pick_files(hwnd_, false, L"Add GGUF model", {{L"GGUF models", L"*.gguf"}, {L"All files", L"*.*"}});
        if (!paths.empty()) {
            cfg_.add_model(wide_to_utf8(paths.front()));
            send_models();
            emit({{"type", "model_added"}, {"id", cfg_.active_model_id}});
        }
        return;
    }

    if (type == "new_session") { store_.create(); send_sessions(); send_history(); return; }
    if (type == "select_session") {
        const std::string id = j.value("id", "");
        if (store_.select(id)) { send_sessions(); send_history(id); }
        return;
    }
    if (type == "delete_session") {
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before deleting conversations"}}); return; }
        store_.remove(j.value("id", "")); send_sessions(); send_history(); return;
    }

    if (type == "pick_attachments") {
        import_files("attachment", L"Attach files or images", true,
                     {{L"Supported files", L"*.txt;*.md;*.json;*.csv;*.pdf;*.docx;*.xlsx;*.pptx;*.png;*.jpg;*.jpeg;*.webp;*.cpp;*.h;*.py;*.js;*.ts;*.yaml;*.yml"}, {L"All files", L"*.*"}});
        return;
    }
    if (type == "import_rag") {
        import_files("rag", L"Add files to Helm RAG", true, {{L"All files", L"*.*"}});
        return;
    }
    if (type == "import_agent_config") {
        import_files("agent_config", L"Import agent task configuration", false,
                     {{L"Task configs", L"*.json;*.yaml;*.yml;*.toml"}, {L"All files", L"*.*"}});
        return;
    }
    if (type == "import_tool_pack") {
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before importing tools"}}); return; }
        const auto paths = pick_files(hwnd_, false, L"Import open-source tool manifest",
                                      {{L"JSON tool manifests", L"*.json"}, {L"All files", L"*.*"}});
        if (!paths.empty()) {
            const auto added_files = workspace_.import_files(paths, "tool_pack");
            const size_t added_tools = workspace_.register_tool_packs(reg_);
            loop_.refresh_tools();
            json list = json::array();
            for (const auto& r : added_files) list.push_back({{"id", r.id}, {"name", r.name}, {"kind", r.kind}, {"size", r.size}});
            emit({{"type", "files_picked"}, {"kind", "tool_pack"}, {"list", list}, {"registered_tools", added_tools}});
            send_workspace();
            send_tools();
        }
        return;
    }
    if (type == "remove_resource") {
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before removing workspace files"}}); return; }
        workspace_.remove_resource(j.value("id", "")); send_workspace(); return;
    }
    if (type == "create_agent") {
        const json definition = j.value("agent", json::object());
        const std::string model_id = definition.value("model_id", "");
        if (model_id.empty() || std::none_of(cfg_.models.begin(), cfg_.models.end(), [&](const auto& model) { return model.id == model_id; })) {
            emit({{"type", "error"}, {"message", "choose a valid model for this agent"}});
            return;
        }
        const AgentProfile a = workspace_.create_agent(definition);
        send_workspace();
        emit({{"type", "agent_created"}, {"id", a.id}});
        return;
    }
    if (type == "delete_agent") {
        if (loop_.has_in_flight_work()) { emit({{"type", "error"}, {"message", "finish or cancel active turns and jobs before deleting agents"}}); return; }
        workspace_.remove_agent(j.value("id", "")); send_workspace(); return;
    }
    // Opening an agent only created a session; something still had to type into
    // it. An agent that needs prompting every time is not autonomous, so this
    // opens the session and immediately drives the first turn from the agent's
    // own configuration.
    if (type == "stop_agent") {
        loop_.request_stop();
        emit({{"type", "note"}, {"text", "Stop requested. The current batch will finish, then the run ends."}});
        return;
    }

    if (type == "run_agent") {
        const std::string agent_id = j.value("id", "");
        AgentProfile agent;
        if (!workspace_.get_agent(agent_id, agent)) {
            emit({{"type", "error"}, {"message", "agent not found"}});
            return;
        }
        if (!eng_.loaded()) {
            emit({{"type", "error"}, {"message", "load a model before running an agent"}});
            return;
        }
        const std::string session_id = store_.create();
        send_sessions(); send_history(session_id);
        emit({{"type", "agent_opened"}, {"agent_id", agent_id}, {"session_id", session_id}, {"autorun", true}});

        // The configuration itself already reaches the model through the
        // workspace context block, so the kickoff only has to start the loop and
        // forbid the usual failure: answering with a plan instead of acting.
        TurnOptions options;
        options.mode = "agent";
        options.effort = j.value("effort", std::string("medium"));
        options.agent_id = agent_id;
        options.autonomous = true;
        options.perpetual = j.value("perpetual", false);
        if (options.perpetual && agent.permissions_configured) {
            const bool has_seen = std::find(agent.allowed_tools.begin(),
                agent.allowed_tools.end(), std::string("archive_seen")) != agent.allowed_tools.end();
            if (!has_seen) {
                emit({{"type", "error"}, {"message",
                    "This agent cannot start a Perpetual Loop: it lacks the Loop state permission "
                    "(archive_seen). Without it, each fresh-context batch would resurvey the same "
                    "items. Recreate the agent with Loop state enabled (the Recommended preset "
                    "includes it)."}});
                return;
            }
        }
        loop_.clear_stop();
        const std::string kickoff = j.value("instruction", std::string(
            "Begin the task defined in your active configuration now. Do not reply with a plan or "
            "ask what to do first: make your first tool call in this response."));
        loop_.user_turn(session_id, kickoff, options);
        return;
    }

    if (type == "open_agent") {
        const std::string agent_id = j.value("id", "");
        AgentProfile agent;
        if (!workspace_.get_agent(agent_id, agent)) {
            emit({{"type", "error"}, {"message", "agent not found"}});
            return;
        }
        const std::string session_id = store_.create();
        send_sessions(); send_history(session_id);
        emit({{"type", "agent_opened"}, {"agent_id", agent_id}, {"session_id", session_id}});
        return;
    }

    log("bridge: unknown message type: " + type);
    } catch (const std::exception& e) {
        log(std::string("bridge action failed: ") + e.what());
        emit({{"type", "error"}, {"message", std::string("UI action failed: ") + e.what()}});
    } catch (...) {
        log("bridge action failed: unknown");
        emit({{"type", "error"}, {"message", "UI action failed"}});
    }
}

} // namespace lar
