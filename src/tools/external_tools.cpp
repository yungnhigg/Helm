#include "tools/external_tools.h"
#include "agent/registry.h"
#include "agent/jobs.h"
#include "common/config.h"
#include "common/util.h"
#include <windows.h>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lar {

static std::wstring quote_arg(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += c; slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}

static void drain_pipe(HANDLE pipe, std::string& output) {
    char buffer[8192];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD read = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, request, &read, nullptr) || read == 0) break;
        output.append(buffer, buffer + read);
        if (output.size() > 1000000) output.erase(0, output.size() - 1000000);
    }
}

ProcessCaptureResult run_process_capture(const std::wstring& executable,
                                         const std::vector<std::wstring>& arguments,
                                         const std::wstring& working_directory,
                                         int timeout_seconds,
                                         JobHandle* job,
                                         const std::string& stdin_text) {
    ProcessCaptureResult result;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) throw std::runtime_error("CreatePipe(stdout) failed");
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(read_pipe); CloseHandle(write_pipe);
        throw std::runtime_error("CreatePipe(stdin) failed");
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = stdin_read;
    PROCESS_INFORMATION pi{};

    std::wstring command = quote_arg(executable);
    for (const auto& arg : arguments) command += L" " + quote_arg(arg);
    std::vector<wchar_t> mutable_cmd(command.begin(), command.end());
    mutable_cmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr,
                                   working_directory.empty() ? nullptr : working_directory.c_str(), &si, &pi);
    CloseHandle(write_pipe);
    CloseHandle(stdin_read);
    if (!ok) {
        CloseHandle(read_pipe); CloseHandle(stdin_write);
        throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
    }

    if (!stdin_text.empty()) {
        DWORD written = 0;
        WriteFile(stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr);
    }
    CloseHandle(stdin_write);

    timeout_seconds = std::clamp(timeout_seconds, 1, 3600);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        drain_pipe(read_pipe, result.output);
        const DWORD state = WaitForSingleObject(pi.hProcess, 100);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (job) job->report(std::min(99, static_cast<int>(elapsed * 100 / timeout_seconds)), "running");
        if (job && job->cancelled()) {
            result.cancelled = true;
            TerminateProcess(pi.hProcess, 2);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        if (elapsed >= timeout_seconds) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 3);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        if (state == WAIT_OBJECT_0) break;
    }
    drain_pipe(read_pipe, result.output);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);
    return result;
}

static std::wstring archive_script() {
    return utf8_to_wide(exe_dir() + "tools_runtime\\archive_index.py");
}

static std::wstring helper_script() {
    return utf8_to_wide(exe_dir() + "tools_runtime\\helm_tools.py");
}

static std::string require_success(const ProcessCaptureResult& r, const std::string& name) {
    if (r.cancelled) return name + " cancelled";
    if (r.timed_out) return "error: " + name + " timed out";
    if (r.exit_code != 0) return "error: " + name + " failed (exit " + std::to_string(r.exit_code) + ")\n" + r.output;
    return r.output;
}

static ProcessCaptureResult run_helper(const Config& cfg, const std::vector<std::wstring>& args,
                                       int timeout, JobHandle* job = nullptr) {
    const std::string python = cfg.resolved_tool_python();
    if (!fs::exists(utf8_to_wide(python))) {
        throw std::runtime_error("Python tool runtime not found at " + python +
                                 ". Run install_helm_tools.cmd (next to Helm.exe) and make sure the "
                                 "Tool root in Settings matches where it installed.");
    }
    std::vector<std::wstring> full{helper_script()};
    full.insert(full.end(), args.begin(), args.end());
    return run_process_capture(utf8_to_wide(python), full, L"", timeout, job);
}

static ProcessCaptureResult run_archive(const Config& cfg, const std::vector<std::wstring>& args,
                                        int timeout, JobHandle* job = nullptr) {
    const std::string python = cfg.resolved_tool_python();
    if (!fs::exists(utf8_to_wide(python)))
        throw std::runtime_error("Python tool runtime not found at " + python +
                                 ". Run install_helm_tools.cmd.");
    std::vector<std::wstring> full{archive_script()};
    full.insert(full.end(), args.begin(), args.end());
    return run_process_capture(utf8_to_wide(python), full, L"", timeout, job);
}

std::string transcribe_audio_file(const Config& cfg, const std::string& input_path, JobHandle* job) {
    fs::path input = utf8_to_wide(input_path);
    fs::path wav = input;
    wav.replace_extension(L".wav");
    if (job) job->report(5, "converting microphone audio");
    auto convert = run_process_capture(utf8_to_wide(cfg.resolved_ffmpeg()),
        {L"-y", L"-i", input.wstring(), L"-ar", L"16000", L"-ac", L"1", wav.wstring()}, L"", 120, job);
    std::string converted = require_success(convert, "FFmpeg conversion");
    if (convert.exit_code != 0 || convert.timed_out || convert.cancelled) return converted;

    fs::path out_base = wav;
    out_base.replace_extension();
    std::string whisper_path = cfg.resolved_whisper();
    if (!fs::exists(utf8_to_wide(whisper_path))) {
        fs::path fallback = utf8_to_wide(cfg.tool_root + "\\whisper.cpp\\build\\bin\\Release\\main.exe");
        if (fs::exists(fallback)) whisper_path = wide_to_utf8(fallback.wstring());
    }
    if (job) job->report(25, "transcribing with Whisper");
    auto transcription = run_process_capture(utf8_to_wide(whisper_path),
        {L"-m", utf8_to_wide(cfg.resolved_whisper_model()), L"-f", wav.wstring(),
         L"-otxt", L"-of", out_base.wstring(), L"-nt"}, L"", 900, job);
    std::string transcribed = require_success(transcription, "Whisper transcription");
    if (transcription.exit_code != 0 || transcription.timed_out || transcription.cancelled) return transcribed;

    const fs::path transcript_path = out_base.wstring() + L".txt";
    std::ifstream f(transcript_path, std::ios::binary);
    if (!f) return "error: Whisper completed but did not create a transcript";
    std::ostringstream text;
    text << f.rdbuf();
    std::error_code ec;
    fs::remove(wav, ec);
    fs::remove(transcript_path, ec);
    return text.str();
}

std::string extract_document_text(const Config& cfg, const std::string& path, size_t max_chars) {
    if (!cfg.enable_document_tools) return {};
    const fs::path source = utf8_to_wide(path);
    const fs::path cache = source.wstring() + L".helm-extracted.txt";
    std::error_code ec;
    if (fs::exists(cache, ec) && fs::last_write_time(cache, ec) >= fs::last_write_time(source, ec)) {
        std::ifstream f(cache, std::ios::binary);
        std::string out(max_chars, '\0');
        f.read(out.data(), static_cast<std::streamsize>(max_chars));
        out.resize(static_cast<size_t>(f.gcount()));
        return out;
    }
    try {
        auto r = run_helper(cfg, {L"extract-document", L"--path", source.wstring(), L"--max-chars", std::to_wstring(max_chars)}, 300);
        if (r.exit_code != 0 || r.timed_out || r.cancelled) {
            log("document extraction failed for " + path + ": " + r.output);
            return {};
        }
        atomic_write_text(cache, r.output);
        return r.output;
    } catch (const std::exception& e) {
        log("document extraction could not start for " + path + ": " + e.what());
        return {};
    }
}

void register_external_tools(Registry& r, const Config& cfg) {
    const Config* c = &cfg;

    r.add({
        "archive_seen",
        "Track which items an ongoing run has already processed, so a perpetual run does not repeat "
        "work after its context is cleared. Call with add set to a comma-separated list of identifiers "
        "(e.g. repository full names) to record them; call with add empty to read back everything "
        "recorded so far and skip anything already in the list.",
        {{"file", ParamType::String, "Absolute path to the seen-list file, e.g. F:\\AgentScratch\\_seen.txt"},
         {"add", ParamType::String, "Comma-separated identifiers to record, or empty to read the list"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            const std::string python = c->resolved_tool_python();
            if (!fs::exists(utf8_to_wide(python)))
                return std::string("error: Python tool runtime not found. Run install_helm_tools.cmd.");
            std::vector<std::wstring> args{helper_script(), L"seen-list",
                L"--file", utf8_to_wide(a.at("file").get<std::string>()),
                L"--add", utf8_to_wide(a.value("add", std::string()))};
            auto res = run_process_capture(utf8_to_wide(python), args, L"", 30, nullptr);
            return require_success(res, "seen list");
        },
        {}
    });

    r.add({
        "github_search",
        "Search GitHub repositories and get verified details back: real URL, star count, license, "
        "last-push date, primary language, archived flag, open issue count, and topics. ALWAYS prefer "
        "this over search_web for finding code repositories. The URL and every field come straight "
        "from the GitHub API - never construct or guess a repository URL yourself, and do not fetch "
        "the page unless you need README detail beyond the description.",
        {{"query", ParamType::String, "GitHub search query, e.g. 'unreal engine 5 ability system language:C++'"},
         {"max_results", ParamType::Integer, "How many repositories to return, 1 to 50"},
         {"sort", ParamType::String, "One of: stars, updated, forks, best-match"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) -> std::string {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            const std::string python = c->resolved_tool_python();
            if (!fs::exists(utf8_to_wide(python)))
                return std::string("error: Python tool runtime not found. Run install_helm_tools.cmd.");
            const int n = std::clamp(a.value("max_results", 15), 1, 50);
            std::vector<std::wstring> args{helper_script(), L"github-search",
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"--max-results", std::to_wstring(n),
                L"--sort", utf8_to_wide(a.value("sort", std::string("best-match")))};
            auto res = run_process_capture(utf8_to_wide(python), args, L"", 60, nullptr);
            return require_success(res, "github search");
        },
        {}
    });

    r.add({
        "search_web",
        "Search the current public web, automatically fetch the top result pages (including a JavaScript browser fallback), and return their readable text. Use it for recent or external information. Do not stop at links or ask permission to continue researching.",
        {{"query", ParamType::String, "Specific search query"},
         {"max_results", ParamType::Integer, "Number of results from 1 to 12"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            const int count = std::clamp(a.at("max_results").get<int>(), 1, 12);
            auto result = run_helper(*c, {L"web-search", L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                                          L"--max-results", std::to_wstring(count)}, 110, &job);
            return require_success(result, "web search");
        }
    });

    r.add({
        "search_archive",
        "Search the local offline Wikipedia archive stored on this machine. Use it before search_web for "
        "encyclopedic, historical, scientific, or biographical questions: it is faster, works with no "
        "network, and every result resolves to a real article. Fall back to the web only for current "
        "events or when the archive has nothing relevant.",
        {{"query", ParamType::String, "Search terms; plain keywords work best"},
         {"max_results", ParamType::Integer, "Number of chunks to return, from 1 to 20"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_archive_tools) return std::string("error: archive tools are disabled in Settings");
            if (c->archive_db.empty())
                return std::string("error: no archive index configured. Build one with "
                                   "archive_index.py and set Archive index in Settings.");
            if (!fs::exists(utf8_to_wide(c->archive_db)))
                return std::string("error: archive index not found at " + c->archive_db);
            const int count = std::clamp(a.at("max_results").get<int>(), 1, 20);
            std::vector<std::wstring> args{L"search", L"--db", utf8_to_wide(c->archive_db),
                L"--query", utf8_to_wide(a.at("query").get<std::string>()),
                L"-n", std::to_wstring(count)};
            // Recorded at index time; only needed when the shards have moved.
            if (!c->archive_shards.empty()) {
                args.push_back(L"--source");
                args.push_back(utf8_to_wide(c->archive_shards));
            }
            // Local disk, so this is fast; the ceiling only catches a cold cache
            // on a multi-gigabyte index.
            auto result = run_archive(*c, args, 60, &job);
            return require_success(result, "archive search");
        }
    });

    r.add({
        "fetch_web_page",
        "Fetch and extract one public web page. Falls back to headless Chromium when the static HTML is empty or JavaScript-rendered. Use it immediately when search results are incomplete; do not ask permission first.",
        {{"url", ParamType::String, "Absolute http or https URL"},
         {"max_chars", ParamType::Integer, "Readable characters to return, 1000 to 20000. Prefer 8000 or less; large values overflow the context window and the result gets truncated anyway."}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_web_tools) return std::string("error: web tools are disabled in Settings");
            // 100000 was reachable and guaranteed an overflow. The loop clamps by
            // context as well, but a sane ceiling here keeps the fetch itself cheap.
            const int limit = std::clamp(a.at("max_chars").get<int>(), 1000, 20000);
            auto result = run_helper(*c, {L"web-fetch", L"--url", utf8_to_wide(a.at("url").get<std::string>()),
                                          L"--max-chars", std::to_wstring(limit)}, 75, &job);
            return require_success(result, "web page fetch");
        }
    });

    r.add({
        "generate_image",
        "Generate an image through local ComfyUI. Helm supplies a starter SDXL workflow and starts ComfyUI automatically when it is installed but not running.",
        {{"prompt", ParamType::String, "Positive image prompt"},
         {"negative_prompt", ParamType::String, "Negative prompt, or an empty string"},
         {"width", ParamType::Integer, "Image width in pixels"},
         {"height", ParamType::Integer, "Image height in pixels"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_image_tools) return std::string("error: image tools are disabled in Settings");
            const fs::path output = utf8_to_wide(c->resolved_image_output_dir());
            fs::create_directories(output);
            auto result = run_helper(*c, {L"comfy-generate", L"--url", utf8_to_wide(c->comfyui_url),
                L"--workflow", utf8_to_wide(c->resolved_comfyui_workflow()),
                L"--prompt", utf8_to_wide(a.at("prompt").get<std::string>()),
                L"--negative-prompt", utf8_to_wide(a.at("negative_prompt").get<std::string>()),
                L"--width", std::to_wstring(std::clamp(a.at("width").get<int>(), 256, 4096)),
                L"--height", std::to_wstring(std::clamp(a.at("height").get<int>(), 256, 4096)),
                L"--output-dir", output.wstring(),
                L"--start-command", utf8_to_wide(c->tool_root + "\\START_COMFYUI.cmd")}, 1200, &job);
            return require_success(result, "ComfyUI image generation");
        }
    });

    r.add({
        "speak_text",
        "Speak text aloud through the local Piper voice and save the generated WAV file.",
        {{"text", ParamType::String, "Text to speak"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_voice_tools) return std::string("error: voice tools are disabled in Settings");
            const fs::path dir = utf8_to_wide(app_data_dir() + "voice");
            fs::create_directories(dir);
            const fs::path output = dir / utf8_to_wide("piper-" + new_uuid() + ".wav");
            // piper1-gpl (pip install piper-tts) takes -m/-f and the text as a
            // trailing argument after "--". The older standalone rhasspy binary
            // used --model/--output_file with text on stdin; passing those to
            // the current CLI fails outright. stdin is still supplied so a
            // legacy binary at the configured path keeps working.
            auto result = run_process_capture(utf8_to_wide(c->resolved_piper()),
                {L"-m", utf8_to_wide(c->resolved_piper_voice()),
                 L"-f", output.wstring(),
                 L"--", utf8_to_wide(a.at("text").get<std::string>())},
                L"", 300, &job, a.at("text").get<std::string>() + "\n");
            const std::string status = require_success(result, "Piper speech");
            if (result.exit_code != 0 || result.timed_out || result.cancelled) return status;
            PlaySoundW(output.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            return "speaking; WAV saved to " + wide_to_utf8(output.wstring());
        }
    });

    r.add({
        "extract_document",
        "Extract text from a local PDF, DOCX, XLSX, or PPTX file using the configured local document tools.",
        {{"path", ParamType::String, "Absolute local document path"},
         {"max_chars", ParamType::Integer, "Maximum characters from 1000 to 500000"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_document_tools) return std::string("error: document tools are disabled in Settings");
            job.report(10, "extracting document text");
            const size_t limit = static_cast<size_t>(std::clamp(a.at("max_chars").get<int>(), 1000, 500000));
            const std::string text = extract_document_text(*c, a.at("path").get<std::string>(), limit);
            return text.empty() ? std::string("error: no text could be extracted") : text;
        }
    });

    r.add({
        "desktop_screenshot",
        "Capture the Windows desktop to a PNG and return its local path. Inspect before clicking unfamiliar interfaces.",
        {{"monitor", ParamType::Integer, "0 for the full virtual desktop, or a monitor index"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            const fs::path dir = utf8_to_wide(app_data_dir() + "screenshots");
            fs::create_directories(dir);
            const fs::path output = dir / utf8_to_wide("desktop-" + new_uuid() + ".png");
            auto result = run_helper(*c, {L"desktop-screenshot", L"--output", output.wstring(), L"--monitor",
                                          std::to_wstring(a.at("monitor").get<int>())}, 60, &job);
            return require_success(result, "desktop screenshot");
        }
    });

    r.add({
        "desktop_click",
        "Click a desktop coordinate. Take a screenshot first and avoid destructive actions without clear user intent.",
        {{"x", ParamType::Integer, "Horizontal screen coordinate"},
         {"y", ParamType::Integer, "Vertical screen coordinate"},
         {"button", ParamType::String, "left, right, or middle"},
         {"clicks", ParamType::Integer, "Number of clicks from 1 to 3"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-click", L"--x", std::to_wstring(a.at("x").get<int>()),
                L"--y", std::to_wstring(a.at("y").get<int>()), L"--button", utf8_to_wide(a.at("button").get<std::string>()),
                L"--clicks", std::to_wstring(std::clamp(a.at("clicks").get<int>(), 1, 3))}, 30, &job);
            return require_success(result, "desktop click");
        }
    });

    r.add({
        "desktop_type",
        "Type text into the currently focused desktop control.",
        {{"text", ParamType::String, "Text to type"},
         {"interval_ms", ParamType::Integer, "Delay between keystrokes from 0 to 500 ms"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-type", L"--text", utf8_to_wide(a.at("text").get<std::string>()),
                L"--interval-ms", std::to_wstring(std::clamp(a.at("interval_ms").get<int>(), 0, 500))}, 120, &job);
            return require_success(result, "desktop typing");
        }
    });

    r.add({
        "desktop_hotkey",
        "Press a keyboard shortcut in the active desktop application.",
        {{"keys", ParamType::String, "Comma-separated keys, for example ctrl,l or alt,tab"}},
        ToolClass::Job, {},
        [c](const nlohmann::json& a, JobHandle& job) {
            if (!c->enable_desktop_tools) return std::string("error: desktop tools are disabled in Settings");
            auto result = run_helper(*c, {L"desktop-hotkey", L"--keys", utf8_to_wide(a.at("keys").get<std::string>())}, 30, &job);
            return require_success(result, "desktop hotkey");
        }
    });
}

} // namespace lar
