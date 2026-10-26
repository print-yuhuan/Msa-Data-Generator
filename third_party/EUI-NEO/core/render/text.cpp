#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "core/render/text.h"
#include "core/render/render_backend.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <new>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

namespace {

constexpr const char* kDefaultUiFontFile = "JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf";
constexpr const char* kDefaultIconFontFile = "Font Awesome 7 Free-Solid-900.otf";
constexpr int kGrayAtlasSize = 2048;
constexpr int kColorAtlasSize = 1024;
constexpr std::size_t kTextAtlasByteBudget = 32u * 1024u * 1024u;
constexpr FT_Int32 kGlyphLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_COLOR | FT_LOAD_NO_SVG | FT_LOAD_TARGET_LIGHT;

using FontBytes = std::vector<unsigned char>;
struct MemoryFont {
    std::shared_ptr<const FontBytes> bytes;
    std::uint64_t revision = 0;
};

std::unordered_map<std::string, MemoryFont>& memoryFonts() {
    static std::unordered_map<std::string, MemoryFont> fonts;
    return fonts;
}

std::uint64_t& memoryFontRevision() {
    static std::uint64_t revision = 0;
    return revision;
}

bool isMemoryFontReference(const std::string& path) {
    return path.compare(0, 7, "memory:") == 0;
}

struct FontFace {
    std::string path;
    std::shared_ptr<const FontBytes> bytes;
    std::uint64_t memoryRevision = 0;
    FT_Face face = nullptr;
    float size = 16.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;
    float glyphScale = 1.0f;
    bool colored = false;

    FontFace() = default;
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    FontFace(FontFace&& other) noexcept {
        *this = std::move(other);
    }

    FontFace& operator=(FontFace&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (face) {
            FT_Done_Face(face);
        }
        path = std::move(other.path);
        bytes = std::move(other.bytes);
        memoryRevision = other.memoryRevision;
        face = other.face;
        size = other.size;
        ascent = other.ascent;
        descent = other.descent;
        lineGap = other.lineGap;
        glyphScale = other.glyphScale;
        colored = other.colored;
        other.face = nullptr;
        return *this;
    }

    ~FontFace() {
        if (face) {
            FT_Done_Face(face);
        }
    }
};

struct FontInfoHolder {
    std::vector<FontFace> faces;
    std::vector<std::string> lazyFallbackPaths;
    std::string primaryPath;
    bool fallbackPathsResolved = false;
};

constexpr std::size_t kFontStackCacheCapacity = 16;
constexpr std::size_t kTextSizeCacheCapacity = 1024;
constexpr std::size_t kTextMetricsCacheCapacity = 256;
constexpr std::size_t kTextMetricsCacheMaxBytes = 4096;

struct FontStackCacheEntry {
    std::shared_ptr<FontInfoHolder> holder;
    std::uint64_t lastUsed = 0;
};

struct FontStackCache {
    std::unordered_map<std::string, FontStackCacheEntry> entries;
    std::uint64_t accessTick = 0;
};

FontStackCache& sharedFontStackCache() {
    static FontStackCache cache;
    return cache;
}

struct TextSizeCacheKey {
    std::string text;
    std::string fontFamily;
    float fontSize = 0.0f;
    float maxWidth = 0.0f;
    float lineHeight = 0.0f;
    int fontWeight = 0;
    bool wrap = false;

    bool operator==(const TextSizeCacheKey& other) const {
        return text == other.text &&
               fontFamily == other.fontFamily &&
               fontSize == other.fontSize &&
               maxWidth == other.maxWidth &&
               lineHeight == other.lineHeight &&
               fontWeight == other.fontWeight &&
               wrap == other.wrap;
    }
};

struct TextSizeCacheKeyHash {
    std::size_t operator()(const TextSizeCacheKey& key) const {
        std::size_t value = std::hash<std::string>{}(key.text);
        const auto combine = [&](std::size_t part) {
            value ^= part + 0x9e3779b9u + (value << 6u) + (value >> 2u);
        };
        combine(std::hash<std::string>{}(key.fontFamily));
        combine(std::hash<float>{}(key.fontSize));
        combine(std::hash<float>{}(key.maxWidth));
        combine(std::hash<float>{}(key.lineHeight));
        combine(std::hash<int>{}(key.fontWeight));
        combine(std::hash<bool>{}(key.wrap));
        return value;
    }
};

struct TextSizeCacheEntry {
    Vec2 size;
    std::uint64_t lastUsed = 0;
};

struct TextSizeCache {
    std::unordered_map<TextSizeCacheKey, TextSizeCacheEntry, TextSizeCacheKeyHash> entries;
    std::uint64_t accessTick = 0;
};

TextSizeCache& sharedTextSizeCache() {
    static TextSizeCache cache;
    return cache;
}

struct TextMetricsCacheEntry {
    TextPrimitive::TextMetrics metrics;
    std::uint64_t lastUsed = 0;
};

struct TextMetricsCache {
    std::unordered_map<TextSizeCacheKey, TextMetricsCacheEntry, TextSizeCacheKeyHash> entries;
    std::uint64_t accessTick = 0;
};

TextMetricsCache& sharedTextMetricsCache() {
    static TextMetricsCache cache;
    return cache;
}

void clearSharedFontStackCache() {
    FontStackCache& cache = sharedFontStackCache();
    cache.entries.clear();
    cache.accessTick = 0;

    TextSizeCache& sizeCache = sharedTextSizeCache();
    sizeCache.entries.clear();
    sizeCache.accessTick = 0;

    TextMetricsCache& metricsCache = sharedTextMetricsCache();
    metricsCache.entries.clear();
    metricsCache.accessTick = 0;
}

struct AtlasPage {
    std::uint64_t id = 0;
    std::uint64_t lastUsed = 0;
    int width = 0;
    int height = 0;
    int channels = 1;
    int x = 1;
    int y = 1;
    int rowHeight = 0;
    std::uint64_t generation = 0;
    std::vector<unsigned char> pixels;
    std::unordered_map<std::string, TextPrimitive::Glyph> glyphs;
};

struct SharedTextAtlas {
    std::vector<std::shared_ptr<AtlasPage>> pages;
    std::size_t residentBytes = 0;
    std::uint64_t accessTick = 0;
    std::uint64_t allocations = 0;
    std::uint64_t evictions = 0;
    int references = 0;
};

unsigned int readUtf8Codepoint(const std::string& text, size_t& index);

FT_Library sharedFreeTypeLibrary() {
    static FT_Library library = [] {
        FT_Library created = nullptr;
        return FT_Init_FreeType(&created) == 0 ? created : nullptr;
    }();
    return library;
}

SharedTextAtlas& sharedTextAtlas() {
    static SharedTextAtlas atlas;
    return atlas;
}

std::uint64_t& textAtlasGenerationCounter() {
    static std::uint64_t generation = 0;
    return generation;
}

bool ensureAtlasPage(AtlasPage& page, int width, int height, int channels) {
    if (!page.pixels.empty()) {
        return true;
    }

    page.width = width;
    page.height = height;
    page.channels = channels;
    page.x = 1;
    page.y = 1;
    page.rowHeight = 0;
    page.generation = ++textAtlasGenerationCounter();
    // 0 保留给旧调用方，1/2 保留给渲染后端的单页兼容槽位。
    page.id = page.generation + 2;
    page.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels), 0);
    return !page.pixels.empty();
}

bool retainSharedTextAtlas() {
    SharedTextAtlas& atlas = sharedTextAtlas();
    ++atlas.references;
    return true;
}

void releaseSharedTextAtlas() {
    SharedTextAtlas& atlas = sharedTextAtlas();
    atlas.references = std::max(0, atlas.references - 1);
    if (atlas.references > 0) {
        return;
    }

    atlas.pages.clear();
    atlas.residentBytes = 0;
}

std::shared_ptr<AtlasPage> findAtlasPage(std::uint64_t id) {
    auto& atlas = sharedTextAtlas();
    for (const auto& page : atlas.pages) {
        if (page->id == id) {
            page->lastUsed = ++atlas.accessTick;
            return page;
        }
    }
    return {};
}

bool cachedAtlasGlyph(const std::string& key, TextPrimitive::Glyph& glyph) {
    auto& atlas = sharedTextAtlas();
    for (const auto& page : atlas.pages) {
        const auto found = page->glyphs.find(key);
        if (found != page->glyphs.end()) {
            // 后备字体可能由不同主字体共用；主字体基线和彩色字形缩放
            // 属于当前实例，只复用栅格页坐标，不能覆盖刚算出的几何量。
            glyph.u0 = found->second.u0;
            glyph.v0 = found->second.v0;
            glyph.u1 = found->second.u1;
            glyph.v1 = found->second.v1;
            glyph.atlasPageId = found->second.atlasPageId;
            page->lastUsed = ++atlas.accessTick;
            return true;
        }
    }
    return false;
}

std::shared_ptr<AtlasPage> newAtlasPage(bool colored, int glyphWidth, int glyphHeight) {
    auto& atlas = sharedTextAtlas();
    int width = colored ? kColorAtlasSize : kGrayAtlasSize;
    int height = width;
    while (width < glyphWidth + 2 && width < 4096) width *= 2;
    while (height < glyphHeight + 2 && height < 4096) height *= 2;
    const int channels = colored ? 4 : 1;
    const auto bytes = std::size_t(width) * height * channels;
    if (glyphWidth + 2 > width || glyphHeight + 2 > height || bytes > kTextAtlasByteBudget)
        return {};
    // 淘汰只移除缓存所有权。正在提交的一段文字最多暂时持有一个旧页，
    // 其像素会在 drawText 同步上传后释放，不会破坏待绘批次。
    while (atlas.residentBytes + bytes > kTextAtlasByteBudget && !atlas.pages.empty()) {
        const auto oldest = std::min_element(atlas.pages.begin(), atlas.pages.end(),
            [](const auto& a, const auto& b) { return a->lastUsed < b->lastUsed; });
        atlas.residentBytes -= (*oldest)->pixels.size();
        atlas.pages.erase(oldest);
        ++atlas.evictions;
    }
    try {
        auto page = std::make_shared<AtlasPage>();
        if (!ensureAtlasPage(*page, width, height, channels)) return {};
        page->lastUsed = ++atlas.accessTick;
        atlas.pages.push_back(page);
        atlas.residentBytes += bytes;
        ++atlas.allocations;
        return page;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

std::uint64_t makeGlyphKey(size_t faceIndex, unsigned int glyphIndex) {
    return ((static_cast<std::uint64_t>(faceIndex) + 1ULL) << 32) | static_cast<std::uint64_t>(glyphIndex);
}

size_t faceIndexFromGlyphKey(std::uint64_t key) {
    return static_cast<size_t>((key >> 32) - 1ULL);
}

unsigned int glyphIndexFromGlyphKey(std::uint64_t key) {
    return static_cast<unsigned int>(key & 0xffffffffULL);
}

std::string glyphCacheKey(const FontFace& face, float fontSize, unsigned int glyphIndex, bool colored) {
    return face.path + "#" + std::to_string(face.memoryRevision) + "#" +
           std::to_string(static_cast<int>(std::round(fontSize * 64.0f))) + "#" +
           (colored ? "color#" : "gray#") +
           std::to_string(glyphIndex);
}

std::string existingPath(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return path.string();
    }
    return {};
}

std::string firstExistingPath(std::initializer_list<const char*> paths) {
    for (const char* path : paths) {
        if (path == nullptr || path[0] == '\0') {
            continue;
        }
        if (const std::string existing = existingPath(path); !existing.empty()) {
            return existing;
        }
    }
    return {};
}

std::string& defaultUiFontFileOverride() {
    static std::string value;
    return value;
}

std::string& defaultIconFontFileOverride() {
    static std::string value;
    return value;
}

std::string resolveFontFilePath(const std::string& path);

std::string resolveSystemUiFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/arial.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-sans-cjk/NotoSansCJK-Regular.ttc",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf"
    });
#endif
}

std::string resolveSystemIconFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/SegMDL2.ttf",
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/segoeui.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Helvetica.ttc"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/fontawesome/fa-solid-900.ttf",
        "/usr/share/fonts/TTF/fa-solid-900.ttf",
        "/usr/share/fonts/truetype/font-awesome/fa-solid-900.ttf",
        "/usr/share/fonts/opentype/font-awesome/Font Awesome 6 Free-Solid-900.otf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans-symbols2/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-symbols-fonts/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-symbols/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSansSymbols[wght].ttf"
    });
#endif
}

std::string resolveSystemEmojiFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/seguisym.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/Apple Color Emoji.ttc"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto/NotoColorEmoji.ttf",
        // Fedora / RHEL family
        "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"
    });
#endif
}

std::string resolveSystemMonospaceFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/CascadiaCode.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Supplemental/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/Hack-Regular.ttf",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans-mono/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-mono-fonts/NotoSansMono-Regular.ttf",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/source-foundry-hack-fonts/Hack-Regular.ttf"
    });
#endif
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::vector<char> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer.data()).parent_path();
#elif defined(__APPLE__)
    std::vector<char> buffer(4096);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
    }
    std::error_code error;
    return std::filesystem::absolute(std::filesystem::path(buffer.data()), error).parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(length)] = '\0';
    std::error_code error;
    return std::filesystem::absolute(std::filesystem::path(buffer.data()), error).parent_path();
#else
    return {};
#endif
}

std::string resolveProjectAssetPath(const std::string& filename) {
    const std::filesystem::path exeDir = executableDirectory();
    const std::filesystem::path candidates[] = {
        exeDir / "assets" / filename,
        std::filesystem::path("assets") / filename,
        std::filesystem::path("..") / "assets" / filename,
        std::filesystem::path("..") / ".." / "assets" / filename
    };

    for (const auto& candidate : candidates) {
        if (const std::string path = existingPath(candidate); !path.empty()) {
            return path;
        }
    }
    return {};
}

std::string resolveDefaultUiFontPath() {
    const std::string& override = defaultUiFontFileOverride();
    const std::string path = override.empty() ? resolveProjectAssetPath(kDefaultUiFontFile) : resolveFontFilePath(override);
    if (isMemoryFontReference(path)) {
        return path;
    }
    if (const std::string existing = existingPath(path); !existing.empty()) {
        return existing;
    }
    return resolveSystemUiFontPath();
}

std::string resolveDefaultIconFontPath() {
    const std::string& override = defaultIconFontFileOverride();
    const std::string path = override.empty() ? resolveProjectAssetPath(kDefaultIconFontFile) : resolveFontFilePath(override);
    if (isMemoryFontReference(path)) {
        return path;
    }
    if (const std::string existing = existingPath(path); !existing.empty()) {
        return existing;
    }
    return resolveSystemIconFontPath();
}

std::string resolveFontFilePath(const std::string& path) {
    if (isMemoryFontReference(path)) {
        return path;
    }
    const std::filesystem::path raw(path);
    if (const std::string existing = existingPath(raw); !existing.empty()) {
        return existing;
    }

    const std::filesystem::path exeDir = executableDirectory();
    const std::filesystem::path candidates[] = {
        exeDir / "assets" / raw.filename(),
        exeDir / raw,
        std::filesystem::path("assets") / raw.filename(),
        std::filesystem::path("..") / "assets" / raw.filename(),
        std::filesystem::path("..") / ".." / "assets" / raw.filename()
    };

    for (const auto& candidate : candidates) {
        if (const std::string existing = existingPath(candidate); !existing.empty()) {
            return existing;
        }
    }
    return path;
}

bool isEmojiFontPath(const std::string& path) {
    const std::string filename = std::filesystem::path(path).filename().string();
    return filename.find("Emoji") != std::string::npos ||
           filename.find("emoji") != std::string::npos ||
           filename.find("seguiemj") != std::string::npos;
}

bool loadFontFace(const std::string& path, float fontSize, FontFace& face) {
    FT_Library library = sharedFreeTypeLibrary();
    if (!library) {
        return false;
    }

    FT_Face loadedFace = nullptr;
    std::shared_ptr<const FontBytes> bytes;
    std::uint64_t revision = 0;
    FT_Error openError = 0;
    if (isMemoryFontReference(path)) {
        const auto stored = memoryFonts().find(path);
        if (stored == memoryFonts().end()) {
            return false;
        }
        bytes = stored->second.bytes;
        revision = stored->second.revision;
        openError = FT_New_Memory_Face(library, bytes->data(),
                                       static_cast<FT_Long>(bytes->size()), 0, &loadedFace);
    } else {
        openError = FT_New_Face(library, path.c_str(), 0, &loadedFace);
    }
    if (openError != 0 || !loadedFace) {
        return false;
    }

    const bool emojiFont = isEmojiFontPath(path);
    float pixelHeightScale = 1.0f;
    if (loadedFace->units_per_EM > 0 && loadedFace->ascender != loadedFace->descender) {
        const float designHeight = static_cast<float>(loadedFace->ascender - loadedFace->descender);
        pixelHeightScale = static_cast<float>(loadedFace->units_per_EM) / designHeight;
    }
    const float scaledFontSize = std::max(1.0f, fontSize * pixelHeightScale);

    FT_Error sizeError = 1;
    float glyphScale = 1.0f;
    if ((emojiFont || FT_HAS_COLOR(loadedFace)) && loadedFace->num_fixed_sizes > 0) {
        const float targetPpem = scaledFontSize * 64.0f;
        int bestStrike = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < loadedFace->num_fixed_sizes; ++i) {
            const float strikePpem = static_cast<float>(loadedFace->available_sizes[i].y_ppem);
            const float distance = std::fabs(strikePpem - targetPpem);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestStrike = i;
            }
        }
        sizeError = FT_Select_Size(loadedFace, bestStrike);
        const float strikePpem = static_cast<float>(loadedFace->available_sizes[bestStrike].y_ppem) / 64.0f;
        if (strikePpem > 0.0f) {
            glyphScale = fontSize / strikePpem;
        }
    }
    if (sizeError != 0) {
        sizeError = FT_Set_Char_Size(
            loadedFace,
            0,
            static_cast<FT_F26Dot6>(std::lround(scaledFontSize * 64.0f)),
            72,
            72);
        if (sizeError != 0) {
            const FT_UInt pixelSize = static_cast<FT_UInt>(std::max(1.0f, std::round(scaledFontSize)));
            sizeError = FT_Set_Pixel_Sizes(loadedFace, 0, pixelSize);
        }
    }
    if (sizeError != 0) {
        FT_Done_Face(loadedFace);
        return false;
    }

    face.path = path;
    face.bytes = std::move(bytes);
    face.memoryRevision = revision;
    face.face = loadedFace;
    face.size = fontSize;
    face.glyphScale = glyphScale;
    face.colored = emojiFont || FT_HAS_COLOR(loadedFace);
    if (loadedFace->size && loadedFace->size->metrics.y_ppem > 0) {
        face.ascent = static_cast<float>(loadedFace->size->metrics.ascender) / 64.0f * glyphScale;
        face.descent = static_cast<float>(loadedFace->size->metrics.descender) / 64.0f * glyphScale;
        const float height = static_cast<float>(loadedFace->size->metrics.height) / 64.0f * glyphScale;
        face.lineGap = height - (face.ascent - face.descent);
    } else {
        face.ascent = fontSize * 0.8f;
        face.descent = -fontSize * 0.2f;
        face.lineGap = 0.0f;
    }
    return true;
}

std::string fontStackCacheKey(const std::string& fontPath, float fontSize) {
    return fontPath + "#" + std::to_string(static_cast<int>(std::round(fontSize * 64.0f)));
}

// 只有主字体缺字时才解析后备字体；正常中文布局不做多余的文件系统探测。
void resolveLazyFallbackPaths(FontInfoHolder& holder) {
    if (holder.fallbackPathsResolved) {
        return;
    }
    auto addLazyFallback = [&](const std::string& fallbackPath) {
        if (fallbackPath.empty() || fallbackPath == holder.primaryPath) {
            return;
        }
        if (std::find(holder.lazyFallbackPaths.begin(), holder.lazyFallbackPaths.end(), fallbackPath) == holder.lazyFallbackPaths.end()) {
            holder.lazyFallbackPaths.push_back(fallbackPath);
        }
    };

    addLazyFallback(resolveDefaultUiFontPath());
    addLazyFallback(resolveDefaultIconFontPath());
    addLazyFallback(resolveSystemUiFontPath());
    addLazyFallback(resolveSystemIconFontPath());
    addLazyFallback(resolveSystemEmojiFontPath());

#ifdef _WIN32
    addLazyFallback("C:/Windows/Fonts/seguiemj.ttf");
    addLazyFallback("C:/Windows/Fonts/seguisym.ttf");
    addLazyFallback("C:/Windows/Fonts/msyh.ttc");
    addLazyFallback("C:/Windows/Fonts/simhei.ttf");
#elif defined(__APPLE__)
    addLazyFallback("/System/Library/Fonts/Apple Color Emoji.ttc");
    addLazyFallback("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    addLazyFallback("/System/Library/Fonts/Supplemental/Arial.ttf");
#else
    // Debian / Ubuntu layout
    addLazyFallback("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf");
    addLazyFallback("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    addLazyFallback("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    // Fedora / RHEL family
    addLazyFallback("/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf");
    addLazyFallback("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf");
    addLazyFallback("/usr/share/fonts/google-noto-sans-cjk/NotoSansCJK-Regular.ttc");
    addLazyFallback("/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc");
    addLazyFallback("/usr/share/fonts/dejavu/DejaVuSans.ttf");
#endif
    holder.fallbackPathsResolved = true;
}

std::shared_ptr<FontInfoHolder> loadSharedFontStack(const std::string& fontPath, float fontSize) {
    const std::string cacheKey = fontStackCacheKey(fontPath, fontSize);
    FontStackCache& cache = sharedFontStackCache();
    const auto existing = cache.entries.find(cacheKey);
    if (existing != cache.entries.end()) {
        existing->second.lastUsed = ++cache.accessTick;
        return existing->second.holder;
    }

    auto holder = std::make_shared<FontInfoHolder>();
    holder->primaryPath = fontPath;

    auto addLoadedFace = [&](const std::string& path) {
        if (path.empty()) {
            return false;
        }
        const bool alreadyLoaded = std::any_of(holder->faces.begin(), holder->faces.end(),
                                               [&](const FontFace& loadedFace) {
                                                   return loadedFace.path == path;
                                               });
        if (alreadyLoaded) {
            return true;
        }
        FontFace face;
        if (!loadFontFace(path, fontSize, face)) {
            return false;
        }
        holder->faces.push_back(std::move(face));
        return true;
    };

    if (isMemoryFontReference(fontPath) && !addLoadedFace(fontPath)) {
        return {};
    }
    if (!addLoadedFace(fontPath) &&
        !addLoadedFace(resolveDefaultUiFontPath()) &&
        !addLoadedFace(resolveSystemUiFontPath())) {
        return {};
    }

    if (cache.entries.size() >= kFontStackCacheCapacity) {
        const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.lastUsed < right.second.lastUsed;
                                             });
        if (oldest != cache.entries.end()) {
            cache.entries.erase(oldest);
        }
    }

    const auto inserted = cache.entries.emplace(cacheKey, FontStackCacheEntry{holder, ++cache.accessTick});
    return inserted.first->second.holder;
}

bool isCombiningMark(unsigned int codepoint) {
    return (codepoint >= 0x0300 && codepoint <= 0x036F) ||
           (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
           (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
           (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
           (codepoint >= 0xFE20 && codepoint <= 0xFE2F);
}

bool isEmojiCodepoint(unsigned int codepoint) {
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) ||
           (codepoint >= 0x2600 && codepoint <= 0x27BF);
}

bool nextCodepointIsEmojiPresentation(const std::string& text, size_t index) {
    if (index >= text.size()) {
        return false;
    }
    const size_t saved = index;
    const unsigned int next = readUtf8Codepoint(text, index);
    (void)saved;
    return next == 0xFE0F;
}

unsigned int readUtf8Codepoint(const std::string& text, size_t& index) {
    const unsigned char first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) {
        return first;
    }
    if ((first >> 5) == 0x6 && index < text.size()) {
        return ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[index++]) & 0x3F);
    }
    if ((first >> 4) == 0xE && index + 1 < text.size()) {
        unsigned int cp = (first & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    if ((first >> 3) == 0x1E && index + 2 < text.size()) {
        unsigned int cp = (first & 0x07) << 18;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    return '?';
}

size_t findFaceForCodepoint(FontInfoHolder& holder, unsigned int codepoint, float fontSize) {
    if (holder.faces.empty()) {
        return 0;
    }
    if (codepoint == ' ' || codepoint == '\t' || codepoint == 0) {
        return 0;
    }

    for (size_t i = 0; i < holder.faces.size(); ++i) {
        if (FT_Get_Char_Index(holder.faces[i].face, codepoint) != 0) {
            return i;
        }
    }

    resolveLazyFallbackPaths(holder);
    for (const std::string& fallbackPath : holder.lazyFallbackPaths) {
        if (fallbackPath.empty()) {
            continue;
        }

        const bool alreadyLoaded = std::any_of(holder.faces.begin(), holder.faces.end(),
                                               [&](const FontFace& loadedFace) {
                                                   return loadedFace.path == fallbackPath;
                                               });
        if (alreadyLoaded) {
            continue;
        }

        FontFace fallback;
        if (!loadFontFace(fallbackPath, fontSize, fallback)) {
            continue;
        }

        const bool hasGlyph = FT_Get_Char_Index(fallback.face, codepoint) != 0;
        holder.faces.push_back(std::move(fallback));
        if (hasGlyph) {
            return holder.faces.size() - 1;
        }
    }

    return 0;
}

size_t findEmojiFaceForCodepoint(FontInfoHolder& holder, unsigned int codepoint, float fontSize) {
    for (size_t i = 0; i < holder.faces.size(); ++i) {
        if (holder.faces[i].colored && FT_Get_Char_Index(holder.faces[i].face, codepoint) != 0) {
            return i;
        }
    }

    resolveLazyFallbackPaths(holder);
    for (const std::string& fallbackPath : holder.lazyFallbackPaths) {
        if (fallbackPath.empty() || !isEmojiFontPath(fallbackPath)) {
            continue;
        }

        const auto loaded = std::find_if(holder.faces.begin(), holder.faces.end(),
                                         [&](const FontFace& loadedFace) {
                                             return loadedFace.path == fallbackPath;
                                         });
        if (loaded != holder.faces.end()) {
            if (FT_Get_Char_Index(loaded->face, codepoint) != 0) {
                return static_cast<size_t>(std::distance(holder.faces.begin(), loaded));
            }
            continue;
        }

        FontFace fallback;
        if (!loadFontFace(fallbackPath, fontSize, fallback)) {
            continue;
        }

        const bool hasGlyph = FT_Get_Char_Index(fallback.face, codepoint) != 0;
        holder.faces.push_back(std::move(fallback));
        if (hasGlyph) {
            return holder.faces.size() - 1;
        }
    }

    return findFaceForCodepoint(holder, codepoint, fontSize);
}

float loadGlyphAdvance(const FontFace& face,
                       unsigned int glyphIndex,
                       unsigned int codepoint,
                       float fontSize,
                       float maxGlyphHeight) {
    if (codepoint == '\t') {
        return fontSize * 4.0f;
    }
    if (isCombiningMark(codepoint)) {
        return 0.0f;
    }
    if (FT_Load_Glyph(face.face, glyphIndex, kGlyphLoadFlags) != 0) {
        return fontSize * 0.5f;
    }

    float glyphScale = face.glyphScale;
    const float glyphHeight = static_cast<float>(face.face->glyph->metrics.height) / 64.0f;
    if (face.colored && glyphHeight > 0.0f) {
        glyphScale = std::min(glyphScale, maxGlyphHeight / glyphHeight);
    }
    if (FT_IS_SCALABLE(face.face)) {
        return static_cast<float>(face.face->glyph->linearHoriAdvance) / 65536.0f * glyphScale;
    }
    return static_cast<float>(face.face->glyph->advance.x) / 64.0f * glyphScale;
}

std::vector<TextPrimitive::ShapedGlyph> shapeWithFallback(FontInfoHolder& holder,
                                                          const std::string& text,
                                                          float fontSize) {
    std::vector<TextPrimitive::ShapedGlyph> shaped;
    const float maxGlyphHeight = holder.faces.front().ascent - holder.faces.front().descent;
    size_t index = 0;
    while (index < text.size()) {
        const size_t start = index;
        const unsigned int codepoint = readUtf8Codepoint(text, index);
        const bool preferEmoji = isEmojiCodepoint(codepoint) || nextCodepointIsEmojiPresentation(text, index);
        const size_t faceIndex = preferEmoji
            ? findEmojiFaceForCodepoint(holder, codepoint, fontSize)
            : findFaceForCodepoint(holder, codepoint, fontSize);
        const FontFace& face = holder.faces[faceIndex];
        const unsigned int glyphIndex = codepoint == '\t' ? 0 : FT_Get_Char_Index(face.face, codepoint);
        const float advance = loadGlyphAdvance(face, glyphIndex, codepoint, fontSize, maxGlyphHeight);
        shaped.push_back({glyphIndex == 0 && codepoint == '\t' ? 0 : makeGlyphKey(faceIndex, glyphIndex),
                          codepoint,
                          static_cast<int>(start),
                          static_cast<int>(index),
                          advance,
                          0.0f,
                          0.0f});
    }
    return shaped;
}

std::vector<TextPrimitive::ShapedGlyph> shapeTextWithFontStack(FontInfoHolder& holder,
                                                               const std::string& text,
                                                               float fontSize) {
    // shapeWithFallback already advances UTF-8 one codepoint at a time, so a
    // glyph's byteEnd is the next glyph's byteStart.  Avoid rescanning every
    // glyph to rediscover that boundary; long editable text is measured often.
    return shapeWithFallback(holder, text, fontSize);
}

TextPrimitive::TextMetrics makeTextMetrics(const std::string& text,
                                           const std::vector<TextPrimitive::ShapedGlyph>& shaped) {
    TextPrimitive::TextMetrics metrics;
    metrics.byteIndices.reserve(shaped.size() + 1);
    metrics.caretX.reserve(shaped.size() + 1);
    auto addStop = [&](int byteIndex, float x) {
        byteIndex = std::clamp(byteIndex, 0, static_cast<int>(text.size()));
        if (!metrics.byteIndices.empty() && metrics.byteIndices.back() == byteIndex) {
            metrics.caretX.back() = x;
            return;
        }
        metrics.byteIndices.push_back(byteIndex);
        metrics.caretX.push_back(x);
    };

    addStop(0, 0.0f);

    float cursorX = 0.0f;
    for (const TextPrimitive::ShapedGlyph& glyph : shaped) {
        const float startX = cursorX;
        cursorX += glyph.advance;
        addStop(glyph.byteStart, startX);
        addStop(glyph.byteEnd, cursorX);
    }

    metrics.width = cursorX;
    addStop(static_cast<int>(text.size()), metrics.width);
    return metrics;
}

bool appendToAtlas(AtlasPage& page,
                   const unsigned char* pixels,
                   int width,
                   int height,
                   int channels,
                   TextPrimitive::Glyph& glyph) {
    if (width <= 0 || height <= 0 || pixels == nullptr || channels <= 0 || page.pixels.empty()) {
        return true;
    }

    int nextX = page.x, nextY = page.y, rowHeight = page.rowHeight;
    if (nextX + width + 1 >= page.width) {
        nextX = 1;
        nextY += rowHeight + 1;
        rowHeight = 0;
    }
    if (width + 2 > page.width || nextY + height + 1 >= page.height) {
        return false;
    }
    page.x = nextX;
    page.y = nextY;
    page.rowHeight = rowHeight;

    const int copyChannels = std::min(channels, page.channels);
    for (int row = 0; row < height; ++row) {
        const auto dstOffset = (static_cast<std::size_t>(page.y + row) * static_cast<std::size_t>(page.width) +
                                static_cast<std::size_t>(page.x)) *
                               static_cast<std::size_t>(page.channels);
        const auto srcOffset = static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
        unsigned char* dst = page.pixels.data() + dstOffset;
        const unsigned char* src = pixels + srcOffset;
        for (int x = 0; x < width; ++x) {
            std::copy(src + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels),
                      src + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels) + copyChannels,
                      dst + static_cast<std::size_t>(x) * static_cast<std::size_t>(page.channels));
        }
    }

    glyph.u0 = static_cast<float>(page.x) / static_cast<float>(page.width);
    glyph.v0 = static_cast<float>(page.y) / static_cast<float>(page.height);
    glyph.u1 = static_cast<float>(page.x + width) / static_cast<float>(page.width);
    glyph.v1 = static_cast<float>(page.y + height) / static_cast<float>(page.height);
    glyph.atlasPageId = page.id;

    page.x += width + 1;
    page.rowHeight = std::max(page.rowHeight, height);
    page.generation = ++textAtlasGenerationCounter();
    return true;
}

bool storeAtlasGlyph(bool colored, const std::string& key,
                     const unsigned char* pixels, int width, int height,
                     TextPrimitive::Glyph& glyph) {
    auto& atlas = sharedTextAtlas();
    const int channels = colored ? 4 : 1;
    for (auto it = atlas.pages.rbegin(); it != atlas.pages.rend(); ++it) {
        auto& page = **it;
        if (page.channels == channels && appendToAtlas(page, pixels, width, height, channels, glyph)) {
            page.glyphs[key] = glyph;
            page.lastUsed = ++atlas.accessTick;
            return true;
        }
    }
    const auto page = newAtlasPage(colored, width, height);
    if (!page || !appendToAtlas(*page, pixels, width, height, channels, glyph)) return false;
    page->glyphs[key] = glyph;
    return true;
}

std::vector<unsigned char> copyGrayBitmap(const FT_Bitmap& bitmap) {
    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer ||
        std::abs(bitmap.pitch) < static_cast<int>(bitmap.width)) {
        return {};
    }
    std::vector<unsigned char> compact(static_cast<size_t>(bitmap.width) * bitmap.rows);
    const int pitch = std::abs(bitmap.pitch);
    const unsigned char* base = bitmap.pitch >= 0
        ? bitmap.buffer
        : bitmap.buffer + static_cast<ptrdiff_t>(bitmap.rows - 1) * pitch;
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        const unsigned char* source = bitmap.pitch >= 0
            ? base + static_cast<ptrdiff_t>(row) * pitch
            : base - static_cast<ptrdiff_t>(row) * pitch;
        std::copy(source, source + bitmap.width, compact.begin() + static_cast<ptrdiff_t>(row) * bitmap.width);
    }
    return compact;
}

std::vector<unsigned char> copyBgraBitmapAsRgba(const FT_Bitmap& bitmap) {
    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer ||
        std::abs(bitmap.pitch) < static_cast<int>(bitmap.width * 4)) {
        return {};
    }
    std::vector<unsigned char> compact(static_cast<size_t>(bitmap.width) * bitmap.rows * 4);
    const int pitch = std::abs(bitmap.pitch);
    const unsigned char* base = bitmap.pitch >= 0
        ? bitmap.buffer
        : bitmap.buffer + static_cast<ptrdiff_t>(bitmap.rows - 1) * pitch;
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        const unsigned char* source = bitmap.pitch >= 0
            ? base + static_cast<ptrdiff_t>(row) * pitch
            : base - static_cast<ptrdiff_t>(row) * pitch;
        unsigned char* target = compact.data() + static_cast<ptrdiff_t>(row) * bitmap.width * 4;
        for (unsigned int x = 0; x < bitmap.width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return compact;
}

} // namespace

struct TextPrimitive::Impl {
    struct LaidOutGlyph {
        Glyph glyph;
        float x = 0.0f;
        float y = 0.0f;
        ShapedGlyph shaped;
    };

    struct Line {
        std::vector<LaidOutGlyph> glyphs;
        float width = 0.0f;
        float inkTop = 0.0f;
        float inkBottom = 0.0f;
        bool hasInk = false;
    };

    Impl() = default;
    Impl(float x, float y) : position_{x, y} {}
    ~Impl() { destroy(); }

    bool initialize();
    void destroy();

    void setPosition(float x, float y);
    void setText(const std::string& text);
    void setFontFamily(const std::string& fontFamily);
    void setFontSize(float fontSize);
    void setFontWeight(int fontWeight);
    void setColor(const Color& color);
    void setMaxWidth(float maxWidth);
    void setWrap(bool wrap);
    void setHorizontalAlign(HorizontalAlign align);
    void setVerticalAlign(VerticalAlign align);
    void setLineHeight(float lineHeight);
    void setStyle(const TextStyle& style);
    void setVisualScale(float originX, float originY, float scale);
    void setTransform(const Transform& transform, const Rect& frame);
    void setTransformMatrix(const TransformMatrix& matrix);

    const TextStyle& style() const;
    Vec2 position() const;
    Vec2 measuredSize();
    static float measureTextWidth(const std::string& text,
                                  const std::string& fontFamily = {},
                                  float fontSize = 16.0f,
                                  int fontWeight = 400);
    static TextMetrics measureTextMetrics(const std::string& text,
                                          const std::string& fontFamily = {},
                                          float fontSize = 16.0f,
                                          int fontWeight = 400);
    static Vec2 measureTextSize(const TextStyle& style);
    static void setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile);

    void prepare();
    void render(int windowWidth, int windowHeight);

    bool loadFont();
    void refreshMemoryFonts();
    bool ensureGlyph(const ShapedGlyph& shaped);
    Glyph* findGlyph(std::uint64_t key);
    void cacheGlyph(std::uint64_t key, const Glyph& glyph);
    void invalidateLayout();
    void invalidateVertices();
    void rebuildLayout();
    void rebuildVertices();
    std::vector<ShapedGlyph> shapeText(const std::string& text);
    void appendShapedGlyphToLine(Line& line, const ShapedGlyph& shaped, float& cursorX);

    static unsigned int readCodepoint(const std::string& text, size_t& index);
    static std::string resolveFontPath(const std::string& fontFamily, int fontWeight);

    Vec2 position_;
    Vec2 visualScaleOrigin_;
    float visualScale_ = 1.0f;
    Transform transform_;
    Rect transformFrame_;
    TransformMatrix transformMatrix_;
    bool hasTransformMatrix_ = false;
    TextStyle style_;
    std::shared_ptr<void> fontInfoStorage_;
    float scale_ = 1.0f;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float lineGap_ = 0.0f;

    std::unordered_map<std::uint64_t, Glyph> glyphs_;

    std::vector<Line> lines_;
    std::vector<float> vertices_;
    // 顶点只缓存几何位置；提交前为每个字形验证页标识并更新 UV。
    std::vector<ShapedGlyph> vertexGlyphs_;
    Vec2 measuredSize_;
    bool layoutDirty_ = true;
    bool verticesDirty_ = true;
    bool fontDirty_ = true;
    std::uint64_t memoryFontRevision_ = 0;
    bool atlasRetained_ = false;
};

bool TextPrimitive::Impl::initialize() {
    if (atlasRetained_) return true;
    if (!loadFont() || !retainSharedTextAtlas()) {
        return false;
    }
    atlasRetained_ = true;
    return true;
}

void TextPrimitive::Impl::destroy() {
    if (atlasRetained_) releaseSharedTextAtlas();
    atlasRetained_ = false;
    fontInfoStorage_.reset();
    glyphs_.clear();
    glyphs_.rehash(0);
    lines_.clear();
    vertices_.clear();
    vertexGlyphs_.clear();
    measuredSize_ = {};
    layoutDirty_ = true;
    verticesDirty_ = true;
    fontDirty_ = true;
}

void TextPrimitive::Impl::setPosition(float x, float y) {
    if (position_.x == x && position_.y == y) {
        return;
    }
    position_ = {x, y};
    invalidateVertices();
}

void TextPrimitive::Impl::setText(const std::string& text) {
    if (style_.text == text) {
        return;
    }
    if (style_.text.capacity() / 4u > text.size()) {
        std::string compactText = text;
        style_.text.swap(compactText);
    } else {
        style_.text = text;
    }
    // 虚拟列表会复用 TextPrimitive。glyph cache 仅服务于当前内容；共享 atlas
    // 仍保留栅格化结果，因此清理实例缓存不会重复分配 GPU atlas。
    glyphs_.clear();
    glyphs_.rehash(0);
    if (lines_.capacity() > 256u) {
        std::vector<Line>().swap(lines_);
    }
    // 长文本产生的顶点容量不能跟随 slot 永久保留；普通文本更新仍复用小缓存。
    constexpr std::size_t kMinimumRetainedVertexFloats = 32u * 1024u;
    const std::size_t desiredCapacity = std::max(
        kMinimumRetainedVertexFloats,
        text.size() <= std::numeric_limits<std::size_t>::max() / 48u
            ? text.size() * 48u
            : std::numeric_limits<std::size_t>::max());
    if (vertices_.capacity() > desiredCapacity) {
        std::vector<float>().swap(vertices_);
        std::vector<ShapedGlyph>().swap(vertexGlyphs_);
    }
    invalidateLayout();
}

void TextPrimitive::Impl::setFontFamily(const std::string& fontFamily) {
    if (style_.fontFamily == fontFamily) {
        return;
    }
    style_.fontFamily = fontFamily;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setFontSize(float fontSize) {
    if (style_.fontSize == fontSize) {
        return;
    }
    style_.fontSize = fontSize;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setFontWeight(int fontWeight) {
    if (style_.fontWeight == fontWeight) {
        return;
    }
    style_.fontWeight = fontWeight;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setColor(const Color& color) {
    style_.color = color;
}

void TextPrimitive::Impl::setMaxWidth(float maxWidth) {
    if (style_.maxWidth == maxWidth) {
        return;
    }
    style_.maxWidth = maxWidth;
    invalidateLayout();
}

void TextPrimitive::Impl::setWrap(bool wrap) {
    if (style_.wrap == wrap) {
        return;
    }
    style_.wrap = wrap;
    invalidateLayout();
}

void TextPrimitive::Impl::setHorizontalAlign(HorizontalAlign align) {
    if (style_.horizontalAlign == align) {
        return;
    }
    style_.horizontalAlign = align;
    invalidateVertices();
}

void TextPrimitive::Impl::setVerticalAlign(VerticalAlign align) {
    if (style_.verticalAlign == align) {
        return;
    }
    style_.verticalAlign = align;
    invalidateVertices();
}

void TextPrimitive::Impl::setLineHeight(float lineHeight) {
    if (style_.lineHeight == lineHeight) {
        return;
    }
    style_.lineHeight = lineHeight;
    invalidateLayout();
}

void TextPrimitive::Impl::setVisualScale(float originX, float originY, float scale) {
    const float nextScale = std::max(0.01f, scale);
    if (visualScaleOrigin_.x == originX && visualScaleOrigin_.y == originY && visualScale_ == nextScale) {
        return;
    }
    visualScaleOrigin_ = {originX, originY};
    visualScale_ = nextScale;
    invalidateVertices();
}

void TextPrimitive::Impl::setTransform(const Transform& transform, const Rect& frame) {
    auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    auto closeVec = [&](const Vec2& left, const Vec2& right) {
        return close(left.x, right.x) && close(left.y, right.y);
    };
    const bool sameTransform =
        closeVec(transform_.translate, transform.translate) &&
        close(transform_.translateZ, transform.translateZ) &&
        closeVec(transform_.scale, transform.scale) &&
        close(transform_.rotate, transform.rotate) &&
        close(transform_.rotateX, transform.rotateX) &&
        close(transform_.rotateY, transform.rotateY) &&
        closeVec(transform_.origin, transform.origin) &&
        close(transform_.perspective, transform.perspective);
    const bool sameFrame =
        close(transformFrame_.x, frame.x) &&
        close(transformFrame_.y, frame.y) &&
        close(transformFrame_.width, frame.width) &&
        close(transformFrame_.height, frame.height);
    if (sameTransform && sameFrame) {
        return;
    }
    transform_ = transform;
    transformFrame_ = frame;
    hasTransformMatrix_ = false;
    invalidateVertices();
}

void TextPrimitive::Impl::setTransformMatrix(const TransformMatrix& matrix) {
    auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    const bool same =
        close(transformMatrix_.m00, matrix.m00) &&
        close(transformMatrix_.m01, matrix.m01) &&
        close(transformMatrix_.tx, matrix.tx) &&
        close(transformMatrix_.m10, matrix.m10) &&
        close(transformMatrix_.m11, matrix.m11) &&
        close(transformMatrix_.ty, matrix.ty) &&
        close(transformMatrix_.px, matrix.px) &&
        close(transformMatrix_.py, matrix.py) &&
        close(transformMatrix_.pw, matrix.pw) &&
        hasTransformMatrix_;
    if (same) {
        return;
    }
    transformMatrix_ = matrix;
    hasTransformMatrix_ = true;
    invalidateVertices();
}

void TextPrimitive::Impl::setStyle(const TextStyle& style) {
    const bool fontChanged = style.fontFamily != style_.fontFamily ||
                             style.fontSize != style_.fontSize ||
                             style.fontWeight != style_.fontWeight;
    style_ = style;
    fontDirty_ = fontDirty_ || fontChanged;
    invalidateLayout();
}

const TextStyle& TextPrimitive::Impl::style() const {
    return style_;
}

Vec2 TextPrimitive::Impl::position() const {
    return position_;
}

Vec2 TextPrimitive::Impl::measuredSize() {
    refreshMemoryFonts();
    if (layoutDirty_) {
        rebuildLayout();
    }
    return measuredSize_;
}

float TextPrimitive::Impl::measureTextWidth(const std::string& text,
                                      const std::string& fontFamily,
                                      float fontSize,
                                      int fontWeight) {
    return measureTextMetrics(text, fontFamily, fontSize, fontWeight).width;
}

TextPrimitive::TextMetrics TextPrimitive::Impl::measureTextMetrics(const std::string& text,
                                                             const std::string& fontFamily,
                                                             float fontSize,
                                                             int fontWeight) {
    TextMetrics empty;
    empty.byteIndices = {0};
    empty.caretX = {0.0f};
    if (text.empty()) {
        return empty;
    }

    const float size = std::max(1.0f, fontSize);
    const std::string fontPath = resolveFontPath(fontFamily, fontWeight);
    // 仅缓存不可变内存字体覆盖的短文本，限制占用，保留外部后备字体的更新行为。
    const bool cacheable = isMemoryFontReference(fontPath) && text.size() <= kTextMetricsCacheMaxBytes;
    TextMetricsCache& cache = sharedTextMetricsCache();
    TextSizeCacheKey cacheKey;
    if (cacheable) {
        cacheKey = {text, fontPath, size, 0.0f, 0.0f, fontWeight, false};
        const auto cached = cache.entries.find(cacheKey);
        if (cached != cache.entries.end()) {
            cached->second.lastUsed = ++cache.accessTick;
            return cached->second.metrics;
        }
    }
    auto holder = loadSharedFontStack(fontPath, size);
    if (!holder || holder->faces.empty()) {
        return empty;
    }

    auto metrics = makeTextMetrics(text, shapeTextWithFontStack(*holder, text, size));
    if (cacheable && !holder->fallbackPathsResolved) {
        if (cache.entries.size() >= kTextMetricsCacheCapacity) {
            const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest != cache.entries.end()) {
                cache.entries.erase(oldest);
            }
        }
        cache.entries.emplace(std::move(cacheKey), TextMetricsCacheEntry{metrics, ++cache.accessTick});
    }
    return metrics;
}

Vec2 TextPrimitive::Impl::measureTextSize(const TextStyle& style) {
    TextSizeCache& cache = sharedTextSizeCache();
    TextSizeCacheKey cacheKey{
        style.text,
        style.fontFamily,
        style.fontSize,
        style.wrap ? style.maxWidth : 0.0f,
        style.lineHeight,
        style.fontWeight,
        style.wrap
    };
    const auto cached = cache.entries.find(cacheKey);
    if (cached != cache.entries.end()) {
        cached->second.lastUsed = ++cache.accessTick;
        return cached->second.size;
    }

    const float lineHeight = style.lineHeight > 0.0f ? style.lineHeight : style.fontSize * 1.2f;
    const float maxWidth = style.wrap && style.maxWidth > 0.0f ? style.maxWidth : 0.0f;
    float measuredWidth = 0.0f;
    int lineCount = 0;

    size_t paragraphStart = 0;
    while (paragraphStart <= style.text.size()) {
        const size_t newline = style.text.find('\n', paragraphStart);
        const size_t paragraphEnd = newline == std::string::npos ? style.text.size() : newline;
        std::string paragraph = style.text.substr(paragraphStart, paragraphEnd - paragraphStart);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }

        const TextMetrics metrics = measureTextMetrics(paragraph, style.fontFamily, style.fontSize, style.fontWeight);
        float lineWidth = 0.0f;
        ++lineCount;
        for (size_t index = 1; index < metrics.caretX.size(); ++index) {
            const float advance = metrics.caretX[index] - metrics.caretX[index - 1];
            if (maxWidth > 0.0f && lineWidth > 0.0f && lineWidth + advance > maxWidth) {
                measuredWidth = std::max(measuredWidth, lineWidth);
                lineWidth = 0.0f;
                ++lineCount;
            }
            lineWidth += advance;
        }
        measuredWidth = std::max(measuredWidth, lineWidth);

        if (newline == std::string::npos) {
            break;
        }
        paragraphStart = newline + 1;
    }

    const Vec2 measuredSize{measuredWidth, static_cast<float>(lineCount) * lineHeight};
    if (cache.entries.size() >= kTextSizeCacheCapacity) {
        const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.lastUsed < right.second.lastUsed;
                                             });
        if (oldest != cache.entries.end()) {
            cache.entries.erase(oldest);
        }
    }
    cache.entries.emplace(std::move(cacheKey), TextSizeCacheEntry{measuredSize, ++cache.accessTick});
    return measuredSize;
}

void TextPrimitive::Impl::setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile) {
    if (defaultUiFontFileOverride() == textFontFile &&
        defaultIconFontFileOverride() == iconFontFile) {
        return;
    }
    defaultUiFontFileOverride() = textFontFile;
    defaultIconFontFileOverride() = iconFontFile;
    ++memoryFontRevision();
    clearSharedFontStackCache();
}

void TextPrimitive::Impl::prepare() {
    refreshMemoryFonts();
    if (layoutDirty_) {
        rebuildLayout();
    }
    if (verticesDirty_) {
        rebuildVertices();
    }
}

void TextPrimitive::Impl::render(int windowWidth, int windowHeight) {
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr || windowWidth <= 0 || windowHeight <= 0) {
        return;
    }

    prepare();

    if (vertices_.empty()) {
        return;
    }

    std::shared_ptr<AtlasPage> runPage;
    std::size_t runStart = 0, runFloats = 0;
    const auto submit = [&] {
        if (!runPage || runFloats == 0) return;
        core::render::TextDrawCommand command{};
        command.vertices = vertices_.data() + runStart;
        command.vertexFloatCount = runFloats;
        command.color = style_.color;
        const auto kind = runPage->channels == 4
            ? core::render::TextAtlasPageKind::Color : core::render::TextAtlasPageKind::Gray;
        const core::render::TextAtlasPageData page{
            kind, runPage->width, runPage->height, runPage->channels,
            runPage->generation, runPage->pixels.data(), runPage->id};
        if (runPage->channels == 4) {
            // 彩色批次不使用灰度采样，但统一接口仍需要一个有效灰度纹理。
            static const unsigned char emptyGray = 0;
            command.grayAtlas = {core::render::TextAtlasPageKind::Gray, 1, 1, 1, 1, &emptyGray};
            command.colorAtlas = page;
        } else {
            command.grayAtlas = page;
        }
        backend->drawText(command, windowWidth, windowHeight);
        runFloats = 0;
    };
    for (std::size_t i = 0; i < vertexGlyphs_.size(); ++i) {
        const auto& shaped = vertexGlyphs_[i];
        if (!ensureGlyph(shaped)) {
            ++core::render::currentRenderFrameStats().textGlyphsUnavailable;
            submit();
            continue;
        }
        const auto* glyph = findGlyph(shaped.key);
        auto page = glyph ? findAtlasPage(glyph->atlasPageId) : nullptr;
        if (!page || !(glyph->u1 > glyph->u0) || !(glyph->v1 > glyph->v0)) {
            ++core::render::currentRenderFrameStats().textGlyphsUnavailable;
            submit();
            continue;
        }
        const std::size_t offset = i * 30;
        if (!runPage || runPage->id != page->id || runStart + runFloats != offset) {
            submit();
            runPage = std::move(page);
            runStart = offset;
        }
        const float uv[12]{glyph->u0, glyph->v0, glyph->u1, glyph->v0,
                           glyph->u1, glyph->v1, glyph->u0, glyph->v0,
                           glyph->u1, glyph->v1, glyph->u0, glyph->v1};
        for (std::size_t vertex = 0; vertex < 6; ++vertex) {
            vertices_[offset + vertex * 5 + 2] = uv[vertex * 2];
            vertices_[offset + vertex * 5 + 3] = uv[vertex * 2 + 1];
            vertices_[offset + vertex * 5 + 4] = glyph->colored ? 1.f : 0.f;
        }
        runFloats += 30;
        ++core::render::currentRenderFrameStats().textGlyphsSubmitted;
    }
    submit();
}

bool TextPrimitive::Impl::loadFont() {
    const std::string fontPath = resolveFontPath(style_.fontFamily, style_.fontWeight);
    auto holder = loadSharedFontStack(fontPath, style_.fontSize);
    if (!holder || holder->faces.empty()) {
        return false;
    }

    fontInfoStorage_ = holder;
    scale_ = 1.0f;
    ascent_ = holder->faces.front().ascent;
    descent_ = holder->faces.front().descent;
    lineGap_ = holder->faces.front().lineGap;

    glyphs_.clear();

    fontDirty_ = false;
    memoryFontRevision_ = memoryFontRevision();
    return true;
}

void TextPrimitive::Impl::refreshMemoryFonts() {
    if (memoryFontRevision_ != memoryFontRevision()) {
        fontDirty_ = true;
        invalidateLayout();
    }
}

bool TextPrimitive::Impl::ensureGlyph(const ShapedGlyph& shaped) {
    if (shaped.key == 0) {
        return true;
    }
    if (const auto* cached = findGlyph(shaped.key)) {
        if (cached->width == 0 || cached->height == 0 || findAtlasPage(cached->atlasPageId))
            return true;
    }

    if (fontDirty_ && !loadFont()) {
        return false;
    }

    auto holder = std::static_pointer_cast<FontInfoHolder>(fontInfoStorage_);
    if (!holder || holder->faces.empty()) {
        return false;
    }

    const size_t faceIndex = faceIndexFromGlyphKey(shaped.key);
    const unsigned int glyphIndex = glyphIndexFromGlyphKey(shaped.key);
    if (faceIndex >= holder->faces.size()) {
        return false;
    }

    FontFace& face = holder->faces[faceIndex];
    Glyph glyph;
    glyph.advance = shaped.advance;
    glyph.colored = face.colored;

    if (glyphIndex == 0 || shaped.codepoint == ' ' || shaped.codepoint == '\t') {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    if (FT_Load_Glyph(face.face, glyphIndex, kGlyphLoadFlags) != 0) {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    FT_GlyphSlot slot = face.face->glyph;
    glyph.advance = shaped.advance;

    if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
    }

    const FT_Bitmap& bitmap = slot->bitmap;
    const bool colorBitmap = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
    float glyphScale = face.glyphScale;
    if (colorBitmap && bitmap.rows > 0) {
        const float emHeight = ascent_ - descent_;
        glyphScale = std::min(glyphScale, emHeight / static_cast<float>(bitmap.rows));
    }
    glyph.colored = colorBitmap;
    glyph.xOffset = static_cast<float>(slot->bitmap_left) * glyphScale;
    glyph.yOffset = ascent_ - static_cast<float>(slot->bitmap_top) * glyphScale;
    glyph.width = static_cast<float>(bitmap.width) * glyphScale;
    glyph.height = static_cast<float>(bitmap.rows) * glyphScale;
    if (colorBitmap) {
        glyph.yOffset = ascent_ - descent_ - glyph.height;
    }

    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer) {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    const std::string cacheKey = glyphCacheKey(face, style_.fontSize, glyphIndex, colorBitmap);
    if (cachedAtlasGlyph(cacheKey, glyph)) {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    if (colorBitmap) {
        std::vector<unsigned char> rgba = copyBgraBitmapAsRgba(bitmap);
        if (rgba.empty()) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        if (!storeAtlasGlyph(true, cacheKey, rgba.data(), static_cast<int>(bitmap.width),
                             static_cast<int>(bitmap.rows), glyph)) return false;
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        std::vector<unsigned char> gray = copyGrayBitmap(bitmap);
        if (gray.empty()) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        if (!storeAtlasGlyph(false, cacheKey, gray.data(), static_cast<int>(bitmap.width),
                             static_cast<int>(bitmap.rows), glyph)) return false;
    } else {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    cacheGlyph(shaped.key, glyph);
    return true;
}

TextPrimitive::Glyph* TextPrimitive::Impl::findGlyph(std::uint64_t key) {
    const auto it = glyphs_.find(key);
    return it == glyphs_.end() ? nullptr : &it->second;
}

void TextPrimitive::Impl::cacheGlyph(std::uint64_t key, const Glyph& glyph) {
    if (key == 0) {
        return;
    }
    glyphs_[key] = glyph;
}

void TextPrimitive::Impl::invalidateLayout() {
    layoutDirty_ = true;
    invalidateVertices();
}

void TextPrimitive::Impl::rebuildLayout() {
    if (fontDirty_ && !loadFont()) {
        layoutDirty_ = false;
        return;
    }

    lines_.clear();
    measuredSize_ = {};

    Line currentLine;
    float cursorX = 0.0f;
    const float lineHeight = style_.lineHeight > 0.0f ? style_.lineHeight : style_.fontSize * 1.2f;
    const float maxWidth = style_.maxWidth > 0.0f ? style_.maxWidth : 0.0f;

    size_t paragraphStart = 0;
    while (paragraphStart <= style_.text.size()) {
        const size_t newline = style_.text.find('\n', paragraphStart);
        const size_t paragraphEnd = newline == std::string::npos ? style_.text.size() : newline;
        std::string paragraph = style_.text.substr(paragraphStart, paragraphEnd - paragraphStart);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }

        const std::vector<ShapedGlyph> shaped = shapeText(paragraph);
        for (const ShapedGlyph& glyph : shaped) {
            const float advance = glyph.advance;
            if (style_.wrap && maxWidth > 0.0f && cursorX > 0.0f && cursorX + advance > maxWidth) {
                measuredSize_.x = std::max(measuredSize_.x, currentLine.width);
                lines_.push_back(currentLine);
                currentLine = {};
                cursorX = 0.0f;
            }

            appendShapedGlyphToLine(currentLine, glyph, cursorX);
        }

        if (newline == std::string::npos) {
            break;
        }

        measuredSize_.x = std::max(measuredSize_.x, currentLine.width);
        lines_.push_back(currentLine);
        currentLine = {};
        cursorX = 0.0f;
        paragraphStart = newline + 1;
    }

    measuredSize_.x = std::max(measuredSize_.x, currentLine.width);
    lines_.push_back(currentLine);
    measuredSize_.y = lines_.empty() ? 0.0f : static_cast<float>(lines_.size()) * lineHeight;
    layoutDirty_ = false;
    invalidateVertices();
}

void TextPrimitive::Impl::invalidateVertices() {
    verticesDirty_ = true;
}

void TextPrimitive::Impl::rebuildVertices() {
    vertices_.clear();
    vertexGlyphs_.clear();
    const float lineHeight = style_.lineHeight > 0.0f ? style_.lineHeight : style_.fontSize * 1.2f;
    const auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    // 纯平移（含滚动和居中的小数位移）仍按最终物理像素对齐字形，避免二次插值。
    // 缩放、旋转和透视保持原变换，不能将变形后的字形强制吸附到像素网格。
    const bool pixelAlignedMatrix = hasTransformMatrix_ &&
        close(transformMatrix_.m00, 1.0f) && close(transformMatrix_.m01, 0.0f) &&
        close(transformMatrix_.m10, 0.0f) && close(transformMatrix_.m11, 1.0f) &&
        close(transformMatrix_.px, 0.0f) && close(transformMatrix_.py, 0.0f) &&
        close(transformMatrix_.pw, 1.0f);
    float blockYOffset = 0.0f;
    if (style_.verticalAlign == VerticalAlign::Center) {
        float inkTop = std::numeric_limits<float>::max();
        float inkBottom = std::numeric_limits<float>::lowest();
        for (size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex) {
            const Line& line = lines_[lineIndex];
            if (!line.hasInk) {
                continue;
            }
            const float lineY = static_cast<float>(lineIndex) * lineHeight;
            inkTop = std::min(inkTop, lineY + line.inkTop);
            inkBottom = std::max(inkBottom, lineY + line.inkBottom);
        }
        if (inkTop <= inkBottom) {
            blockYOffset = -(inkTop + inkBottom) * 0.5f;
        } else {
            blockYOffset = -measuredSize_.y * 0.5f;
        }
    } else if (style_.verticalAlign == VerticalAlign::Bottom) {
        blockYOffset = -measuredSize_.y;
    }

    for (size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex) {
        const Line& line = lines_[lineIndex];
        float lineX = position_.x;
        if (style_.horizontalAlign == HorizontalAlign::Center) {
            lineX -= line.width * 0.5f;
        } else if (style_.horizontalAlign == HorizontalAlign::Right) {
            lineX -= line.width;
        }

        const float lineY = position_.y + blockYOffset + static_cast<float>(lineIndex) * lineHeight;
        for (const LaidOutGlyph& laidOut : line.glyphs) {
            const Glyph& glyph = laidOut.glyph;
            if (glyph.width <= 0 || glyph.height <= 0) continue;
            const float x0 = lineX + laidOut.x + glyph.xOffset;
            const float y0 = lineY + laidOut.y + glyph.yOffset;
            const float x1 = x0 + glyph.width;
            const float y1 = y0 + glyph.height;
            const float colored = glyph.colored ? 1.0f : 0.0f;
            Vec2 p0{x0, y0};
            Vec2 p1{x1, y0};
            Vec2 p2{x1, y1};
            Vec2 p3{x0, y1};

            if (hasTransformMatrix_) {
                p0 = core::transformPoint(transformMatrix_, p0.x, p0.y);
                p1 = core::transformPoint(transformMatrix_, p1.x, p1.y);
                p2 = core::transformPoint(transformMatrix_, p2.x, p2.y);
                p3 = core::transformPoint(transformMatrix_, p3.x, p3.y);
            } else if (std::fabs(transform_.translate.x) > 0.0001f ||
                std::fabs(transform_.translate.y) > 0.0001f ||
                std::fabs(transform_.scale.x - 1.0f) > 0.0001f ||
                std::fabs(transform_.scale.y - 1.0f) > 0.0001f ||
                std::fabs(transform_.rotate) > 0.0001f) {
                const Vec2 origin{
                    transformFrame_.x + transformFrame_.width * transform_.origin.x,
                    transformFrame_.y + transformFrame_.height * transform_.origin.y
                };
                const float cosine = std::cos(transform_.rotate);
                const float sine = std::sin(transform_.rotate);
                auto transformPoint = [&](Vec2 point) {
                    const float scaledX = (point.x - origin.x) * transform_.scale.x;
                    const float scaledY = (point.y - origin.y) * transform_.scale.y;
                    return Vec2{
                        origin.x + scaledX * cosine - scaledY * sine + transform_.translate.x,
                        origin.y + scaledX * sine + scaledY * cosine + transform_.translate.y
                    };
                };
                p0 = transformPoint(p0);
                p1 = transformPoint(p1);
                p2 = transformPoint(p2);
                p3 = transformPoint(p3);
            }

            if (!hasTransformMatrix_ && std::fabs(visualScale_ - 1.0f) > 0.0001f) {
                auto scalePoint = [&](Vec2 point) {
                    return Vec2{
                        visualScaleOrigin_.x + (point.x - visualScaleOrigin_.x) * visualScale_,
                        visualScaleOrigin_.y + (point.y - visualScaleOrigin_.y) * visualScale_
                    };
                };
                p0 = scalePoint(p0);
                p1 = scalePoint(p1);
                p2 = scalePoint(p2);
                p3 = scalePoint(p3);
            }

            if (!glyph.colored && pixelAlignedMatrix) {
                const float offsetX = std::round(p0.x) - p0.x;
                const float offsetY = std::round(p0.y) - p0.y;
                p0 = {p0.x + offsetX, p0.y + offsetY};
                p1 = {p1.x + offsetX, p1.y + offsetY};
                p2 = {p2.x + offsetX, p2.y + offsetY};
                p3 = {p3.x + offsetX, p3.y + offsetY};
            }

            vertices_.insert(vertices_.end(), {
                p0.x, p0.y, glyph.u0, glyph.v0, colored,
                p1.x, p1.y, glyph.u1, glyph.v0, colored,
                p2.x, p2.y, glyph.u1, glyph.v1, colored,
                p0.x, p0.y, glyph.u0, glyph.v0, colored,
                p2.x, p2.y, glyph.u1, glyph.v1, colored,
                p3.x, p3.y, glyph.u0, glyph.v1, colored
            });
            vertexGlyphs_.push_back(laidOut.shaped);
        }
    }
    verticesDirty_ = false;
}

std::vector<TextPrimitive::ShapedGlyph> TextPrimitive::Impl::shapeText(const std::string& text) {
    if (fontDirty_ && !loadFont()) {
        return {};
    }
    auto holder = std::static_pointer_cast<FontInfoHolder>(fontInfoStorage_);
    if (!holder || holder->faces.empty()) {
        return {};
    }
    return shapeTextWithFontStack(*holder, text, std::max(1.0f, style_.fontSize));
}

void TextPrimitive::Impl::appendShapedGlyphToLine(Line& line, const ShapedGlyph& shaped, float& cursorX) {
    if (!ensureGlyph(shaped)) {
        cursorX += shaped.advance;
        line.width = cursorX;
        return;
    }

    const Glyph* glyph = findGlyph(shaped.key);
    if (glyph && shaped.key != 0 && shaped.codepoint != ' ' && shaped.codepoint != '\t') {
        line.glyphs.push_back({*glyph, cursorX + shaped.xOffset, shaped.yOffset, shaped});
        const float top = shaped.yOffset + glyph->yOffset;
        const float bottom = top + glyph->height;
        if (!line.hasInk) {
            line.inkTop = top;
            line.inkBottom = bottom;
            line.hasInk = true;
        } else {
            line.inkTop = std::min(line.inkTop, top);
            line.inkBottom = std::max(line.inkBottom, bottom);
        }
    }
    cursorX += shaped.advance;
    line.width = cursorX;
}

unsigned int TextPrimitive::Impl::readCodepoint(const std::string& text, size_t& index) {
    return readUtf8Codepoint(text, index);
}

std::string TextPrimitive::Impl::resolveFontPath(const std::string& fontFamily, int fontWeight) {
    if (isMemoryFontReference(fontFamily)) {
        return fontFamily;
    }
    if (!fontFamily.empty() && fontFamily.find('.') != std::string::npos) {
        return resolveFontFilePath(fontFamily);
    }

    if (fontFamily == "YouSheBiaoTiHei" || fontFamily == "YouShe") {
        return resolveFontFilePath("YouSheBiaoTiHei-2.ttf");
    }

    if (fontFamily == "Title" || fontFamily == "PingFang" || fontFamily == "PingFang SC") {
        return resolveDefaultUiFontPath();
    }

    if (fontFamily == "FontAwesome" || fontFamily == "Font Awesome" ||
        fontFamily == "Font Awesome 7 Free" || fontFamily == "Icon") {
        return resolveDefaultIconFontPath();
    }

#ifdef _WIN32
    if (fontFamily == "monospace" || fontFamily == "Mono" ||
        fontFamily == "Cascadia Mono" || fontFamily == "Cascadia Code") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
    }
    if (fontFamily == "Microsoft YaHei" || fontFamily == "YaHei") {
        return "C:/Windows/Fonts/msyh.ttc";
    }
    if (fontFamily == "Segoe UI Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    if (fontFamily == "SimHei") {
        return "C:/Windows/Fonts/simhei.ttf";
    }
    if (fontWeight >= 600) {
        return resolveDefaultUiFontPath();
    }
    return resolveDefaultUiFontPath();
#elif defined(__APPLE__)
    if (fontFamily == "monospace" || fontFamily == "Mono" ||
        fontFamily == "SF Mono" || fontFamily == "Menlo" || fontFamily == "Monaco") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
    }
    if (fontFamily == "Noto Color Emoji" || fontFamily == "Apple Color Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    if (fontWeight >= 600) {
        return resolveDefaultUiFontPath();
    }
    return resolveDefaultUiFontPath();
#else
    (void)fontWeight;
    if (fontFamily == "monospace" || fontFamily == "Mono") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    if (fontFamily == "Noto Color Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    return resolveDefaultUiFontPath();
#endif
}

TextPrimitive::TextPrimitive()
    : impl_(std::make_unique<Impl>()) {}

TextPrimitive::TextPrimitive(float x, float y)
    : impl_(std::make_unique<Impl>(x, y)) {}

TextPrimitive::~TextPrimitive() = default;
TextPrimitive::TextPrimitive(TextPrimitive&&) noexcept = default;
TextPrimitive& TextPrimitive::operator=(TextPrimitive&&) noexcept = default;

bool TextPrimitive::initialize() { return impl_->initialize(); }
void TextPrimitive::destroy() { impl_->destroy(); }
void TextPrimitive::setPosition(float x, float y) { impl_->setPosition(x, y); }
void TextPrimitive::setText(const std::string& text) { impl_->setText(text); }
void TextPrimitive::setFontFamily(const std::string& fontFamily) { impl_->setFontFamily(fontFamily); }
void TextPrimitive::setFontSize(float fontSize) { impl_->setFontSize(fontSize); }
void TextPrimitive::setFontWeight(int fontWeight) { impl_->setFontWeight(fontWeight); }
void TextPrimitive::setColor(const Color& color) { impl_->setColor(color); }
void TextPrimitive::setMaxWidth(float maxWidth) { impl_->setMaxWidth(maxWidth); }
void TextPrimitive::setWrap(bool wrap) { impl_->setWrap(wrap); }
void TextPrimitive::setHorizontalAlign(HorizontalAlign align) { impl_->setHorizontalAlign(align); }
void TextPrimitive::setVerticalAlign(VerticalAlign align) { impl_->setVerticalAlign(align); }
void TextPrimitive::setLineHeight(float lineHeight) { impl_->setLineHeight(lineHeight); }
void TextPrimitive::setStyle(const TextStyle& style) { impl_->setStyle(style); }
void TextPrimitive::setVisualScale(float originX, float originY, float scale) { impl_->setVisualScale(originX, originY, scale); }
void TextPrimitive::setTransform(const Transform& transform, const Rect& frame) { impl_->setTransform(transform, frame); }
void TextPrimitive::setTransformMatrix(const TransformMatrix& matrix) { impl_->setTransformMatrix(matrix); }
const TextStyle& TextPrimitive::style() const { return impl_->style(); }
Vec2 TextPrimitive::position() const { return impl_->position(); }
Vec2 TextPrimitive::measuredSize() { return impl_->measuredSize(); }
void TextPrimitive::prepare() { impl_->prepare(); }
float TextPrimitive::measureTextWidth(const std::string& text,
                                      const std::string& fontFamily,
                                      float fontSize,
                                      int fontWeight) {
    return Impl::measureTextWidth(text, fontFamily, fontSize, fontWeight);
}
TextPrimitive::TextMetrics TextPrimitive::measureTextMetrics(const std::string& text,
                                                             const std::string& fontFamily,
                                                             float fontSize,
                                                             int fontWeight) {
    return Impl::measureTextMetrics(text, fontFamily, fontSize, fontWeight);
}

Vec2 TextPrimitive::measureTextSize(const TextStyle& style) {
    return Impl::measureTextSize(style);
}
void TextPrimitive::setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile) {
    Impl::setDefaultFontFiles(textFontFile, iconFontFile);
}
bool TextPrimitive::registerMemoryFont(const std::string& family, const void* data, std::size_t size) {
    if (!isMemoryFontReference(family) || family.size() == 7 || data == nullptr || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
        return false;
    }
    auto& fonts = memoryFonts();
    const auto existing = fonts.find(family);
    if (existing != fonts.end() && existing->second.bytes->size() == size &&
        std::memcmp(existing->second.bytes->data(), data, size) == 0) {
        return true;
    }
    try {
        const auto* begin = static_cast<const unsigned char*>(data);
        return registerMemoryFontShared(family, std::make_shared<const FontBytes>(begin, begin + size));
    } catch (const std::bad_alloc&) {
        return false;
    }
}
bool TextPrimitive::registerMemoryFontShared(const std::string& family,
                                            std::shared_ptr<const std::vector<unsigned char>> bytes) {
    if (!isMemoryFontReference(family) || family.size() == 7 || !bytes || bytes->empty() ||
        bytes->size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
        return false;
    }
    auto& fonts = memoryFonts();
    const auto existing = fonts.find(family);
    if (existing != fonts.end() &&
        (existing->second.bytes == bytes ||
         (existing->second.bytes->size() == bytes->size() &&
          std::memcmp(existing->second.bytes->data(), bytes->data(), bytes->size()) == 0))) {
        return true;
    }
    FT_Library library = sharedFreeTypeLibrary();
    if (!library) {
        return false;
    }
    try {
        FT_Face validation = nullptr;
        if (FT_New_Memory_Face(library, bytes->data(), static_cast<FT_Long>(bytes->size()), 0,
                               &validation) != 0 || validation == nullptr) {
            return false;
        }
        FT_Done_Face(validation);
        const auto revision = memoryFontRevision() + 1;
        fonts.insert_or_assign(family, MemoryFont{std::move(bytes), revision});
        memoryFontRevision() = revision;
        clearSharedFontStackCache();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
void TextPrimitive::render(int windowWidth, int windowHeight) { impl_->render(windowWidth, windowHeight); }

TextPrimitive::AtlasStatistics TextPrimitive::atlasStatistics() {
    const auto& atlas = sharedTextAtlas();
    return {atlas.pages.size(), atlas.residentBytes, kTextAtlasByteBudget,
            atlas.allocations, atlas.evictions};
}

} // namespace core
