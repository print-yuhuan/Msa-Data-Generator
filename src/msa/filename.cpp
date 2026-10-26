#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace msa {
const std::vector<FilenameVariable> &filename_variables() {
    static const std::vector<FilenameVariable> variables{
        {"YYYY", "", "四位年份", "2026"},
        {"YY", "", "两位年份", "26"},
        {"MM", "", "月份，两位", "10"},
        {"DD", "", "日期，两位", "24"},
        {"H", "", "小时，24 小时制、两位", "22"},
        {"M", "", "分钟，两位；MM 是月份", "08"},
        {"S", "", "秒，两位", "12"},
        {"MS", "", "毫秒，三位", "037"},
        {"TIMESTAMP", "时间戳", "Unix 时间戳，秒；从 1970-01-01 UTC 起算", "1792850892"},
        {"DATE", "", "完整日期", "2026-10-24"},
        {"TIME", "", "完整时间", "22-08-12"},
        {"DATETIME", "", "完整日期与时间", "2026-10-24_22-08-12"},
        {"OPERATORS", "人员数量", "生成结果的人员数量", "3"},
        {"REPLICATES", "测量次数", "生成结果的测量次数", "3"},
        {"PARTS", "产品件数", "生成结果的产品件数", "10"},
        {"USL", "上规格限", "上规格限；未设置为 NA", "10.5"},
        {"LSL", "下规格限", "下规格限；未设置为 NA", "9.5"},
        {"CENTER", "数据中心", "中心设定值；留空时取实际数据均值", "10"},
        {"MULTIPLIER", "研究变异倍数", "研究变异倍数", "6"},
        {"SEED", "随机种子", "本次实际使用的随机种子", "123"},
        {"DP", "小数位数", "数据输出小数位数", "4"},
        {"EV_PCT", "EV实际值", "EV/TV%，两位小数，不含百分号", "10.00"},
        {"AV_PCT", "AV实际值", "AV/TV%，两位小数，不含百分号", "9.81"},
        {"GRR_PCT", "GRR实际值", "GRR/TV%，两位小数，不含百分号", "14.01"},
        {"PT_PCT", "PT实际值", "P/T%，两位小数，不含百分号", "14.28"},
        {"NDC", "ndc实际值", "实际 ndc；不可计算为 NA", "9"},
        {"VERSION", "程序版本号", "程序版本号", ui_version}};
    return variables;
}
namespace {
const FilenameVariable &variable(const std::string &name) {
    for (const auto &v : filename_variables())
        if (name == v.name || (*v.alias && name == v.alias))
            return v;
    throw ConfigError("未知文件名变量：[" + name + "]；请查看帮助页的变量表。");
}
std::string expand(const std::string &name,
                   const std::function<std::string(const FilenameVariable &)> &value) {
    std::string out;
    for (std::size_t i = 0; i < name.size();) {
        if (name.compare(i, 2, "[[") == 0) {
            out += '[';
            i += 2;
        } else if (name.compare(i, 2, "]]") == 0) {
            out += ']';
            i += 2;
        } else if (name[i] == '[') {
            const auto end = name.find(']', i + 1);
            if (end == std::string::npos || end == i + 1 || name.find('[', i + 1) < end)
                throw ConfigError("文件名变量括号未配对、为空或存在嵌套；请查看帮助页。");
            out += value(variable(name.substr(i + 1, end - i - 1)));
            i = end + 1;
        } else if (name[i] == ']')
            throw ConfigError("文件名存在未配对的右方括号；字面量方括号请使用 [[ 和 ]]。");
        else
            out += name[i++];
    }
    return out;
}
void validate_name(const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    if (name.size() <= 4 || lower.substr(lower.size() - 4) != ".csv" ||
        name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        throw ConfigError("输出文件名须为不含路径的 .csv 文件名");
    // Count UTF-16 code units, including surrogate pairs, on every platform.
    std::size_t units = 0;
    for (std::size_t i = 0; i < name.size();) {
        const auto first = static_cast<unsigned char>(name[i]);
        unsigned code = first;
        int count = 1;
        if (first >= 0xF0 && first <= 0xF4) {
            count = 4;
            code &= 7;
        } else if (first >= 0xE0 && first <= 0xEF) {
            count = 3;
            code &= 15;
        } else if (first >= 0xC2 && first <= 0xDF) {
            count = 2;
            code &= 31;
        } else if (first >= 0x80 || first < 32)
            throw ConfigError("文件名包含无效字符或控制字符");
        if (i + count > name.size())
            throw ConfigError("文件名不是有效 UTF-8 文本");
        for (int j = 1; j < count; ++j) {
            auto c = static_cast<unsigned char>(name[i + j]);
            if ((c & 0xC0) != 0x80)
                throw ConfigError("文件名不是有效 UTF-8 文本");
            code = (code << 6) | (c & 63);
        }
        if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
            (count == 4 && code < 0x10000) || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            throw ConfigError("文件名不是有效 UTF-8 文本");
        units += code > 0xFFFF ? 2 : 1;
        i += count;
    }
    if (units > 255)
        throw ConfigError("展开后的 CSV 文件名过长，请缩短模板或减少变量");
    auto base = lower.substr(0, lower.find('.'));
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$")
        throw ConfigError("输出文件名不能使用 Windows 保留名称");
    if (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") {
        const auto suffix = base.substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') || suffix == "¹" ||
            suffix == "²" || suffix == "³")
            throw ConfigError("输出文件名不能使用 Windows 保留名称");
    }
}
std::string padded(int n, int width = 2) {
    auto s = std::to_string(n);
    return std::string(std::max(0, width - int(s.size())), '0') + s;
}
std::string compact(const Json &v) {
    if (v.is_null())
        return "NA";
    if (!v.is_number() || !std::isfinite(v.get<double>()))
        return "NA";
    if (v.get<double>() == 0)
        return "0";
    auto s = v.dump();
    if (s.size() >= 2 && s.substr(s.size() - 2) == ".0")
        s.resize(s.size() - 2);
    return s;
}
std::filesystem::path directory(const Json &output) {
    auto path = std::filesystem::u8path(output.at("directory").get<std::string>());
    if (path.has_root_name() && !path.has_root_directory())
        throw ConfigError("输出目录请使用完整路径或普通相对路径，不能使用盘符相对路径。");
    if (path.empty() || path.is_relative())
        path = config_path().parent_path() / path;
    return path.lexically_normal();
}
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b) {
    auto aa = std::filesystem::weakly_canonical(a), bb = std::filesystem::weakly_canonical(b);
#ifdef _WIN32
    // Ordinal comparison for Unicode Windows filenames, independent of locale.
    return CompareStringOrdinal(aa.c_str(), -1, bb.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return aa == bb;
#endif
}
} // namespace
void validate_filename_template(const std::string &name) {
    if (name.size() > 16384)
        throw ConfigError("文件名模板过长");
    validate_name(expand(name, [](const FilenameVariable &) { return "0"; }));
}
Json normalize_output(const Json &output) {
    auto cfg = default_config();
    cfg["output"] = output;
    return normalize_config(cfg)["output"];
}
std::string render_filename(const std::string &name, const Result &result,
                            std::chrono::system_clock::time_point when) {
    validate_filename_template(name);
    const auto cfg = normalize_config(result.config);
    const auto metrics = calculate(result.matrix, cfg);
    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(when);
    if (seconds > when)
        seconds -= std::chrono::seconds(1);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time))
        throw ConfigError("无法读取本地导出时间");
#else
    if (!localtime_r(&time, &local))
        throw ConfigError("无法读取本地导出时间");
#endif
    const auto date = padded(local.tm_year + 1900, 4) + "-" + padded(local.tm_mon + 1) + "-" +
                      padded(local.tm_mday);
    const auto clock =
        padded(local.tm_hour) + "-" + padded(local.tm_min) + "-" + padded(local.tm_sec);
    auto out = expand(name, [&](const FilenameVariable &v) -> std::string {
        const std::string key = v.name;
        if (key == "YYYY")
            return padded(local.tm_year + 1900, 4);
        if (key == "YY")
            return padded((local.tm_year + 1900) % 100);
        if (key == "MM")
            return padded(local.tm_mon + 1);
        if (key == "DD")
            return padded(local.tm_mday);
        if (key == "H")
            return padded(local.tm_hour);
        if (key == "M")
            return padded(local.tm_min);
        if (key == "S")
            return padded(local.tm_sec);
        if (key == "MS")
            return padded(
                int(std::chrono::duration_cast<std::chrono::milliseconds>(when - seconds).count()),
                3);
        if (key == "TIMESTAMP")
            return std::to_string(time);
        if (key == "DATE")
            return date;
        if (key == "TIME")
            return clock;
        if (key == "DATETIME")
            return date + "_" + clock;
        if (key == "OPERATORS")
            return compact(cfg["design"]["operators"]);
        if (key == "REPLICATES")
            return compact(cfg["design"]["replicates"]);
        if (key == "PARTS")
            return compact(cfg["design"]["parts"]);
        if (key == "USL")
            return compact(cfg["specification"]["usl"]);
        if (key == "LSL")
            return compact(cfg["specification"]["lsl"]);
        if (key == "CENTER")
            return compact(cfg["generation"]["center"].is_null() ? metrics["mean"]
                                                                 : cfg["generation"]["center"]);
        if (key == "MULTIPLIER")
            return compact(cfg["analysis"]["study_multiplier"]);
        if (key == "SEED")
            return std::to_string(result.seed);
        if (key == "DP")
            return compact(cfg["generation"]["decimal_places"]);
        if (key == "VERSION")
            return ui_version;
        const std::string metric = key == "EV_PCT"    ? "EV_pct"
                                   : key == "AV_PCT"  ? "AV_pct"
                                   : key == "GRR_PCT" ? "GRR_pct"
                                   : key == "PT_PCT"  ? "PT_pct"
                                                      : "ndc";
        if (metrics[metric].is_null())
            return "NA";
        return key == "NDC" ? compact(metrics[metric]) : fixed(metrics[metric].get<double>(), 2);
    });
    validate_name(out);
    return out;
}
std::filesystem::path configured_csv_path(const Result &result, const Json &raw,
                                          std::chrono::system_clock::time_point when) {
    const auto output = normalize_output(raw);
    return (directory(output) /
            std::filesystem::u8path(render_filename(output["filename"], result, when)))
        .lexically_normal();
}
std::filesystem::path export_configured_csv(const Result &result, const Json &raw,
                                            std::chrono::system_clock::time_point when,
                                            const std::filesystem::path &reserved) {
    const auto output = normalize_output(raw);
    const auto base = configured_csv_path(result, output, when);
    const bool number = output["existing_file"] == "number";
    // Expand once. All collision retries retain the same timestamp and result.
    for (int i = 0; i < 1000000; ++i) {
        auto path = i == 0
                        ? base
                        : base.parent_path() /
                              std::filesystem::u8path(base.stem().u8string() + "_" + padded(i, 3) +
                                                      base.extension().u8string());
        validate_name(path.filename().u8string());
        if (!reserved.empty() && same_path(path, reserved))
            throw ConfigError("CSV 与报告路径不能相同");
        if (number && std::filesystem::exists(path))
            continue;
        try {
            export_csv(result, path, output,
                       number ? ExistingFile::Preserve : ExistingFile::Replace);
            return path;
        } catch (const FileExistsError &) {
            if (!number)
                throw;
        }
    }
    throw std::runtime_error("同名文件过多，无法分配导出序号，请修改文件名或目录。");
}
} // namespace msa
