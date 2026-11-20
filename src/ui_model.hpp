#pragma once
#include "msa/core.hpp"
#include "msa/part_sampling.hpp"
#include <cmath>

namespace native_ui {
// 以逻辑客户区为基准，预留窗口装饰和屏幕边距，不把任务栏计入可用空间。
inline std::pair<int, int> initial_window_size(int workWidth, int workHeight, float scale,
                                               int frameWidth, int frameHeight) {
    const int availableWidth = std::max(1, workWidth - frameWidth - 32);
    const int availableHeight = std::max(1, workHeight - frameHeight - 32);
    return {std::min(int(1280 * scale), availableWidth),
            std::min(int(820 * scale), availableHeight)};
}
inline bool finite_metric(const msa::Json &value) {
    return value.is_number() && std::isfinite(value.get<double>());
}
inline std::string review_number(const msa::Json &value, int dp) {
    if (value.is_null())
        return "不可计算";
    if (!finite_metric(value))
        return "数值异常";
    try {
        return msa::fixed(value.get<double>(), dp);
    } catch (const std::exception &) {
        return "数值异常";
    }
}
inline std::string trim(const std::string &value) {
    auto begin = value.find_first_not_of(" \t\r\n"), end = value.find_last_not_of(" \t\r\n");
    return begin == value.npos ? std::string{} : value.substr(begin, end - begin + 1);
}
struct Target {
    int mode = 0;
    std::string first, upper{}, tolerance{};
};
inline constexpr std::array<const char *, 6> csv_formats{"default", "minitab", "spss",
                                                         "jmp19",   "origin",  "matlab"};
inline constexpr std::array<const char *, 3> run_order_modes{"standard", "all", "within_operator"};
inline constexpr std::array<const char *, 3> interaction_modes{"auto", "include", "exclude"};
struct Draft {
    std::string operators = "3", replicates = "3", parts = "10", lsl, usl, multiplier = "6",
                reference, minimumPartSpanPct;
    std::array<std::string, 3> operatorNames{};
    std::string center, decimals = "4", percentError = "0.1", ndcError = "0", seed,
                        attempts = "5000", timeout = "30", directory, filename = "msa_data";
    int priority = 0, compliance = 0, scale = 0, existingFile = 0, csvFormat = 0, runMode = 0;
    int partSampling = 0;
    bool requireNormality = false, variedResiduals = false;
    bool randomOperators = false, storeStandardOrder = false;
    std::string runSeed;
    bool silentCsv = false, silentCharts = false, silentReport = false;
    int dataFileType = 0, reportFormat = 0;
    std::string importDirectory, importFilename, chartDirectory, chartFilename = msa::default_chart_basename;
    msa::ChartPreferences charts;
    std::string reportDirectory, reportFilename = msa::default_report_basename;
    int analysisMethod = 0, interactionMode = 0;
    std::string interactionAlpha = "0.05";
    std::string analysisTolerance, historicalSd, historicalMean;
    int processVariation = 0;
    bool misclassification = false;
    bool confidenceEnabled = false;
    std::string confidenceLevel = "95";
    int confidenceBound = 0, confidenceMethod = 0;
    std::array<Target, 5> targets;
};
inline msa::Json numeric(const std::string &input, const std::string &label, bool integer = false,
                         bool nullable = false) {
    const auto s = trim(input);
    if (s.empty() && nullable)
        return nullptr;
    try {
        if (integer)
            return msa::parse_integer(s);
        return msa::parse_number(s);
    } catch (const std::exception &) {
        throw msa::ConfigError(label + (integer ? "必须是整数" : "必须是有限数字"));
    }
}
inline msa::Json number_when_enabled(const std::string &input, const std::string &label,
                                     bool enabled, double minimum, double maximum,
                                     const msa::Json &fallback) {
    if (enabled)
        return numeric(input, label, false, fallback.is_null());
    // A hidden field may contain an incomplete edit. Keep a valid preference.
    try {
        const double value = msa::parse_number(trim(input));
        if (minimum <= value && value <= maximum)
            return value;
    } catch (const msa::ConfigError &) {
    }
    return fallback;
}
// Naming an import is possible before the file is opened or a complete study is configured.
// Read only the individual parameters available to filename variables.
inline msa::Json filename_options(const Draft &d) {
    const auto parameter = [](const std::string &text, bool integer, double low, double high) -> msa::Json {
        try {
            auto value = numeric(text, "", integer, true);
            if (value.is_number() && value.get<double>() >= low && value.get<double>() <= high)
                return value;
        } catch (const msa::ConfigError &) {
            // An unfinished parameter has no value to suggest in the import dialog.
        }
        return nullptr;
    };
    return {{"design", {{"operators", parameter(d.operators, true, 2, 3)},
                        {"replicates", parameter(d.replicates, true, 2, 3)},
                        {"parts", parameter(d.parts, true, 5, 10)}}},
            {"specification", {{"lsl", parameter(d.lsl, false, -1e12, 1e12)},
                               {"usl", parameter(d.usl, false, -1e12, 1e12)}}},
            {"generation", {{"center", parameter(d.center, false, -1e12, 1e12)},
                            {"decimal_places", parameter(d.decimals, true, 0, 10)},
                            {"seed", parameter(d.seed, true, -INFINITY, INFINITY)}}},
            {"analysis", {{"study_multiplier", parameter(d.multiplier, false, 1e-6, 100)}}}};
}
inline msa::Json data_output_fields(const Draft &d, bool saveInactiveSeed) {
    if (d.dataFileType < 0 || d.dataFileType > 1)
        throw msa::ConfigError("数据文件类型选择无效");
    if (d.csvFormat < 0 || d.csvFormat >= int(csv_formats.size()) || d.runMode < 0 ||
        d.runMode >= int(run_order_modes.size()))
        throw msa::ConfigError("数据布局或运行顺序选择无效");
    msa::Json seed = nullptr;
    if (d.csvFormat != 0 && d.runMode != 0) {
        seed = numeric(d.runSeed, "运行顺序随机数种子", true, true);
    } else if (saveInactiveSeed) {
        // 保留有效的禁用项设置，未完成的草稿不影响保存其他参数。
        try {
            const auto stored = numeric(d.runSeed, "运行顺序随机数种子", true, true);
            if (!stored.is_null() && stored.get<std::int64_t>() > 0)
                seed = stored;
        } catch (const msa::ConfigError &) {
            seed = nullptr;
        }
    }
    return {{"directory", trim(d.directory)},
            {"filename", trim(d.filename)},
            {"file_type", d.dataFileType == 1 ? "xlsx" : "csv"},
            {"existing_file", d.existingFile ? "overwrite" : "number"},
            {"format", csv_formats[d.csvFormat]},
            {"silent", {{"csv", d.silentCsv}, {"charts", d.silentCharts},
                        {"comparison_report", d.silentReport}}},
            {"run_order", {{"mode", run_order_modes[d.runMode]},
                           {"randomize_operators", d.randomOperators},
                           {"seed", seed}, {"store_standard_order", d.storeStandardOrder}}}};
}
inline msa::Json data_output_config(const Draft &d) {
    return msa::normalize_output(data_output_fields(d, false));
}
inline msa::Json csv_output_config(const Draft &d) { return data_output_config(d); }
inline msa::Json output_config(const Draft &d, bool saveInactiveSeed = false) {
    if (d.reportFormat < 0 || d.reportFormat > 1)
        throw msa::ConfigError("对照报告格式选择无效");
    auto output = data_output_fields(d, saveInactiveSeed);
    output["charts"] = {{"directory", trim(d.chartDirectory)}, {"filename", d.chartFilename}};
    d.charts.write_to(output["charts"]);
    output["comparison_report"] = {{"format", d.reportFormat == 1 ? "html" : "pdf"},
                                   {"directory", trim(d.reportDirectory)},
                                   {"filename", d.reportFilename}};
    return msa::normalize_output(output);
}
enum class ConfigUse { Full, Generation, Analysis, DataImport };
inline msa::Json to_config(const Draft &d, ConfigUse use = ConfigUse::Full) {
    auto c = msa::default_config();
    if (use != ConfigUse::DataImport) {
        c["design"] = {{"operators", numeric(d.operators, "操作员数", true)},
                       {"operator_names", d.operatorNames},
                       {"replicates", numeric(d.replicates, "重复测量次数", true)},
                       {"parts", numeric(d.parts, "部件数", true)}};
    }
    c["analysis"]["study_multiplier"] = numeric(d.multiplier, "研究变异倍数");
    if (d.analysisMethod < 0 || d.analysisMethod > 1 || d.interactionMode < 0 ||
        d.interactionMode > 2)
        throw msa::ConfigError("分析方法或交互项选择无效");
    c["analysis"]["method"] = d.analysisMethod ? "crossed_anova" : "crossed_xbarr";
    c["analysis"]["interaction"] = interaction_modes[d.interactionMode];
    const bool anova = d.analysisMethod == 1;
    c["analysis"]["interaction_alpha"] =
        number_when_enabled(d.interactionAlpha, "交互项显著性水平", anova && d.interactionMode == 0,
                            std::nextafter(0., 1.), std::nextafter(1., 0.),
                            c["analysis"]["interaction_alpha"]);
    c["analysis"]["tolerance"] = numeric(d.analysisTolerance, "分析容差", false, true);
    c["analysis"]["historical_sd"] = numeric(d.historicalSd, "历史标准差", false, true);
    c["analysis"]["historical_mean"] = numeric(d.historicalMean, "历史均值", false, true);
    c["analysis"]["process_variation"] = d.processVariation ? "study" : "historical";
    c["analysis"]["misclassification"] = d.misclassification;
    if (d.confidenceBound < 0 || d.confidenceBound > 2 || d.confidenceMethod < 0 ||
        d.confidenceMethod > 1)
        throw msa::ConfigError("置信区间类型或算法选择无效");
    c["analysis"]["confidence"] = {{"enabled", d.confidenceEnabled},
                                   {"level", number_when_enabled(d.confidenceLevel, "置信水平",
                                                                 anova && d.confidenceEnabled,
                                                                 .01, 99.99,
                                                                 c["analysis"]["confidence"]["level"])},
                                   {"bound", d.confidenceBound == 1   ? "lower"
                                             : d.confidenceBound == 2 ? "upper"
                                                                      : "both"},
                                   {"method", d.confidenceMethod ? "satterthwaite" : "mls"}};
    c["specification"] = {{"lsl", numeric(d.lsl, "LSL", false, true)},
                          {"usl", numeric(d.usl, "USL", false, true)},
                          {"compliance", d.compliance ? "relaxed" : "strict"}};
    if (use == ConfigUse::DataImport)
        return c; // 完整分析校验使用实际导入的设计（包括 MLS 置信限所需的操作员人数）。
    if (use == ConfigUse::Analysis)
        return msa::normalize_config(c);
    if (d.partSampling < 0 || d.partSampling >= int(msa::part_sampling_methods.size()))
        throw msa::ConfigError("部件取样方案无效");
    const bool needsReference =
        d.scale == 1 || c["specification"]["lsl"].is_null() || c["specification"]["usl"].is_null();
    c["generation"] = {{"center", numeric(d.center, "中心", false, true)},
                       {"priority", d.priority ? "indicators" : "center"},
                       {"require_normality", d.requireNormality},
                       {"residual_model", anova && d.variedResiduals ? "varied" : "standard"},
                       {"part_sampling", msa::part_sampling_methods[d.partSampling].key},
                       {"minimum_part_span_pct",
                        numeric(d.minimumPartSpanPct, "最低部件跨度", false, true)},
                       {"scale_mode", d.scale ? "reference_tv_sd" : "auto"},
                       {"reference_tv_sd", number_when_enabled(d.reference, "参考 TV_SD",
                                                             needsReference, 1e-12, 1e12, nullptr)},
                       {"decimal_places", numeric(d.decimals, "小数位", true)},
                       {"seed", numeric(d.seed, "随机种子", true, true)},
                       {"max_attempts", numeric(d.attempts, "候选次数上限", true)},
                       {"timeout_seconds", numeric(d.timeout, "搜索时限")}};
    c["target_defaults"] = {{"percentage_tolerance_pp", numeric(d.percentError, "默认百分比误差")},
                            {"ndc_tolerance", numeric(d.ndcError, "默认 ndc 误差", true)}};
    if (use == ConfigUse::Full) {
        c["output"] = output_config(d, true);
        c["input"]["directory"] = trim(d.importDirectory);
        c["input"]["filename"] = d.importFilename;
    }
    const char *modes[]{"", "target", "between", "at_most", "at_least"};
    for (int i = 0; i < 5; ++i) {
        const auto &t = d.targets[i];
        if (!t.mode)
            continue;
        if (t.mode < 0 || t.mode > 4)
            throw msa::ConfigError("约束模式无效");
        auto &out = c["targets"][msa::metric_keys[i]];
        out = {{"mode", modes[t.mode]}};
        out[t.mode == 2 ? "min" : "value"] = numeric(t.first, msa::metric_keys[i], i == 4);
        if (t.mode == 2)
            out["max"] = numeric(t.upper, msa::metric_keys[i], i == 4);
        else if (t.mode == 1 && !trim(t.tolerance).empty())
            out[i == 4 ? "tolerance" : "tolerance_pp"] = numeric(t.tolerance, "目标允许误差", i == 4);
    }
    return msa::normalize_config(c);
}
inline Draft from_config(const msa::Json &raw) {
    auto c = msa::normalize_config(raw);
    Draft d;
    auto text = [](const msa::Json &v) { return v.is_null() ? std::string{} : v.dump(); };
    d.operators = text(c["design"]["operators"]);
    d.operatorNames = c["design"]["operator_names"].get<decltype(d.operatorNames)>();
    d.replicates = text(c["design"]["replicates"]);
    d.parts = text(c["design"]["parts"]);
    d.lsl = text(c["specification"]["lsl"]);
    d.usl = text(c["specification"]["usl"]);
    d.multiplier = text(c["analysis"]["study_multiplier"]);
    d.analysisMethod = c["analysis"]["method"] == "crossed_anova";
    d.interactionAlpha = text(c["analysis"]["interaction_alpha"]);
    d.analysisTolerance = text(c["analysis"]["tolerance"]);
    d.historicalSd = text(c["analysis"]["historical_sd"]);
    d.historicalMean = text(c["analysis"]["historical_mean"]);
    d.processVariation = c["analysis"]["process_variation"] == "study";
    d.misclassification = c["analysis"]["misclassification"];
    d.confidenceEnabled = c["analysis"]["confidence"]["enabled"];
    d.confidenceLevel = text(c["analysis"]["confidence"]["level"]);
    d.confidenceMethod = c["analysis"]["confidence"]["method"] == "satterthwaite";
    d.confidenceBound = c["analysis"]["confidence"]["bound"] == "lower"   ? 1
                        : c["analysis"]["confidence"]["bound"] == "upper" ? 2
                                                                          : 0;
    for (int i = 0; i < int(interaction_modes.size()); ++i)
        if (c["analysis"]["interaction"] == interaction_modes[i])
            d.interactionMode = i;
    d.compliance = c["specification"]["compliance"] == "relaxed";
    auto &g = c["generation"];
    d.center = text(g["center"]);
    d.reference = text(g["reference_tv_sd"]);
    d.decimals = text(g["decimal_places"]);
    d.seed = text(g["seed"]);
    d.attempts = text(g["max_attempts"]);
    d.timeout = text(g["timeout_seconds"]);
    d.priority = g["priority"] == "indicators";
    d.requireNormality = g["require_normality"].get<bool>();
    d.variedResiduals = g["residual_model"] == "varied";
    for (int i = 0; i < int(msa::part_sampling_methods.size()); ++i)
        if (g["part_sampling"] == msa::part_sampling_methods[i].key)
            d.partSampling = i;
    d.minimumPartSpanPct = text(g["minimum_part_span_pct"]);
    d.scale = g["scale_mode"] == "reference_tv_sd";
    d.percentError = text(c["target_defaults"]["percentage_tolerance_pp"]);
    d.ndcError = text(c["target_defaults"]["ndc_tolerance"]);
    d.directory = c["output"]["directory"];
    d.importDirectory = c["input"]["directory"];
    d.importFilename = c["input"]["filename"];
    d.chartDirectory = c["output"]["charts"]["directory"];
    d.chartFilename = c["output"]["charts"]["filename"];
    d.charts = msa::ChartPreferences::read(c["output"]["charts"]);
    d.filename = c["output"]["filename"];
    d.dataFileType = c["output"]["file_type"] == "xlsx";
    const auto &silent = c["output"]["silent"];
    d.silentCsv = silent["csv"];
    d.silentCharts = silent["charts"];
    d.silentReport = silent["comparison_report"];
    const auto &report = c["output"]["comparison_report"];
    d.reportFormat = report["format"] == "html";
    d.reportDirectory = report["directory"];
    d.reportFilename = report["filename"];
    d.existingFile = c["output"]["existing_file"] == "overwrite";
    for (int i = 0; i < int(csv_formats.size()); ++i)
        if (c["output"]["format"] == csv_formats[i])
            d.csvFormat = i;
    const auto &order = c["output"]["run_order"];
    for (int i = 0; i < int(run_order_modes.size()); ++i)
        if (order["mode"] == run_order_modes[i])
            d.runMode = i;
    d.randomOperators = order["randomize_operators"];
    d.storeStandardOrder = order["store_standard_order"];
    d.runSeed = text(order["seed"]);
    for (int i = 0; i < 5; ++i) {
        auto &t = c["targets"][msa::metric_keys[i]];
        if (t.is_null())
            continue;
        std::string mode = t["mode"];
        auto &target = d.targets[i];
        target.mode = mode == "target" ? 1 : mode == "between" ? 2 : mode == "at_most" ? 3 : 4;
        target.first = text(t[target.mode == 2 ? "min" : "value"]);
        if (target.mode == 2)
            target.upper = text(t["max"]);
        else if (target.mode == 1 && t.contains(i == 4 ? "tolerance" : "tolerance_pp"))
            target.tolerance = text(t[i == 4 ? "tolerance" : "tolerance_pp"]);
    }
    return d;
}
} // namespace native_ui
