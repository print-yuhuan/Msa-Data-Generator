#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::size_t maximumFontBytes = 64u * 1024u * 1024u;
constexpr std::array<char, 8> magic{'M', 'S', 'A', 'F', 'N', 'T', 'Z', '1'};

struct Block {
    std::uint32_t originalSize;
    Bytes compressed;
};

void writeUnsigned(std::ostream &output, std::uint32_t value) {
    // 容器中的整数统一按小端序保存，与构建主机的字节序无关。
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffu), static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu), static_cast<char>((value >> 24u) & 0xffu)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

Bytes readFont(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("无法读取原始字体文件");
    const std::streamoff length = input.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(maximumFontBytes))
        throw std::runtime_error("字体文件大小必须大于零且不超过 64 MiB");
    Bytes data(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!input)
        throw std::runtime_error("字体文件读取不完整");
    return data;
}

void pack(const std::filesystem::path &inputPath, const std::filesystem::path &outputPath) {
    std::error_code error;
    if (std::filesystem::equivalent(inputPath, outputPath, error))
        throw std::runtime_error("压缩输出不能覆盖原始字体文件");
    const auto font = readFont(inputPath);
    const auto count = std::min<std::size_t>(4, font.size());
    std::vector<Block> blocks;
    blocks.reserve(count);
    std::size_t offset = 0;
    std::size_t packedSize = magic.size() + 12 + count * 8;
    for (std::size_t index = 0; index < count; ++index) {
        // 各块尽量等长；保留所有原始字节，不裁剪任何字形或字体表。
        const auto size = font.size() / count + (index < font.size() % count ? 1 : 0);
        auto compressedSize = compressBound(static_cast<uLong>(size));
        Bytes compressed(static_cast<std::size_t>(compressedSize));
        if (compress2(compressed.data(), &compressedSize, font.data() + offset,
                      static_cast<uLong>(size), Z_BEST_COMPRESSION) != Z_OK)
            throw std::runtime_error("字体分块压缩失败");
        if (compressedSize > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("压缩字体块超过容器长度限制");
        compressed.resize(static_cast<std::size_t>(compressedSize));
        packedSize += compressed.size();
        blocks.push_back({static_cast<std::uint32_t>(size), std::move(compressed)});
        offset += size;
    }
    const auto checksum = crc32(crc32(0L, Z_NULL, 0), font.data(), static_cast<uInt>(font.size()));
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("无法创建压缩字体资源文件");
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    writeUnsigned(output, static_cast<std::uint32_t>(font.size()));
    writeUnsigned(output, static_cast<std::uint32_t>(blocks.size()));
    writeUnsigned(output, static_cast<std::uint32_t>(checksum));
    for (const auto &block : blocks) {
        writeUnsigned(output, block.originalSize);
        writeUnsigned(output, static_cast<std::uint32_t>(block.compressed.size()));
    }
    for (const auto &block : blocks)
        output.write(reinterpret_cast<const char *>(block.compressed.data()),
                     static_cast<std::streamsize>(block.compressed.size()));
    output.flush();
    if (!output)
        throw std::runtime_error("压缩字体资源写入失败");
    std::cout << "完整字体无损压缩：" << font.size() << " 字节 → " << packedSize << " 字节，"
              << blocks.size() << " 个独立块\n";
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) {
        std::cerr << "用法：msa_pack_font.exe 输入字体路径 输出资源路径\n";
        return 2;
    }
    try {
        pack(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "字体资源生成失败：" << error.what() << '\n';
        return 1;
    }
}
