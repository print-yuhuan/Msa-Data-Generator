#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include "components/scrollview.h"
#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr int width = 512, height = 384;
using Pixels = std::vector<unsigned char>;

Pixels readPixels() {
    Pixels pixels(width * height * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR)
        throw std::runtime_error("could not read the rendered framebuffer");
    return pixels;
}

void save(const std::filesystem::path &path, const Pixels &pixels) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        output.write(reinterpret_cast<const char *>(pixels.data() + y * width * 3), width * 3);
    if (!output)
        throw std::runtime_error("could not save the rendering evidence");
}

void compose(core::dsl::Ui &ui, float w, float h, float offset) {
    components::scrollView(ui, "scroll")
        .size(w, h)
        .offset(offset)
        .content([](core::dsl::Ui &u, float cw, float) {
            u.column("cards")
                .width(cw)
                .gap(12)
                .content([&] {
                    for (int i = 0; i < 6; ++i) {
                        const auto id = "card" + std::to_string(i);
                        u.stack(id)
                            .size(cw, 106)
                            .content([&] {
                                u.rect(id + ".bg")
                                    .size(cw, 106)
                                    .color("#FFFFFF")
                                    .border(1, "#D5DFEF")
                                    .radius(10)
                                    .build();
                                for (int row = 0; row < 4; ++row)
                                    u.text(id + ".text" + std::to_string(row))
                                        .position(12, 6.f + row * 24.25f)
                                        .size(cw - 24, 24)
                                        .text(row == 0 ? "指标复核 / 测量数据"
                                                       : "GRR / TV %  13.95  0.00  满足")
                                        .fontSize(row == 0 ? 20.f : 16.f)
                                        .color("#20334B")
                                        .build();
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

void check(core::window::Handle window, core::render::RenderBackend &backend, float scale,
           const std::filesystem::path &output) {
    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const auto render = [&](float offset) {
        runtime.compose("test", width / scale, height / scale,
                        [&](core::dsl::Ui &u, const core::dsl::Screen &screen) {
                            compose(u, screen.width, screen.height, offset);
                        });
        for (int frame = 0; frame < 5; ++frame) {
            runtime.update(window, 0, 1, scale, false);
            runtime.requestFullPaint();
            backend.beginFrame(
                {window, core::window::nativeWindowInfo(window), width, height, scale});
            runtime.render(width, height, scale, core::Color("#F3F6FC"));
            if (core::render::lastRenderFrameStats().retainedLayerDraws != 0)
                throw std::runtime_error("scrolling descendants entered the static layer cache");
        }
        return readPixels();
    };
    const auto top = render(0);
    if (std::count_if(top.begin(), top.end(), [](unsigned char v) { return v < 96; }) < 100 ||
        std::count_if(top.begin(), top.end(), [](unsigned char v) { return v > 224; }) < 1000)
        throw std::runtime_error("fixture did not render the text and panel backgrounds");
    const auto scrolled = render(24);
    const auto returned = render(0);
    const auto name = std::to_string(int(scale * 100));
    save(output / (name + "_top.ppm"), top);
    save(output / (name + "_scrolled.ppm"), scrolled);
    save(output / (name + "_returned.ppm"), returned);
    int maxDelta = 0;
    std::size_t changed = 0;
    // Compare the same content after an integral physical-pixel scroll. Exclude
    // the viewport's clipped edges and scrollbar, whose thumb must move.
    const int shift = int(24 * scale);
    for (int y = shift + 2; y < height - 2; ++y)
        for (int x = 2; x < width - int(24 * scale); ++x)
            for (int c = 0; c < 3; ++c) {
                const auto atTop = ((height - 1 - y) * width + x) * 3 + c;
                const auto atScrolled = ((height - 1 - y + shift) * width + x) * 3 + c;
                const int delta = std::abs(int(top[atTop]) - int(scrolled[atScrolled]));
                maxDelta = std::max(maxDelta, delta);
                if (delta > 2)
                    ++changed;
            }
    std::cout << name << "%: max_delta=" << maxDelta << ", channels_over_2=" << changed << '\n';
    if (changed != 0)
        throw std::runtime_error("top-of-scroll UI differs from pixel-aligned scrolled content");
    if (top != returned)
        throw std::runtime_error("returning to the top changed the rendered pixels");

    // 连续滚动时，所有文字行及背景应共用同一物理像素位移，不能逐行跨阈值跳动。
    for (int frame = 0; frame < 24; ++frame) {
        const float offset = (24.f + frame * .137f) / scale;
        const auto expected = render(std::round(offset * scale) / scale);
        const auto actual = render(offset);
        for (int y = 2; y < height - 2; ++y)
            for (int x = 2; x < width - int(24 * scale); ++x)
                for (int c = 0; c < 3; ++c) {
                    const auto at = (y * width + x) * 3 + c;
                    if (expected[at] != actual[at])
                        throw std::runtime_error("scrolling rows move out of sync at " + name +
                                                 "% frame " + std::to_string(frame));
                }
    }
    std::cout << name << "%: 24 fractional scroll frames move as one surface\n";
    runtime.shutdown();
}
// 对比同一物理位置由布局坐标和父级平移表达时的字形，覆盖小数滚动与居中布局。
void checkFractionalText(core::window::Handle window, core::render::RenderBackend &backend,
                         float scale, const std::filesystem::path &output) {
    core::TextPrimitive text;
    if (!text.initialize())
        throw std::runtime_error("text initialization failed");
    text.setText("测量系统分析 / AI 结果解读\nGRR 13.95%  P/T 17.39  ndc 9");
    text.setFontSize(18 * scale);
    text.setLineHeight(28 * scale);
    text.setColor(core::Color("#20334B"));
    const auto render = [&](float dx, float dy, bool translated) {
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, scale});
        backend.setScissor(false, {}, height);
        backend.clear(core::Color("#FFFFFF"));
        text.setPosition(24 + (translated ? 0 : dx), 40 + (translated ? 0 : dy));
        core::TransformMatrix matrix;
        matrix.tx = translated ? dx : 0;
        matrix.ty = translated ? dy : 0;
        text.setTransformMatrix(matrix);
        text.render(width, height);
        backend.endRenderCacheFrame();
        return readPixels();
    };
    int caseIndex = 0;
    for (const auto delta : {core::Vec2{0.25f, 0.375f}, core::Vec2{-0.375f, -24.25f},
                             core::Vec2{0.75f, -24.625f}}) {
        const auto expected = render(delta.x, delta.y, false);
        const auto actual = render(delta.x, delta.y, true);
        if (std::count_if(actual.begin(), actual.end(), [](unsigned char v) { return v < 96; }) < 100)
            throw std::runtime_error("fractional text fixture did not draw visible glyphs");
        const auto prefix = std::to_string(int(scale * 100)) + "_fractional_" +
                            std::to_string(caseIndex++);
        save(output / (prefix + "_layout.ppm"), expected);
        save(output / (prefix + "_translation.ppm"), actual);
        if (actual != expected)
            throw std::runtime_error("fractional parent translation blurs text: " + prefix);
    }
    std::cout << int(scale * 100) << "%: fractional text pixel comparison passed\n";
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    core::render::initializeRenderBackendLoader();
    if (!glfwInit())
        return 2;
    core::window::WindowCreateRequest request;
    request.width = width;
    request.height = height;
    request.highDpi = false;
    request.visible = false;
    auto window = core::window::createWindow(request);
    if (!window) {
        glfwTerminate();
        return 2;
    }
    auto backend = core::render::createRenderBackend(window);
    int status = 0;
    try {
        if (!backend || !backend->initialize())
            throw std::runtime_error("OpenGL initialization failed");
        const auto font = std::filesystem::path(argv[1]).u8string();
        core::TextPrimitive::setDefaultFontFiles(font, font);
        const auto output = std::filesystem::path(argv[2]);
        std::filesystem::create_directories(output);
        core::render::ScopedRenderBackend scope(*backend);
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            check(window, *backend, scale, output);
            checkFractionalText(window, *backend, scale, output);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    backend.reset();
    core::window::destroyWindow(window);
    glfwTerminate();
    return status;
}
