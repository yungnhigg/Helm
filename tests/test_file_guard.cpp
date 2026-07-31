#include "tools/file_guard.h"
#include "common/util.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <iostream>
#include <windows.h>

namespace fs = std::filesystem;
using namespace lar;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path() / utf8_to_wide("helm-file-guard-" + new_uuid());
    const fs::path global = base / L"global";
    const fs::path scoped = global / L"agent";
    const fs::path outside = base / L"outside";
    fs::create_directories(scoped, ec);
    fs::create_directories(outside, ec);

    auto inside = resolve_tool_file_path("notes\\a.txt", wide_to_utf8(global.wstring()),
                                         wide_to_utf8(scoped.wstring()), FileAccessMode::Write);
    expect("relative write resolves inside scoped root", inside.ok);

    auto normalized_inside = resolve_tool_file_path("sub\\..\\b.txt", wide_to_utf8(global.wstring()),
                                                    wide_to_utf8(scoped.wstring()), FileAccessMode::Write);
    expect("dot-dot remains legal when resolved inside scoped root", normalized_inside.ok);

    auto scoped_escape = resolve_tool_file_path(wide_to_utf8((scoped / L"..\\global-peer.txt").wstring()),
                                                wide_to_utf8(global.wstring()), wide_to_utf8(scoped.wstring()),
                                                FileAccessMode::Write);
    expect("scoped root blocks sibling path inside global root", !scoped_escape.ok);

    auto global_escape = resolve_tool_file_path(wide_to_utf8((global / L"..\\outside\\bad.txt").wstring()),
                                                wide_to_utf8(global.wstring()), {}, FileAccessMode::Write);
    expect("global write root blocks parent escape", !global_escape.ok);

    const fs::path invalid_agent_root = outside / L"must-not-be-created";
    auto invalid_root = resolve_tool_file_path(wide_to_utf8((invalid_agent_root / L"bad.txt").wstring()),
                                               wide_to_utf8(global.wstring()),
                                               wide_to_utf8(invalid_agent_root.wstring()),
                                               FileAccessMode::Write);
    expect("agent root outside global root is rejected", !invalid_root.ok);
    expect("rejected agent root is not created", !fs::exists(invalid_agent_root));

    auto read_escape = resolve_tool_file_path(wide_to_utf8((outside / L"read.txt").wstring()), {},
                                              wide_to_utf8(scoped.wstring()), FileAccessMode::Read);
    expect("scoped root also confines reads", !read_escape.ok);

    // Best effort: developer mode or admin rights may be required. When Windows
    // permits creation, verify that a directory symlink/junction-like reparse
    // path cannot redirect a scoped write outside the boundary.
    const fs::path link = scoped / L"redirect";
    const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2; // ALLOW_UNPRIVILEGED_CREATE
    if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), flags) ||
        CreateSymbolicLinkW(link.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        auto redirected = resolve_tool_file_path(wide_to_utf8((link / L"bad.txt").wstring()),
                                                 wide_to_utf8(global.wstring()), wide_to_utf8(scoped.wstring()),
                                                 FileAccessMode::Write);
        expect("directory reparse escape is rejected", !redirected.ok);
    }

    fs::remove_all(base, ec);
    if (failures) return 1;
    std::cout << "all file-guard tests passed\n";
    return 0;
}
