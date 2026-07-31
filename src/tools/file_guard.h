#pragma once
#include <filesystem>
#include <string>

namespace lar {

enum class FileAccessMode { Read, Write };

struct FilePathResolution {
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

// Resolves the requested path through existing symlinks/junctions and enforces
// the effective filesystem boundary. A scoped agent root narrows both reads and
// writes; the global write_root still remains the outer write boundary.
FilePathResolution resolve_tool_file_path(const std::string& requested_utf8,
                                          const std::string& global_write_root,
                                          const std::string& scoped_agent_root,
                                          FileAccessMode mode);

} // namespace lar
