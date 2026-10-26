#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows headers require this order.
#include <windows.h>
#include <imm.h>
#include <shobjidl.h>
// clang-format on
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include "platform/clipboard_win.hpp"
namespace platform {
inline std::filesystem::path executableDirectory() {
    wchar_t n[32768]{};
    GetModuleFileNameW(nullptr, n, 32768);
    return std::filesystem::path(n).parent_path();
}
inline HWND owner() {
    if (GLFWwindow *w = glfwGetCurrentContext())
        return glfwGetWin32Window(w);
    return GetActiveWindow();
}
inline bool enableChineseInput() {
    HKL layouts[32]{};
    int n = GetKeyboardLayoutList(32, layouts);
    for (int i = 0; i < n; ++i)
        if (PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(layouts[i]))) == LANG_CHINESE) {
            ActivateKeyboardLayout(layouts[i], 0);
            ImmAssociateContextEx(owner(), nullptr, IACE_DEFAULT);
            HIMC context = ImmGetContext(owner());
            if (context) {
                ImmSetOpenStatus(context, TRUE);
                ImmSetConversionStatus(context, IME_CMODE_NATIVE, 0);
                ImmReleaseContext(owner(), context);
            }
            return context != nullptr;
        }
    return false;
}
inline std::optional<std::filesystem::path> choose(bool save, bool folder,
                                                   const wchar_t *initial = L"msa_generator.json") {
    IFileDialog *d = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        throw std::runtime_error("无法创建文件对话框");
    FILEOPENDIALOGOPTIONS o{};
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM |
                  (folder ? (FOS_PICKFOLDERS | FOS_PATHMUSTEXIST)
                   : save ? FOS_OVERWRITEPROMPT
                          : FOS_FILEMUSTEXIST));
    d->SetTitle(folder ? L"选择输出目录" : save ? L"保存文件" : L"载入配置");
    if (!folder) {
        auto initialPath = std::filesystem::path(initial);
        auto extension = initialPath.extension().wstring();
        const bool csv =
            CompareStringOrdinal(extension.c_str(), -1, L".csv", -1, TRUE) == CSTR_EQUAL;
        if (csv)
            d->SetTitle(save ? L"保存 CSV" : L"导入测量数据");
        COMDLG_FILTERSPEC f[]{{csv ? L"CSV 数值文件" : L"JSON 配置", csv ? L"*.csv" : L"*.json"}};
        d->SetFileTypes(1, f);
        d->SetDefaultExtension(csv ? L"csv" : L"json");
        d->SetFileName(initialPath.filename().c_str());
        if (initialPath.has_parent_path() && std::filesystem::exists(initialPath.parent_path())) {
            IShellItem *location = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.parent_path().c_str(), nullptr,
                                                      IID_PPV_ARGS(&location)))) {
                d->SetFolder(location);
                location->Release();
            }
        }
    }
    HRESULT hr = d->Show(owner());
    std::optional<std::filesystem::path> p;
    if (SUCCEEDED(hr)) {
        IShellItem *i = nullptr;
        if (SUCCEEDED(d->GetResult(&i))) {
            PWSTR n = nullptr;
            if (SUCCEEDED(i->GetDisplayName(SIGDN_FILESYSPATH, &n))) {
                p = n;
                CoTaskMemFree(n);
            }
            i->Release();
        }
    }
    d->Release();
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        throw std::runtime_error("文件对话框失败");
    return p;
}
inline void clipboard(const std::wstring &s) {
    writeClipboard(owner(), s);
}
inline std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("文本不是有效的 UTF-8");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), result.data(),
                        count);
    return result;
}
inline std::filesystem::path compatibilityConfig() {
    wchar_t p[32768]{};
    GetEnvironmentVariableW(L"USERPROFILE", p, 32768);
    return std::filesystem::path(p) / L"MSA数据生成器" / L"msa_generator.json";
}
inline void openDirectory(const std::filesystem::path &p) {
    if (reinterpret_cast<INT_PTR>(
            ShellExecuteW(owner(), L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("无法打开目录");
}
inline void openWebsite(const std::wstring &url) {
    if (url.rfind(L"https://", 0) != 0)
        throw std::runtime_error("网站地址必须使用 HTTPS");
    if (reinterpret_cast<INT_PTR>(
            ShellExecuteW(owner(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("无法打开网站");
}
} // namespace platform
