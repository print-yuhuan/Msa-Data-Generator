#include "core.hpp"
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
    return {{"schema_version", 1},
            {"design", {{"operators", 3}, {"replicates", 3}, {"parts", 10}}},
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
              {"scale_mode", "auto"},
              {"reference_tv_sd", nullptr},
              {"decimal_places", 4},
              {"seed", nullptr},
              {"max_attempts", 5000},
              {"timeout_seconds", 30}}},
            {"target_defaults", {{"percentage_tolerance_pp", 0.1}, {"ndc_tolerance", 0}}},
            {"targets", targets},
            {"output",
             {{"directory", ""},
              {"filename", "msa_data.csv"},
              {"encoding", "utf-8-sig"},
              {"existing_file", "number"},
              {"format", "default"},
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
Json normalize_config(const Json &raw) {
    auto c = merge(default_config(), raw);
    if (!c["schema_version"].is_number_integer() || c["schema_version"] != 1)
        throw ConfigError("不支持的配置版本");
    auto &d = c["design"], &a = c["analysis"], &s = c["specification"], &g = c["generation"],
         &t = c["target_defaults"], &out = c["output"];
    number(d["operators"], "人员数", 2, 3, true);
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
    number(g["center"], "数据中心", -1e12, 1e12, false, true);
    number(g["reference_tv_sd"], "参考 TV_SD", 1e-12, 1e12, false, true);
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
    if (!out["directory"].is_string())
        throw ConfigError("输出目录必须是字符串");
    if (!out["filename"].is_string())
        throw ConfigError("输出文件名须为 .csv 文件名");
    validate_filename_template(out["filename"].get<std::string>());
    if (!one_of(out["existing_file"], {"number", "overwrite"}))
        throw ConfigError("同名文件策略须为 number/overwrite");
    if (!one_of(out["format"], {"default", "minitab", "spss", "jmp19", "origin", "matlab"}))
        throw ConfigError("CSV 导出格式无效");
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
    if (s["compliance"] == "strict" && s["lsl"].is_null() && s["usl"].is_null())
        throw ConfigError("严格模式至少需要一个规格限；也可切换宽松模式。");
    const auto &a = c["analysis"];
    if (!c["targets"]["PT_pct"].is_null() && s["lsl"].is_null() && s["usl"].is_null() &&
        a["tolerance"].is_null())
        throw ConfigError("P/T 约束需要规格限或有效的分析容差。");
    if ((g["scale_mode"] == "reference_tv_sd" || !both) && g["reference_tv_sd"].is_null() &&
        a["historical_sd"].is_null())
        throw ConfigError("当前缺少波动尺度，请在选项页填写参考 TV_SD。中心可继续留空。");
    ratios(c);
    if (s["compliance"] == "strict" && both &&
        grid_bound(s["lsl"], g["decimal_places"], true) >=
            grid_bound(s["usl"], g["decimal_places"], false))
        throw InfeasibleError(
            "规格范围内不足两个不同的输出精度数值，无法形成非零 TV；请增加小数位。");
    return c;
}
} // namespace msa
