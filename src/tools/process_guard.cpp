#include "tools/process_guard.h"
#include <algorithm>
#include <cctype>

namespace lar {
namespace {

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string basename_of(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

} // namespace

ProcessDecision check_process_allowed(const std::vector<std::string>& allow,
                                      const std::string& executable_utf8) {
    ProcessDecision d;
    if (executable_utf8.empty()) {
        d.error = "error: no executable was given";
        return d;
    }

    const std::string full = lower_ascii(executable_utf8);
    const std::string name = lower_ascii(basename_of(executable_utf8));
    bool any_entry = false;
    for (const auto& entry : allow) {
        if (entry.empty()) continue;
        any_entry = true;
        const std::string e = lower_ascii(entry);
        if (e == full || e == name) { d.allowed = true; return d; }
    }

    if (!any_entry) {
        d.error = "error: process_allowlist is empty, so no executable may be started. "
                  "Add the executables this machine should permit (full path or bare "
                  "filename) to process_allowlist in %LOCALAPPDATA%\\Helm\\app.json "
                  "and restart Helm.";
        return d;
    }
    d.error = "error: executable is not in process_allowlist. Permitted:";
    for (const auto& entry : allow) {
        if (!entry.empty()) d.error += " " + entry;
    }
    return d;
}

} // namespace lar
