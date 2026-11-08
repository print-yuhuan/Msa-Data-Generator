#pragma once
#include "msa/charts.hpp"
#include <filesystem>

namespace platform {
enum class ChartFormat { Png, Jpeg, Pdf };
struct ChartExportOptions {
    ChartFormat format = ChartFormat::Png;
    int columns = 2;
    // 每幅图的输出宽度；按 Scene 的长宽比计算高度，不拉伸图形。
    int panel_width = 1600;
    int dpi = 192;
    int jpeg_quality = 95;
};
const char *chart_extension(ChartFormat format);
// 多幅图按输入顺序组合为一张图片或一页 PDF；PDF 使用高分辨率图像内容。
// 未指定扩展名时自动补齐，已有扩展名必须与格式匹配。默认原子写入且不覆盖。
std::filesystem::path export_charts(const std::vector<msa::charts::Scene> &scenes,
                                    const std::filesystem::path &path,
                                    const ChartExportOptions &options = {},
                                    msa::ExistingFile policy = msa::ExistingFile::Preserve);
} // namespace platform
