#include "agent/registry.h"
#include "common/config.h"
#include "common/util.h"
#include "tools/file_guard.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;
namespace lar {

void register_tool_files(Registry& r, const Config& cfg) {
    const Config* c = &cfg;

    r.add({
        "list_directory",
        "List files and folders at a local path. Use this before changing an unfamiliar location.",
        {{"path", ParamType::String, "Absolute or user-provided directory path"}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) {
            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(a.at("path").get<std::string>(),
                                                         c->write_root, scoped_root,
                                                         FileAccessMode::Read);
            if (!resolved.ok) return resolved.error;
            const fs::path& p = resolved.path;
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
        "Read a local text or source file, a page at a time. Returns up to max_chars bytes starting "
        "at offset. If the file is larger, the result ends with a cursor line telling you the exact "
        "offset to pass next. Read pages in sequence rather than requesting a huge max_chars, which "
        "would overflow the context. Binary data is not decoded.",
        {{"path", ParamType::String, "Absolute or user-provided file path"},
         {"max_chars", ParamType::Integer, "Bytes to return this page, 1 to 60000. 8000-16000 is a good page size."},
         {"offset", ParamType::Integer, "Byte offset to start at. 0 for the first page; use the value from the previous page's cursor line."}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) {
            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(a.at("path").get<std::string>(),
                                                         c->write_root, scoped_root,
                                                         FileAccessMode::Read);
            if (!resolved.ok) return resolved.error;
            const fs::path& p = resolved.path;
            const int limit = std::clamp(a.value("max_chars", 16000), 1, 60000);
            const long long offset = std::max<long long>(0, a.value("offset", 0));
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (!f) return std::string("error: cannot open file");
            const long long total = static_cast<long long>(f.tellg());
            if (offset >= total && total > 0)
                return std::string("error: offset ") + std::to_string(offset) +
                       " is past end of file (" + std::to_string(total) + " bytes)";
            f.seekg(offset, std::ios::beg);
            std::string text(static_cast<size_t>(limit), '\0');
            f.read(text.data(), limit);
            text.resize(static_cast<size_t>(f.gcount()));
            const long long next = offset + static_cast<long long>(text.size());
            // Paging cursor: only when more remains. A large file is never
            // returned whole; the model is told precisely how to continue.
            if (next < total) {
                text += "\n\n[page ends at byte " + std::to_string(next) + " of " +
                        std::to_string(total) + ". To continue, call read_text_file again with "
                        "offset=" + std::to_string(next) + ".]";
            }
            return text;
        },
        {}
    });

    // Built at registration time, not as a static literal, so the tool's own
    // description names the ACTUAL configured write_root - the model reads
    // this before it ever calls the tool, instead of discovering the
    // restriction only after a rejected write. When write_root is empty
    // (unrestricted) the sentence is omitted rather than pointing at nothing.
    std::string write_desc =
        "Write UTF-8 text to a local file. Parent directories are created. A file large enough to "
        "risk exceeding your generation token budget in one call MUST be split: write the first part "
        "with overwrite=true, append=false, then write each following part with append=true. Never "
        "try to emit an entire large file's content in a single call - it will be cut off mid-string "
        "and nothing will be saved. Use overwrite=false (and append=false) unless replacement is intended.";
    if (!cfg.write_root.empty()) {
        write_desc += " Writes are confined to " + cfg.write_root +
            " and its subfolders - always use a path under there, never guess a different location. "
            "That directory is already known and does not need to be searched for, listed, or "
            "verified first: pick a filename yourself and call this tool directly. Do not call "
            "search_web, search_archive, or list_directory before a simple write - none of them can "
            "tell you anything about the destination that is not already stated here.";
    }

    r.add({
        "write_text_file",
        write_desc,
        {{"path", ParamType::String, "Destination path"},
         {"content", ParamType::String, "UTF-8 text for this part of the file"},
         {"overwrite", ParamType::Boolean, "Whether an existing file may be replaced. Must be false when append is true."},
         {"append", ParamType::Boolean, "True to add this content to the end of an existing file. append=true and overwrite=true is invalid."}},
        ToolClass::Sync,
        [c](const nlohmann::json& a) {
            const bool append = a.value("append", false);
            const bool overwrite = a.at("overwrite").get<bool>();
            if (append && overwrite)
                return std::string("error: append and overwrite cannot both be true; use append=true, overwrite=false for continuation parts");

            const std::string scoped_root = a.value("_helm_fs_root", std::string{});
            const auto resolved = resolve_tool_file_path(a.at("path").get<std::string>(),
                                                         c->write_root, scoped_root,
                                                         FileAccessMode::Write);
            if (!resolved.ok) {
                log("blocked file write: " + a.at("path").get<std::string>() + " (" + resolved.error + ")");
                return resolved.error;
            }
            const fs::path& p = resolved.path;
            std::error_code ec;
            const std::string content = a.at("content").get<std::string>();
            if (append) {
                // Plain ofstream append - atomic_write_text's write-then-rename
                // pattern would clobber prior parts instead of extending them.
                std::ofstream f(p, std::ios::binary | std::ios::app);
                if (!f) return std::string("error: could not open file to append");
                f.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!f) return std::string("error: append failed");
                return std::string("appended ") + std::to_string(content.size()) + " bytes to " + wide_to_utf8(p.wstring());
            }
            if (fs::exists(p, ec) && !overwrite) return std::string("error: file already exists");
            if (!atomic_write_text(p, content)) return std::string("error: write failed");
            return std::string("wrote ") + wide_to_utf8(p.wstring());
        },
        {},
        [](const nlohmann::json& a) -> std::string {
            if (!a.contains("append") || !a["append"].is_boolean() ||
                !a.contains("overwrite") || !a["overwrite"].is_boolean())
                return "error: append and overwrite must both be boolean values";
            if (a["append"].get<bool>() && a["overwrite"].get<bool>())
                return "error: append and overwrite cannot both be true; use append=true, overwrite=false for continuation parts";
            return {};
        }
    });
}

} // namespace lar
