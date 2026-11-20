#include "core.hpp"
#include "part_sampling.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace msa {
Json default_config() {
    Json targets = Json::object();
    for (auto key : metric_keys)
        targets[key] = nullptr;
    return {{"schema_version", 2},
            {"input", {{"directory", ""}, {"filename", ""}}},
            {"design", {{"operators", 3}, {"replicates", 3}, {"parts", 10},
                        {"operator_names", {"", "", ""}}}},
            {"analysis",
             {{"method", "crossed_xbarr"},
              {"percentage_basis", "sd_over_tv"},
              {"study_multiplier", 6},
              {"interaction", "auto"},
              {"interaction_alpha", 0.05},
              {"tolerance", nullptr},
              {"historical_sd", nullptr},
              {"historical_mean", nullptr},
              {"process_variation", "historical"},
              {"misclassification", false},
              {"confidence", {{"enabled", false}, {"level", 95}, {"bound", "both"},
                              {"method", "mls"}}}}},
            {"specification", {{"lsl", nullptr}, {"usl", nullptr}, {"compliance", "strict"}}},
            {"generation",
             {{"center", nullptr},
              {"priority", "center"},
              {"require_normality", false},
              {"part_sampling", "default"},
              {"residual_model", "standard"},
              {"scale_mode", "auto"},
              {"reference_tv_sd", nullptr},
              {"minimum_part_span_pct", nullptr},
              {"decimal_places", 4},
              {"seed", nullptr},
              {"max_attempts", 5000},
              {"timeout_seconds", 30}}},
            {"target_defaults", {{"percentage_tolerance_pp", 0.1}, {"ndc_tolerance", 0}}},
            {"targets", targets},
            {"output",
             {{"directory", ""},
              {"filename", "msa_data"},
              {"file_type", "csv"},
              {"encoding", "utf-8-sig"},
              {"existing_file", "number"},
              {"format", "default"},
              {"silent", {{"csv", false}, {"charts", false}, {"comparison_report", false}}},
              {"charts", {{"directory", ""}, {"filename", default_chart_basename}}},
              {"comparison_report", {{"format", "pdf"}, {"directory", ""},
                                     {"filename", default_report_basename}}},
              {"run_order",
               {{"mode", "standard"},
                {"randomize_operators", false},
                {"seed", nullptr},
                {"store_standard_order", false}}}}}};
}
namespace {
Json merge(const Json &def, const Json &raw, const std::string &prefix = "") {
    if (!raw.is_object())
        throw ConfigError(prefix + "必须是 JSON 对象");
    Json out = def;
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        if (!def.contains(it.key()))
            throw ConfigError("未知配置字段：" + prefix + it.key());
        out[it.key()] = def[it.key()].is_object()
                            ? merge(def[it.key()], it.value(), prefix + it.key() + ".")
                            : it.value();
    }
    return out;
}
void number(const Json &value, const std::string &name, double lo, double hi, bool integer = false,
            bool nullable = false) {
    if (value.is_null() && nullable)
        return;
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw ConfigError(name + "必须是有限数字");
    if (integer && !value.is_number_integer())
        throw ConfigError(name + "必须是整数");
    if (value.get<double>() < lo || value.get<double>() > hi)
        throw ConfigError(name + "超出允许范围");
}
bool one_of(const Json &value, std::initializer_list<const char *> values) {
    for (auto v : values)
        if (value == v)
            return true;
    return false;
}
} // namespace
std::array<std::string, 3> operator_names(const Json &design) {
    std::array<std::string, 3> names{"A", "B", "C"};
    if (design.contains("operator_names")) {
        const auto &configured = design.at("operator_names");
        if (!configured.is_array() || configured.size() != names.size())
            throw ConfigError("操作员名称须包含三个文本项，留空时使用 A/B/C");
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (!configured[i].is_string())
                throw ConfigError("操作员名称必须是文本");
            const auto &name = configured[i].get_ref<const std::string &>();
            const auto first = name.find_first_not_of(" \t");
            if (first != std::string::npos)
                names[i] = name.substr(first, name.find_last_not_of(" \t") - first + 1);
        }
    }
    return names;
}
Json normalize_config(const Json &raw) {
    auto c = merge(default_config(), raw);
    if (!c["schema_version"].is_number_integer() ||
        (c["schema_version"] != 1 && c["schema_version"] != 2))
        throw ConfigError("不支持的配置版本");
    if (c["schema_version"] == 1) {
        // Only measurement export used template syntax in version 1. Preserve
        // literal brackets in the other names, including text that resembles a token.
        for (Json *field : {&c["input"]["filename"], &c["output"]["charts"]["filename"],
                            &c["output"]["comparison_report"]["filename"]}) {
            if (!field->is_string())
                continue;
            std::string escaped;
            for (char ch : field->get_ref<const std::string &>()) {
                if (ch == '[' || ch == ']')
                    escaped += ch;
                escaped += ch;
            }
            *field = std::move(escaped);
        }
        c["schema_version"] = 2;
    }
    auto &d = c["design"], &a = c["analysis"], &s = c["specification"], &g = c["generation"],
         &t = c["target_defaults"], &out = c["output"];
    number(d["operators"], "人员数", 2, 3, true);
    const auto names = operator_names(d);
    std::set<std::string> active_names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto &name = names[i];
        if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }) ||
            name.find("\xef\xbf\xbe") != name.npos || name.find("\xef\xbf\xbf") != name.npos)
            throw ConfigError("操作员名称不能包含换行或其他控制字符");
        if (i < d["operators"].get<std::size_t>() && !active_names.insert(name).second)
            throw ConfigError("启用的操作员名称不能重复：" + name);
        auto &stored = d["operator_names"][i];
        if (stored.get_ref<const std::string &>().find_first_not_of(" \t") == std::string::npos)
            stored = "";
        else
            stored = name;
    }
    number(d["replicates"], "测量次数", 2, 3, true);
    number(d["parts"], "产品数", 5, 10, true);
    if (!one_of(a["method"], {"crossed_xbarr", "crossed_anova"}) ||
        a["percentage_basis"] != "sd_over_tv")
        throw ConfigError("分析方法须为 crossed_xbarr/crossed_anova，百分比口径为 SD/TV");
    if (!one_of(a["interaction"], {"auto", "include", "exclude"}))
        throw ConfigError("ANOVA 交互项须为 auto/include/exclude");
    number(a["interaction_alpha"], "移除交互项显著性水平", 0, 1);
    if (a["interaction_alpha"] == 0 || a["interaction_alpha"] == 1)
        throw ConfigError("移除交互项显著性水平须大于 0 且小于 1");
    number(a["study_multiplier"], "标准差倍数", .000001, 100);
    number(a["tolerance"], "分析容差", 1e-12, 2e12, false, true);
    number(a["historical_sd"], "历史标准差", 1e-12, 1e12, false, true);
    number(a["historical_mean"], "历史均值", -1e12, 1e12, false, true);
    if (!one_of(a["process_variation"], {"historical", "study"}))
        throw ConfigError("过程变异估计来源须为 historical/study");
    if (!a["misclassification"].is_boolean())
        throw ConfigError("误分类概率开关必须为布尔值");
    if (!a["confidence"]["enabled"].is_boolean())
        throw ConfigError("置信区间开关必须为布尔值");
    number(a["confidence"]["level"], "置信水平", 0.01, 99.99);
    if (!one_of(a["confidence"]["bound"], {"both", "lower", "upper"}))
        throw ConfigError("置信区间类型须为 both/lower/upper");
    if (!one_of(a["confidence"]["method"], {"mls", "satterthwaite"}))
        throw ConfigError("置信区间算法须为 mls/satterthwaite");
    if (a["method"] == "crossed_anova" && a["confidence"]["enabled"].get<bool>())
        validate_mls_confidence(d["operators"].get<int>(), a["confidence"]);
    for (auto k : {"lsl", "usl"})
        number(s[k], k, -1e12, 1e12, false, true);
    if (!s["lsl"].is_null() && !s["usl"].is_null() &&
        s["usl"].get<double>() <= s["lsl"].get<double>())
        throw ConfigError("上规格限必须大于下规格限");
    if (!one_of(s["compliance"], {"strict", "relaxed"}))
        throw ConfigError("规格规则只能为 strict/relaxed");
    if (!one_of(g["priority"], {"center", "indicators"}) ||
        !one_of(g["scale_mode"], {"auto", "reference_tv_sd"}))
        throw ConfigError("生成规则或尺度模式无效");
    if (!g["require_normality"].is_boolean())
        throw ConfigError("要求近似正态必须为布尔值");
    if (!g["part_sampling"].is_string() ||
        !find_part_sampling(g["part_sampling"].get_ref<const std::string &>()))
        throw ConfigError("部件取样方案无效");
    if (!one_of(g["residual_model"], {"standard", "varied"}))
        throw ConfigError("ANOVA 子组残差模式须为 standard 或 varied");
    number(g["center"], "数据中心", -1e12, 1e12, false, true);
    number(g["reference_tv_sd"], "参考 TV_SD", 1e-12, 1e12, false, true);
    number(g["minimum_part_span_pct"], "最低部件跨度（规格宽度的百分比）", 0, 100, false, true);
    if (g["minimum_part_span_pct"] == 0)
        throw ConfigError("最低部件跨度须大于 0%");
    number(g["decimal_places"], "小数位", 0, 10, true);
    number(g["seed"], "随机种子", -9223372036854775808.0, 9223372036854775808.0, true, true);
    if (g["seed"].is_number_unsigned() && g["seed"].get<std::uint64_t>() > std::uint64_t(INT64_MAX))
        throw ConfigError("随机种子超出 int64 范围");
    number(g["max_attempts"], "搜索次数", 1, 1000000, true);
    number(g["timeout_seconds"], "搜索秒数", .01, 3600);
    number(t["percentage_tolerance_pp"], "百分比误差（百分点）", 0, 1000);
    number(t["ndc_tolerance"], "ndc 误差", 0, 1000000, true);
    for (auto k : metric_keys) {
        const auto &target = c["targets"][k];
        if (target.is_null())
            continue;
        if (!target.is_object())
            throw ConfigError(std::string(k) + "约束必须是对象或 null");
        if (!target.contains("mode") ||
            !one_of(target["mode"], {"target", "between", "at_most", "at_least"}))
            throw ConfigError(std::string(k) + "约束模式无效");
        std::string mode = target["mode"],
                    tol = std::string(k) == "ndc" ? "tolerance" : "tolerance_pp";
        std::set<std::string> fields = mode == "between"
                                           ? std::set<std::string>{"mode", "min", "max"}
                                           : std::set<std::string>{"mode", "value"};
        if (mode == "target")
            fields.insert(tol);
        for (auto it = target.begin(); it != target.end(); ++it)
            if (!fields.count(it.key()))
                throw ConfigError(std::string(k) + "存在未知或不适用的约束字段");
        bool nd = std::string(k) == "ndc";
        for (auto key : mode == "between" ? std::vector<std::string>{"min", "max"}
                                          : std::vector<std::string>{"value"}) {
            if (!target.contains(key))
                throw ConfigError(std::string(k) + "缺少 " + key);
            number(target[key], std::string(k) + key, nd ? 1 : 0,
                   nd || std::string(k) == "PT_pct" ? 1000000 : 100, nd);
        }
        if (mode == "between" && target["min"].get<double>() > target["max"].get<double>())
            throw ConfigError("区间下限不可大于上限");
        if (target.contains(tol))
            number(target[tol], std::string(k) + "允许误差", 0, 1000000, nd);
    }
    const auto check_directory = [](const Json &directory, const char *label) {
        if (!directory.is_string() ||
            directory.get_ref<const std::string &>().find('\0') != std::string::npos)
            throw ConfigError(std::string(label) + "须为有效的路径字符串");
    };
    check_directory(c["input"]["directory"], "测量数据导入目录");
    check_directory(out["directory"], "测量数据导出目录");
    check_directory(out["charts"]["directory"], "图表导出目录");
    check_directory(out["comparison_report"]["directory"], "方法对比报告导出目录");
    const auto normalize_name = [](Json &name, const char *label, const auto &normalize) {
        if (!name.is_string())
            throw ConfigError(std::string(label) + "须为字符串");
        try {
            name = normalize(name.get_ref<const std::string &>());
        } catch (const ConfigError &e) {
            throw ConfigError(std::string(label) + "：" + e.what());
        }
    };
    normalize_name(c["input"]["filename"], "测量数据导入名称", normalize_import_filename);
    normalize_name(out["charts"]["filename"], "图表导出名称", [](const auto &name) {
        return normalize_export_basename(name, ExportBasenameKind::Chart, ".png");
    });
    normalize_name(out["comparison_report"]["filename"], "方法对比报告导出名称", [&](const auto &name) {
        const auto extension = out["comparison_report"]["format"] == "html" ? ".html" : ".pdf";
        return normalize_export_basename(name, ExportBasenameKind::ComparisonReport, extension);
    });
    for (const char *key : {"csv", "charts", "comparison_report"})
        if (!out["silent"][key].is_boolean())
            throw ConfigError(std::string("静默导出开关须为布尔值：output.silent.") + key);
    const auto &report = out["comparison_report"];
    if (!one_of(report["format"], {"html", "pdf"}))
        throw ConfigError("对照报告格式须为 html 或 pdf");
    if (!one_of(out["file_type"], {"csv", "xlsx"}))
        throw ConfigError("数据文件类型须为 csv 或 xlsx");
    normalize_name(out["filename"], "测量数据导出名称", [&](const auto &name) {
        return normalize_filename_template(name, out["file_type"].get_ref<const std::string &>());
    });
    if (!one_of(out["existing_file"], {"number", "overwrite"}))
        throw ConfigError("同名文件策略须为 number/overwrite");
    if (!one_of(out["format"], {"default", "minitab", "spss", "jmp19", "origin", "matlab"}))
        throw ConfigError("数据行列布局无效");
    if (!one_of(out["encoding"], {"utf-8-sig", "utf-8"}))
        throw ConfigError("CSV 仅支持 UTF-8 编码");
    out["encoding"] = out["format"] == "default" ? "utf-8-sig" : "utf-8";
    auto &order = out["run_order"];
    if (!one_of(order["mode"], {"standard", "all", "within_operator"}))
        throw ConfigError("运行顺序模式无效");
    for (auto key : {"randomize_operators", "store_standard_order"})
        if (!order[key].is_boolean())
            throw ConfigError("运行顺序复选配置必须为布尔值");
    const auto &seed = order["seed"];
    if (!seed.is_null() &&
        (!seed.is_number_integer() ||
         (seed.is_number_unsigned() && seed.get<std::uint64_t>() > std::uint64_t(INT64_MAX)) ||
         seed.get<std::int64_t>() <= 0))
        throw ConfigError("运行顺序随机数种子必须为正整数（1～9223372036854775807）或留空");
    return c;
}
Bounds acceptance(const Json &c, const std::string &key) {
    const auto &t = c["targets"][key];
    if (t.is_null())
        return {};
    bool nd = key == "ndc";
    double lo = nd ? 1 : 0, hi = (nd || key == "PT_pct") ? INFINITY : 100;
    std::string mode = t["mode"];
    if (mode == "target") {
        double tol = nd ? t.value("tolerance", c["target_defaults"]["ndc_tolerance"].get<double>())
                        : t.value("tolerance_pp",
                                  c["target_defaults"]["percentage_tolerance_pp"].get<double>());
        double v = t["value"];
        return {{std::max(lo, v - tol), std::min(hi, v + tol)}};
    }
    if (mode == "between")
        return {{t["min"].get<double>(), t["max"].get<double>()}};
    if (mode == "at_most")
        return {{lo, t["value"].get<double>()}};
    return {{t["value"].get<double>(), hi}};
}
std::array<double, 6> ratios(const Json &c) {
    auto e = acceptance(c, "EV_pct").value_or(std::make_pair(0., 100.));
    auto a = acceptance(c, "AV_pct").value_or(std::make_pair(0., 100.));
    auto g = acceptance(c, "GRR_pct").value_or(std::make_pair(0., 100.));
    double el = e.first / 100, eh = e.second / 100, al = a.first / 100, ah = a.second / 100,
           gl = g.first / 100, gh = g.second / 100;
    auto n = acceptance(c, "ndc"), pt = acceptance(c, "PT_pct");
    if (n) {
        if (std::isfinite(n->second))
            gl = std::max(gl, std::nextafter(1.41 / std::hypot(1.41, n->second + 1), INFINITY));
        if (n->first > 1)
            gh = std::min(gh, 1.41 / std::hypot(1.41, n->first));
    }
    if (pt && pt->second == 0)
        gh = 0;
    if (pt && pt->first > 0)
        gl = std::max(gl, 1e-15);
    gl = std::max(gl, std::hypot(el, al));
    gh = std::min({gh, std::hypot(eh, ah), 1.});
    if (n && gh == 0)
        throw InfeasibleError("当前约束要求量具 R&R 为零，此时 ndc 无法计算。请调整指标约束。");
    if (gl > gh || el > eh || al > ah)
        throw InfeasibleError(
            "EV%、AV%、GRR%、P/T 或 ndc 的允许范围没有共同解；请调整目标或允许误差。");
    return {el, eh, al, ah, gl, gh};
}
Json validate_request(const Json &raw) {
    auto c = normalize_config(raw);
    auto &s = c["specification"];
    auto &g = c["generation"];
    bool both = !s["lsl"].is_null() && !s["usl"].is_null();
    if (g["part_sampling"] == "uniform_random" && !both)
        throw ConfigError("随机取样需要同时填写下规格限和上规格限。");
    if (!g["minimum_part_span_pct"].is_null() && !both)
        throw ConfigError("设置最低部件跨度时，请填写下规格限和上规格限。也可将最低部件跨度留空。");
    if (s["compliance"] == "strict" && s["lsl"].is_null() && s["usl"].is_null())
        throw ConfigError("严格模式至少需要一个规格限；也可切换宽松模式。");
    const auto &a = c["analysis"];
    if (!c["targets"]["PT_pct"].is_null() && a["tolerance"].is_null()) {
        if (s["lsl"].is_null() && s["usl"].is_null())
            throw ConfigError("P/T 约束需要规格限或有效的分析容差。");
        if (!both && !a["historical_mean"].is_null() &&
            a["historical_mean"] == (s["lsl"].is_null() ? s["usl"] : s["lsl"]))
            throw InfeasibleError("历史均值等于单侧规格限，P/T 无法计算。请设置分析容差、修改历史均值或清除 P/T 约束。");
    }
    if ((g["scale_mode"] == "reference_tv_sd" || !both) && g["reference_tv_sd"].is_null() &&
        a["historical_sd"].is_null())
        throw ConfigError("当前缺少波动尺度，请在选项页填写参考 TV_SD。中心可继续留空。");
    ratios(c);
    if (s["compliance"] == "strict" && both) {
        const double lsl = s["lsl"], usl = s["usl"];
        const int dp = g["decimal_places"];
        const double grid_low = grid_bound(lsl, dp, true), grid_high = grid_bound(usl, dp, false);
        if (grid_low >= grid_high)
            throw InfeasibleError(
                "规格范围内不足两个不同的输出精度数值，无法形成非零 TV；请增加小数位。");
        if (!g["minimum_part_span_pct"].is_null()) {
            const double required_span = (usl - lsl) * (g["minimum_part_span_pct"].get<double>() / 100);
            const double grid_span = grid_high - grid_low;
            if (g["minimum_part_span_pct"] == 100 && (grid_low != lsl || grid_high != usl))
                throw InfeasibleError("严格模式下 100% 部件跨度要求两个规格限都可以按输出精度表示。");
            // Leave headroom for decimal conversion and subtraction at the bounds.
            const double slack = 8 * std::numeric_limits<double>::epsilon() *
                                 std::max(std::abs(lsl), std::abs(usl));
            if (required_span - grid_span > slack)
                throw InfeasibleError(
                    "最低部件跨度超过规格内按当前小数位可表示的范围。请降低最低部件跨度或增加小数位。");
        }
    }
    return c;
}
} // namespace msa
