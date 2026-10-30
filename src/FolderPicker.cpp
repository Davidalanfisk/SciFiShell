#include "FolderPicker.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace sf {

FolderPicker::~FolderPicker() {
    if (thread_.joinable()) thread_.join();
}

void FolderPicker::start(void* owner) {
    if (busy_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, owner] {
        std::vector<std::string> picked;
#ifdef _WIN32
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dlg->SetTitle(L"Choose folders with music or videos");
                dlg->SetOkButtonLabel(L"Add folders");
                if (SUCCEEDED(dlg->Show(static_cast<HWND>(owner)))) {
                    ComPtr<IShellItemArray> items;
                    DWORD n = 0;
                    if (SUCCEEDED(dlg->GetResults(&items)) && SUCCEEDED(items->GetCount(&n)))
                        for (DWORD i = 0; i < n; ++i) {
                            ComPtr<IShellItem> it;
                            PWSTR w = nullptr;
                            if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
                                const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                                if (len > 1) {
                                    std::string s(static_cast<size_t>(len - 1), '\0');
                                    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
                                    picked.push_back(std::move(s));
                                }
                                CoTaskMemFree(w);
                            }
                        }
                }
            }
            CoUninitialize();
        }
#else
        (void)owner;
#endif
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!picked.empty()) {
                result_ = std::move(picked);
                ready_ = true;
            }
        }
        busy_ = false;
    });
}

bool FolderPicker::poll(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!ready_) return false;
    out = std::move(result_);
    result_.clear();
    ready_ = false;
    return true;
}

}  // namespace sf
