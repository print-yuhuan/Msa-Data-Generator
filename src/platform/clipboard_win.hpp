#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace platform {
// Injectable API boundary lets failure paths run without touching the clipboard.
struct ClipboardApi {
    bool open(HWND window) { return OpenClipboard(window) != FALSE; }
    void close() { CloseClipboard(); }
    HGLOBAL allocate(SIZE_T bytes) { return GlobalAlloc(GMEM_MOVEABLE, bytes); }
    void release(HGLOBAL memory) { GlobalFree(memory); }
    void *lock(HGLOBAL memory) { return GlobalLock(memory); }
    void unlock(HGLOBAL memory) { GlobalUnlock(memory); }
    bool empty() { return EmptyClipboard() != FALSE; }
    bool put(HGLOBAL memory) { return SetClipboardData(CF_UNICODETEXT, memory) != nullptr; }
};

template <typename Api> void writeClipboard(HWND owner, const std::wstring &text, Api &api) {
    if (!api.open(owner))
        throw std::runtime_error("剪贴板暂不可用");
    struct Close {
        Api &api;
        ~Close() { api.close(); }
    } close{api};
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    struct Memory {
        Api &api;
        HGLOBAL handle;
        ~Memory() {
            if (handle)
                api.release(handle);
        }
    } memory{api, api.allocate(bytes)};
    if (!memory.handle)
        throw std::bad_alloc();
    void *buffer = api.lock(memory.handle);
    if (!buffer)
        throw std::runtime_error("剪贴板分配失败");
    std::memcpy(buffer, text.c_str(), bytes);
    api.unlock(memory.handle);
    if (!api.empty())
        throw std::runtime_error("无法清空剪贴板");
    if (!api.put(memory.handle))
        throw std::runtime_error("剪贴板写入失败");
    memory.handle = nullptr; // Ownership transfers only after a successful commit.
}
inline void writeClipboard(HWND owner, const std::wstring &text) {
    ClipboardApi api;
    writeClipboard(owner, text, api);
}
} // namespace platform
