#include "agent/registry.h"
#include "common/config.h"
#include "common/util.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwctype>

namespace fs = std::filesystem;
namespace lar {

static std::wstring lower_w(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Empty write_root means unrestricted. Otherwise the resolved destination must
// sit under the configured directory. Comparison is on the canonical path with
// a trailing separator, so C:\work does not admit C:\workspace.
static bool within_write_root(const fs::path& target, const std::string& root_utf8, std::string& err) {
    if (root_utf8.empty()) return true;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(utf8_to_wide(root_utf8)), ec);
    if (ec) { err = "error: write_root in app.json is not a resolvable path"; return false; }
    const fs::path abs = fs::weakly_canonical(target, ec);
    if (ec) { err = "error: destination path could not be resolved"; return false; }

    std::wstring r = lower_w(root.wstring());
    const std::wstring a = lower_w(abs.wstring());
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    if (a.size() < r.size() || a.compare(0, r.size(), r) != 0) {
        err = "error: destination is outside the configured write_root";
        return false;
    }
    return true;
}

void register_tool_files(Registry& r, const Config& cfg) {
    const Config* c = &cfg;

    r.add({
        "list_directory",
        "List files and folders at a local path. Use this before changing an unfamiliar location.",
        {{"path", ParamType::String, "Absolute or user-provided directory path"}},
        ToolClass::Sync,
        [](const nlohmann::json& a) {
            const fs::path p = utf8_to_wide(a.at("path").get<std::string>());
            std::error_code ec;
            if (!fs::is_directory(p, ec)) return std::string("error: not a directory");
            std::ostringstream out;
            size_t count = 0;
            for (const auto& e : fs::directory_iterator(p, ec)) {
                if (ec) break;
                out << (e.is_directory(ec) ? "[dir] " : "[file] ") << wide_to_utf8(e.path().filename().wstring());
                if (e.is_regular_file(ec)) out << " (" << e.file_size(ec) << " bytes)";
                out << "\n";
                if (++count >= 500) { out << "... truncated at 500 entries\n"; break; }
            }
            return out.str();
        },
        {}
    });

    r.add({
        "read_text_file",
        "Read a local text or source file. Binary data is not decoded.",
        {{"path", ParamType::String, "Absolute or user-provided file path"},
         {"max_chars", ParamType::Integer, "Maximum bytes to read, from 1 to 200000"}},
        ToolClass::Sync,
        [](const nlohmann::json& a) {
            const fs::path p = utf8_to_wide(a.at("path").get<std::string>());
            int limit = std::clamp(a.at("max_chars").get<int>(), 1, 200000);
            std::ifstream f(p, std::ios::binary);
            if (!f) return std::string("error: cannot open file");
            std::string text(static_cast<size_t>(limit), '\0');
            f.read(text.data(), limit);
            text.resize(static_cast<size_t>(f.gcount()));
            return text;
        },
        {}
    });

    r.add({
        "write_text_file",
        "Write UTF-8 text to a local file. Parent directories are created. Use overwrite=false unless replacement is intended.",
        {{"path", ParamType::String, "Destination path"},
         {"content", ParamType::String, "UTF-8 file contents"},
         {"overwrite", ParamType::Boolean, "Whether an existing file may be replaced"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) {
            const fs::path p = utf8_to_wide(a.at("path").get<std::string>());
            std::string err;
            if (!within_write_root(p, c->write_root, err)) {
                log("blocked write outside write_root: " + wide_to_utf8(p.wstring()));
                return err;
            }
            const bool overwrite = a.at("overwrite").get<bool>();
            std::error_code ec;
            if (fs::exists(p, ec) && !overwrite) return std::string("error: file already exists");
            fs::create_directories(p.parent_path(), ec);
            if (!atomic_write_text(p, a.at("content").get<std::string>())) return std::string("error: write failed");
            return std::string("wrote ") + wide_to_utf8(p.wstring());
        },
        {}
    });
}

} // namespace lar
