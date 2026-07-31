#pragma once
#include <string>
#include <vector>

namespace lar {

struct ProcessDecision {
    bool allowed = false;
    std::string error;   // starts with "error: " whenever allowed is false
};

// Decide whether an executable may be started, against the configured
// process_allowlist. An entry matches the full path or the bare filename,
// ASCII-case-insensitively; the bare-filename form is deliberate so
// "makemkvcon64.exe" works without pinning an install location.
//
// An EMPTY allowlist denies everything: a security control that fails open
// is not a control. The refusal explains how to configure the list.
//
// Basename splitting handles both separators manually rather than through
// std::filesystem::path::filename(), whose behavior is platform-dependent -
// this keeps the policy byte-identical and testable off Windows.
ProcessDecision check_process_allowed(const std::vector<std::string>& allow,
                                      const std::string& executable_utf8);

} // namespace lar
