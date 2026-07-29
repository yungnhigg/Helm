#include "workspace/workspace.h"
#include "agent/registry.h"
#include "agent/jobs.h"
#include "common/util.h"
#include <windows.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <stdexcept>

using nlohmann::json;

namespace lar {

static ParamType manifest_type(const std::string& value) {
    if (value == "number") return ParamType::Number;
    if (value == "integer") return ParamType::Integer;
    if (value == "boolean") return ParamType::Boolean;
    return ParamType::String;
}

static std::wstring quote_executable(const std::wstring& s) {
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'\"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

static void drain_tool_pipe(HANDLE pipe, std::string& output) {
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

static std::string run_stdio_json_tool(const std::string& executable,
                                       const std::string& process_arguments,
                                       const std::string& working_directory,
                                       const std::string& tool_name,
                                       const json& arguments,
                                       JobHandle& job) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_stdout_read = nullptr, child_stdout_write = nullptr;
    HANDLE child_stdin_read = nullptr, child_stdin_write = nullptr;
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0) ||
        !CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
        if (child_stdout_read) CloseHandle(child_stdout_read);
        if (child_stdout_write) CloseHandle(child_stdout_write);
        if (child_stdin_read) CloseHandle(child_stdin_read);
        if (child_stdin_write) CloseHandle(child_stdin_write);
        throw std::runtime_error("failed to create tool-pack pipes");
    }
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stdout_write;
    si.hStdInput = child_stdin_read;
    PROCESS_INFORMATION pi{};

    std::wstring command = quote_executable(utf8_to_wide(executable));
    if (!process_arguments.empty()) command += L" " + utf8_to_wide(process_arguments);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const std::wstring cwd = utf8_to_wide(working_directory);

    BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stdin_read);
    if (!ok) {
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdin_write);
        throw std::runtime_error("tool-pack executable failed to start: " + std::to_string(GetLastError()));
    }

    const std::string request = json{{"tool", tool_name}, {"arguments", arguments}}.dump() + "\n";
    DWORD written = 0;
    WriteFile(child_stdin_write, request.data(), static_cast<DWORD>(request.size()), &written, nullptr);
    CloseHandle(child_stdin_write);

    std::string output;
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if (job.cancelled()) {
            TerminateProcess(pi.hProcess, 2);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        drain_tool_pipe(child_stdout_read, output);
        job.report(50, "running imported tool");
    }
    drain_tool_pipe(child_stdout_read, output);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(child_stdout_read);
    if (job.cancelled()) return "imported tool cancelled";
    if (exit_code != 0) return "imported tool exited with code " + std::to_string(exit_code) + "\n" + output;
    try {
        json response = json::parse(output);
        if (response.is_object() && response.contains("result")) {
            return response["result"].is_string() ? response["result"].get<std::string>() : response["result"].dump();
        }
    } catch (...) {}
    return output;
}

size_t WorkspaceStore::register_tool_packs(Registry& registry) const {
    const auto packs = resources("tool_pack");
    size_t added = 0;
    for (const auto& pack : packs) {
        std::ifstream f(utf8_to_wide(pack.path), std::ios::binary);
        if (!f) continue;
        json manifest;
        try { manifest = json::parse(f); }
        catch (...) { log("tool pack is not valid JSON: " + pack.name); continue; }
        if (manifest.value("adapter", "") != "stdio-json") {
            log("unsupported tool-pack adapter in " + pack.name + "; expected stdio-json");
            continue;
        }
        const std::string executable = manifest.value("executable", "");
        const std::string process_arguments = manifest.value("arguments", "");
        const std::string working_directory = manifest.value("working_directory", "");
        if (executable.empty()) continue;
        for (const auto& item : manifest.value("tools", json::array())) {
            Tool tool;
            tool.name = item.value("name", "");
            tool.description = item.value("description", "Imported open-source tool");
            if (tool.name.empty()) continue;
            for (const auto& p : item.value("parameters", json::array())) {
                const std::string name = p.value("name", "");
                if (name.empty()) continue;
                tool.params.push_back({name, manifest_type(p.value("type", "string")), p.value("description", "")});
            }
            tool.cls = ToolClass::Job;
            const std::string tool_name = tool.name;
            tool.run_job = [executable, process_arguments, working_directory, tool_name](const json& args, JobHandle& job) {
                return run_stdio_json_tool(executable, process_arguments, working_directory, tool_name, args, job);
            };
            if (registry.add(std::move(tool))) ++added;
        }
    }
    return added;
}

} // namespace lar
