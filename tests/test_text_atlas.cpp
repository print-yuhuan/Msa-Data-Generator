#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/render/text.h"
#include "core/window/window_backend.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int width = 640, height = 360;
constexpr const char* family = "memory:msa-atlas-regression";
using Pixels = std::vector<unsigned char>;
std::vector<unsigned> coveredCodepoints;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(unsigned value) {
    std::string result;
    result += char(0xe0 | (value >> 12));
    result += char(0x80 | ((value >> 6) & 63));
    result += char(0x80 | (value & 63));
    return result;
}

std::string repertoire(unsigned count) {
    std::string result;
    result.reserve(count * 3);
    require(count <= coveredCodepoints.size(), "字体覆盖不足以构造压力样本");
    for (unsigned i = 0; i < count; ++i) result += utf8(coveredCodepoints[i]);
    return result;
}

Pixels pixels() {
    Pixels image(width * height * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, image.data());
    require(glGetError() == GL_NO_ERROR, "读取字形图集回归帧失败");
    return image;
}

void save(const std::filesystem::path& path, const Pixels& image) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int row = height - 1; row >= 0; --row)
        output.write(reinterpret_cast<const char*>(image.data() + row * width * 3), width * 3);
    require(bool(output), "字形回归证据写入失败");
}

void hasInk(const Pixels& image) {
    require(std::count_if(image.begin(), image.end(), [](unsigned char v) { return v < 120; }) > 100,
            "文字缺少可见像素");
}

void budgets() {
    const auto cpu = core::TextPrimitive::atlasStatistics();
    require(cpu.residentBytes <= cpu.byteBudget, "CPU 字形缓存超过预算");
    const auto& gpu = core::render::currentRenderFrameStats();
    require(gpu.textAtlasResidentBytes <= 32u * 1024u * 1024u, "GPU 字形缓存超过预算");
}

std::unique_ptr<core::TextPrimitive> makeText(const std::string& value, float size) {
    auto text = std::make_unique<core::TextPrimitive>();
    text->setFontFamily(family);
    text->setFontSize(size);
    text->setPosition(12, 18);
    text->setText(value);
    text->setColor(core::Color("#20334B"));
    text->setTransformMatrix({});
    require(text->initialize(), "文字实例初始化失败");
    return text;
}

Pixels draw(core::window::Handle window, core::render::RenderBackend& backend,
            core::TextPrimitive& text) {
    backend.makeCurrent();
    core::render::ScopedRenderBackend scope(backend);
    backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
    backend.setScissor(false, {}, height);
    backend.clear(core::Color("#FFFFFF"));
    text.render(width, height);
    backend.endRenderCacheFrame();
    budgets();
    return pixels();
}

void lazyAtlasGrowth(core::window::Handle window, core::render::RenderBackend& backend) {
    require(core::TextPrimitive::atlasStatistics().residentBytes == 0,
            "按需分配回归必须从空字形缓存开始");
    auto blank = makeText(" \t ", 24);
    blank->prepare();
    require(core::TextPrimitive::atlasStatistics().residentBytes == 0,
            "空白文字提前分配了字形位图");
    auto small = makeText("测量 MSA", 24);
    const auto baseline = draw(window, backend, *small);
    const auto initial = core::TextPrimitive::atlasStatistics();
    require(initial.residentPages == 1 && initial.residentBytes > 0 &&
                initial.residentBytes < 2048u * 2048u,
            "少量字形没有按需分配小于完整页的位图");
    auto more = makeText(repertoire(1200), 24);
    more->prepare();
    require(core::TextPrimitive::atlasStatistics().residentBytes > initial.residentBytes,
            "追加字形没有扩展按需位图");
    require(draw(window, backend, *small) == baseline,
            "CPU行缓冲扩容或局部纹理更新改变了旧字形像素");
}

void partialAtlasUpload(core::window::Handle window, core::render::RenderBackend& backend) {
    // 相同逻辑页、相同纹理坐标；局部上传与整页清零参考必须逐像素一致。
    const auto render = [&](int usedWidth, int usedHeight, bool complete, bool color,
                            std::uint64_t pageId, std::uint64_t generation) {
        constexpr int logicalSize = 16;
        const int channels = color ? 4 : 1;
        const int uploadedRows = complete ? logicalSize : usedHeight;
        std::vector<unsigned char> bitmap(logicalSize * uploadedRows * channels, 0);
        for (int y = 1; y < usedHeight - 1; ++y)
            for (int x = 1; x < usedWidth - 1; ++x) {
                const auto at = (y * logicalSize + x) * channels;
                bitmap[at] = static_cast<unsigned char>(192 + (x * 29 + y * 17) % 64);
                if (color) {
                    bitmap[at + 1] = bitmap[at] / 2;
                    bitmap[at + 2] = bitmap[at] / 3;
                    bitmap[at + 3] = 255;
                }
            }
        const float right = 40.f + usedWidth * 12.75f, bottom = 32.f + usedHeight * 13.25f;
        const float u = float(usedWidth - 1) / logicalSize, v = float(usedHeight - 1) / logicalSize;
        const float colored = color ? 1.f : 0.f;
        const float vertices[]{40,32,0,0,colored, right,32,u,0,colored, right,bottom,u,v,colored,
                               40,32,0,0,colored, right,bottom,u,v,colored, 40,bottom,0,v,colored};
        core::render::TextDrawCommand command;
        command.vertices = vertices; command.vertexFloatCount = 30;
        command.color = core::Color("#20334B");
        core::render::TextAtlasPageData page{
            color ? core::render::TextAtlasPageKind::Color : core::render::TextAtlasPageKind::Gray,
            logicalSize, logicalSize, complete ? logicalSize : usedWidth,
            uploadedRows, channels, generation, bitmap.data(), pageId};
        static const unsigned char transparent = 0;
        if (color) {
            command.grayAtlas = {core::render::TextAtlasPageKind::Gray, 1, 1, 1, 1, 1, 1, &transparent};
            command.colorAtlas = page;
        } else command.grayAtlas = page;
        backend.makeCurrent();
        core::render::ScopedRenderBackend scope(backend);
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        backend.setScissor(false, {}, height);
        backend.clear(core::Color("#FFFFFF"));
        backend.drawText(command, width, height);
        backend.endRenderCacheFrame();
        return pixels();
    };
    for (bool color : {false, true}) {
        const auto baseId = (std::uint64_t(1) << 61) + (color ? 10 : 0);
        int generation = 1;
        for (const auto used : {std::array<int, 2>{5, 4}, {11, 7}, {14, 12}}) {
            const auto expected = render(used[0], used[1], true, color, baseId, generation);
            const auto actual = render(used[0], used[1], false, color, baseId + 1, generation++);
            hasInk(expected);
            require(actual == expected, "局部图集上传的步幅、透明边缘或同页更新错误");
        }
    }
}

void fontRevisionAndAdvance(const std::shared_ptr<const std::vector<unsigned char>>& bytes) {
    using Text = core::TextPrimitive;
    const auto initial = Text::fontConfigurationRevision();
    require(Text::registerMemoryFontShared(family, bytes), "相同共享字体重复注册失败");
    require(Text::registerMemoryFont(family, bytes->data(), bytes->size()), "相同字节复制注册失败");
    require(Text::fontConfigurationRevision() == initial, "相同字体使配置修订号失效");
    const unsigned char invalid[]{0, 1, 2, 3};
    require(!Text::registerMemoryFont(family, invalid, sizeof invalid), "无效字体替换未被拒绝");
    require(Text::fontConfigurationRevision() == initial, "无效字体替换改变了修订号");
    auto variant = std::make_shared<std::vector<unsigned char>>(*bytes);
    variant->push_back(0); // 合法字体末尾的填充不改变字形，但字节已发生变化。
    require(Text::registerMemoryFontShared(family, variant), "有效字体替换失败");
    require(Text::fontConfigurationRevision() == initial + 1, "有效字体替换没有更新修订号");
    require(Text::registerMemoryFontShared(family, bytes), "恢复基准字体失败");
    require(Text::fontConfigurationRevision() == initial + 2, "恢复字体没有更新修订号");
    Text::setDefaultFontFiles(family, family);
    require(Text::fontConfigurationRevision() == initial + 2, "相同默认字体改变了修订号");
    Text::setDefaultFontFiles(family, "");
    require(Text::fontConfigurationRevision() == initial + 3, "改变默认字体没有更新修订号");
    Text::setDefaultFontFiles(family, family);

    struct FreeTypeReference {
        FT_Library library{};
        FT_Face face{};
        ~FreeTypeReference() { if (face) FT_Done_Face(face); if (library) FT_Done_FreeType(library); }
    } reference;
    require(FT_Init_FreeType(&reference.library) == 0, "独立FreeType度量初始化失败");
    require(FT_New_Memory_Face(reference.library, bytes->data(), static_cast<FT_Long>(bytes->size()),
                             0, &reference.face) == 0, "独立FreeType度量加载字体失败");
    require(coveredCodepoints.size() >= 2000, "字体覆盖不足以验证advance缓存替换");
    std::vector<unsigned> sample(coveredCodepoints.begin(), coveredCodepoints.begin() + 2000);
    for (float size : {13.f, 24.5f, 51.75f}) {
        const float ratio = float(reference.face->units_per_EM) /
                            (reference.face->ascender - reference.face->descender);
        require(FT_Set_Char_Size(reference.face, 0, static_cast<FT_F26Dot6>(std::lround(size * ratio * 64)),
                                72, 72) == 0, "独立FreeType度量字号设置失败");
        for (int round = 0; round < 2; ++round) {
            std::string value;
            float expected = 0;
            const auto append = [&](unsigned codepoint) {
                value += codepoint < 128 ? std::string(1, char(codepoint)) : utf8(codepoint);
                const auto glyph = FT_Get_Char_Index(reference.face, codepoint);
                require(FT_Load_Glyph(reference.face, glyph,
                        FT_LOAD_DEFAULT | FT_LOAD_COLOR | FT_LOAD_NO_SVG | FT_LOAD_TARGET_LIGHT) == 0,
                        "独立FreeType字形度量失败");
                expected += float(reference.face->glyph->linearHoriAdvance) / 65536.f;
            };
            for (const auto cp : sample) { append(cp); append('W'); append('i'); }
            const auto actual = Text::measureTextWidth(value, family, size);
            require(std::abs(actual - expected) < .01f, "advance缓存与独立FreeType度量不一致");
            std::reverse(sample.begin(), sample.end());
        }
    }
}

// 延迟读取覆盖样本，避免逐帧同步改变连续提交时序；每个保留层都须完整覆盖面板。
class RetainedDrawCoverage {
public:
    RetainedDrawCoverage() : original_(glDrawArrays) {
        require(active_ == nullptr, "保留层绘制覆盖查询不可嵌套");
        active_ = this;
        glDrawArrays = capture;
    }
    RetainedDrawCoverage(const RetainedDrawCoverage&) = delete;
    RetainedDrawCoverage& operator=(const RetainedDrawCoverage&) = delete;
    ~RetainedDrawCoverage() {
        stopCapture();
        if (count_) glDeleteQueries(static_cast<GLsizei>(count_), queries_.data());
    }

    void verify() {
        stopCapture();
        require(!captureFailed_ && count_ > 0, "保留层绘制覆盖查询未完整记录");
        for (std::size_t i = 0; i < count_; ++i) {
            GLuint samples = 0;
            glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT, &samples);
            if (samples != 620u * 340u) {
                std::cerr << "保留层绘制 " << i + 1 << "：覆盖样本 " << samples
                          << "，预期 " << 620u * 340u << '\n';
                require(false, "保留层绘制覆盖不完整");
            }
        }
    }

private:
    static void APIENTRY capture(GLenum mode, GLint first, GLsizei count) {
        auto& recorder = *active_;
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        const bool imageDraw = program && glGetUniformLocation(program, "uYuvMode") >= 0;
        GLuint query = 0;
        if (imageDraw) {
            if (recorder.count_ < recorder.queries_.size()) {
                glGenQueries(1, &query);
                if (query) {
                    recorder.queries_[recorder.count_++] = query;
                    glBeginQuery(GL_SAMPLES_PASSED, query);
                } else recorder.captureFailed_ = true;
            } else recorder.captureFailed_ = true;
        }
        recorder.original_(mode, first, count);
        if (query) glEndQuery(GL_SAMPLES_PASSED);
    }

    void stopCapture() {
        if (active_ == this) {
            glDrawArrays = original_;
            active_ = nullptr;
        }
    }

    inline static RetainedDrawCoverage* active_ = nullptr;
    PFNGLDRAWARRAYSPROC original_;
    // 本样本每帧只有一个保留面板；预分配避免 OpenGL 回调分配内存或抛出异常。
    std::array<GLuint, 20> queries_{};
    std::size_t count_ = 0;
    bool captureFailed_ = false;
};

void retainedCompose(core::dsl::Runtime& runtime) {
    runtime.compose("atlas-retained", width, height,
        [](core::dsl::Ui& ui, const core::dsl::Screen&) {
            ui.stack("panel").position(8, 8).size(620, 340).content([&] {
                ui.rect("background").size(620, 340).color("#F5F7FA").build();
                for (int i = 0; i < 9; ++i)
                    ui.text("line" + std::to_string(i))
                        .position(14, 6.f + i * 35.f).size(590, 34)
                        .text("测量系统分析 / 保留层 " + std::to_string(i) + " 龙龘新字")
                        .fontFamily(family).fontSize(26).color("#20334B").build();
            }).build();
        });
}

Pixels drawRetained(core::window::Handle window, core::render::RenderBackend& backend,
                    core::dsl::Runtime& runtime) {
    backend.makeCurrent();
    core::render::ScopedRenderBackend scope(backend);
    RetainedDrawCoverage coverage;
    for (int frame = 0; frame < 20; ++frame) {
        runtime.update(window, 0, 1, 1, false);
        runtime.requestFullPaint();
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        runtime.render(width, height, 1, core::Color("#FFFFFF"));
    }
    coverage.verify();
    require(core::render::lastRenderFrameStats().retainedLayerDraws > 0,
            "保留层回归没有实际使用纹理缓存");
    budgets();
    return pixels();
}

void fallbackGeometry(core::window::Handle window, core::render::RenderBackend& backend) {
    // Consolas 中的中文回退到默认 Noto；同一栅格字形在两个主字体中
    // 有不同基线。冷缓存像素分别作为基准，再验证两个缓存命中方向。
    const auto make = [](bool mono) {
        auto text = makeText("测量系统 中文回退 🔬 A1", 38);
        if (mono) text->setFontFamily("monospace");
        return text;
    };
    std::array<Pixels, 2> baseline;
    for (int i = 0; i < 2; ++i) {
        auto text = make(i != 0);
        baseline[i] = draw(window, backend, *text);
        hasInk(baseline[i]);
    }
    for (int first = 0; first < 2; ++first) {
        auto a = make(first != 0);
        require(draw(window, backend, *a) == baseline[first], "后备字体冷缓存基线不一致");
        auto b = make(first == 0);
        require(draw(window, backend, *b) == baseline[1 - first],
                "共享字形 UV 缓存覆盖了另一主字体的基线或缩放");
        require(draw(window, backend, *a) == baseline[first], "交错使用后备字体改变了旧实例基线");
    }
}

void independentPageVersions(core::window::Handle window, core::render::RenderBackend& backend) {
    const std::array<unsigned char, 16> solid{255,255,255,255,255,255,255,255,
                                             255,255,255,255,255,255,255,255};
    const std::array<unsigned char, 16> striped{0,0,255,255,0,0,255,255,
                                               0,0,255,255,0,0,255,255};
    const auto render = [&](bool flushBetween) {
        backend.makeCurrent();
        core::render::ScopedRenderBackend scope(backend);
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        backend.setScissor(false, {}, height);
        backend.clear(core::Color("#FFFFFF"));
        for (int i = 0; i < 2; ++i) {
            const float left = 20.f + i * 140.f, right = left + 100.f;
            const float vertices[]{left,30,0,0,0, right,30,1,0,0, right,130,1,1,0,
                                   left,30,0,0,0, right,130,1,1,0, left,130,0,1,0};
            core::render::TextDrawCommand command;
            command.vertices = vertices; command.vertexFloatCount = 30;
            command.color = core::Color("#20334B");
            command.grayAtlas = {core::render::TextAtlasPageKind::Gray, 4, 4, 4, 4, 1, 7,
                i ? striped.data() : solid.data(), (std::uint64_t(1) << 62) + i};
            backend.drawText(command, width, height);
            if (flushBetween) backend.endRenderCacheFrame();
        }
        backend.endRenderCacheFrame();
        budgets();
        return pixels();
    };
    const auto expected = render(true); // 同时预热两页的 GPU 缓存。
    const auto actual = render(false);
    require(actual == expected, "同版本号的不同页合批后采样了错误纹理");
}

void run(core::window::Handle window, core::render::RenderBackend& backend,
         const std::filesystem::path& output) {
    lazyAtlasGrowth(window, backend);
    partialAtlasUpload(window, backend);
    std::array<std::unique_ptr<core::TextPrimitive>, 4> retained;
    std::array<Pixels, 4> baselines;
    const std::array<float, 4> sizes{18, 36, 38, 52};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        retained[i] = makeText("测量系统 / GRR 结果 13.95%\n丁朧擝龙龘 / 新旧字形", sizes[i]);
        baselines[i] = draw(window, backend, *retained[i]);
        hasInk(baselines[i]);
        save(output / ("before-" + std::to_string(int(sizes[i])) + ".ppm"), baselines[i]);
    }

    // 同一文本在多页间交错取字，必须维持原绘制次序和 UV 对应关系。
    const auto mixedValue = utf8(0x4e01) + utf8(0x6800) + utf8(0x7800) + " / " +
                            utf8(0x7800) + utf8(0x4e01) + utf8(0x6800);
    auto mixed = makeText(mixedValue, 36);
    const auto mixedBaseline = draw(window, backend, *mixed);
    auto pressure = makeText(repertoire(12000), 36);
    pressure->prepare();
    require(core::TextPrimitive::atlasStatistics().residentPages > 1,
            "没有超过旧单页图集容量");
    require(draw(window, backend, *mixed) == mixedBaseline, "交错页字形绘制改变了像素");

    core::dsl::Runtime runtime;
    require(runtime.initialize(window), "保留层界面初始化失败");
    retainedCompose(runtime);
    const auto retainedBaseline = drawRetained(window, backend, runtime);
    save(output / "retained-before.ppm", retainedBaseline);
    hasInk(retainedBaseline);

    // 单个 TextPrimitive、单次 render 超过 CPU/GPU 缓存容量；旧页淘汰后
    // 仍要能流式提交后续页，不可只支持多个短字符串的累积。
    pressure->setFontSize(76);
    pressure->setText(repertoire(20000) + "测量结束 ABC");
    const auto before = core::TextPrimitive::atlasStatistics();
    pressure->prepare();
    const auto prepared = core::TextPrimitive::atlasStatistics();
    require(prepared.evictions > before.evictions, "容量压力没有触发 LRU 淘汰");
    const auto submittedBefore = core::render::currentRenderFrameStats().textGlyphsSubmitted;
    const auto missingBefore = core::render::currentRenderFrameStats().textGlyphsUnavailable;
    const auto head = draw(window, backend, *pressure);
    require(core::render::currentRenderFrameStats().textGlyphsSubmitted - submittedBefore == 20007,
            "单次超过预算的文本没有提交全部有效字形");
    require(core::render::currentRenderFrameStats().textGlyphsUnavailable == missingBefore,
            "压力绘制出现无页或零 UV 字形");
    hasInk(head);
    save(output / "over-budget-head.ppm", head);
    const float tailWidth = core::TextPrimitive::measureTextWidth("测量结束 ABC", family, 76);
    pressure->setPosition(12 - pressure->measuredSize().x + tailWidth, 18);
    const auto tail = draw(window, backend, *pressure);
    hasInk(tail);
    save(output / "over-budget-tail.ppm", tail);
    const auto middleText = utf8(coveredCodepoints[10000]);
    auto middleReference = makeText(middleText, 76);
    const auto middleBaseline = draw(window, backend, *middleReference);
    const float prefixWidth = core::TextPrimitive::measureTextWidth(repertoire(10000), family, 76);
    pressure->setPosition(12 - prefixWidth, 18);
    const auto middle = draw(window, backend, *pressure);
    hasInk(middle);
    const int glyphRight = 12 + int(core::TextPrimitive::measureTextWidth(middleText, family, 76));
    for (int y = 18; y < 110; ++y)
        for (int x = 16; x < glyphRight - 4; ++x)
            for (int c = 0; c < 3; ++c) {
                const auto at = ((height - 1 - y) * width + x) * 3 + c;
                require(middle[at] == middleBaseline[at], "超预算文本中间字形与独立参考像素不同");
            }
    save(output / "over-budget-middle.ppm", middle);
    middleReference.reset();
    budgets();

    // 旧实例不重建文本，直接复用原布局/顶点；页淘汰应被自动识别。
    for (std::size_t i = 0; i < retained.size(); ++i) {
        const auto actual = draw(window, backend, *retained[i]);
        require(actual == baselines[i], "LRU 淘汰后的旧实例像素改变或缺字");
        save(output / ("after-" + std::to_string(int(sizes[i])) + ".ppm"), actual);
    }
    const auto retainedAfter = drawRetained(window, backend, runtime);
    save(output / "retained-after.ppm", retainedAfter);
    hasInk(retainedAfter);
    require(retainedAfter == retainedBaseline,
            "atlas 淘汰改变了已经缓存的保留层");
    {
        core::render::ScopedRenderBackend scope(backend);
        runtime.releaseGraphicsResources();
    }
    const auto retainedRebuilt = drawRetained(window, backend, runtime);
    save(output / "retained-rebuilt.ppm", retainedRebuilt);
    hasInk(retainedRebuilt);
    require(retainedRebuilt == retainedBaseline,
            "保留层重建后缺字或像素改变");

    // 两个真实 OpenGL 上下文分别维护纹理页；上下文销毁后不遗留悬挂句柄。
    for (int round = 0; round < 2; ++round) {
        core::window::WindowCreateRequest request;
        request.width = width; request.height = height;
        request.highDpi = false; request.visible = false;
        auto otherWindow = core::window::createWindow(request);
        require(bool(otherWindow), "第二窗口创建失败");
        auto otherBackend = core::render::createRenderBackend(otherWindow);
        try {
            require(otherBackend && otherBackend->initialize(), "第二 OpenGL 上下文初始化失败");
            require(draw(otherWindow, *otherBackend, *retained[1]) == baselines[1],
                    "第二上下文渲染的字形像素不一致");
        } catch (...) {
            otherBackend.reset(); core::window::destroyWindow(otherWindow); throw;
        }
        otherBackend.reset();
        core::window::destroyWindow(otherWindow);
        require(draw(window, backend, *retained[1]) == baselines[1],
                "恢复原上下文后字形纹理失效");
    }
    {
        core::render::ScopedRenderBackend scope(backend);
        runtime.shutdown();
    }
    const auto finalStats = core::TextPrimitive::atlasStatistics();
    for (auto& item : retained) item.reset();
    mixed.reset(); pressure.reset();
    require(core::TextPrimitive::atlasStatistics().residentBytes == 0,
            "最后一个文字实例销毁后 CPU 页仍被持有");
    fallbackGeometry(window, backend);
    independentPageVersions(window, backend);
    auto restarted = makeText("测量系统 / GRR 结果 13.95%\n丁朧擝龙龘 / 新旧字形", sizes[0]);
    require(draw(window, backend, *restarted) == baselines[0], "释放后重新创建字形图集失败");
    std::ofstream report(output / "text-atlas-regression.json");
    report << "{\n  \"passed\": true,\n  \"real_opengl_pixels\": true,\n"
           << "  \"sizes\": [18,36,38,52,76],\n  \"single_text_unique_characters\": 20000,\n"
           << "  \"cpu_byte_budget\": " << finalStats.byteBudget << ",\n"
           << "  \"page_allocations\": " << finalStats.allocations << ",\n"
           << "  \"page_evictions\": " << finalStats.evictions << ",\n"
           << "  \"old_new_pixels_identical\": true,\n  \"retained_layer_rebuilt\": true,\n"
           << "  \"retained_draw_coverage\": true,\n  \"retained_baseline_visible\": true,\n"
           << "  \"multiple_contexts\": true,\n  \"fallback_geometry_preserved\": true,\n"
           << "  \"release_and_recreate\": true,\n  \"lazy_cpu_allocation_and_growth\": true,\n"
           << "  \"partial_gray_color_upload\": true,\n  \"font_revision_invalidation\": true,\n"
           << "  \"advance_matches_freetype\": true\n}\n";
    require(bool(report), "无法写入字形图集回归报告");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    core::window::Handle window{};
    std::unique_ptr<core::render::RenderBackend> backend;
    bool glfwStarted = false;
    int status = 0;
    try {
        std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
        require(bool(input), "无法读取回归字体");
        auto bytes = std::make_shared<const std::vector<unsigned char>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        FT_Library ft{};
        FT_Face face{};
        require(FT_Init_FreeType(&ft) == 0, "FreeType 初始化失败");
        if (FT_New_Memory_Face(ft, bytes->data(), static_cast<FT_Long>(bytes->size()), 0, &face)) {
            FT_Done_FreeType(ft);
            throw std::runtime_error("无法读取压力样本的字体映射");
        }
        for (unsigned cp = 0x4e00; cp <= 0x9fff && coveredCodepoints.size() < 20000; ++cp)
            if (FT_Get_Char_Index(face, cp)) coveredCodepoints.push_back(cp);
        FT_Done_Face(face); FT_Done_FreeType(ft);
        require(core::TextPrimitive::registerMemoryFontShared(family, bytes), "共享字体注册失败");
        core::TextPrimitive::setDefaultFontFiles(family, family);
        fontRevisionAndAdvance(bytes);
        core::render::initializeRenderBackendLoader();
        glfwStarted = glfwInit() == GLFW_TRUE;
        require(glfwStarted, "GLFW 初始化失败");
        core::window::WindowCreateRequest request;
        request.width = width; request.height = height;
        request.highDpi = false; request.visible = false;
        window = core::window::createWindow(request);
        require(bool(window), "隐藏回归窗口创建失败");
        backend = core::render::createRenderBackend(window);
        require(backend && backend->initialize(), "OpenGL 初始化失败");
        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        run(window, *backend, output);
        std::cout << "字形图集分页、预算淘汰、跨上下文与真实像素回归通过\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; status = 1;
    }
    backend.reset();
    if (window) core::window::destroyWindow(window);
    if (glfwStarted) glfwTerminate();
    return status;
}
