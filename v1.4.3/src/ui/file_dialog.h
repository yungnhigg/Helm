#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace lar {

std::vector<std::wstring> pick_files(HWND owner, bool multiple, const std::wstring& title,
                                     const std::vector<std::pair<std::wstring, std::wstring>>& filters = {});

} // namespace lar
