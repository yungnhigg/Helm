#include "agent/registry.h"
#include "agent/jobs.h"
#include "common/config.h"
#include "common/util.h"
#include "tools/process_guard.h"
#include <windows.h>
#include <filesystem>
#include <vector>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <cwctype>

namespace lar {

static void drain_process_pipe(HANDLE pipe, std::string& output) {
    char buffer[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, requested, &read, nullptr) || read == 0) break;
        output.append(buffer, buffer + read);
        if (output.size() > 250000) output.erase(0, output.size() - 250000);
    }
}

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

void register_tool_process(Registry& r, const Config& cfg) {
    const Config* c = &cfg;

    std::string description =
        "Run a local executable and capture stdout/stderr. This is a powerful computer-control tool; "
        "explain material changes after using it.";
    if (!cfg.process_allowlist.empty()) {
        description += " Only these executables may be started:";
        for (const auto& entry : cfg.process_allowlist) description += " " + entry;
        description += ".";
    } else {
        description += " The process allowlist is currently empty, so every launch will be "
                       "refused until process_allowlist is configured in app.json.";
    }

    r.add({
        "run_process",
        description,
        {{"executable", ParamType::String, "Executable path or command resolvable by Windows"},
         {"arguments", ParamType::String, "Raw command-line arguments"},
         {"working_directory", ParamType::String, "Working directory, or an empty string"},
         {"timeout_seconds", ParamType::Integer, "Timeout from 1 to 3600 seconds"}},
        ToolClass::Job,
        {},
        [c](const nlohmann::json& a, JobHandle& job) {
            const std::string exe_utf8 = a.at("executable").get<std::string>();
            const std::wstring exe = utf8_to_wide(exe_utf8);
            const std::wstring args = utf8_to_wide(a.at("arguments").get<std::string>());
            const std::wstring cwd = utf8_to_wide(a.at("working_directory").get<std::string>());
            const int timeout = std::clamp(a.at("timeout_seconds").get<int>(), 1, 3600);

            const ProcessDecision decision = check_process_allowed(c->process_allowlist, exe_utf8);
            if (!decision.allowed) {
                log("blocked run_process outside allowlist: " + exe_utf8);
                return decision.error;
            }

            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            HANDLE read_pipe = nullptr, write_pipe = nullptr;
            if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) throw std::runtime_error("CreatePipe failed");
            SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = write_pipe;
            si.hStdError = write_pipe;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION pi{};
            std::wstring command = quote_arg(exe);
            if (!args.empty()) command += L" " + args;
            std::vector<wchar_t> mutable_cmd(command.begin(), command.end());
            mutable_cmd.push_back(L'\0');

            BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
            CloseHandle(write_pipe);
            if (!ok) { CloseHandle(read_pipe); throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError())); }

            const auto start = std::chrono::steady_clock::now();
            std::string output;
            for (;;) {
                drain_process_pipe(read_pipe, output);
                DWORD state = WaitForSingleObject(pi.hProcess, 100);
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
                job.report(std::min(99, static_cast<int>(elapsed * 100 / timeout)), "running process");
                if (job.cancelled() || elapsed >= timeout) {
                    TerminateProcess(pi.hProcess, job.cancelled() ? 2 : 3);
                    WaitForSingleObject(pi.hProcess, 3000);
                    break;
                }
                if (state == WAIT_OBJECT_0) break;
            }
            drain_process_pipe(read_pipe, output);
            DWORD exit_code = 0;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(read_pipe);
            if (job.cancelled()) return std::string("process cancelled");
            return "exit code " + std::to_string(exit_code) + "\n" + output;
        }
    });
}

} // namespace lar
