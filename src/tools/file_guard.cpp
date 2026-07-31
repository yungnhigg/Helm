#include "tools/file_guard.h"
#include "common/util.h"
#include <algorithm>
#include <cwctype>
#include <utility>
#include <windows.h>

namespace fs = std::filesystem;

namespace lar {
namespace {

std::wstring lower_w(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool same_or_child(const fs::path& root, const fs::path& target) {
    std::wstring r = lower_w(root.lexically_normal().wstring());
    std::wstring t = lower_w(target.lexically_normal().wstring());
    if (t == r) return true;
    if (r.empty() || t.size() <= r.size() || t.compare(0, r.size(), r) != 0) return false;

    // Drive and UNC roots already end in a separator (C:\, \\host\share\).
    // For every other root, the next character must be a path boundary so
    // C:\work does not accidentally admit C:\workspace.
    if (r.back() == L'\\' || r.back() == L'/') return true;
    const wchar_t boundary = t[r.size()];
    return boundary == L'\\' || boundary == L'/';
}

bool existing_reparse_point(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool absolute_normal(const fs::path& path, fs::path& out, std::string& error) {
    std::error_code ec;
    out = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) {
        error = "error: path could not be made absolute: " + ec.message();
        return false;
    }
    out = out.lexically_normal();
    return true;
}

bool canonicalize(const fs::path& path, fs::path& out, std::string& error) {
    fs::path absolute;
    if (!absolute_normal(path, absolute, error)) return false;
    std::error_code ec;
    out = fs::weakly_canonical(absolute, ec);
    if (ec) {
        error = "error: path could not be resolved: " + ec.message();
        return false;
    }
    return true;
}

bool enforce_boundary(const fs::path& effective_root,
                      const fs::path& resolved,
                      bool scoped,
                      FilePathResolution& result) {
    if (effective_root.empty() || same_or_child(effective_root, resolved)) return true;
    result.error = scoped
        ? "error: path is outside this agent's configured filesystem root"
        : "error: destination is outside the configured write_root";
    return false;
}

} // namespace

FilePathResolution resolve_tool_file_path(const std::string& requested_utf8,
                                          const std::string& global_write_root,
                                          const std::string& scoped_agent_root,
                                          FileAccessMode mode) {
    FilePathResolution result;
    if (requested_utf8.empty()) {
        result.error = "error: path is empty";
        return result;
    }

    const bool scoped = !scoped_agent_root.empty();
    const std::string effective_root_utf8 = scoped
        ? scoped_agent_root
        : (mode == FileAccessMode::Write ? global_write_root : std::string{});

    fs::path effective_root;
    fs::path global_root;
    std::string error;

    fs::path raw_effective_root;
    if (!effective_root_utf8.empty()) {
        raw_effective_root = utf8_to_wide(effective_root_utf8);
        if (!raw_effective_root.is_absolute()) {
            result.error = scoped
                ? "error: agent filesystem root must be an absolute path"
                : "error: configured write_root must be an absolute path";
            return result;
        }
        // Resolve before creating the root. This prevents a misconfigured
        // per-agent root from creating directories outside the global write
        // boundary before the boundary check rejects it.
        if (!canonicalize(raw_effective_root, effective_root, error)) {
            result.error = "error: configured filesystem root is not resolvable";
            return result;
        }
    }

    if (mode == FileAccessMode::Write && scoped && !global_write_root.empty()) {
        const fs::path raw_global = utf8_to_wide(global_write_root);
        if (!raw_global.is_absolute()) {
            result.error = "error: configured write_root must be an absolute path";
            return result;
        }
        if (!canonicalize(raw_global, global_root, error)) {
            result.error = "error: global write_root is not resolvable";
            return result;
        }
        if (!same_or_child(global_root, effective_root)) {
            result.error = "error: agent filesystem root is outside the configured global write_root";
            return result;
        }
    }

    if (mode == FileAccessMode::Write && !raw_effective_root.empty()) {
        std::error_code ec;
        fs::create_directories(raw_effective_root, ec);
        if (ec) {
            result.error = "error: filesystem root could not be created: " + ec.message();
            return result;
        }
        // A junction may have appeared while the directory was being created.
        // Resolve and enforce both boundaries again before resolving the target.
        if (!canonicalize(raw_effective_root, effective_root, error)) {
            result.error = "error: configured filesystem root is not resolvable";
            return result;
        }
        if (scoped && !global_root.empty() && !same_or_child(global_root, effective_root)) {
            result.error = "error: agent filesystem root is outside the configured global write_root";
            return result;
        }
    }

    fs::path candidate = utf8_to_wide(requested_utf8);
    if (candidate.is_relative() && !effective_root.empty()) candidate = effective_root / candidate;

    // Resolve existing parents before creating anything. weakly_canonical follows
    // symlinks and junctions in the existing prefix and appends the missing tail,
    // which lets us reject an escape without first creating directories outside
    // the allowed boundary.
    fs::path resolved;
    if (!canonicalize(candidate, resolved, error)) {
        result.error = error;
        return result;
    }
    if (!enforce_boundary(effective_root, resolved, scoped, result)) return result;

    if (mode == FileAccessMode::Write) {
        // A final reparse-point file can redirect the open after validation.
        // Parent reparse points have already been resolved and boundary-checked.
        if (existing_reparse_point(candidate)) {
            result.error = "error: refusing to write through a symlink or junction destination";
            return result;
        }

        std::error_code ec;
        fs::create_directories(resolved.parent_path(), ec);
        if (ec) {
            result.error = "error: destination directory could not be created: " + ec.message();
            return result;
        }

        // Re-resolve after creation to close the normal junction/symlink case and
        // narrow the remaining time-of-check/time-of-use window as far as this
        // non-sandboxed Win32 design reasonably can.
        fs::path verified;
        if (!canonicalize(resolved, verified, error)) {
            result.error = error;
            return result;
        }
        if (!enforce_boundary(effective_root, verified, scoped, result)) return result;
        resolved = std::move(verified);
    }

    result.ok = true;
    result.path = resolved;
    return result;
}

} // namespace lar
