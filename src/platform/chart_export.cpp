#include "chart_export.hpp"
#include "embedded_resources_win.hpp"
// clang-format off: GDI+ 的头文件依赖 Windows 和 COM 类型定义。
#include <objidl.h>
#include <gdiplus.h>
// clang-format on
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>

namespace platform {
namespace {
using Scene = msa::charts::Scene;
using Primitive = msa::charts::Primitive;
constexpr std::int64_t maxPixels = 32 * 1024 * 1024;

void validate_path(const std::filesystem::path &path) {
    if (path.filename().empty() || path.filename() == L"." || path.filename() == L"..")
        throw std::invalid_argument("请指定图表导出文件名");
    if (path.has_root_name() && !path.has_root_directory())
        throw std::invalid_argument("导出路径不能使用 C:folder 形式，请填写完整路径");
    const auto raw = path.native();
    if (raw.find(L'\0') != std::wstring::npos || raw.rfind(L"\\\\?\\", 0) == 0 ||
        raw.rfind(L"\\\\.\\", 0) == 0)
        throw std::invalid_argument("图表导出路径包含无效字符或设备路径");
    for (const auto &component : path.relative_path()) {
        auto name = component.native();
        if (name == L"." || name == L"..")
            continue;
        if (name.empty() || name.back() == L'.' || name.back() == L' ' || name.size() > 255 ||
            name.find_first_of(L"<>:\"|?*") != std::wstring::npos ||
            std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
            throw std::invalid_argument("图表导出路径包含无效文件名");
        name = name.substr(0, name.find(L'.'));
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) {
            return c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c;
        });
        const bool numberedDevice = name.size() == 4 &&
                                    (name.substr(0, 3) == L"COM" || name.substr(0, 3) == L"LPT") &&
                                    ((name[3] >= L'1' && name[3] <= L'9') || name[3] == L'¹' ||
                                     name[3] == L'²' || name[3] == L'³');
        if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || numberedDevice)
            throw std::invalid_argument("图表导出文件名不能使用系统保留设备名");
    }
}

void require(Gdiplus::Status status, const char *operation) {
    if (status != Gdiplus::Ok)
        throw std::runtime_error(std::string(operation) + "（图形状态 " +
                                 std::to_string(static_cast<int>(status)) + "）");
}
struct GraphicsSession {
    ULONG_PTR token = 0;
    GraphicsSession() {
        Gdiplus::GdiplusStartupInput input;
        require(Gdiplus::GdiplusStartup(&token, &input, nullptr), "无法初始化图表导出");
    }
    ~GraphicsSession() {
        if (token)
            Gdiplus::GdiplusShutdown(token);
    }
};

Gdiplus::Color color(const std::string &value) {
    if (value.size() != 7 || value.front() != '#')
        throw std::invalid_argument("图表颜色必须为 #RRGGBB");
    unsigned rgb = 0;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const auto c = value[i];
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0)
            throw std::invalid_argument("图表颜色包含无效字符");
        rgb = rgb * 16 + static_cast<unsigned>(digit);
    }
    return Gdiplus::Color(255, BYTE((rgb >> 16) & 255), BYTE((rgb >> 8) & 255), BYTE(rgb & 255));
}

std::vector<std::uint32_t> codepoints(const std::string &text) {
    if (text.empty())
        return {};
    if (text.size() > 64 * 1024)
        throw std::invalid_argument("图表文本过长");
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (!n)
        throw std::invalid_argument("图表文本不是有效的 UTF-8");
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), wide.data(), n))
        throw std::invalid_argument("无法转换图表文本");
    std::vector<std::uint32_t> result;
    result.reserve(wide.size());
    for (std::size_t i = 0; i < wide.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(wide[i]);
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (++i == wide.size())
                throw std::invalid_argument("图表文本包含不完整字符");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (static_cast<std::uint16_t>(wide[i]) - 0xdc00);
        }
        if (cp != '\r')
            result.push_back(cp);
    }
    return result;
}

void validate_scene(const Scene &scene, float scale, std::size_t &primitiveCount,
                    std::size_t &textBytes, std::size_t &drawSegments) {
    primitiveCount += scene.items.size();
    if (primitiveCount > 100000)
        throw std::invalid_argument("组合图表内容过多，请减少导出图表数量");
    for (const auto &item : scene.items) {
        for (const float value : {item.x, item.y, item.x2, item.y2})
            if (!std::isfinite(value) || std::abs(value) > 1000000)
                throw std::invalid_argument("图表含有无效或超出范围的绘制坐标");
        if (!std::isfinite(item.font_size) || !std::isfinite(item.stroke_width) ||
            item.font_size <= 0 || item.stroke_width <= 0 || item.font_size * scale > 1024 ||
            item.stroke_width * scale > 1024)
            throw std::invalid_argument("图表字号或线宽超出导出范围");
        color(item.color);
        switch (item.type) {
        case Primitive::Type::Line: {
            const double length = std::hypot(double(item.x2) - item.x, double(item.y2) - item.y);
            drawSegments += item.dashed ? static_cast<std::size_t>(std::ceil(length / 9)) : 1;
            break;
        }
        case Primitive::Type::Rect:
        case Primitive::Type::Text:
            if (item.x2 < 0 || item.y2 < 0)
                throw std::invalid_argument("图表文本框或矩形尺寸不能为负数");
            if (item.type == Primitive::Type::Text) {
                if (item.align != Primitive::Align::Left &&
                    item.align != Primitive::Align::Center && item.align != Primitive::Align::Right)
                    throw std::invalid_argument("图表文本对齐方式无效");
                textBytes += item.text.size();
                if (textBytes > 1024 * 1024)
                    throw std::invalid_argument("组合图表文字过多");
                codepoints(item.text);
                const auto width = std::ceil(double(item.x2) * scale);
                const auto height = std::ceil(double(item.y2) * scale);
                if (width > 16384 || height > 16384 || width * height > maxPixels)
                    throw std::invalid_argument("图表文本框超出导出尺寸限制");
            }
            ++drawSegments;
            break;
        case Primitive::Type::Circle:
            if (item.x2 < 0)
                throw std::invalid_argument("图表数据点半径不能为负数");
            ++drawSegments;
            break;
        default:
            throw std::invalid_argument("图表含有未知绘制类型");
        }
        if (drawSegments > 1000000)
            throw std::invalid_argument("组合图表绘制内容过多，请减少导出数量");
    }
}

// 使用与界面相同的完整内嵌字体，不依赖系统是否安装中文字体。
class FontRasterizer {
  public:
    FontRasterizer() : bytes_(embeddedFont()) {
        if (FT_Init_FreeType(&library_))
            throw std::runtime_error("无法初始化导出字体");
        if (FT_New_Memory_Face(library_, bytes_->data(), static_cast<FT_Long>(bytes_->size()), 0,
                               &face_)) {
            FT_Done_FreeType(library_);
            library_ = nullptr;
            throw std::runtime_error("无法载入完整中文导出字体");
        }
    }
    ~FontRasterizer() {
        if (face_)
            FT_Done_Face(face_);
        if (library_)
            FT_Done_FreeType(library_);
    }
    FontRasterizer(const FontRasterizer &) = delete;
    FontRasterizer &operator=(const FontRasterizer &) = delete;

    void draw(Gdiplus::Graphics &graphics, const Primitive &item, float scale) {
        if (item.text.empty() || item.x2 <= 0 || item.y2 <= 0)
            return;
        const double scaledWidth = std::ceil(double(item.x2) * scale);
        const double scaledHeight = std::ceil(double(item.y2) * scale);
        if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) || scaledWidth > 16384 ||
            scaledHeight > 16384 || scaledWidth * scaledHeight > maxPixels)
            throw std::invalid_argument("图表文本框超出导出尺寸限制");
        const int width = std::max(1, static_cast<int>(scaledWidth));
        const int height = std::max(1, static_cast<int>(scaledHeight));
        const double scaledFont = double(item.font_size) * scale;
        if (!std::isfinite(scaledFont) || scaledFont > 1024)
            throw std::invalid_argument("图表导出字号无效");
        const int pixels = std::max(1, static_cast<int>(std::lround(scaledFont)));
        if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixels)))
            throw std::invalid_argument("图表导出字号无效");
        const auto cps = codepoints(item.text);
        struct Glyph {
            FT_UInt index;
            int x;
        };
        struct TextLine {
            std::vector<Glyph> glyphs;
            int width = 0;
        };
        std::vector<TextLine> lines(1);
        FT_UInt previous = 0;
        const int lineHeight = std::max(1, static_cast<int>(std::ceil(pixels * 1.35)));
        const int maxLines = height / lineHeight + 1;
        for (const auto cp : cps) {
            if (cp == '\n') {
                if (static_cast<int>(lines.size()) >= maxLines)
                    break;
                lines.emplace_back();
                previous = 0;
                continue;
            }
            const auto index = FT_Get_Char_Index(face_, cp);
            if (FT_Load_Glyph(face_, index, FT_LOAD_DEFAULT))
                throw std::runtime_error("无法读取导出字形");
            int kerning = 0;
            if (previous && index && FT_HAS_KERNING(face_)) {
                FT_Vector delta{};
                if (!FT_Get_Kerning(face_, previous, index, FT_KERNING_DEFAULT, &delta))
                    kerning = static_cast<int>(delta.x >> 6);
            }
            const int advance = static_cast<int>((face_->glyph->advance.x + 32) >> 6);
            if (!lines.back().glyphs.empty() && lines.back().width + kerning + advance > width) {
                if (static_cast<int>(lines.size()) >= maxLines)
                    break;
                lines.emplace_back();
                kerning = 0;
            }
            auto &line = lines.back();
            line.glyphs.push_back({index, line.width + kerning});
            line.width += kerning + advance;
            previous = index;
        }

        Gdiplus::Bitmap glyphImage(width, height, PixelFormat32bppARGB);
        require(glyphImage.GetLastStatus(), "无法分配图表文本缓冲区");
        Gdiplus::Rect bounds(0, 0, width, height);
        Gdiplus::BitmapData data{};
        require(
            glyphImage.LockBits(&bounds, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data),
            "无法写入图表文本缓冲区");
        try {
            for (int y = 0; y < height; ++y)
                std::memset(static_cast<BYTE *>(data.Scan0) + y * data.Stride, 0,
                            static_cast<std::size_t>(width) * 4);
            const auto ink = color(item.color);
            const int ascender = static_cast<int>((face_->size->metrics.ascender + 32) >> 6);
            for (std::size_t l = 0; l < lines.size(); ++l) {
                const auto &line = lines[l];
                const int offset = item.align == Primitive::Align::Center ? (width - line.width) / 2
                                   : item.align == Primitive::Align::Right ? width - line.width
                                                                           : 0;
                const int baseline = ascender + static_cast<int>(l) * lineHeight;
                for (const auto &glyph : line.glyphs) {
                    if (FT_Load_Glyph(face_, glyph.index, FT_LOAD_DEFAULT) ||
                        FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL))
                        throw std::runtime_error("无法渲染导出字形");
                    const auto &bitmap = face_->glyph->bitmap;
                    for (unsigned y = 0; y < bitmap.rows; ++y) {
                        const int py = baseline - face_->glyph->bitmap_top + static_cast<int>(y);
                        if (py < 0 || py >= height)
                            continue;
                        const auto *source =
                            bitmap.pitch >= 0
                                ? bitmap.buffer + y * bitmap.pitch
                                : bitmap.buffer + (bitmap.rows - 1 - y) * (-bitmap.pitch);
                        auto *row = static_cast<BYTE *>(data.Scan0) + py * data.Stride;
                        for (unsigned x = 0; x < bitmap.width; ++x) {
                            const int px =
                                offset + glyph.x + face_->glyph->bitmap_left + static_cast<int>(x);
                            if (px < 0 || px >= width)
                                continue;
                            const unsigned alpha =
                                bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                                    ? ((source[x >> 3] & (0x80 >> (x & 7))) ? 255u : 0u)
                                    : source[x];
                            if (!alpha)
                                continue;
                            auto *target = row + px * 4;
                            target[0] = ink.GetB();
                            target[1] = ink.GetG();
                            target[2] = ink.GetR();
                            target[3] = static_cast<BYTE>(
                                alpha + (unsigned(target[3]) * (255 - alpha) + 127) / 255);
                        }
                    }
                }
            }
        } catch (...) {
            glyphImage.UnlockBits(&data);
            throw;
        }
        require(glyphImage.UnlockBits(&data), "无法提交图表文本缓冲区");
        // 显式源像素范围避免 DPI 元数据改变字形缩放。
        require(graphics.DrawImage(&glyphImage, Gdiplus::RectF(item.x, item.y, item.x2, item.y2), 0,
                                   0, static_cast<float>(width), static_cast<float>(height),
                                   Gdiplus::UnitPixel),
                "无法绘制图表文本");
    }

  private:
    std::shared_ptr<const FontBytes> bytes_;
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
};

void draw_scene(Gdiplus::Graphics &graphics, FontRasterizer &font, const Scene &scene, float x,
                float y, float scale) {
    const auto saved = graphics.Save();
    try {
        require(graphics.TranslateTransform(x, y), "无法定位图表");
        require(graphics.ScaleTransform(scale, scale), "无法缩放图表");
        require(graphics.SetClip(Gdiplus::RectF(0, 0, scene.width, scene.height)),
                "无法限定图表绘制范围");
        Gdiplus::SolidBrush paper(Gdiplus::Color(255, 255, 255, 255));
        require(graphics.FillRectangle(&paper, 0.f, 0.f, scene.width, scene.height),
                "无法绘制图表背景");
        for (const auto &item : scene.items) {
            if (!std::isfinite(item.x) || !std::isfinite(item.y) || !std::isfinite(item.x2) ||
                !std::isfinite(item.y2) || !std::isfinite(item.font_size) ||
                !std::isfinite(item.stroke_width) || item.font_size <= 0 || item.stroke_width <= 0)
                throw std::invalid_argument("图表含有无效的绘制坐标或样式");
            const auto ink = color(item.color);
            switch (item.type) {
            case Primitive::Type::Line: {
                Gdiplus::Pen pen(ink, item.stroke_width);
                // 与界面的四边形线段保持相同的平端点和逻辑像素虚线节奏。
                pen.SetStartCap(Gdiplus::LineCapFlat);
                pen.SetEndCap(Gdiplus::LineCapFlat);
                const float dx = item.x2 - item.x, dy = item.y2 - item.y;
                const float length = std::hypot(dx, dy);
                if (!std::isfinite(length) || length > 1000000)
                    throw std::invalid_argument("图表线段长度超出导出范围");
                if (length < .001f)
                    break;
                const int pieces = item.dashed ? std::max(1, int(std::ceil(length / 9))) : 1;
                for (int j = 0; j < pieces; ++j) {
                    const float a = float(j) / pieces;
                    const float b = item.dashed ? std::min(1.f, (j + .58f) / pieces) : 1.f;
                    require(graphics.DrawLine(&pen, item.x + dx * a, item.y + dy * a,
                                              item.x + dx * b, item.y + dy * b),
                            "无法绘制图表线条");
                }
                break;
            }
            case Primitive::Type::Rect: {
                if (item.x2 < 0 || item.y2 < 0)
                    throw std::invalid_argument("图表矩形尺寸无效");
                Gdiplus::SolidBrush brush(ink);
                require(graphics.FillRectangle(&brush, item.x, item.y, item.x2, item.y2),
                        "无法绘制图表矩形");
                break;
            }
            case Primitive::Type::Circle: {
                if (item.x2 < 0)
                    throw std::invalid_argument("图表数据点半径无效");
                Gdiplus::SolidBrush brush(ink);
                require(graphics.FillEllipse(&brush, item.x - item.x2, item.y - item.x2,
                                             2 * item.x2, 2 * item.x2),
                        "无法绘制图表数据点");
                break;
            }
            case Primitive::Type::Text:
                font.draw(graphics, item, scale);
                break;
            default:
                throw std::invalid_argument("图表含有未知绘制类型");
            }
        }
    } catch (...) {
        graphics.Restore(saved);
        throw;
    }
    require(graphics.Restore(saved), "无法恢复图表画布");
}

CLSID encoder(const wchar_t *mime) {
    UINT count = 0, size = 0;
    require(Gdiplus::GetImageEncodersSize(&count, &size), "无法读取图像编码器");
    if (!count || !size)
        throw std::runtime_error("系统未提供图像编码器");
    std::vector<BYTE> buffer(size);
    auto *info = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    require(Gdiplus::GetImageEncoders(count, size, info), "无法取得图像编码器");
    for (UINT i = 0; i < count; ++i)
        if (std::wcscmp(info[i].MimeType, mime) == 0)
            return info[i].Clsid;
    throw std::runtime_error("系统不支持所选图像编码格式");
}

std::string encode_image(Gdiplus::Bitmap &bitmap, bool jpeg, int quality) {
    IStream *raw = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &raw)))
        throw std::runtime_error("无法创建图像编码缓冲区");
    const auto release = [](IStream *s) { s->Release(); };
    std::unique_ptr<IStream, decltype(release)> stream(raw, release);
    const auto codec = encoder(jpeg ? L"image/jpeg" : L"image/png");
    ULONG qualityValue = static_cast<ULONG>(quality);
    Gdiplus::EncoderParameters parameters{};
    parameters.Count = 1;
    parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
    parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    parameters.Parameter[0].NumberOfValues = 1;
    parameters.Parameter[0].Value = &qualityValue;
    require(bitmap.Save(stream.get(), &codec, jpeg ? &parameters : nullptr), "图像编码失败");
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 ||
        stat.cbSize.QuadPart > 256 * 1024 * 1024)
        throw std::runtime_error("图像编码长度无效");
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
        throw std::runtime_error("无法读取已编码图像");
    std::string bytes(static_cast<std::size_t>(stat.cbSize.QuadPart), '\0');
    ULONG read = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) ||
        read != bytes.size())
        throw std::runtime_error("已编码图像读取不完整");
    return bytes;
}

std::string pdf_document(const std::string &jpeg, int width, int height, int dpi) {
    std::ostringstream dimensions;
    dimensions.imbue(std::locale::classic());
    dimensions << std::fixed << std::setprecision(3) << (width * 72.0 / dpi) << ' '
               << (height * 72.0 / dpi);
    const auto media = dimensions.str();
    const auto split = media.find(' ');
    const auto content = "q\n" + media.substr(0, split) + " 0 0 " + media.substr(split + 1) +
                         " 0 0 cm\n/ChartImage Do\nQ\n";
    std::string result = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets{0};
    const auto object = [&](const std::string &body) {
        offsets.push_back(result.size());
        result += std::to_string(offsets.size() - 1) + " 0 obj\n" + body + "\nendobj\n";
    };
    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + media +
           "] /Resources << /XObject << /ChartImage 4 0 R >> >> /Contents 5 0 R >>");
    object("<< /Type /XObject /Subtype /Image /Width " + std::to_string(width) + " /Height " +
           std::to_string(height) +
           " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " +
           std::to_string(jpeg.size()) + " >>\nstream\n" + jpeg + "\nendstream");
    object("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
           "endstream");
    object("<< /Creator (MSA Data Generator) /Producer (MSA chart export) >>");
    const auto xref = result.size();
    std::ostringstream trailer;
    trailer.imbue(std::locale::classic());
    trailer << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i)
        trailer << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
    trailer << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R /Info 6 0 R >>\nstartxref\n"
            << xref << "\n%%EOF\n";
    result += trailer.str();
    return result;
}
} // namespace

const char *chart_extension(ChartFormat format) {
    switch (format) {
    case ChartFormat::Png:
        return ".png";
    case ChartFormat::Jpeg:
        return ".jpg";
    case ChartFormat::Pdf:
        return ".pdf";
    }
    throw std::invalid_argument("未知图表导出格式");
}

std::filesystem::path export_charts(const std::vector<Scene> &scenes,
                                    const std::filesystem::path &requestedPath,
                                    const ChartExportOptions &options, msa::ExistingFile policy) {
    if (scenes.empty() || scenes.size() > 16)
        throw std::invalid_argument("请选择 1～16 幅图表导出");
    if (options.columns < 1 || options.columns > 4 || options.panel_width < 640 ||
        options.panel_width > 4096 || options.dpi < 72 || options.dpi > 600 ||
        options.jpeg_quality < 50 || options.jpeg_quality > 100)
        throw std::invalid_argument("导出参数无效：列数 1～4、单图宽度 640～4096、DPI 72～600");
    auto path = requestedPath;
    validate_path(path);
    if (policy != msa::ExistingFile::Preserve && policy != msa::ExistingFile::Replace)
        throw std::invalid_argument("图表同名文件处理方式无效");
    const auto extension = chart_extension(options.format);
    if (path.extension().empty())
        path += extension;
    auto actualExtension = path.extension().u8string();
    std::transform(actualExtension.begin(), actualExtension.end(), actualExtension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actualExtension != extension &&
        !(options.format == ChartFormat::Jpeg && actualExtension == ".jpeg"))
        throw std::invalid_argument("文件扩展名与所选图表格式不一致");
    validate_path(path);
    if (std::filesystem::is_directory(path))
        throw std::invalid_argument("图表导出目标是文件夹，请指定文件名");
    if (policy == msa::ExistingFile::Preserve && std::filesystem::exists(path))
        throw msa::FileExistsError("文件已存在，未覆盖：" + path.u8string());

    const int columns = std::min(options.columns, static_cast<int>(scenes.size()));
    const int rows = (static_cast<int>(scenes.size()) + columns - 1) / columns;
    const int margin = std::max(16, options.panel_width / 40);
    int panelHeight = 0;
    std::size_t primitiveCount = 0, textBytes = 0, drawSegments = 0;
    for (const auto &scene : scenes) {
        if (!std::isfinite(scene.width) || !std::isfinite(scene.height) || scene.width < 1 ||
            scene.height < 1 || scene.width > 16384 || scene.height > 16384 ||
            scene.items.size() > 100000)
            throw std::invalid_argument("图表场景尺寸或内容数量无效");
        const double scaledHeight = double(scene.height) / scene.width * options.panel_width;
        if (!std::isfinite(scaledHeight) || scaledHeight > 16384)
            throw std::invalid_argument("图表长宽比超出导出范围");
        panelHeight = std::max(panelHeight, static_cast<int>(std::ceil(scaledHeight)));
        validate_scene(scene, options.panel_width / scene.width, primitiveCount, textBytes,
                       drawSegments);
    }
    const int width = columns * options.panel_width + (columns + 1) * margin;
    const int height = rows * panelHeight + (rows + 1) * margin;
    if (width > 16384 || height > 16384 || std::int64_t(width) * height > maxPixels)
        throw std::invalid_argument("组合图片过大，请降低单图宽度、增加列数或减少图表数量");
    GraphicsSession session;
    FontRasterizer font;
    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
    require(bitmap.GetLastStatus(), "无法分配图表画布，请降低导出分辨率");
    require(bitmap.SetResolution(static_cast<float>(options.dpi), static_cast<float>(options.dpi)),
            "无法设置图表分辨率");
    {
        Gdiplus::Graphics graphics(&bitmap);
        require(graphics.GetLastStatus(), "无法创建图表绘图上下文");
        require(graphics.Clear(Gdiplus::Color(255, 255, 255, 255)), "无法初始化图表背景");
        require(graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias), "无法设置抗锯齿");
        require(graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic),
                "无法设置图像采样");
        require(graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf), "无法设置像素对齐");
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            const float x = float(margin + int(i % columns) * (options.panel_width + margin));
            const float y = float(margin + int(i / columns) * (panelHeight + margin));
            draw_scene(graphics, font, scenes[i], x, y, options.panel_width / scenes[i].width);
        }
    }
    auto bytes = encode_image(bitmap, options.format != ChartFormat::Png, options.jpeg_quality);
    if (options.format == ChartFormat::Pdf)
        bytes = pdf_document(bytes, width, height, options.dpi);
    // atomic_text 按原始字节写入，名称与扩展名不会改变二进制安全写入语义。
    msa::atomic_text(path, bytes, policy);
    return path;
}
} // namespace platform
