#include "ui/file_dialog.h"
#include <shobjidl.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace lar {

std::vector<std::wstring> pick_files(HWND owner, bool multiple, const std::wstring& title,
                                     const std::vector<std::pair<std::wstring, std::wstring>>& filters) {
    std::vector<std::wstring> result;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
    if (multiple) options |= FOS_ALLOWMULTISELECT;
    dialog->SetOptions(options);
    if (!title.empty()) dialog->SetTitle(title.c_str());

    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& f : filters) specs.push_back({f.first.c_str(), f.second.c_str()});
    if (!specs.empty()) dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());

    if (FAILED(dialog->Show(owner))) return result;
    if (multiple) {
        ComPtr<IShellItemArray> items;
        if (FAILED(dialog->GetResults(&items))) return result;
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item))) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result.emplace_back(path);
                CoTaskMemFree(path);
            }
        }
    } else {
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return result;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            result.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return result;
}

} // namespace lar
