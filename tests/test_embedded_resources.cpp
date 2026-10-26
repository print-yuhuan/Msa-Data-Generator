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
#include "platform/embedded_resources_win.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int width = 640, height = 256;
constexpr const char *memoryFont = "memory:msa-noto-cjk";
const std::array<std::string, 4> labels{"测量数据生成器 / 指标复核", "人员 3 × 重复 3 × 产品 10",
                                        "EV/TV% 6.00  GRR/TV% 10.00", "龘𠮷🔬 · 随机运行顺序"};
const std::array<float, 4> sizes{14, 18, 24, 28};
using Bytes = std::vector<unsigned char>;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

Bytes readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "could not open reference font");
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}

void rejectArchive(const void *data, std::size_t size) {
    bool rejected = false;
    try {
        (void)platform::decodeEmbeddedFont(data, size);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "damaged compressed font archive was accepted");
}

void archiveChecks(const platform::EmbeddedBytes &resource) {
    const auto *begin = static_cast<const unsigned char *>(resource.data);
    require(begin != nullptr && resource.size > 28, "compressed font resource is missing");
    const Bytes original(begin, begin + resource.size);
    rejectArchive(nullptr, original.size());
    for (std::size_t size : {std::size_t(0), std::size_t(7), std::size_t(19), original.size() - 1})
        rejectArchive(original.data(), size);
    auto changed = original;
    changed.front() ^= 1;
    rejectArchive(changed.data(), changed.size());
    changed = original;
    std::fill(changed.begin() + 8, changed.begin() + 12, static_cast<unsigned char>(255));
    rejectArchive(changed.data(), changed.size());
    changed = original;
    std::fill(changed.begin() + 12, changed.begin() + 16, static_cast<unsigned char>(0));
    rejectArchive(changed.data(), changed.size());
    changed = original;
    std::fill(changed.begin() + 12, changed.begin() + 16, static_cast<unsigned char>(255));
    rejectArchive(changed.data(), changed.size());
    changed = original;
    changed[16] ^= 1;
    rejectArchive(changed.data(), changed.size());
    changed = original;
    std::fill(changed.begin() + 20, changed.begin() + 24, static_cast<unsigned char>(0));
    rejectArchive(changed.data(), changed.size());
    changed = original;
    std::fill(changed.begin() + 24, changed.begin() + 28, static_cast<unsigned char>(255));
    rejectArchive(changed.data(), changed.size());
    changed = original;
    changed.back() ^= 255;
    rejectArchive(changed.data(), changed.size());
    changed = original;
    changed.push_back(0);
    rejectArchive(changed.data(), changed.size());
}

struct WorkingDirectory {
    std::filesystem::path original = std::filesystem::current_path();
    explicit WorkingDirectory(const std::filesystem::path &output) {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto empty = output / ("empty-working-directory-" + suffix);
        require(std::filesystem::create_directory(empty),
                "could not create isolated working directory");
        std::filesystem::current_path(empty);
        require(std::filesystem::is_empty(empty), "isolated working directory is not empty");
    }
    ~WorkingDirectory() {
        std::error_code error;
        std::filesystem::current_path(original, error);
    }
};

void compareMetrics(const std::string &referenceFont, const std::string &family = memoryFont) {
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto reference =
            core::TextPrimitive::measureTextMetrics(labels[i], referenceFont, sizes[i]);
        const auto memory = core::TextPrimitive::measureTextMetrics(labels[i], family, sizes[i]);
        require(reference.width > 0 && !reference.caretX.empty(),
                "reference font metrics are empty");
        if (reference.width != memory.width || reference.byteIndices != memory.byteIndices ||
            reference.caretX != memory.caretX)
            throw std::runtime_error("embedded font metrics differ at size " +
                                     std::to_string(sizes[i]) +
                                     ": file=" + std::to_string(reference.width) +
                                     ", memory=" + std::to_string(memory.width));
    }
}

Bytes render(core::window::Handle window, core::render::RenderBackend &backend,
             const std::string &family) {
    core::TextPrimitive::setDefaultFontFiles(family, family);
    core::dsl::Runtime runtime;
    require(runtime.initialize(window), "could not initialize text rendering runtime");
    runtime.compose("embedded-font-test", width, height,
                    [&](core::dsl::Ui &ui, const core::dsl::Screen &) {
                        for (std::size_t i = 0; i < labels.size(); ++i)
                            ui.text("label" + std::to_string(i))
                                .position(16, 10.f + static_cast<float>(i) * 54)
                                .size(width - 32, 48)
                                .text(labels[i])
                                .fontFamily(family)
                                .fontSize(sizes[i])
                                .color("#20334B")
                                .build();
                    });
    for (int frame = 0; frame < 4; ++frame) {
        runtime.update(window, 0, 1, 1, false);
        runtime.requestFullPaint();
        backend.beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1});
        runtime.render(width, height, 1, core::Color("#FFFFFF"));
    }
    Bytes pixels(width * height * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    require(glGetError() == GL_NO_ERROR, "could not read embedded font framebuffer");
    runtime.shutdown();
    require(std::count_if(pixels.begin(), pixels.end(),
                          [](unsigned char value) { return value < 96; }) > 300,
            "Chinese text rendering produced an empty image");
    return pixels;
}

void save(const std::filesystem::path &path, const Bytes &pixels) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int row = height - 1; row >= 0; --row)
        output.write(reinterpret_cast<const char *>(pixels.data() + row * width * 3), width * 3);
    require(bool(output), "could not save embedded font rendering evidence");
}

void comparePixels(const Bytes &reference, const Bytes &actual, const char *stage) {
    if (reference == actual)
        return;
    std::size_t changed = 0;
    int maximum = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const int delta = std::abs(int(reference[i]) - int(actual[i]));
        maximum = std::max(maximum, delta);
        if (delta)
            ++changed;
    }
    throw std::runtime_error(std::string(stage) + ": changed channels=" + std::to_string(changed) +
                             ", maximum delta=" + std::to_string(maximum));
}

void registrationChecks(const Bytes &resource) {
    const unsigned char invalid[]{0, 1, 2, 3, 4, 5, 6, 7};
    require(core::TextPrimitive::measureTextWidth(labels[0], "memory:unregistered-font", 18) == 0,
            "an unregistered memory font silently fell back to a system font");
    require(!core::TextPrimitive::registerMemoryFont(memoryFont, nullptr, resource.size()),
            "null font bytes were accepted");
    require(!core::TextPrimitive::registerMemoryFont(memoryFont, resource.data(), 0),
            "empty font bytes were accepted");
    require(!core::TextPrimitive::registerMemoryFont(memoryFont, invalid, sizeof(invalid)),
            "invalid font bytes were accepted");
    require(!core::TextPrimitive::registerMemoryFont("", resource.data(), resource.size()),
            "empty memory font alias was accepted");
    require(!core::TextPrimitive::registerMemoryFont("file.otf", resource.data(), resource.size()),
            "file path was accepted as a memory font alias");
    {
        // 复制接口必须独立持有字节，调用方随后改写并释放原输入。
        Bytes callerOwned = resource;
        require(core::TextPrimitive::registerMemoryFont(memoryFont, callerOwned.data(),
                                                        callerOwned.size()),
                "could not register the embedded Chinese font");
        std::fill(callerOwned.begin(), callerOwned.end(), static_cast<unsigned char>(0));
    }
}

void sharedRegistrationChecks(const Bytes &resource, const std::string &referenceFont) {
    constexpr const char *family = "memory:msa-noto-shared-test";
    require(!core::TextPrimitive::registerMemoryFontShared(family, {}),
            "null shared font storage was accepted");
    require(!core::TextPrimitive::registerMemoryFontShared(family, std::make_shared<const Bytes>()),
            "empty shared font storage was accepted");
    auto owner = std::make_shared<const Bytes>(resource);
    const std::weak_ptr<const Bytes> lifetime = owner;
    require(core::TextPrimitive::registerMemoryFontShared(family, owner),
            "shared font registration failed");
    require(owner.use_count() > 1, "shared registration did not retain the caller's owner");
    owner.reset();
    require(!lifetime.expired(), "font bytes were released while the registry still needs them");
    compareMetrics(referenceFont, family);
    const auto invalid = std::make_shared<const Bytes>(Bytes{0, 1, 2, 3});
    require(!core::TextPrimitive::registerMemoryFontShared(family, invalid),
            "invalid shared font replacement was accepted");
    compareMetrics(referenceFont, family);
}

void metricCacheChecks(const std::string &referenceFont) {
    // 超过缓存容量后再读取最初文本，确认逐出不会影响宽度、光标位置和字节映射。
    for (int i = 0; i < 300; ++i) {
        const auto text = "缓存边界测量 " + std::to_string(i) + " 仪器与产品";
        const auto expected = core::TextPrimitive::measureTextMetrics(text, referenceFont, 18);
        const auto actual = core::TextPrimitive::measureTextMetrics(text, memoryFont, 18);
        require(expected.width == actual.width && expected.caretX == actual.caretX &&
                    expected.byteIndices == actual.byteIndices,
                "font metrics cache changed text layout or caret mapping");
    }
    compareMetrics(referenceFont);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    core::window::Handle window{};
    std::unique_ptr<core::render::RenderBackend> backend;
    bool glfwStarted = false;
    int status = 0;
    try {
        const auto referencePath = std::filesystem::absolute(argv[1]);
        const auto output = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(output);
        WorkingDirectory workingDirectory(output);
        const auto compressed = platform::embeddedFontResource();
        const auto owner = platform::embeddedFont();
        const auto &resource = *owner;
        require(resource == readFile(referencePath),
                "embedded font bytes differ from the locked source");
        require(compressed.size < resource.size(), "font archive did not reduce resource size");
        require(platform::embeddedFont().get() == owner.get(),
                "embedded font was decompressed more than once");
        archiveChecks(compressed);
        registrationChecks(resource);
        const auto referenceFont = referencePath.u8string();
        compareMetrics(referenceFont);
        sharedRegistrationChecks(resource, referenceFont);
        metricCacheChecks(referenceFont);

        core::render::initializeRenderBackendLoader();
        glfwStarted = glfwInit() == GLFW_TRUE;
        require(glfwStarted, "GLFW initialization failed");
        core::window::WindowCreateRequest request;
        request.width = width;
        request.height = height;
        request.highDpi = false;
        request.visible = false;
        window = core::window::createWindow(request);
        require(bool(window), "could not create hidden rendering window");
        backend = core::render::createRenderBackend(window);
        require(backend && backend->initialize(), "OpenGL initialization failed");
        {
            core::render::ScopedRenderBackend scope(*backend);
            const auto referencePixels = render(window, *backend, referenceFont);
            const auto memoryPixels = render(window, *backend, memoryFont);
            comparePixels(referencePixels, memoryPixels,
                          "embedded font differs from original font");
            const auto sharedPixels = render(window, *backend, "memory:msa-noto-shared-test");
            comparePixels(referencePixels, sharedPixels,
                          "shared embedded font differs from original font");
            require(core::TextPrimitive::registerMemoryFont(memoryFont, resource.data(),
                                                            resource.size()),
                    "identical font re-registration failed");
            const unsigned char invalid[]{0, 1, 2, 3};
            require(!core::TextPrimitive::registerMemoryFont(memoryFont, invalid, sizeof(invalid)),
                    "invalid replacement font was accepted");
            compareMetrics(referenceFont);
            const auto repeatedPixels = render(window, *backend, memoryFont);
            comparePixels(memoryPixels, repeatedPixels,
                          "re-registering or rejecting invalid bytes changed Chinese text");
            save(output / "reference-font.ppm", referencePixels);
            save(output / "embedded-font.ppm", memoryPixels);
        }
        std::ofstream report(output / "embedded-resources.json");
        report
            << "{\n  \"passed\": true,\n  \"embedded_font_bytes\": " << resource.size()
            << ",\n  \"compressed_resource_bytes\": " << compressed.size
            << ",\n  \"reference_bytes_identical\": true,\n  \"empty_working_directory\": true,\n"
               "  \"invalid_fonts_rejected\": true,\n  \"caller_storage_released\": true,\n"
               "  \"malformed_archives_rejected\": true,\n  \"full_font_preserved\": true,\n"
               "  \"shared_owner_retained\": true,\n  \"decompression_cache_reused\": true,\n"
               "  \"metrics_cache_eviction_valid\": true,\n"
               "  \"chinese_metrics_identical\": true,\n  \"font_sizes_tested\": [14,18,24,28],\n"
               "  \"rendered_pixels_identical\": true,\n  \"repeat_registration_valid\": true\n}\n";
        require(bool(report), "could not save embedded resource verification report");
        std::cout
            << "PASS: lossless compressed font, malformed archive rejection, shared lifetime, "
               "four Chinese sizes and framebuffer pixels match the original font\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    backend.reset();
    if (window)
        core::window::destroyWindow(window);
    if (glfwStarted)
        glfwTerminate();
    return status;
}
