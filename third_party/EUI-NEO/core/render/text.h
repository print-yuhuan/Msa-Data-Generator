#pragma once

#include "core/render/render_types.h"
#include "core/render/text_types.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

class TextPrimitive {
public:
    struct Glyph {
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        bool colored = false;
        // 页标识不持有页内存；LRU 淘汰后在绘制前按原字形重新准备。
        std::uint64_t atlasPageId = 0;
    };

    struct ShapedGlyph {
        std::uint64_t key = 0;
        unsigned int codepoint = 0;
        int byteStart = 0;
        int byteEnd = 0;
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
    };

    struct TextMetrics {
        float width = 0.0f;
        std::vector<int> byteIndices;
        std::vector<float> caretX;
    };

    TextPrimitive();
    TextPrimitive(float x, float y);
    ~TextPrimitive();

    TextPrimitive(const TextPrimitive&) = delete;
    TextPrimitive& operator=(const TextPrimitive&) = delete;
    TextPrimitive(TextPrimitive&&) noexcept;
    TextPrimitive& operator=(TextPrimitive&&) noexcept;

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
    // 在 UI/渲染线程调用。此接口复制字体字节，调用方可立即释放或改写输入。
    // 相同字节重复注册不改变缓存；无效替换保留原注册及现有字体实例。
    static bool registerMemoryFont(const std::string& family, const void* data, std::size_t size);
    // 共享只读字体的所有权，不复制底层字节；交接后不得通过其他别名改写数据。
    // 调用方可释放自己的 shared_ptr，注册表与字体实例会保持资源生命周期。
    static bool registerMemoryFontShared(
        const std::string& family,
        std::shared_ptr<const std::vector<unsigned char>> bytes);

    struct AtlasStatistics {
        std::size_t residentPages = 0;
        std::size_t residentBytes = 0;
        std::size_t byteBudget = 0;
        std::uint64_t allocations = 0;
        std::uint64_t evictions = 0;
    };
    // 只读诊断接口，与字体准备一样在 UI/渲染线程使用。
    static AtlasStatistics atlasStatistics();

    void prepare();
    void render(int windowWidth, int windowHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
