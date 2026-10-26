#include "embedded_resources_win.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace platform {
namespace {
constexpr unsigned char magic[]{'M', 'S', 'A', 'F', 'N', 'T', 'Z', '1'};
constexpr std::size_t maxFontBytes = 64 * 1024 * 1024;
constexpr std::size_t maxBlocks = 32;

std::uint32_t read32(const unsigned char *data) {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
           (std::uint32_t(data[3]) << 24);
}

struct Block {
    const unsigned char *source;
    std::size_t sourceSize;
    std::size_t destinationOffset;
    std::size_t destinationSize;
};
} // namespace

std::shared_ptr<const FontBytes> decodeEmbeddedFont(const void *input, std::size_t size) {
    const auto *data = static_cast<const unsigned char *>(input);
    if (!data || size < 20 || std::memcmp(data, magic, sizeof(magic)) != 0)
        throw std::runtime_error("内嵌字体压缩格式无效");
    const auto total = static_cast<std::size_t>(read32(data + 8));
    const auto count = static_cast<std::size_t>(read32(data + 12));
    const auto expectedCrc = read32(data + 16);
    if (!total || total > maxFontBytes || !count || count > maxBlocks || size < 20 + count * 8)
        throw std::runtime_error("内嵌字体压缩长度无效");

    std::vector<Block> blocks;
    blocks.reserve(count);
    std::size_t sourceOffset = 20 + count * 8;
    std::size_t destinationOffset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto raw = static_cast<std::size_t>(read32(data + 20 + i * 8));
        const auto compressed = static_cast<std::size_t>(read32(data + 24 + i * 8));
        if (!raw || !compressed || raw > total - destinationOffset ||
            compressed > size - sourceOffset)
            throw std::runtime_error("内嵌字体分块边界无效");
        blocks.push_back({data + sourceOffset, compressed, destinationOffset, raw});
        sourceOffset += compressed;
        destinationOffset += raw;
    }
    if (sourceOffset != size || destinationOffset != total)
        throw std::runtime_error("内嵌字体分块长度不一致");

    auto decoded = std::make_shared<FontBytes>(total);
    std::vector<uLong> blockCrc(count);
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    const auto work = [&] {
        for (;;) {
            const auto i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= blocks.size())
                return;
            const auto &block = blocks[i];
            auto destinationSize = static_cast<uLongf>(block.destinationSize);
            auto sourceSize = static_cast<uLong>(block.sourceSize);
            const auto status = uncompress2(decoded->data() + block.destinationOffset,
                                            &destinationSize, block.source, &sourceSize);
            if (status != Z_OK || destinationSize != block.destinationSize ||
                sourceSize != block.sourceSize) {
                failed.store(true, std::memory_order_relaxed);
            } else {
                blockCrc[i] = crc32(0, decoded->data() + block.destinationOffset,
                                    static_cast<uInt>(block.destinationSize));
            }
        }
    };

    // 各块直接写入最终缓冲区的独立范围，避免中间拼接和第二份完整字体复制。
    const auto hardware = std::max(1u, std::thread::hardware_concurrency());
    const auto workers = std::min({count, std::size_t(4), std::size_t(hardware)});
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    try {
        for (std::size_t i = 1; i < workers; ++i)
            threads.emplace_back(work);
    } catch (...) {
        // 线程资源紧张时由已启动线程和当前线程完成，保持启动可用。
    }
    work();
    for (auto &thread : threads)
        thread.join();
    if (failed.load(std::memory_order_relaxed))
        throw std::runtime_error("内嵌字体解压失败或数据已损坏");
    // 合并各工作线程算出的校验值，避免再次串行扫描完整字体。
    auto actualCrc = crc32(0, Z_NULL, 0);
    for (std::size_t i = 0; i < count; ++i)
        actualCrc =
            crc32_combine(actualCrc, blockCrc[i], static_cast<z_off_t>(blocks[i].destinationSize));
    if (actualCrc != expectedCrc)
        throw std::runtime_error("内嵌字体完整性校验失败");
    return decoded;
}
} // namespace platform
