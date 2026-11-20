#include "msa/charts.hpp"
#include "msa/brand.hpp"
#include "platform/chart_export.hpp"
#include "platform/save_target.hpp"
#include "platform/embedded_resources_win.hpp"
// clang-format off: GDI+ 需要先声明 Windows 与 COM 类型。
#include <objidl.h>
#include <gdiplus.h>
// clang-format on
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>

namespace {
using msa::Json;
using msa::charts::Editor;
using msa::charts::Kind;
using msa::charts::Primitive;
using msa::charts::Scene;
using msa::charts::Style;
Json checks = Json::array(), files = Json::array();

void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Work> void check(const std::string &name, Work work) {
    try {
        work();
        checks.push_back({{"name", name}, {"passed", true}});
    } catch (const std::exception &e) {
        checks.push_back({{"name", name}, {"passed", false}, {"error", e.what()}});
        std::cerr << name << "：" << e.what() << '\n';
    }
}
template <class Work> void rejects(Work work) {
    bool rejected = false;
    try {
        work();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "异常输入没有被拒绝");
}
std::string bytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "无法读取测试输出");
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
msa::Result study(int operators = 3, int repeats = 3, int parts = 10,
                  const std::string &method = "crossed_xbarr", double center = 10,
                  double step = .04) {
    msa::Result result;
    auto config = msa::default_config();
    config["design"] = {{"operators", operators}, {"replicates", repeats}, {"parts", parts}};
    config["analysis"]["method"] = method;
    config["specification"]["lsl"] = center - step * 10;
    config["specification"]["usl"] = center + step * 10;
    result.config = msa::normalize_config(config);
    for (int op = 0; op < operators; ++op)
        for (int trial = 0; trial < repeats; ++trial) {
            std::vector<double> row;
            for (int part = 0; part < parts; ++part)
                row.push_back(center +
                              step * ((part - (parts - 1) / 2.) + op * .13 + (trial - 1.) * .07 +
                                      ((part + 2 * trial + op) % 5 - 2) * .025));
            result.matrix.push_back(std::move(row));
        }
    result.metrics = msa::calculate(result.matrix, result.config);
    return result;
}
Json snapshot(const Scene &scene) {
    Json items = Json::array();
    for (const auto &p : scene.items)
        items.push_back({int(p.type), p.x, p.y, p.x2, p.y2, p.color, p.stroke_width, p.font_size,
                         p.text, int(p.align), p.dashed});
    return {{"width", scene.width}, {"height", scene.height}, {"items", items}};
}
void validate_scene(const Scene &scene) {
    require(std::isfinite(scene.width) && std::isfinite(scene.height) && scene.width >= 360 &&
                scene.height >= 260 && !scene.items.empty() && scene.items.size() < 100000,
            "场景尺寸或图元数量无效");
    for (const auto &p : scene.items) {
        for (const float value : {p.x, p.y, p.x2, p.y2, p.font_size, p.stroke_width})
            require(std::isfinite(value) && std::abs(value) < 1000000, "存在非法图元坐标或样式");
        require(p.font_size > 0 && p.stroke_width > 0 &&
                    std::regex_match(p.color, std::regex("#[0-9A-Fa-f]{6}")),
                "图元字号、线宽或颜色无效");
        require(p.type == Primitive::Type::Line || p.type == Primitive::Type::Rect ||
                    p.type == Primitive::Type::Circle || p.type == Primitive::Type::Text,
                "未知图元类型");
        if (p.type == Primitive::Type::Rect || p.type == Primitive::Type::Text)
            require(p.x2 >= 0 && p.y2 >= 0, "存在负尺寸文本框或矩形");
        if (p.type == Primitive::Type::Circle)
            require(p.x2 >= 0, "存在负半径数据点");
        if (p.type == Primitive::Type::Text)
            require(p.align == Primitive::Align::Left || p.align == Primitive::Align::Center ||
                        p.align == Primitive::Align::Right,
                    "文字对齐方式无效");
    }
}
void validate_control_limits(const Scene &scene, const msa::Result &result, bool ranges) {
    const int operators = result.config["design"]["operators"];
    const int repeats = result.config["design"]["replicates"];
    const int parts = result.config["design"]["parts"];
    long double total = 0, totalRange = 0;
    for (int op = 0; op < operators; ++op)
        for (int part = 0; part < parts; ++part) {
            double low = std::numeric_limits<double>::max(), high = -low;
            for (int trial = 0; trial < repeats; ++trial) {
                const auto value = result.matrix[op * repeats + trial][part];
                total += value;
                low = std::min(low, value);
                high = std::max(high, value);
            }
            totalRange += high - low;
        }
    const double rbar = static_cast<double>(totalRange / (operators * parts));
    const double grand = static_cast<double>(total / (operators * repeats * parts));
    const double a2 = repeats == 2 ? 1.880 : 1.023, d4 = repeats == 2 ? 3.267 : 2.574;
    const std::array<double, 3> expected{ranges ? d4 * rbar : grand + a2 * rbar,
                                         ranges ? rbar : grand, ranges ? 0. : grand - a2 * rbar};
    const std::regex pattern(R"(UCL = ([^ ]+) +CL = ([^ ]+) +LCL = ([^ ]+))");
    int found = 0;
    for (const auto &item : scene.items) {
        std::smatch match;
        if (item.type != Primitive::Type::Text || !std::regex_search(item.text, match, pattern))
            continue;
        ++found;
        for (int i = 0; i < 3; ++i) {
            const auto actual = std::stod(match[std::size_t(i + 1)]);
            require(std::abs(actual - expected[i]) <= 1e-9 + std::abs(expected[i]) * 5e-6,
                    "控制限或中心线未使用全体子组共同估计");
        }
    }
    require(found == 1, "控制图缺少唯一完整控制限数值说明");
}
Scene primitive_scene() {
    Scene scene{640, 400, {}};
    Primitive p;
    p.type = Primitive::Type::Rect;
    p.x = 20;
    p.y = 30;
    p.x2 = 90;
    p.y2 = 50;
    p.color = "#FF0000";
    scene.items.push_back(p);
    p.type = Primitive::Type::Circle;
    p.x = 180;
    p.y = 55;
    p.x2 = 20;
    p.y2 = 0;
    p.color = "#00AA00";
    scene.items.push_back(p);
    p.type = Primitive::Type::Line;
    p.x = 250;
    p.y = 55;
    p.x2 = 540;
    p.y2 = 55;
    p.color = "#0000FF";
    p.stroke_width = 3;
    p.dashed = true;
    scene.items.push_back(p);
    p.type = Primitive::Type::Text;
    p.x = 20;
    p.y = 110;
    p.x2 = 600;
    p.y2 = 52;
    p.color = "#20334B";
    p.font_size = 24;
    p.text = "量具 R&R：中文测量系统、均值极差、方差分析";
    scene.items.push_back(p);
    p.y = 180;
    p.x2 = 220;
    p.y2 = 135;
    p.font_size = 18;
    p.text = "长标题换行检查：测量系统分析与交叉量具研究，覆盖中文、数字 0123456789、英文 ABC。";
    scene.items.push_back(p);
    p.x = 280;
    p.y = 180;
    p.x2 = 300;
    p.y2 = 100;
    p.align = Primitive::Align::Right;
    p.text = "右对齐\n上规格限 USL\n下规格限 LSL";
    scene.items.push_back(p);
    return scene;
}
struct GdiSession {
    ULONG_PTR token = 0;
    GdiSession() {
        Gdiplus::GdiplusStartupInput input;
        require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok,
                "无法初始化图片解码测试");
    }
    ~GdiSession() { Gdiplus::GdiplusShutdown(token); }
};
struct ImageStats {
    int width, height;
    double dpi;
    std::size_t ink;
};
ImageStats inspect(Gdiplus::Bitmap &bitmap) {
    require(bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() && bitmap.GetHeight(),
            "图片无法解码");
    ImageStats stats{int(bitmap.GetWidth()), int(bitmap.GetHeight()),
                     bitmap.GetHorizontalResolution(), 0};
    for (int y = 0; y < stats.height; y += 3)
        for (int x = 0; x < stats.width; x += 3) {
            Gdiplus::Color pixel;
            require(bitmap.GetPixel(x, y, &pixel) == Gdiplus::Ok, "图片像素不可读取");
            require(pixel.GetA() == 255, "图表背景存在意外透明像素");
            stats.ink += pixel.GetR() < 240 || pixel.GetG() < 240 || pixel.GetB() < 240;
        }
    require(stats.ink > 20, "图像没有实际绘制内容");
    return stats;
}
ImageStats inspect_file(const std::filesystem::path &path) {
    GdiSession session;
    Gdiplus::Bitmap bitmap(path.c_str());
    return inspect(bitmap);
}
void inspect_pdf(const std::filesystem::path &path, int width, int height, int dpi) {
    const auto pdf = bytes(path);
    require(pdf.rfind("%PDF-1.4", 0) == 0 && pdf.find("/Count 1") != std::string::npos &&
                pdf.find("/Filter /DCTDecode") != std::string::npos &&
                pdf.find("/Width " + std::to_string(width)) != std::string::npos &&
                pdf.find("/Height " + std::to_string(height)) != std::string::npos &&
                pdf.find("%%EOF") != std::string::npos,
            "PDF 文件头、页数或内嵌图像信息错误");
    const auto xrefStart = pdf.rfind("startxref\n");
    require(xrefStart != std::string::npos, "PDF 缺少交叉引用表");
    const auto xref = std::stoull(pdf.substr(xrefStart + 10));
    require(pdf.compare(xref, 5, "xref\n") == 0, "PDF 交叉引用偏移不正确");
    const auto image = pdf.find("4 0 obj\n");
    const auto length = pdf.find("/Length ", image), start = pdf.find("stream\n", image);
    require(image != std::string::npos && length != std::string::npos && start != std::string::npos,
            "PDF 缺少图像流");
    const auto size = std::stoull(pdf.substr(length + 8));
    const auto jpeg = pdf.substr(start + 7, size);
    require(jpeg.size() == size && jpeg.size() > 2 && static_cast<unsigned char>(jpeg[0]) == 0xff &&
                static_cast<unsigned char>(jpeg[1]) == 0xd8,
            "PDF 中的 JPEG 数据长度不正确");
    IStream *raw = nullptr;
    require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &raw)), "无法创建 PDF 图像解码缓冲区");
    const auto release = [](IStream *stream) { stream->Release(); };
    std::unique_ptr<IStream, decltype(release)> stream(raw, release);
    ULONG written = 0;
    require(SUCCEEDED(stream->Write(jpeg.data(), static_cast<ULONG>(jpeg.size()), &written)) &&
                written == jpeg.size(),
            "无法写入 PDF 图像解码缓冲区");
    LARGE_INTEGER zero{};
    require(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)), "无法回读 PDF 图像缓冲区");
    GdiSession session;
    Gdiplus::Bitmap bitmap(stream.get());
    const auto stats = inspect(bitmap);
    require(stats.width == width && stats.height == height, "PDF 图像尺寸错误");
    const auto mediaStart = pdf.find("/MediaBox [0 0 ");
    require(mediaStart != std::string::npos, "PDF 缺少页面尺寸");
    std::istringstream media(pdf.substr(mediaStart + 15));
    double paperWidth = 0, paperHeight = 0;
    media >> paperWidth >> paperHeight;
    require(std::abs(paperWidth - width * 72. / dpi) < .001 &&
                std::abs(paperHeight - height * 72. / dpi) < .001,
            "PDF 页面物理尺寸与 DPI 不一致");
}
void record_file(const std::filesystem::path &path, const platform::ChartExportOptions &options,
                 const std::vector<Scene> &scenes) {
    const int columns = std::min(options.columns, int(scenes.size()));
    const int rows = (int(scenes.size()) + columns - 1) / columns;
    const int margin = std::max(16, options.panel_width / 40);
    int panelHeight = 0;
    for (const auto &scene : scenes)
        panelHeight = std::max(
            panelHeight, int(std::ceil(double(scene.height) / scene.width * options.panel_width)));
    const int width = columns * options.panel_width + (columns + 1) * margin;
    const int height = rows * panelHeight + (rows + 1) * margin;
    require(std::filesystem::file_size(path) > 100, "图表文件为空");
    if (options.format == platform::ChartFormat::Pdf)
        inspect_pdf(path, width, height, options.dpi);
    else if (options.format == platform::ChartFormat::Svg) {
        const auto svg = bytes(path);
        require(svg.find("<svg") != std::string::npos && svg.find("<text") != std::string::npos &&
                    svg.find("<image") == std::string::npos,
                "SVG 缺少可编辑图形或文字");
    } else {
        const auto image = inspect_file(path);
        require(image.width == width && image.height == height, "图片尺寸与组合布局不一致");
        require(std::abs(image.dpi - options.dpi) < 1, "图片 DPI 不一致");
    }
    files.push_back({{"path", std::filesystem::absolute(path).u8string()},
                     {"format", platform::chart_extension(options.format)},
                     {"width", width},
                     {"height", height},
                     {"dpi", options.dpi},
                     {"panels", scenes.size()}});
}

void model_checks() {
    check("另存为按文件类型补齐扩展名并允许在 HTML/PDF 之间切换", [] {
        using platform::completeExportFilename;
        using platform::ExportKind;
        require(completeExportFilename(L"测量", ExportKind::Csv) == L"测量.csv" &&
                    completeExportFilename(L"结果", ExportKind::Png) == L"结果.png" &&
                    completeExportFilename(L"结果.JPEG", ExportKind::Jpeg) == L"结果.JPEG" &&
                    completeExportFilename(L"图表", ExportKind::ChartPdf) == L"图表.pdf",
                "CSV 或图表扩展名无效");
        require(completeExportFilename(L"研究", ExportKind::Xlsx) == L"研究.xlsx" &&
                    completeExportFilename(L"测量.v2.CSV", ExportKind::Xlsx) == L"测量.v2.xlsx" &&
                    completeExportFilename(L"测量.v2.XLSX", ExportKind::Csv) == L"测量.v2.csv",
                "改变测量数据文件类型未保留基本名或引入重复后缀");
        require(completeExportFilename(L"量具报告.htm", ExportKind::ReportPdf) == L"量具报告.pdf" &&
                    completeExportFilename(L"量具报告.PDF", ExportKind::Html) == L"量具报告.html" &&
                    completeExportFilename(L"报告.HTM", ExportKind::Html) == L"报告.HTM",
                "方法对比报告的后缀与所选格式不符");
        rejects([] { completeExportFilename(L"研究.json", ExportKind::Csv); });
        rejects([] { completeExportFilename(L"报告.csv", ExportKind::ReportPdf); });
        rejects([] { completeExportFilename(L"文件夹/", ExportKind::Html); });
    });
    check("大中心值下的规格限和均值控制限保留差异", [] {
        auto result = study(2, 2, 5, "crossed_xbarr", 1e6, .01);
        for (int row = 0; row < 4; ++row)
            for (int part = 0; part < 5; ++part)
                result.matrix[row][part] = 1e6 + (part - 2) * .01 + (row % 2 ? .01 : -.01);
        result.metrics = msa::calculate(result.matrix, result.config);
        for (auto kind : {Kind::MeanChart, Kind::MeasurementByPart, Kind::Histogram}) {
            auto editor = msa::charts::default_editor(kind);
            editor.show_limits = true;
            const auto scene = msa::charts::make_scene(result, kind, editor, 900, 560);
            if (kind == Kind::MeanChart) {
                const std::regex limits(R"(UCL = ([^ ]+) +CL = ([^ ]+) +LCL = ([^ ]+))");
                int found = 0;
                for (const auto &item : scene.items) {
                    std::smatch match;
                    if (std::regex_match(item.text, match, limits)) {
                        ++found;
                        require(std::abs(std::stod(match[1]) - 1000000.0376) < .0001 &&
                                    std::stod(match[2]) == 1e6 &&
                                    std::abs(std::stod(match[3]) - 999999.9624) < .0001,
                                "控制限舍入丢失子组波动");
                    }
                }
                require(found == 1, "缺少控制限说明");
            } else {
                int found = 0;
                for (const auto &item : scene.items)
                    if (item.text == "LSL 999999.9" || item.text == "USL 1000000.1")
                        ++found;
                require(found == 2, "规格参考线丢失精度");
            }
        }
    });
    check("图表类型完整且说明不为空", [] {
        require(msa::charts::all_kinds().size() == 7, "图表类型数量不正确");
        for (const auto kind : msa::charts::all_kinds())
            require(std::string(msa::charts::label(kind)).size() &&
                        std::string(msa::charts::description(kind)).size(),
                    "图表说明为空");
    });
    check("两种预设在七类图使用各自配色，保留相同几何、标签与编辑能力", [] {
        const auto result = study();
        const auto hasColor = [](const Scene &scene, const std::string &color) {
            return std::any_of(scene.items.begin(), scene.items.end(),
                               [&](const Primitive &item) { return item.color == color; });
        };
        for (const auto kind : msa::charts::all_kinds()) {
            auto app = msa::charts::default_editor(kind);
            auto origin = msa::charts::default_editor(kind, Style::Origin);
            require(app.style == Style::Default && origin.style == Style::Origin &&
                        app.primary_color == msa::brand_blue && origin.primary_color == "#2F65A4",
                    "默认风格未采用程序蓝或 Origin 首色不兼容");
            const auto a = msa::charts::make_scene(result, kind, app, 840, 480);
            auto b = msa::charts::make_scene(result, kind, origin, 840, 480);
            require(hasColor(a, app.primary_color) && hasColor(b, origin.primary_color) &&
                        hasColor(a, "#182C45") && hasColor(b, "#263543") &&
                        hasColor(a, "#E3EAF5") && hasColor(b, "#E4E9EE"),
                    "图表系列、文字或网格未采用所选预设");
            if (kind == Kind::Interaction)
                require(hasColor(a, "#087F75") && hasColor(a, "#AD426F") &&
                            hasColor(b, "#C85F22") && hasColor(b, "#10857D"),
                        "交互图完整系列未跟随风格");
            if (kind == Kind::MeanChart || kind == Kind::RangeChart)
                require(hasColor(a, "#176C52") && hasColor(a, "#AC3934") &&
                            hasColor(b, "#18785C") && hasColor(b, "#B13E48"),
                        "中心线与控制限未跟随风格");
            require(a.items.size() == b.items.size(), "切换风格改变了图元数量");
            for (std::size_t i = 0; i < a.items.size(); ++i)
                b.items[i].color = a.items[i].color;
            require(snapshot(a) == snapshot(b), "切换配色改变了几何坐标或统计文字");
            for (auto e : {app, origin}) {
                e.title = "样式独立编辑";
                e.primary_color = "#087F75";
                e.marker_shape = msa::charts::MarkerShape::Diamond;
                const auto palette = msa::charts::series_colors(e);
                require(palette.size() == 4 && palette.front() == e.primary_color &&
                            std::count(palette.begin(), palette.end(), e.primary_color) == 1,
                        "编辑主色后色序与预览不一致或出现同色系列");
                const auto edited = msa::charts::make_scene(result, kind, e, 840, 480);
                require(hasColor(edited, e.primary_color) &&
                            std::any_of(edited.items.begin(), edited.items.end(),
                                        [&](const Primitive &p) { return p.text == e.title; }),
                        "预设未应用自定义颜色或标题");
            }
        }
        rejects([] { msa::charts::default_editor(Kind::Histogram, static_cast<Style>(-1)); });
    });
    check("交互图按系列编码点形，图例可移至轴下并调整纵轴刻度密度", [] {
        const auto result = study();
        const auto originalMatrix = result.matrix;
        const auto metrics = result.metrics, config = result.config;
        auto e = msa::charts::default_editor(Kind::Interaction);
        e.y_min = 9;
        e.y_max = 11;
        e.y_tick_intervals = 2;
        const auto original = msa::charts::make_scene(result, Kind::Interaction, e, 840, 480);
        const auto &primary = e.primary_color;
        auto countPoints = [](const Scene &s, Primitive::Type type, const std::string &color) {
            return std::count_if(s.items.begin(), s.items.end(), [&](const Primitive &p) {
                return p.type == type && p.color == color;
            });
        };
        const auto textY = [](const Scene &s, const std::string &label) {
            const auto found = std::find_if(s.items.begin(), s.items.end(),
                                            [&](const Primitive &p) { return p.text == label; });
            require(found != s.items.end(), "图例或坐标轴说明丢失");
            return found->y;
        };
        const auto ticks = [](const Scene &s) {
            return std::count_if(s.items.begin(), s.items.end(), [](const Primitive &p) {
                return p.align == Primitive::Align::Right && p.x < 10;
            });
        };
        require(countPoints(original, Primitive::Type::Rect, primary) == 11 &&
                    textY(original, "操作员 A") < textY(original, e.y_label),
                "第一系列方形符号或顶部图例错误");
        e.marker_shape = msa::charts::MarkerShape::Circle;
        e.legend_position = msa::charts::LegendPosition::Bottom;
        e.y_tick_intervals = 10;
        const auto edited = msa::charts::make_scene(result, Kind::Interaction, e, 840, 480);
        require(countPoints(edited, Primitive::Type::Circle, primary) == 11 &&
                    countPoints(edited, Primitive::Type::Rect, primary) == 0 &&
                    textY(edited, "操作员 A") >= textY(edited, e.x_label) + 18 &&
                    ticks(edited) > ticks(original),
                "编辑后图例和数据点形不一致，或纵轴密度及图例布局没有生效");
        auto compact = msa::charts::make_scene(result, Kind::Interaction, e, 360, 260);
        require(ticks(compact) < ticks(edited), "最小画布未限制过密的数值标签");
        auto control = msa::charts::default_editor(Kind::MeanChart);
        control.show_legend = false;
        control.marker_shape = msa::charts::MarkerShape::Circle;
        const auto meanChart = msa::charts::make_scene(result, Kind::MeanChart, control, 840, 480);
        const auto highlighted =
            std::count_if(meanChart.items.begin(), meanChart.items.end(), [&](const Primitive &p) {
                return p.type == Primitive::Type::Circle && p.x2 > control.point_radius &&
                       p.color != control.primary_color;
            });
        require(highlighted > 0, "控制图超限点没有沿用用户选择的点形");
        require(result.matrix == originalMatrix && result.metrics == metrics &&
                    result.config == config,
                "修改图形样式改变了研究数据或统计量");
    });
    check("手动分箱的频数守恒、区间相邻且包括样本最大值", [] {
        auto result = study(2, 2, 6);
        int value = 0;
        for (auto &row : result.matrix)
            for (auto &measurement : row)
                measurement = value++;
        result.metrics = msa::calculate(result.matrix, result.config);
        const auto metrics = result.metrics;
        const auto originalMatrix = result.matrix;
        auto e = msa::charts::default_editor(Kind::Histogram);
        e.show_limits = e.show_legend = false;
        e.y_min = 0;
        e.y_max = 10;
        e.y_tick_intervals = 5;
        for (int bins : {3, 8}) {
            e.histogram_bins = bins;
            const auto scene = msa::charts::make_scene(result, Kind::Histogram, e, 840, 480);
            std::vector<Primitive> bars;
            float bottom = 0, top = 0;
            for (const auto &p : scene.items) {
                if (p.type == Primitive::Type::Rect && p.color != "#FFFFFF")
                    bars.push_back(p);
                if (p.align == Primitive::Align::Right && p.text == "0")
                    bottom = p.y;
                if (p.align == Primitive::Align::Right && p.text == "10")
                    top = p.y;
            }
            require(bottom > top && int(bars.size()) == bins, "频数轴或直方图分箱数量错误");
            float previousRight = bars.front().x;
            for (const auto &bar : bars) {
                require(std::abs(bar.x - previousRight) < .01 &&
                            std::abs(bar.y2 * 10 / (bottom - top) - 24. / bins) < .001,
                        "相邻直方柱间距或包含最大值的分箱频数错误");
                previousRight = bar.x + bar.x2;
            }
        }
        require(result.metrics == metrics && result.matrix == originalMatrix,
                "手动分箱改变了数据或统计结果");
    });
    check("交互图和直方图优先显示样本波动，规格线按需扩展坐标范围", [] {
        const auto result = study();
        for (auto kind : {Kind::Interaction, Kind::Histogram}) {
            auto e = msa::charts::default_editor(kind);
            require(!e.show_limits, "默认应聚焦样本范围");
            e.show_legend = false;
            const auto observed = msa::charts::make_scene(result, kind, e, 840, 480);
            e.show_limits = true;
            const auto withLimits = msa::charts::make_scene(result, kind, e, 840, 480);
            const auto extent = [&](const Scene &s) {
                float first = s.width, last = 0;
                for (const auto &p : s.items) {
                    if (p.type != Primitive::Type::Rect || p.color == "#FFFFFF")
                        continue;
                    if (kind == Kind::Interaction && p.color != e.primary_color)
                        continue;
                    const bool xAxis = kind == Kind::Histogram;
                    first = std::min(first, xAxis ? p.x : p.y);
                    last = std::max(last, xAxis ? p.x + p.x2 : p.y + p.y2);
                }
                return last - first;
            };
            require(extent(observed) > 1.5f * extent(withLimits) && extent(withLimits) > 0,
                    "关闭规格线时未将绘图区用于呈现样本波动");
            const auto specCount = [](const Scene &s) {
                return std::count_if(s.items.begin(), s.items.end(), [](const Primitive &p) {
                    return p.text.find("LSL ") == 0 || p.text.find("USL ") == 0;
                });
            };
            require(specCount(observed) == 0 && specCount(withLimits) == 2,
                    "规格线及其数值未与可见性一起更新");
        }
        for (auto kind : {Kind::MeanChart, Kind::RangeChart})
            require(msa::charts::default_editor(kind).show_limits, "控制限应默认可见");
    });
    check("图表按研究顺序标记操作员姓名，兼容旧配置并缩略狭窄标签", [] {
        auto contains = [](const Scene &scene, const std::string &text) {
            return std::any_of(scene.items.begin(), scene.items.end(),
                               [&](const Primitive &item) { return item.text == text; });
        };
        for (int count : {2, 3}) {
            auto result = study(count);
            result.config["design"]["operator_names"] = {"赵<&一", "", "周明"};
            for (auto kind : {Kind::MeasurementByPart, Kind::MeasurementByOperator,
                              Kind::RangeChart, Kind::MeanChart, Kind::Interaction}) {
                const auto scene = msa::charts::make_scene(
                    result, kind, msa::charts::default_editor(kind), 840, 480);
                require(contains(scene, "操作员 赵<&一") && contains(scene, "操作员 B") &&
                            contains(scene, "操作员 周明") == (count == 3),
                        "图例或分面未按启用人数显示有效名称");
                if (kind == Kind::MeasurementByOperator)
                    require(contains(scene, "赵<&一") && contains(scene, "B") &&
                                contains(scene, "周明") == (count == 3),
                            "操作员分布刻度与图例不匹配");
            }
        }
        auto result = study(3);
        const auto kind = Kind::MeanChart;
        const auto editor = msa::charts::default_editor(kind);
        result.config["design"].erase("operator_names");
        const auto legacy = msa::charts::make_scene(result, kind, editor, 360, 260);
        require(contains(legacy, "操作员 A") && contains(legacy, "操作员 B") &&
                    contains(legacy, "操作员 C"),
                "旧研究缺少名称时应仍显示 A/B/C");
        result.config["design"]["operator_names"] = {"质量实验室第一测量工位", "", ""};
        const auto compact = msa::charts::make_scene(result, kind, editor, 360, 260);
        require(std::any_of(compact.items.begin(), compact.items.end(), [](const Primitive &item) {
                    return item.text.find("操作员 质") == 0 &&
                           item.text.find("…") != std::string::npos;
                }),
                "最小图表尺寸下长名称应带省略号");
        (void)snapshot(compact).dump(); // 序列化也会检查截断后的 UTF-8。
    });
    for (const auto *method : {"crossed_xbarr", "crossed_anova"})
        for (int operators : {2, 3})
            for (int repeats : {2, 3})
                for (int parts = 5; parts <= 10; ++parts) {
                    const auto result = study(operators, repeats, parts, method);
                    for (const auto kind : msa::charts::all_kinds())
                        check(std::string(method) + " " + std::to_string(operators) + "×" +
                                  std::to_string(repeats) + "×" + std::to_string(parts) + " " +
                                  msa::charts::label(kind),
                              [&] {
                                  const auto matrix = result.matrix;
                                  const auto config = result.config, metrics = result.metrics;
                                  auto editor = msa::charts::default_editor(kind);
                                  const auto first =
                                      msa::charts::make_scene(result, kind, editor, 840, 480);
                                  validate_scene(first);
                                  if (kind == Kind::MeanChart || kind == Kind::RangeChart)
                                      validate_control_limits(first, result,
                                                              kind == Kind::RangeChart);
                                  require(snapshot(first) == snapshot(msa::charts::make_scene(
                                                                 result, kind, editor, 840, 480)),
                                          "相同数据的图形不稳定");
                                  validate_scene(
                                      msa::charts::make_scene(result, kind, editor, 360, 260));
                                  editor.show_grid = false;
                                  editor.show_legend = false;
                                  editor.show_limits = false;
                                  editor.line_width = 3;
                                  editor.point_radius = 5;
                                  editor.primary_color = "#16866B";
                                  validate_scene(
                                      msa::charts::make_scene(result, kind, editor, 840, 480));
                                  require(result.matrix == matrix && result.config == config &&
                                              result.metrics == metrics,
                                          "图表编辑改变了研究数据或统计结果");
                              });
                }
    const auto invalid_data = [&](const std::string &name,
                                  const std::function<void(msa::Result &)> &mutate) {
        check(name, [&] {
            auto result = study();
            mutate(result);
            for (const auto kind : msa::charts::all_kinds())
                rejects([&] {
                    msa::charts::make_scene(result, kind, msa::charts::default_editor(kind), 840,
                                            480);
                });
        });
    };
    invalid_data("拒绝空数据", [](msa::Result &r) { r.matrix.clear(); });
    invalid_data("拒绝缺少测量行", [](msa::Result &r) { r.matrix.pop_back(); });
    invalid_data("拒绝缺少部件值", [](msa::Result &r) { r.matrix[0].pop_back(); });
    invalid_data("拒绝 NaN 测量值",
                 [](msa::Result &r) { r.matrix[0][0] = std::numeric_limits<double>::quiet_NaN(); });
    invalid_data("拒绝无穷测量值",
                 [](msa::Result &r) { r.matrix[0][0] = std::numeric_limits<double>::infinity(); });
    invalid_data("拒绝研究设计与数据不符",
                 [](msa::Result &r) { r.config["design"]["operators"] = 5; });
    check("拒绝缺失及无效变异分量", [] {
        for (const auto &value :
             std::vector<Json>{nullptr, "10", -1, Json(std::numeric_limits<double>::quiet_NaN())}) {
            auto result = study();
            result.metrics["TV"] = value;
            rejects([&] {
                msa::charts::make_scene(result, Kind::VarianceComponents,
                                        msa::charts::default_editor(Kind::VarianceComponents), 840,
                                        480);
            });
        }
        auto result = study();
        result.metrics.erase("TV");
        rejects([&] {
            msa::charts::make_scene(result, Kind::VarianceComponents,
                                    msa::charts::default_editor(Kind::VarianceComponents), 840,
                                    480);
        });
    });
    check("极大中心和微小变异仍生成有限稳定图元", [] {
        const auto result = study(3, 3, 10, "crossed_anova", 1e9, .0001);
        for (const auto kind : msa::charts::all_kinds())
            validate_scene(
                msa::charts::make_scene(result, kind, msa::charts::default_editor(kind), 840, 480));
    });
    check("常量数据和零极差仍可展示且不生成无穷坐标", [] {
        for (const auto *method : {"crossed_xbarr", "crossed_anova"}) {
            auto result = study(3, 3, 10, method);
            for (auto &row : result.matrix)
                std::fill(row.begin(), row.end(), 10.);
            result.metrics = msa::calculate(result.matrix, result.config);
            for (const auto kind : msa::charts::all_kinds())
                validate_scene(msa::charts::make_scene(
                    result, kind, msa::charts::default_editor(kind), 840, 480));
        }
    });
    check("缩窄纵轴后数据线正确裁剪且图元有限", [] {
        const auto result = study();
        for (const auto kind : {Kind::MeasurementByPart, Kind::MeasurementByOperator,
                                Kind::MeanChart, Kind::Interaction}) {
            auto editor = msa::charts::default_editor(kind);
            editor.y_min = 9.99;
            editor.y_max = 10.01;
            validate_scene(msa::charts::make_scene(result, kind, editor, 840, 480));
        }
    });
    check("拒绝错误编辑范围和尺寸", [] {
        const auto result = study();
        auto editor = msa::charts::default_editor(Kind::MeasurementByPart);
        for (const auto dims : {std::pair<float, float>{359.f, 480.f},
                                {840.f, 259.f},
                                {NAN, 480.f},
                                {840.f, INFINITY}})
            rejects([&] {
                msa::charts::make_scene(result, Kind::MeasurementByPart, editor, dims.first,
                                        dims.second);
            });
        editor.y_min = 10;
        editor.y_max = 9;
        rejects(
            [&] { msa::charts::make_scene(result, Kind::MeasurementByPart, editor, 840, 480); });
        editor.y_min = std::numeric_limits<double>::quiet_NaN();
        editor.y_max = 11;
        rejects(
            [&] { msa::charts::make_scene(result, Kind::MeasurementByPart, editor, 840, 480); });
        editor = msa::charts::default_editor(Kind::MeasurementByPart);
        editor.primary_color = "red";
        rejects(
            [&] { msa::charts::make_scene(result, Kind::MeasurementByPart, editor, 840, 480); });
        for (int invalid : {-1, 1, 11}) {
            editor = msa::charts::default_editor(Kind::RangeChart);
            editor.y_tick_intervals = invalid;
            rejects([&] { msa::charts::make_scene(result, Kind::RangeChart, editor, 840, 480); });
        }
        for (int invalid : {-1, 2, 41}) {
            editor = msa::charts::default_editor(Kind::Histogram);
            editor.histogram_bins = invalid;
            rejects([&] { msa::charts::make_scene(result, Kind::Histogram, editor, 840, 480); });
        }
    });
}

void export_checks(const std::filesystem::path &output) {
    using platform::ChartExportOptions;
    using platform::ChartFormat;
    ChartExportOptions options;
    options.panel_width = 1000;
    options.columns = 2;
    check("内嵌字体包含图表中文和常用数学字符", [] {
        const auto font = platform::embeddedFont();
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        require(FT_Init_FreeType(&library) == 0, "无法初始化字体检查");
        const auto status =
            FT_New_Memory_Face(library, font->data(), static_cast<FT_Long>(font->size()), 0, &face);
        if (status) {
            FT_Done_FreeType(library);
            throw std::runtime_error("内嵌字体不可加载");
        }
        bool supported = true;
        for (const auto character :
             std::wstring(L"量具测量系统均值极差方差分析交互作用操作员部件重复次数标准上下限±×％"))
            supported = supported && FT_Get_Char_Index(face, character) != 0;
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        require(supported, "内嵌字体缺少图表中文或数学字符");
    });
    for (const auto *method : {"crossed_xbarr", "crossed_anova"}) {
        std::vector<Scene> scenes;
        const auto result = study(3, 3, 10, method);
        int index = 0;
        for (const auto kind : msa::charts::all_kinds()) {
            auto editor = msa::charts::default_editor(kind);
            if (index == 0)
                editor.title = "交叉量具研究：中文长标题验证，包含研究设计、分析方法与测量系统结果";
            scenes.push_back(msa::charts::make_scene(result, kind, editor, 840, 480));
            const auto name = std::string(method) + "_" + std::to_string(++index);
            for (const auto format : {ChartFormat::Png, ChartFormat::Svg}) {
                options.format = format;
                check(name + " 单图 " + platform::chart_extension(format), [&] {
                    const std::vector<Scene> single{scenes.back()};
                    const auto path = platform::export_charts(
                        single, output / (name + platform::chart_extension(format)), options,
                        msa::ExistingFile::Replace);
                    record_file(path, options, single);
                });
            }
        }
        for (const auto format :
             {ChartFormat::Png, ChartFormat::Jpeg, ChartFormat::Pdf, ChartFormat::Svg}) {
            options.format = format;
            check(std::string(method) + " 七图组合 " + platform::chart_extension(format), [&] {
                const auto path =
                    platform::export_charts(scenes,
                                            output / (std::string(method) + "_combined" +
                                                      platform::chart_extension(format)),
                                            options, msa::ExistingFile::Replace);
                record_file(path, options, scenes);
            });
        }
        options.format = ChartFormat::Png;
    }
    const auto scene = primitive_scene();
    options.panel_width = 640;
    options.columns = 1;
    options.format = ChartFormat::Png;
    check("基础图元位置、色彩及中文换行样图", [&] {
        const auto path = platform::export_charts({scene}, output / L"中文图元与长标题.png",
                                                  options, msa::ExistingFile::Replace);
        record_file(path, options, {scene});
        GdiSession session;
        Gdiplus::Bitmap bitmap(path.c_str());
        Gdiplus::Color pixel;
        require(bitmap.GetPixel(50, 60, &pixel) == Gdiplus::Ok && pixel.GetR() > 240 &&
                    pixel.GetG() < 20,
                "矩形位置、颜色或缩放不正确");
        require(bitmap.GetPixel(196, 71, &pixel) == Gdiplus::Ok && pixel.GetG() > 140 &&
                    pixel.GetR() < 20,
                "圆形位置或颜色不正确");
        std::size_t chineseInk = 0;
        for (int y = 126; y < 162; ++y)
            for (int x = 36; x < 300; ++x) {
                bitmap.GetPixel(x, y, &pixel);
                chineseInk += pixel.GetR() < 200 && pixel.GetG() < 200 && pixel.GetB() < 200;
            }
        require(chineseInk > 400, "中文文字未绘制或被错误裁剪");
    });
    check("高分辨率缩放保持图形和外边距的位置", [&] {
        auto doubled = options;
        doubled.panel_width = 1280;
        const auto path = platform::export_charts({scene}, output / "double-scale.png", doubled,
                                                  msa::ExistingFile::Replace);
        record_file(path, doubled, {scene});
        GdiSession session;
        Gdiplus::Bitmap bitmap(path.c_str());
        Gdiplus::Color pixel;
        require(bitmap.GetPixel(80, 110, &pixel) == Gdiplus::Ok && pixel.GetR() > 240 &&
                    pixel.GetG() < 20,
                "缩放错误地放大了外边距或改变了图元位置");
    });
    check("文本按盒宽换行并严格裁剪在文本框内", [&] {
        Scene textScene{640, 400, {}};
        Primitive text;
        text.type = Primitive::Type::Text;
        text.x = 100;
        text.y = 100;
        text.x2 = 90;
        text.y2 = 120;
        text.font_size = 24;
        text.text = "测量系统分析量具研究中文文字换行和裁剪边界检查";
        textScene.items.push_back(text);
        const auto path = platform::export_charts({textScene}, output / "text-clipping.png",
                                                  options, msa::ExistingFile::Replace);
        record_file(path, options, {textScene});
        GdiSession session;
        Gdiplus::Bitmap bitmap(path.c_str());
        std::size_t wrappedInk = 0;
        for (int y = 0; y < 432; ++y)
            for (int x = 0; x < 672; ++x) {
                Gdiplus::Color pixel;
                bitmap.GetPixel(x, y, &pixel);
                if (pixel.GetR() >= 220 && pixel.GetG() >= 220 && pixel.GetB() >= 220)
                    continue;
                require(x >= 116 && x < 206 && y >= 116 && y < 236, "文字绘制到了文本框外");
                wrappedInk += y > 160;
            }
        require(wrappedInk > 100, "长文本未换行到后续行");
    });
    check("单图 PDF 可解析且物理尺寸正确", [&] {
        auto pdfOptions = options;
        pdfOptions.format = ChartFormat::Pdf;
        const auto path = platform::export_charts({scene}, output / "single.pdf", pdfOptions,
                                                  msa::ExistingFile::Replace);
        record_file(path, pdfOptions, {scene});
    });
    check("支持大小写扩展名、JPEG 别名及自动扩展名", [&] {
        const auto automatic = platform::export_charts({scene}, output / "auto-extension", options,
                                                       msa::ExistingFile::Replace);
        require(automatic.extension() == L".png", "没有补齐扩展名");
        auto jpeg = options;
        jpeg.format = ChartFormat::Jpeg;
        const auto path = platform::export_charts({scene}, output / "alias.JPEG", jpeg,
                                                  msa::ExistingFile::Replace);
        record_file(path, jpeg, {scene});
    });
    check("默认保护同名文件且显式替换可用", [&] {
        const auto path = output / "collision.png";
        msa::atomic_text(path, "original-file");
        bool collision = false;
        try {
            platform::export_charts({scene}, path, options);
        } catch (const msa::FileExistsError &) {
            collision = true;
        }
        require(collision && bytes(path) == "original-file", "默认导出覆盖了原文件");
        platform::export_charts({scene}, path, options, msa::ExistingFile::Replace);
        require(inspect_file(path).width == 672, "显式覆盖没有写入新图表");
    });
    check("非法路径不会创建文件", [&] {
        for (const auto *name : {L"CON.png", L"nul.png", L"COM1.png", L"stream:alternate.png",
                                 L"bad?.png", L"tail.png ", L"tail.png.", L"../NUL/file.png"})
            rejects([&] { platform::export_charts({scene}, output / name, options); });
        rejects([&] { platform::export_charts({scene}, L"C:chart.png", options); });
        rejects([&] { platform::export_charts({scene}, std::filesystem::path(), options); });
        rejects([&] { platform::export_charts({scene}, output / "wrong.pdf", options); });
        std::filesystem::create_directories(output / "folder.png");
        rejects([&] { platform::export_charts({scene}, output / "folder.png", options); });
    });
    check("拒绝无效导出设置和过大组合", [&] {
        const auto path = output / "invalid-options.png";
        for (const int width : {0, 639, 4097}) {
            auto changed = options;
            changed.panel_width = width;
            rejects([&] { platform::export_charts({scene}, path, changed); });
        }
        for (const int dpi : {0, 71, 601}) {
            auto changed = options;
            changed.dpi = dpi;
            rejects([&] { platform::export_charts({scene}, path, changed); });
        }
        for (const int columns : {0, 5}) {
            auto changed = options;
            changed.columns = columns;
            rejects([&] { platform::export_charts({scene}, path, changed); });
        }
        for (const int quality : {0, 49, 101}) {
            auto changed = options;
            changed.jpeg_quality = quality;
            rejects([&] { platform::export_charts({scene}, path, changed); });
        }
        rejects([&] { platform::export_charts({}, path, options); });
        rejects([&] { platform::export_charts(std::vector<Scene>(17, scene), path, options); });
        auto large = options;
        large.panel_width = 4096;
        large.columns = 4;
        rejects([&] { platform::export_charts(std::vector<Scene>(16, scene), path, large); });
        auto unknown = options;
        unknown.format = static_cast<ChartFormat>(-1);
        rejects([&] { platform::export_charts({scene}, path, unknown); });
    });
    check("渲染输入异常不覆盖已有文件且没有临时残留", [&] {
        const auto path = output / "protected-invalid.png";
        msa::atomic_text(path, "untouched");
        const auto invalid = [&](const std::function<void(Scene &)> &mutate) {
            auto changed = scene;
            mutate(changed);
            rejects([&] {
                platform::export_charts({changed}, path, options, msa::ExistingFile::Replace);
            });
            require(bytes(path) == "untouched", "渲染失败改变了已有文件");
        };
        invalid([](Scene &s) { s.width = NAN; });
        invalid([](Scene &s) { s.height = 0; });
        invalid([](Scene &s) { s.items[0].x = INFINITY; });
        invalid([](Scene &s) { s.items[0].x2 = -1; });
        invalid([](Scene &s) { s.items[1].x2 = -1; });
        invalid([](Scene &s) { s.items[0].color = "#GG0000"; });
        invalid([](Scene &s) { s.items[0].type = static_cast<Primitive::Type>(-1); });
        invalid([](Scene &s) { s.items[3].align = static_cast<Primitive::Align>(-1); });
        invalid([](Scene &s) { s.items[3].text = std::string("\xc0\xaf", 2); });
        invalid([](Scene &s) { s.items[3].x2 = -1; });
        invalid([](Scene &s) { s.items[3].font_size = 2000; });
        invalid([](Scene &s) { s.items[3].text = std::string(65537, 'a'); });
        invalid([](Scene &s) { s.items.resize(100001); });
        for (const auto &entry : std::filesystem::directory_iterator(output))
            require(entry.path().extension() != L".tmp", "失败导出遗留临时文件");
    });
    check("文件系统写入失败保持现有文件并清理临时输出", [&] {
        const auto parent = output / "parent-as-file";
        msa::atomic_text(parent, "original-parent");
        rejects([&] { platform::export_charts({scene}, parent / "child.png", options); });
        require(bytes(parent) == "original-parent", "写入失败破坏了父路径文件");
    });
    check("编码完成后替换只读文件失败仍保留原文件", [&] {
        const auto path = output / "readonly-target.png";
        msa::atomic_text(path, "readonly-original");
        require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
                "无法准备只读目标文件");
        bool failed = false;
        try {
            platform::export_charts({scene}, path, options, msa::ExistingFile::Replace);
        } catch (const std::exception &) {
            failed = true;
        }
        const auto restored = SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        require(restored != 0 && failed && bytes(path) == "readonly-original",
                "替换只读文件没有失败或破坏了原文件");
        for (const auto &entry : std::filesystem::directory_iterator(output))
            require(entry.path().extension() != L".tmp", "写入失败后遗留临时文件");
    });
    check("反复成功及失败导出后图形和进程句柄稳定", [&] {
        const auto path = output / "resource-reuse.png";
        platform::export_charts({scene}, path, options, msa::ExistingFile::Replace);
        DWORD handlesBefore = 0, handlesAfter = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 15; ++i) {
            platform::export_charts({scene}, path, options, msa::ExistingFile::Replace);
            auto bad = scene;
            bad.items[0].x = NAN;
            rejects(
                [&] { platform::export_charts({bad}, path, options, msa::ExistingFile::Replace); });
        }
        GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        const auto gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        require(handlesAfter <= handlesBefore + 4 && gdiAfter <= gdiBefore + 4,
                "重复导出后资源句柄持续增长");
    });
    check("最小尺寸控制图与历史过程占比分量图样图", [&] {
        auto result = study(3, 3, 10, "crossed_anova");
        result.config["analysis"]["historical_sd"] = 0.2;
        result.metrics = msa::calculate(result.matrix, result.config);
        for (const auto kind : {Kind::RangeChart, Kind::MeanChart, Kind::VarianceComponents})
            for (const bool compact : {true, false}) {
                const auto sample =
                    msa::charts::make_scene(result, kind, msa::charts::default_editor(kind),
                                            compact ? 360.f : 840.f, compact ? 260.f : 480.f);
                validate_scene(sample);
                const auto name = std::string("anova_history_") + std::to_string(int(kind)) +
                                  (compact ? "_compact.png" : "_normal.png");
                auto sampleOptions = options;
                sampleOptions.panel_width = compact ? 720 : 1000;
                const auto path = platform::export_charts({sample}, output / name, sampleOptions,
                                                          msa::ExistingFile::Replace);
                record_file(path, sampleOptions, {sample});
            }
    });
}

void native_reference(const std::filesystem::path &reportPath,
                      const std::filesystem::path &output) {
    const auto input = Json::parse(msa::read_text(reportPath));
    const auto matrix = input.at("matrix").get<msa::Matrix>();
    const auto originalConfig = msa::normalize_config(input.at("config"));
    const auto folder = output / "native-reference";
    platform::ChartExportOptions options;
    options.panel_width = 1200;
    options.columns = 2;
    for (const auto *method : {"crossed_xbarr", "crossed_anova"}) {
        msa::Result result;
        result.matrix = matrix;
        result.config = originalConfig;
        result.config["analysis"]["method"] = method;
        result.metrics = msa::calculate(result.matrix, result.config);
        std::vector<Scene> scenes;
        std::vector<Scene> defaultScenes;
        int index = 0;
        for (const auto kind : msa::charts::all_kinds()) {
            scenes.push_back(msa::charts::make_scene(
                result, kind, msa::charts::default_editor(kind, Style::Origin), 840, 480));
            defaultScenes.push_back(
                msa::charts::make_scene(result, kind, msa::charts::default_editor(kind), 840, 480));
            const auto name = std::string(method) + "_" + std::to_string(++index) + ".png";
            const std::vector<Scene> single{scenes.back()};
            const auto path =
                platform::export_charts(single, folder / name, options, msa::ExistingFile::Replace);
            record_file(path, options, single);
        }
        for (const auto format : {platform::ChartFormat::Png, platform::ChartFormat::Jpeg,
                                  platform::ChartFormat::Pdf, platform::ChartFormat::Svg}) {
            options.format = format;
            const auto name = std::string(method) + "_combined" + platform::chart_extension(format);
            const auto path =
                platform::export_charts(scenes, folder / name, options, msa::ExistingFile::Replace);
            record_file(path, options, scenes);
            if (format == platform::ChartFormat::Png || format == platform::ChartFormat::Svg) {
                const auto appName =
                    std::string(method) + "_default" + platform::chart_extension(format);
                const auto appPath = platform::export_charts(defaultScenes, folder / appName,
                                                             options, msa::ExistingFile::Replace);
                record_file(appPath, options, defaultScenes);
            }
        }
        options.format = platform::ChartFormat::Png;
        require(result.matrix == matrix, "参考图表改变了共同测量数据");
        msa::atomic_text(
            folder / (std::string(method) + "_metrics.json"),
            Json({{"config", result.config}, {"matrix", matrix}, {"metrics", result.metrics}})
                    .dump(2) +
                '\n');
    }
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : L"chart-test-output";
        std::filesystem::create_directories(output);
        model_checks();
        export_checks(output);
        if (argc > 2)
            check("共同参考数据按两种方法计算并导出完整七图",
                  [&] { native_reference(argv[2], output); });
        const auto failed = std::count_if(checks.begin(), checks.end(), [](const Json &item) {
            return !item.at("passed").get<bool>();
        });
        const Json report{{"passed", failed == 0},  {"checks", checks},
                          {"count", checks.size()}, {"failed", failed},
                          {"files", files},         {"pdf_content", "single_page_raster"}};
        msa::atomic_text(output / "chart-regression.json", report.dump(2) + '\n');
        std::cout << "图表模型与导出检查：" << checks.size() << " 项，失败 " << failed << " 项。\n";
        return failed ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << "图表测试入口失败：" << e.what() << '\n';
        return 1;
    }
}
