#pragma once
#include "msa/core.hpp"
#include <algorithm>

namespace platform {
enum class ExportKind { Csv, Xlsx, Png, Jpeg, ChartPdf, Html, ReportPdf, Svg };

struct SaveTarget {
    std::filesystem::path path;
    ExportKind format;
    msa::ExistingFile policy;
};

struct ExportFileType {
    const wchar_t *label;
    const wchar_t *filter;
    const wchar_t *extension;
};
inline ExportFileType exportFileType(ExportKind kind) {
    switch (kind) {
    case ExportKind::Csv:
        return {L"CSV 测量数据 (*.csv)", L"*.csv", L"csv"};
    case ExportKind::Xlsx:
        return {L"Excel 工作簿 (*.xlsx)", L"*.xlsx", L"xlsx"};
    case ExportKind::Png:
        return {L"PNG 图像 (*.png)", L"*.png", L"png"};
    case ExportKind::Jpeg:
        return {L"JPEG 图像 (*.jpg;*.jpeg)", L"*.jpg;*.jpeg", L"jpg"};
    case ExportKind::ChartPdf:
        return {L"PDF 图表 (*.pdf)", L"*.pdf", L"pdf"};
    case ExportKind::Svg:
        return {L"SVG 矢量图 (*.svg)", L"*.svg", L"svg"};
    case ExportKind::Html:
        return {L"HTML 方法对比报告 (*.html;*.htm)", L"*.html;*.htm", L"html"};
    case ExportKind::ReportPdf:
        return {L"PDF 方法对比报告 (*.pdf)", L"*.pdf", L"pdf"};
    }
    throw std::invalid_argument("未知导出格式");
}
inline bool reportFormat(ExportKind kind) {
    return kind == ExportKind::Html || kind == ExportKind::ReportPdf;
}
inline bool dataFormat(ExportKind kind) {
    return kind == ExportKind::Csv || kind == ExportKind::Xlsx;
}
inline std::filesystem::path importFilenameSuggestion(const std::filesystem::path &directory,
                                                       const std::string &basename) {
    if (basename.empty())
        return {};
    const auto name = std::filesystem::u8path(basename);
    const auto csv = std::filesystem::u8path(basename + ".csv");
    const auto xlsx = std::filesystem::u8path(basename + ".xlsx");
    std::error_code ec;
    const bool hasCsv = std::filesystem::is_regular_file(directory / csv, ec);
    const bool hasXlsx = std::filesystem::is_regular_file(directory / xlsx, ec);
    if (hasCsv != hasXlsx)
        return hasCsv ? csv : xlsx;
    return name;
}
inline std::filesystem::path completeExportFilename(std::filesystem::path path, ExportKind kind) {
    if (path.filename().empty() || path.filename() == L"." || path.filename() == L"..")
        throw std::invalid_argument("请指定导出文件名");
    const std::wstring expected = std::wstring(L".") + exportFileType(kind).extension;
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; });
    if (ext == expected || (kind == ExportKind::Jpeg && ext == L".jpeg") ||
        (kind == ExportKind::Html && ext == L".htm"))
        return path;
    if (ext.empty() || (dataFormat(kind) && (ext == L".csv" || ext == L".xlsx")) ||
        (reportFormat(kind) && (ext == L".htm" || ext == L".html" || ext == L".pdf")))
        return path.replace_extension(expected);
    throw std::invalid_argument("文件扩展名与所选导出格式不一致");
}
} // namespace platform
