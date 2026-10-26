#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/render/text.h"
#include "core/window/window_backend.h"
#ifndef MSA_FONT_BASELINE
#include "platform/embedded_resources_win.hpp"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
constexpr int samples = 7, iterations = 10000;
constexpr int width = 640, height = 256;
const std::string label = "测量数据生成器：人员、重复轮次、产品、标准运行顺序 / GRR 10.00%";
const std::array<std::string, 4> labels{"测量数据生成器 / 指标复核", "人员 3 × 重复 3 × 产品 10",
                                        "EV/TV% 6.00  GRR/TV% 10.00", "龘𠮷🔬 · 随机运行顺序"};
const std::array<float, 4> sizes{14, 18, 24, 28};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    require(!values.empty(), "性能样本为空");
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::shared_ptr<const Bytes> readFont(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "无法读取字体基准文件");
    return std::make_shared<const Bytes>(std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>());
}

void compose(core::dsl::Runtime &runtime) {
    runtime.compose("font-performance", width, height,
                    [&](core::dsl::Ui &ui, const core::dsl::Screen &) {
                        for (std::size_t i = 0; i < labels.size(); ++i)
                            ui.text("label" + std::to_string(i))
                                .position(16, 10.f + static_cast<float>(i) * 54)
                                .size(width - 32, 48)
                                .text(labels[i])
                                .fontSize(sizes[i])
                                .color("#20334B")
                                .build();
                    });
}

void frame(core::dsl::Runtime &runtime, core::window::Handle window,
           core::render::RenderBackend &backend) {
    compose(runtime);
    runtime.update(window, 0, 1, 1, false);
    runtime.requestFullPaint();
    backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
    runtime.render(width, height, 1, core::Color("#FFFFFF"));
    // 等待 GPU 完成，避免把异步排队时间误当成实际绘制耗时。
    glFinish();
    require(glGetError() == GL_NO_ERROR, "字体基准渲染失败");
}

std::pair<double, double> rendering(core::window::Handle window,
                                    core::render::RenderBackend &backend, const std::string &family,
                                    const std::string &reference) {
    std::vector<double> first, steady;
    for (int sample = 0; sample < samples; ++sample) {
        // 切换默认字体清空度量缓存，使首帧样本具备相同的冷缓存起点。
        core::TextPrimitive::setDefaultFontFiles(reference, reference);
        core::TextPrimitive::setDefaultFontFiles(family, family);
        core::dsl::Runtime runtime;
        const auto start = Clock::now();
        require(runtime.initialize(window), "无法初始化字体基准界面");
        frame(runtime, window, backend);
        first.push_back(elapsed(start));
        for (int warmup = 0; warmup < 5; ++warmup)
            frame(runtime, window, backend);
        for (int pass = 0; pass < 20; ++pass) {
            const auto begin = Clock::now();
            frame(runtime, window, backend);
            steady.push_back(elapsed(begin));
        }
        runtime.shutdown();
    }
    return {median(first), median(steady)};
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    core::window::Handle window{};
    std::unique_ptr<core::render::RenderBackend> backend;
    bool glfwStarted = false;
    try {
        const auto reference = std::filesystem::absolute(argv[1]);
        const auto output = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(output.parent_path());
        const auto bytes = readFont(reference);
        require(!bytes->empty(), "字体基准文件为空");
        // 两种实现都先初始化 FreeType，避免首次库初始化影响注册对比。
        require(core::TextPrimitive::registerMemoryFont("memory:perf-warmup", bytes->data(),
                                                        bytes->size()),
                "无法初始化字体基准");
        std::vector<double> copyRegistration, firstMetrics, repeatMetrics, repeatSize;
#ifndef MSA_FONT_BASELINE
        std::vector<double> sharedRegistration, decompression;
#endif
        std::string family;
        for (int sample = 0; sample < samples; ++sample) {
            family = "memory:perf-copy-" + std::to_string(sample);
            auto begin = Clock::now();
            require(core::TextPrimitive::registerMemoryFont(family, bytes->data(), bytes->size()),
                    "复制字体注册失败");
            copyRegistration.push_back(elapsed(begin));
            begin = Clock::now();
            const auto metrics = core::TextPrimitive::measureTextMetrics(label, family, 18);
            firstMetrics.push_back(elapsed(begin));
            require(metrics.width > 0 && metrics.caretX.size() > 1, "首次字体度量为空");
#ifndef MSA_FONT_BASELINE
            const auto sharedFamily = "memory:perf-shared-" + std::to_string(sample);
            begin = Clock::now();
            require(core::TextPrimitive::registerMemoryFontShared(sharedFamily, bytes),
                    "共享字体注册失败");
            sharedRegistration.push_back(elapsed(begin));
            require(bytes.use_count() > 1, "共享字体注册没有保留原始所有权");
#endif
        }
        core::TextStyle style;
        style.text = label;
        style.fontFamily = family;
        style.fontSize = 18;
        const auto expected = core::TextPrimitive::measureTextMetrics(label, family, 18);
        const auto expectedSize = core::TextPrimitive::measureTextSize(style);
        require(expected.width > 0 && expectedSize.x > 0, "重复度量的基准为空");
        double consumed = 0;
        for (int sample = 0; sample < samples; ++sample) {
            auto begin = Clock::now();
            for (int pass = 0; pass < iterations; ++pass)
                consumed += core::TextPrimitive::measureTextMetrics(label, family, 18).width;
            repeatMetrics.push_back(elapsed(begin) * 1000 / iterations);
            begin = Clock::now();
            for (int pass = 0; pass < iterations; ++pass)
                consumed += core::TextPrimitive::measureTextSize(style).x;
            repeatSize.push_back(elapsed(begin) * 1000 / iterations);
        }
        require(consumed > 0, "度量基准结果无效");
#ifndef MSA_FONT_BASELINE
        const auto resource = platform::embeddedFontResource();
        for (int sample = 0; sample < samples; ++sample) {
            const auto begin = Clock::now();
            const auto decoded = platform::decodeEmbeddedFont(resource.data, resource.size);
            decompression.push_back(elapsed(begin));
            require(*decoded == *bytes, "性能测量中解压字体字节发生变化");
        }
        const auto loadBegin = Clock::now();
        const auto loaded = platform::embeddedFont();
        const auto firstLoadMs = elapsed(loadBegin);
        const auto repeatedLoadBegin = Clock::now();
        for (int pass = 0; pass < iterations; ++pass)
            require(platform::embeddedFont().get() == loaded.get(),
                    "内嵌字体缓存没有复用同一份资源");
        const auto cachedLoadUs = elapsed(repeatedLoadBegin) * 1000 / iterations;
#endif
        core::render::initializeRenderBackendLoader();
        glfwStarted = glfwInit() == GLFW_TRUE;
        require(glfwStarted, "GLFW 初始化失败");
        core::window::WindowCreateRequest request;
        request.width = width;
        request.height = height;
        request.highDpi = false;
        request.visible = false;
        window = core::window::createWindow(request);
        require(bool(window), "无法创建性能基准窗口");
        backend = core::render::createRenderBackend(window);
        require(backend && backend->initialize(), "OpenGL 初始化失败");
        std::pair<double, double> renderTimes;
        {
            core::render::ScopedRenderBackend scope(*backend);
            renderTimes = rendering(window, *backend, family, reference.u8string());
        }
        std::ofstream report(output);
        report << std::fixed << std::setprecision(9)
               << "{\n  \"passed\": true,\n  \"samples\": " << samples
               << ",\n  \"iterations_per_sample\": " << iterations
               << ",\n  \"font_bytes\": " << bytes->size()
#ifdef MSA_FONT_BASELINE
               << ",\n  \"baseline\": true"
#else
               << ",\n  \"baseline\": false"
#endif
               << ",\n  \"copy_registration_ms_median\": " << median(copyRegistration)
               << ",\n  \"first_metrics_ms_median\": " << median(firstMetrics)
               << ",\n  \"repeat_metrics_us_per_call_median\": " << median(repeatMetrics)
               << ",\n  \"repeat_size_us_per_call_median\": " << median(repeatSize)
               << ",\n  \"first_render_frame_ms_median\": " << renderTimes.first
               << ",\n  \"steady_render_frame_ms_median\": " << renderTimes.second
#ifndef MSA_FONT_BASELINE
               << ",\n  \"shared_registration_ms_median\": " << median(sharedRegistration)
               << ",\n  \"decompression_ms_median\": " << median(decompression)
               << ",\n  \"first_embedded_load_ms\": " << firstLoadMs
               << ",\n  \"cached_embedded_load_us_per_call\": " << cachedLoadUs
               << ",\n  \"decoded_bytes_identical\": true,\n  \"cached_owner_identical\": true"
#endif
               << ",\n  \"render_timing_includes_gl_finish\": true,\n"
                  "  \"timing_thresholds_enforced\": false\n}\n";
        require(bool(report), "无法写入字体性能报告");
        std::cout << "字体性能基准完成：复制注册 " << median(copyRegistration) << " ms，首次度量 "
                  << median(firstMetrics) << " ms，重复度量 " << median(repeatMetrics)
                  << " us/次，稳态帧 " << renderTimes.second << " ms\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        backend.reset();
        if (window)
            core::window::destroyWindow(window);
        if (glfwStarted)
            glfwTerminate();
        return 1;
    }
    backend.reset();
    if (window)
        core::window::destroyWindow(window);
    if (glfwStarted)
        glfwTerminate();
    return 0;
}
