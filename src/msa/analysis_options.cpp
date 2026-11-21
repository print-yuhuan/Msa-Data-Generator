#include "analysis_detail.hpp"
#include <algorithm>
#include <cmath>

namespace msa {
namespace {
// 中心区间使用 erf，避免两个接近 1 的值相减。同侧远尾使用 erfc 保留小概率。
double normal_interval(double lo, double hi) {
    constexpr double root2 = 1.4142135623730950488;
    if (lo >= hi)
        return 0;
    if ((lo < 0 && hi > 0) || (lo > -1 && hi < 1))
        return .5 * (std::erf(hi / root2) - std::erf(lo / root2));
    if (lo >= 0)
        return .5 * (std::erfc(lo / root2) - std::erfc(hi / root2));
    return .5 * (std::erfc(-hi / root2) - std::erfc(-lo / root2));
}
double integrate(const std::function<double(double)> &f, double lo, double hi, double eps = 1e-13) {
    if (lo >= hi)
        return 0;
    const auto simpson = [](double a, double b, double fa, double fm, double fb) {
        return (b - a) * (fa + 4 * fm + fb) / 6;
    };
    std::function<double(double, double, double, double, double, double, double, int)> step;
    step = [&](double a, double b, double fa, double fm, double fb, double whole, double error,
               int depth) {
        const double m = (a + b) / 2, l = (a + m) / 2, r = (m + b) / 2;
        const double fl = f(l), fr = f(r);
        const double left = simpson(a, m, fa, fl, fm), right = simpson(m, b, fm, fr, fb);
        const double delta = left + right - whole;
        if (!depth || std::abs(delta) <= 15 * error)
            return left + right + delta / 15;
        return step(a, m, fa, fl, fm, left, error / 2, depth - 1) +
               step(m, b, fm, fr, fb, right, error / 2, depth - 1);
    };
    const double fa = f(lo), fm = f((lo + hi) / 2), fb = f(hi);
    return step(lo, hi, fa, fm, fb, simpson(lo, hi, fa, fm, fb), eps, 24);
}
Json classification(double mean, double part, double gage, const Json &spec) {
    const double lo = spec["lsl"].is_null() ? -INFINITY : spec["lsl"].get<double>();
    const double hi = spec["usl"].is_null() ? INFINITY : spec["usl"].get<double>();
    // Use offsets from the process mean so small part deviations survive at a large origin.
    const double lo_offset = lo - mean, hi_offset = hi - mean;
    const auto probability = [&](double deviation) {
        return gage ? normal_interval((lo_offset - deviation) / gage,
                                     (hi_offset - deviation) / gage)
                    : double(deviation >= lo_offset && deviation <= hi_offset);
    };
    const auto rejection = [&](double deviation) {
        return gage ? normal_interval(-INFINITY, (lo_offset - deviation) / gage) +
                          normal_interval((hi_offset - deviation) / gage, INFINITY)
                    : double(deviation < lo_offset || deviation > hi_offset);
    };
    const double good = part ? normal_interval(lo_offset / part, hi_offset / part)
                             : double(lo_offset <= 0 && hi_offset >= 0);
    const double bad = part ? normal_interval(-INFINITY, lo_offset / part) +
                                  normal_interval(hi_offset / part, INFINITY)
                            : 1 - good;
    double bad_accept = 0, good_reject = 0;
    if (!part) {
        bad_accept = bad * probability(0);
        good_reject = good * rejection(0);
    } else if (gage) {
        const double lower = lo_offset / part, upper = hi_offset / part;
        // 在规格位置及测量误差过渡处拆分积分，避免窄过渡被采样网格漏掉。
        std::vector<double> cuts{-40, 40};
        for (double limit : {lower, upper})
            if (std::isfinite(limit))
                for (double offset : {-8., -2., 0., 2., 8.}) {
                    const double cut = limit + offset * gage / part;
                    if (cut > -40 && cut < 40)
                        cuts.push_back(cut);
                }
        std::sort(cuts.begin(), cuts.end());
        for (std::size_t i = 1; i < cuts.size(); ++i) {
            const double a = cuts[i - 1], b = cuts[i], middle = (a + b) / 2;
            const bool inside = middle >= lower && middle <= upper;
            const double mass = normal_interval(a, b);
            if (!mass)
                continue;
            const double value =
                mass *
                integrate(
                    [&](double z) {
                        const double deviation = part * z;
                        return std::exp(-z * z / 2 - .5 * std::log(2 * std::acos(-1.)) -
                                        std::log(mass)) *
                               (inside ? rejection(deviation) : probability(deviation));
                    },
                    a, b);
            (inside ? good_reject : bad_accept) += value;
        }
    }
    bad_accept = std::clamp(bad_accept, 0., bad);
    good_reject = std::clamp(good_reject, 0., good);
    return {{"bad_accepted", bad_accept},
            {"good_rejected", good_reject},
            {"accept_given_bad", bad > 0 ? Json(bad_accept / bad) : Json(nullptr)},
            {"reject_given_good", good > 0 ? Json(good_reject / good) : Json(nullptr)},
            {"mean", mean},
            {"part_sd", part},
            {"gage_sd", gage},
            {"distribution", "normal_independent_part_and_gage"}};
}
} // namespace

void apply_analysis_options(Json &m, const Json &cfg) {
    const auto &a = cfg.at("analysis");
    const auto &s = cfg["specification"];
    const double grr = m["GRR"], sample_pv = m["PV"], sample_tv = m["TV"];
    const auto &historical_sd = a.at("historical_sd");
    const auto &historical_mean = a.at("historical_mean");
    const auto &process_variation = a.at("process_variation");
    const bool history = !historical_sd.is_null();
    const double historical = history ? historical_sd.get<double>() : 0;
    const bool use_history = history && process_variation == "historical" && historical >= grr;
    if (use_history) {
        m["PV"] = std::sqrt(std::max(0., (historical - grr) * (historical + grr)));
        m["TV"] = historical;
        for (const auto *key : {"EV", "AV", "GRR"})
            m[std::string(key) + "_pct"] = 100 * m[key].get<double>() / historical;
        if (grr) {
            const double ndc = std::max(1., std::floor(1.41 * m["PV"].get<double>() / grr));
            if (!std::isfinite(ndc))
                throw std::overflow_error("历史标准差对应的 ndc 超出有限范围");
            m["ndc"] = ndc < 9223372036854775808.0 ? Json(std::int64_t(ndc)) : Json(ndc);
        }
    }
    const double mean = historical_mean.is_null() ? m["mean"].get<double>()
                                                  : historical_mean.get<double>();
    const auto [width, tolerance_source] = detail::effective_tolerance(a, s, mean);
    Json study{{"sample_pv", sample_pv},
               {"sample_tv", sample_tv},
               {"historical_applied", use_history},
               {"historical_sd", historical_sd},
               {"process_variation", process_variation},
               {"tolerance_source", tolerance_source},
               {"effective_tolerance", width > 0 ? Json(width) : Json(nullptr)},
               {"reference_mean", mean}};
    Json components{
        {"EV", m["EV"]}, {"AV", m["AV"]}, {"GRR", grr}, {"PV", m["PV"]}, {"TV", m["TV"]}};
    if (m.contains("anova")) {
        components["Operator"] = m["anova"]["stddev"]["Operator"];
        components["OperatorPart"] = m["anova"]["stddev"]["OperatorPart"];
    }
    const double tv = m["TV"], multiplier = a["study_multiplier"];
    for (const auto &entry : components.items()) {
        const double sd = entry.value(), sv = multiplier * sd, variance = sd * sd;
        const double pt = width > 0 ? 100 * sv / width : 0;
        const double process = history ? 100 * sd / historical : 0;
        if (!std::isfinite(variance) || !std::isfinite(sv) || !std::isfinite(process))
            throw std::overflow_error("方差、研究变异或百分比超出有限范围，请调整测量单位");
        if (!std::isfinite(pt))
            throw std::overflow_error("P/T 超出有限数值范围，请检查规格与尺度");
        Json row{{"sd", sd},
                 {"variance", variance},
                 {"study_variation", sv},
                 {"study_variation_pct", tv ? Json(100 * sd / tv) : Json(nullptr)},
                 {"contribution_pct", tv ? Json(100 * (sd / tv) * (sd / tv)) : Json(nullptr)},
                 {"tolerance_pct", width > 0 ? Json(pt) : Json(nullptr)},
                 {"process_pct", history ? Json(process) : Json(nullptr)}};
        study["components"][entry.key()] = row;
        if (m.contains("anova")) {
            auto &an = m["anova"];
            // Historical process variation changes the PV and TV decomposition.
            if (use_history && (entry.key() == "PV" || entry.key() == "TV")) {
                an["variance_components"][entry.key()] = sd * sd;
                an["stddev"][entry.key()] = sd;
                an["study_variation"][entry.key()] = sv;
            }
            if (use_history) {
                an["study_variation_pct"][entry.key()] = row["study_variation_pct"];
                an["contribution_pct"][entry.key()] = row["contribution_pct"];
            }
            an["tolerance_pct"][entry.key()] = row["tolerance_pct"];
            if (history)
                an["process_pct"][entry.key()] = row["process_pct"];
        }
    }
    m["PT_pct"] = study["components"]["GRR"]["tolerance_pct"];
    m["study"] = std::move(study);
    if (a.at("confidence").at("enabled") == true && m.contains("anova")) {
        auto confidence = anova_confidence_intervals(m["anova"], cfg);
        auto &intervals = confidence["variance_components"];
        if (use_history) {
            const double variance = historical * historical;
            const std::string bound = a["confidence"]["bound"];
            auto grr_for_pv = intervals["GRR"];
            if (bound != "both") {
                // PV方差=历史TV方差-GRR方差，为递减变换。
                // PV单侧下限需要GRR同尾概率的上限，反之亦然；不能使用已隐藏的端点。
                auto opposite_config = cfg;
                const auto opposite = bound == "lower" ? "upper" : "lower";
                opposite_config["analysis"]["confidence"]["bound"] = opposite;
                grr_for_pv[opposite] =
                    anova_confidence_intervals(m["anova"], opposite_config)
                        ["variance_components"]["GRR"][opposite];
            }
            intervals["PV"] = {
                {"lower",
                 bound == "upper" || grr_for_pv["upper"].is_null()
                     ? Json(nullptr)
                     : Json(std::max(0., variance - grr_for_pv["upper"].get<double>()))},
                {"upper",
                 bound == "lower" || grr_for_pv["lower"].is_null()
                     ? Json(nullptr)
                     : Json(std::max(0., variance - grr_for_pv["lower"].get<double>()))}};
            intervals["TV"] = {
                {"lower", a["confidence"]["bound"] == "upper" ? Json(nullptr) : Json(variance)},
                {"upper", a["confidence"]["bound"] == "lower" ? Json(nullptr) : Json(variance)}};
            confidence["interval_source"] = {{"PV", "historical_tv_minus_grr_variance"},
                                             {"TV", "fixed_historical_variance"}};
            if (confidence.contains("effective_df")) {
                confidence["effective_df"]["PV"] = nullptr;
                confidence["effective_df"]["TV"] = nullptr;
            }
        }
        for (const auto &entry : intervals.items()) {
            for (const auto *bound : {"lower", "upper"}) {
                const auto &value = entry.value()[bound];
                const Json sd =
                    value.is_null() ? Json(nullptr) : Json(std::sqrt(value.get<double>()));
                confidence["stddev"][entry.key()][bound] = sd;
                confidence["study_variation"][entry.key()][bound] =
                    sd.is_null() ? Json(nullptr) : Json(multiplier * sd.get<double>());
                confidence["tolerance_pct"][entry.key()][bound] =
                    sd.is_null() || width <= 0 ? Json(nullptr)
                                               : Json(100 * multiplier * sd.get<double>() / width);
                confidence["process_pct"][entry.key()][bound] =
                    sd.is_null() || !history ? Json(nullptr)
                                             : Json(100 * sd.get<double>() / historical);
            }
        }
        m["confidence"] = std::move(confidence);
    }
    if (a.at("misclassification").get<bool>() && a["method"] == "crossed_anova" &&
        (!s["lsl"].is_null() || !s["usl"].is_null())) {
        // 误分类使用历史过程波动（若可用），与 %Study Var 选择研究部件的设置相互独立。
        const double part = history && historical >= grr
                                ? std::sqrt(std::max(0., (historical - grr) * (historical + grr)))
                                : sample_pv;
        m["misclassification"] = classification(mean, part, grr, s);
    }
}
} // namespace msa
