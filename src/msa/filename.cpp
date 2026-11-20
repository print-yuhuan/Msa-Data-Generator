#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>

namespace msa {
const std::vector<FilenameVariable> &filename_variables() {
    static const std::vector<FilenameVariable> variables{
        {"YYYY", "四位年份", "四位年份", "2026"},
        {"YY", "两位年份", "两位年份", "26"},
        {"MM", "月份", "月份，两位", "10"},
        {"DD", "日", "日期，两位", "24"},
        {"H", "小时", "小时，24 小时制、两位", "22"},
        {"M", "分钟", "分钟，两位；MM 是月份", "08"},
        {"S", "秒", "秒，两位", "12"},
        {"MS", "毫秒", "毫秒，三位", "037"},
        {"TIMESTAMP", "时间戳", "Unix 时间戳，秒；从 1970-01-01 UTC 起算", "1792850892"},
        {"DATE", "日期", "完整日期", "2026-10-24"},
        {"TIME", "时间", "完整时间", "22-08-12"},
        {"DATETIME", "日期时间", "完整日期与时间", "2026-10-24_22-08-12"},
        {"OPERATORS", "人员数量", "研究的操作员数量", "3"},
        {"REPLICATES", "测量次数", "每名操作员对每件部件的重复测量次数", "3"},
        {"PARTS", "产品件数", "研究的部件数量", "10"},
        {"USL", "上规格限", "上规格限；未设置为 NA", "10.5"},
        {"LSL", "下规格限", "下规格限；未设置为 NA", "9.5"},
        {"CENTER", "数据中心", "中心设定值；留空时取实际数据均值", "10"},
        {"MULTIPLIER", "研究变异倍数", "研究变异倍数", "6"},
        {"SEED", "随机种子", "生成研究的实际种子；导入研究显示 NA", "123"},
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
std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return text;
}
std::string trim_name(const std::string &text) {
    const auto first = text.find_first_not_of(' ');
    return first == text.npos ? "" : text.substr(first, text.find_last_not_of(' ') - first + 1);
}
void validate_component(const std::string &name, std::size_t extension_units = 0) {
    if (name.empty() || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        throw ConfigError("请填写不含路径、非法字符或结尾句点的文件名");
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
    if (units + extension_units > 255)
        throw ConfigError("文件名过长，请缩短名称");
    auto base = ascii_lower(name.substr(0, name.find('.')));
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" ||
        base == "conout$")
        throw ConfigError("文件名不能使用 Windows 保留名称");
    if (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") {
        const auto suffix = base.substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') || suffix == "¹" ||
            suffix == "²" || suffix == "³")
            throw ConfigError("文件名不能使用 Windows 保留名称");
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
std::string without_format_suffix(const std::string &name, ExportBasenameKind kind) {
    auto base = trim_name(name);
    // Strip known format suffixes so repeated saves and switching formats preserve one base.
    for (auto dot = base.rfind('.'); dot != base.npos; dot = base.rfind('.')) {
        const auto ext = ascii_lower(base.substr(dot));
        const bool recognized = ext == ".csv" || ext == ".xlsx" ||
            (kind != ExportBasenameKind::Data && ext == ".pdf") ||
            (kind == ExportBasenameKind::Chart &&
             (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg")) ||
            (kind == ExportBasenameKind::ComparisonReport && (ext == ".html" || ext == ".htm"));
        if (!recognized)
            break;
        base.resize(dot);
    }
    return base;
}
std::string normalize_template(const std::string &name, ExportBasenameKind kind,
                               std::size_t extension_units) {
    if (name.size() > 16384)
        throw ConfigError("文件名模板过长");
    const auto base = without_format_suffix(name, kind);
    validate_component(expand(base, [](const FilenameVariable &) { return "0"; }), extension_units);
    return base;
}
} // namespace
std::string normalize_export_basename(const std::string &name, ExportBasenameKind kind,
                                      const std::string &extension) {
    return normalize_template(name, kind, extension.empty() ? 4 : extension.size());
}
std::string normalize_import_filename(const std::string &name) {
    if (trim_name(name).empty())
        return {};
    return normalize_export_basename(name, ExportBasenameKind::Data);
}
std::string normalize_filename_template(const std::string &name, const std::string &file_type) {
    return normalize_template(name, ExportBasenameKind::Data, file_type.size() + 1);
}
void validate_filename_template(const std::string &name) { (void)normalize_filename_template(name, "csv"); }
Json normalize_output(const Json &output) {
    auto cfg = default_config();
    cfg["output"] = output;
    return normalize_config(cfg)["output"];
}
namespace {
std::string render_context(const std::string &base, const Json &options, const Result *study,
                           StudySource source, std::chrono::system_clock::time_point when,
                           std::size_t extension_units) {
    const Json missing = nullptr;
    const auto option = [&](const char *section, const char *field) -> const Json & {
        const auto group = options.find(section);
        if (group == options.end() || !group->is_object())
            return missing;
        const auto value = group->find(field);
        return value == group->end() ? missing : *value;
    };
    std::optional<Json> metrics;
    const auto measured = [&](const char *key) -> const Json & {
        if (!study)
            return missing;
        if (!metrics) {
            auto config = normalize_config(study->config);
            config["analysis"]["confidence"]["enabled"] = false;
            config["analysis"]["misclassification"] = false;
            metrics = calculate(study->matrix, config);
        }
        return metrics->at(key);
    };
    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(when);
    if (seconds > when)
        seconds -= std::chrono::seconds(1);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time))
        throw ConfigError("无法读取本地时间");
#else
    if (!localtime_r(&time, &local))
        throw ConfigError("无法读取本地时间");
#endif
    const auto date = padded(local.tm_year + 1900, 4) + "-" + padded(local.tm_mon + 1) + "-" +
                      padded(local.tm_mday);
    const auto clock =
        padded(local.tm_hour) + "-" + padded(local.tm_min) + "-" + padded(local.tm_sec);
    auto out = expand(base, [&](const FilenameVariable &v) -> std::string {
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
            return compact(option("design", "operators"));
        if (key == "REPLICATES")
            return compact(option("design", "replicates"));
        if (key == "PARTS")
            return compact(option("design", "parts"));
        if (key == "USL")
            return compact(option("specification", "usl"));
        if (key == "LSL")
            return compact(option("specification", "lsl"));
        if (key == "CENTER") {
            const auto &center = option("generation", "center");
            return compact(center.is_null() ? measured("mean") : center);
        }
        if (key == "MULTIPLIER")
            return compact(option("analysis", "study_multiplier"));
        if (key == "SEED") {
            if (!study)
                return compact(option("generation", "seed"));
            return source == StudySource::Generated ? std::to_string(study->seed) : "NA";
        }
        if (key == "DP")
            return compact(option("generation", "decimal_places"));
        if (key == "VERSION")
            return ui_version;
        const auto *metric = key == "EV_PCT"    ? "EV_pct"
                                   : key == "AV_PCT"  ? "AV_pct"
                                   : key == "GRR_PCT" ? "GRR_pct"
                                   : key == "PT_PCT"  ? "PT_pct"
                                                      : "ndc";
        const auto &value = measured(metric);
        if (value.is_null())
            return "NA";
        return key == "NDC" ? compact(value) : fixed(value.get<double>(), 2);
    });
    validate_component(out, extension_units);
    return out;
}
} // namespace
std::string render_study_basename(const std::string &name, ExportBasenameKind kind,
                                  const Result &study, StudySource source,
                                  std::chrono::system_clock::time_point when,
                                  const std::string &extension) {
    const auto base = normalize_template(name, kind, extension.size());
    return render_context(base, normalize_config(study.config), &study, source, when, extension.size());
}
std::string render_import_basename(const std::string &name, const Json &options,
                                   const Result *active_study, StudySource source,
                                   std::chrono::system_clock::time_point when,
                                   const std::string &extension) {
    if (trim_name(name).empty())
        return {};
    const auto units = extension.empty() ? 4 : extension.size();
    const auto base = normalize_template(name, ExportBasenameKind::Data, units);
    return render_context(base, options, active_study, source, when, units);
}
std::string render_filename(const std::string &name, const Result &result,
                            std::chrono::system_clock::time_point when) {
    // Legacy callers can still pass a suffixed template to obtain a complete name.
    const auto cleaned = trim_name(name);
    const auto base = without_format_suffix(name, ExportBasenameKind::Data);
    const auto suffix = cleaned == base ? std::string{} : cleaned.substr(cleaned.rfind('.'));
    return render_study_basename(name, ExportBasenameKind::Data, result, StudySource::Generated,
                                 when, suffix) + suffix;
}
std::filesystem::path configured_data_path(const Result &result, const Json &raw,
                                          std::chrono::system_clock::time_point when) {
    const auto output = normalize_output(raw);
    const auto extension = "." + output.at("file_type").get<std::string>();
    const auto name = render_study_basename(output.at("filename"), ExportBasenameKind::Data, result,
                                            StudySource::Generated, when, extension) + extension;
    return (resolve_output_directory(output.at("directory").get<std::string>()) /
            std::filesystem::u8path(name))
        .lexically_normal();
}
std::filesystem::path configured_csv_path(const Result &result, const Json &raw,
                                         std::chrono::system_clock::time_point when) {
    return configured_data_path(result, raw, when);
}
std::filesystem::path export_configured_data(const Result &result, const Json &raw,
                                            std::chrono::system_clock::time_point when,
                                            const std::filesystem::path &reserved_report,
                                            const std::filesystem::path &source_config,
                                            const std::filesystem::path &comparison_report,
                                            Json *receipt) {
    const auto output = normalize_output(raw);
    const auto base = configured_data_path(result, output, when);
    const bool number = output["existing_file"] == "number";
    // Expand once. All collision retries retain the same timestamp and result.
    for (int i = 0; i < 1000000; ++i) {
        auto path = i == 0
                        ? base
                        : base.parent_path() /
                              std::filesystem::u8path(base.stem().u8string() + "_" + padded(i, 3) +
                                                      base.extension().u8string());
        validate_component(path.filename().u8string());
        if (!reserved_report.empty() && same_file_path(path, reserved_report))
            throw ConfigError("数据文件与报告路径不能相同");
        if (!comparison_report.empty() && same_file_path(path, comparison_report))
            throw ConfigError("数据文件与对照报告路径不能相同");
        if (!source_config.empty() && same_file_path(path, source_config)) {
            if (number)
                continue;
            throw ConfigError("数据输出路径不能覆盖本次使用的配置文件");
        }
        if (number && std::filesystem::exists(path))
            continue;
        try {
            export_data(result, path, output,
                       number ? ExistingFile::Preserve : ExistingFile::Replace, receipt);
            return path;
        } catch (const FileExistsError &) {
            if (!number)
                throw;
        }
    }
    throw std::runtime_error("同名文件过多，无法分配导出序号，请修改文件名或目录。");
}
std::filesystem::path export_configured_csv(const Result &result, const Json &output,
                                            std::chrono::system_clock::time_point when,
                                            const std::filesystem::path &report,
                                            const std::filesystem::path &config,
                                            const std::filesystem::path &comparison, Json *receipt) {
    return export_configured_data(result, output, when, report, config, comparison, receipt);
}
} // namespace msa
