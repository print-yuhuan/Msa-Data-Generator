#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

namespace platform {
struct EmbeddedBytes {
    const void *data;
    std::size_t size;
};

// 压缩资源由模块持有，解码后的完整字体由只读共享内存持有。
using FontBytes = std::vector<unsigned char>;
std::shared_ptr<const FontBytes> decodeEmbeddedFont(const void *data, std::size_t size);

inline EmbeddedBytes embeddedFontResource() {
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, L"MSA_FONT_NOTO_CJK", MAKEINTRESOURCEW(10));
    if (!resource)
        throw std::runtime_error("无法读取内嵌中文字体资源");
    const auto size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    const auto data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !size)
        throw std::runtime_error("内嵌中文字体资源为空");
    return {data, static_cast<std::size_t>(size)};
}

inline std::shared_ptr<const FontBytes> embeddedFont() {
    static const auto font = [] {
        const auto resource = embeddedFontResource();
        return decodeEmbeddedFont(resource.data, resource.size);
    }();
    return font;
}
} // namespace platform
