#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows headers require this order.
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
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
#include "platform/save_target.hpp"
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
namespace dialog {
inline void require(HRESULT result, const char *message) {
    if (FAILED(result))
        throw std::runtime_error(message);
}
inline std::filesystem::path existingParent(std::filesystem::path folder) {
    while (!folder.empty()) {
        std::error_code error;
        if (std::filesystem::is_directory(folder, error))
            return folder;
        const auto parent = folder.parent_path();
        if (parent == folder)
            break;
        folder = parent;
    }
    return {};
}
inline void startIn(IFileDialog *picker, const std::filesystem::path &directory) {
    // SetFolder overrides the common dialog's last-used location. Do not create folders when
    // showing a dialog; missing configured folders can be created later by an actual export.
    auto folder = existingParent(msa::resolve_output_directory(directory.u8string()));
    if (folder.empty())
        folder = existingParent(msa::desktop_directory());
    if (folder.empty())
        throw std::runtime_error("无法找到可访问的数据目录，请设置有效的默认目录。");
    Microsoft::WRL::ComPtr<IShellItem> location;
    require(
        SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(location.GetAddressOf())),
        "无法定位文件选择器的起始目录");
    require(picker->SetFolder(location.Get()), "无法打开文件选择器的起始目录");
}
inline std::optional<std::filesystem::path> show(IFileDialog *picker, HWND window) {
    const auto shown = picker->Show(window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::nullopt;
    require(shown, "文件选择未完成");
    Microsoft::WRL::ComPtr<IShellItem> item;
    require(picker->GetResult(item.GetAddressOf()), "无法读取所选位置");
    PWSTR raw = nullptr;
    require(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "无法取得所选位置的完整路径");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(raw, &CoTaskMemFree);
    return std::filesystem::path(raw);
}
inline std::optional<std::filesystem::path> open(const std::filesystem::path &directory,
                                                 bool folder, const std::string &filename) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    require(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(picker.GetAddressOf())),
            "无法打开 Windows 文件选择器");
    FILEOPENDIALOGOPTIONS options{};
    require(picker->GetOptions(&options), "无法读取文件选择选项");
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    options |= folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    require(picker->SetOptions(options), "无法设置文件选择选项");
    require(picker->SetTitle(folder ? L"选择默认目录" : L"导入数据"), "无法设置文件选择窗口标题");
    if (!folder) {
        const auto csv = exportFileType(ExportKind::Csv);
        const auto xlsx = exportFileType(ExportKind::Xlsx);
        const COMDLG_FILTERSPEC filters[] = {{L"测量数据 (*.csv;*.xlsx)", L"*.csv;*.xlsx"},
                                              {csv.label, csv.filter}, {xlsx.label, xlsx.filter}};
        require(picker->SetFileTypes(3, filters), "无法设置测量数据类型筛选");
        require(picker->SetFileTypeIndex(1), "无法设置默认数据文件筛选");
        require(picker->SetDefaultExtension(L"csv"), "无法设置测量数据扩展名");
        if (!filename.empty()) {
            const auto name = importFilenameSuggestion(directory, filename);
            require(picker->SetFileName(name.c_str()), "无法设置默认测量数据文件名");
        }
    }
    startIn(picker.Get(), directory);
    return show(picker.Get(), owner());
}
} // namespace dialog
inline std::optional<std::filesystem::path> chooseImportData(const std::filesystem::path &directory,
                                                             const std::string &filename = {}) {
    return dialog::open(directory, false, filename);
}
inline std::optional<std::filesystem::path>
chooseDirectory(const std::filesystem::path &directory) {
    return dialog::open(directory, true, {});
}
inline std::optional<SaveTarget> chooseExportPath(const std::filesystem::path &initialPath,
                                                  ExportKind initialFormat) {
    using dialog::require;
    using Microsoft::WRL::ComPtr;
    const bool report = reportFormat(initialFormat);
    const bool data = dataFormat(initialFormat);
    const ExportKind formats[] = {report ? ExportKind::ReportPdf : data ? ExportKind::Csv : initialFormat,
                                  report ? ExportKind::Html : ExportKind::Xlsx};
    const UINT count = report || data ? 2 : 1;
    COMDLG_FILTERSPEC filters[2]{};
    for (UINT i = 0; i < count; ++i) {
        const auto type = exportFileType(formats[i]);
        filters[i] = {type.label, type.filter};
    }
    auto suggested = initialPath;
    auto selected = initialFormat;
    for (;;) {
        ComPtr<IFileSaveDialog> dialog;
        require(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(dialog.GetAddressOf())),
                "无法打开 Windows 另存为对话框");
        FILEOPENDIALOGOPTIONS options{};
        require(dialog->GetOptions(&options), "无法读取保存选项");
        // Confirm the final, normalized path explicitly. A newly created destination then uses
        // Preserve all the way to the atomic commit, including during asynchronous chart rendering.
        options = (options & ~FOS_OVERWRITEPROMPT) | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                  FOS_PATHMUSTEXIST | FOS_STRICTFILETYPES | FOS_DONTADDTORECENT |
                  FOS_NOTESTFILECREATE;
        require(dialog->SetOptions(options), "无法设置保存选项");
        require(dialog->SetTitle(report ? L"方法对比报告另存为" : data ? L"导出数据" : L"图表另存为"),
                "无法设置保存窗口标题");
        require(dialog->SetFileTypes(count, filters), "无法设置导出文件类型");
        require(dialog->SetFileTypeIndex(selected == formats[0] ? 1 : 2),
                "无法设置默认导出格式");
        require(dialog->SetDefaultExtension(exportFileType(selected).extension),
                "无法设置默认扩展名");
        require(dialog->SetFileName(suggested.filename().c_str()), "无法设置默认文件名");
        platform::dialog::startIn(dialog.Get(), suggested.parent_path());
        const auto window = owner();
        const auto path = platform::dialog::show(dialog.Get(), window);
        if (!path)
            return std::nullopt;
        UINT index = 0;
        require(dialog->GetFileTypeIndex(&index), "无法读取所选文件类型");
        if (index < 1 || index > count)
            throw std::runtime_error("文件选择器返回了未知导出格式");
        selected = formats[index - 1];
        suggested = completeExportFilename(*path, selected);
        if (!std::filesystem::exists(suggested))
            return SaveTarget{suggested, selected, msa::ExistingFile::Preserve};
        if (std::filesystem::is_directory(suggested))
            throw std::runtime_error("导出目标是目录，请指定文件名");
        const auto question = L"文件已存在，是否替换？\n\n" + suggested.wstring();
        const auto answer = MessageBoxW(window, question.c_str(), L"确认另存为",
                                        MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer == IDYES)
            return SaveTarget{suggested, selected, msa::ExistingFile::Replace};
        if (answer == IDCANCEL)
            return std::nullopt;
        if (answer != IDNO)
            throw std::runtime_error("无法确认替换已有文件");
    }
}
inline void clipboard(const std::wstring &s) { writeClipboard(owner(), s); }
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
