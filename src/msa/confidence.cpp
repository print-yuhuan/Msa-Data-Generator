#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace msa {
namespace {
// 不完全 Gamma 的级数与连分数分别用于分布左尾、右尾。
double gamma_cdf(double a, double x) {
    if (x <= 0)
        return 0;
    const double factor = std::exp(a * std::log(x) - x - std::lgamma(a));
    if (x < a + 1) {
        double term = 1 / a, sum = term;
        for (int n = 1; n < 10000; ++n) {
            term *= x / (a + n);
            sum += term;
            if (std::abs(term) <= std::abs(sum) * 2e-15)
                return std::clamp(sum * factor, 0., 1.);
        }
    } else {
        constexpr double tiny = 1e-300;
        double b = x + 1 - a, c = 1 / tiny, d = 1 / b, h = d;
        for (int n = 1; n < 10000; ++n) {
            const double an = -double(n) * (n - a);
            b += 2;
            d = an * d + b;
            if (std::abs(d) < tiny)
                d = std::copysign(tiny, d);
            c = b + an / c;
            if (std::abs(c) < tiny)
                c = std::copysign(tiny, c);
            d = 1 / d;
            const double delta = d * c;
            h *= delta;
            if (std::abs(delta - 1) < 2e-15)
                return std::clamp(1 - factor * h, 0., 1.);
        }
    }
    throw std::runtime_error("卡方分布概率计算未收敛");
}
double f_quantile(double p, int first, int second) {
    double lo = 0, hi = 1;
    while (f_survival(hi, first, second) > 1 - p)
        hi *= 2;
    for (int i = 0; i < 160; ++i) {
        const double middle = (lo + hi) / 2;
        if (f_survival(middle, first, second) > 1 - p)
            lo = middle;
        else
            hi = middle;
    }
    return (lo + hi) / 2;
}
struct Factors {
    std::array<double, 4> h{}, g{};
    std::array<std::array<double, 4>, 4> cross_h{}, cross_g{};
    double positive_cross = 0;
};
Factors factors(double tail, const std::array<int, 4> &n) {
    // 同一搜索期间反复分析候选矩阵，分布分位数仅与设计和置信水平有关。
    using Key = std::pair<std::array<int, 4>, double>;
    static thread_local std::map<Key, Factors> cache;
    const Key key{n, tail};
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    Factors result;
    for (int q = 0; q < 4; ++q) {
        result.h[q] = n[q] / chi_square_quantile(tail, n[q]) - 1;
        result.g[q] = 1 - n[q] / chi_square_quantile(1 - tail, n[q]);
    }
    for (int q = 0; q < 4; ++q)
        for (int r = 0; r < 4; ++r) {
            const double lower = f_quantile(tail, n[q], n[r]);
            const double upper = f_quantile(1 - tail, n[q], n[r]);
            result.cross_h[q][r] = (std::pow(1 - lower, 2) - std::pow(result.h[q] * lower, 2) -
                                    std::pow(result.g[r], 2)) /
                                   lower;
            result.cross_g[q][r] = (std::pow(upper - 1, 2) - std::pow(result.g[q] * upper, 2) -
                                    std::pow(result.h[r], 2)) /
                                   upper;
        }
    const double joint = 1 - (n[1] + n[2]) / chi_square_quantile(1 - tail, n[1] + n[2]);
    result.positive_cross = joint * joint * std::pow(double(n[1] + n[2]), 2) / (n[1] * n[2]) -
                            std::pow(result.g[1], 2) * n[1] / n[2] -
                            std::pow(result.g[2], 2) * n[2] / n[1];
    if (cache.size() > 128)
        cache.clear();
    cache[key] = result;
    return result;
}
} // namespace

double chi_square_quantile(double probability, double df) {
    if (df <= 0 || !std::isfinite(df) || probability <= 0 || probability >= 1 ||
        !std::isfinite(probability))
        throw std::invalid_argument("卡方分布分位数参数无效");
    double lo = 0, hi = std::max(1., double(df));
    while (gamma_cdf(df / 2., hi / 2) < probability)
        hi *= 2;
    for (int i = 0; i < 180; ++i) {
        const double middle = (lo + hi) / 2;
        if (gamma_cdf(df / 2., middle / 2) < probability)
            lo = middle;
        else
            hi = middle;
    }
    return (lo + hi) / 2;
}

void validate_mls_confidence(int operators, const Json &confidence) {
    if (confidence.value("method", std::string("mls")) != "mls")
        return;
    // MLS 的平方根分支要求 g=1-df/chi2(1-tail,df) 非负，否则平方会丢失方向。
    // 当前设计中操作员自由度最小；df=1/2 在其均值处的 CDF 有封闭解。
    const double probability_at_mean = operators == 2 ? std::erf(std::sqrt(.5))
                                                       : -std::expm1(-1.);
    const bool both = confidence["bound"] == "both";
    const double minimum = 100 * (both ? 2 * probability_at_mean - 1 : probability_at_mean);
    if (confidence["level"].get<double>() < minimum) {
        const double displayed = std::ceil(minimum * 1e6) / 1e6;
        throw ConfigError("当前 " + std::to_string(operators) + " 人研究的 MLS " +
                          (both ? "双侧" : "单侧") + "置信水平须不低于 " +
                          fixed(displayed, 6) +
                          "%；请提高置信水平，或明确选择 Satterthwaite 算法。");
    }
}

Json anova_confidence_intervals(const Json &anova, const Json &cfg) {
    const auto &confidence = cfg["analysis"]["confidence"];
    // calculate 也可接受已有的配置快照，不能只依赖配置保存时的校验。
    validate_mls_confidence(cfg["design"]["operators"].get<int>(), confidence);
    const std::string bound = confidence["bound"];
    const double alpha = 1 - confidence["level"].get<double>() / 100;
    const double tail = bound == "both" ? alpha / 2 : alpha;
    const int i = cfg["design"]["parts"], j = cfg["design"]["operators"],
              k = cfg["design"]["replicates"];
    const bool interaction = anova["interaction_included"];
    std::array<int, 4> df{i - 1, j - 1, (i - 1) * (j - 1), i * j * (k - 1)};
    std::array<double, 4> ms{};
    for (int q = 0; q < 4; ++q)
        ms[q] = anova["full_table"][q]["ms"];
    if (!interaction) {
        ms[2] = (ms[2] * df[2] + ms[3] * df[3]) / (df[2] + df[3]);
        df[2] += df[3];
    }
    if (confidence.value("method", std::string("mls")) == "satterthwaite") {
        Json result{{"level", confidence["level"]}, {"bound", bound},
                    {"method", "Satterthwaite_variance_components"},
                    {"variance_components", Json::object()}, {"effective_df", Json::object()}};
        const auto estimate = [&](const char *key, const std::array<double, 4> &weights) {
            double center = 0, sampling = 0;
            for (int q = 0; q < 4; ++q) {
                const double term = weights[q] * ms[q];
                center += term;
                sampling += term * term / df[q];
            }
            Json limits{{"lower", bound == "upper" ? Json(nullptr) : Json(0.)},
                        {"upper", nullptr}};
            // 边界零估计不能给出有意义的Satterthwaite有效自由度，不伪造零宽区间。
            if (center > 0 && sampling > 0) {
                const double effective = center * center / sampling;
                result["effective_df"][key] = effective;
                const auto endpoint = [&](double probability) -> Json {
                    const double quantile = chi_square_quantile(probability, effective);
                    // 极小自由度时下尾分位数可能小到无法可靠表示，保留为不可计算。
                    if (quantile <= 1e-50)
                        return nullptr;
                    const double value = center * effective / quantile;
                    return std::isfinite(value) ? Json(value) : Json(nullptr);
                };
                if (bound != "upper")
                    limits["lower"] = endpoint(1 - tail);
                if (bound != "lower")
                    limits["upper"] = endpoint(tail);
            } else {
                result["effective_df"][key] = nullptr;
            }
            result["variance_components"][key] = std::move(limits);
        };
        const int residual = interaction ? 3 : 2;
        std::array<double, 4> ev{};
        ev[residual] = 1;
        estimate("EV", ev);
        estimate("Operator", {0, 1. / (i * k), -1. / (i * k), 0});
        estimate("PV", {1. / (j * k), 0, -1. / (j * k), 0});
        if (interaction) {
            estimate("OperatorPart", {0, 0, 1. / k, -1. / k});
            estimate("AV", {0, 1. / (i * k), double(i - 1) / (i * k), -1. / k});
            estimate("GRR", {0, 1. / (i * k), double(i - 1) / (i * k), double(k - 1) / k});
            estimate("TV", {1. / (j * k), 1. / (i * k),
                            double(i * j - i - j) / (i * j * k), double(k - 1) / k});
        } else {
            result["variance_components"]["OperatorPart"] = {
                {"lower", bound == "upper" ? Json(nullptr) : Json(0.)},
                {"upper", bound == "lower" ? Json(nullptr) : Json(0.)}};
            result["effective_df"]["OperatorPart"] = nullptr;
            result["variance_components"]["AV"] = result["variance_components"]["Operator"];
            result["effective_df"]["AV"] = result["effective_df"]["Operator"];
            estimate("GRR", {0, 1. / (i * k), double(i * k - 1) / (i * k), 0});
            estimate("TV", {1. / (j * k), 1. / (i * k),
                            double(i * j * k - i - j) / (i * j * k), 0});
        }
        // 截断负方差后，组合量不再是原均方的线性组合，暂不报告其近似区间。
        if (ms[1] < ms[2] || (interaction && ms[2] < ms[3]))
            for (const char *key : {"AV", "GRR", "TV"}) {
                result["variance_components"][key] = {{"lower", nullptr}, {"upper", nullptr}};
                result["effective_df"][key] = nullptr;
            }
        if (ms[0] < ms[2]) {
            result["variance_components"]["TV"] = {{"lower", nullptr}, {"upper", nullptr}};
            result["effective_df"]["TV"] = nullptr;
        }
        return result;
    }
    const auto f = factors(tail, df);
    const auto interval = [&](double center, double lower_variance, double upper_variance) {
        return Json{
            {"lower", bound == "upper"
                          ? Json(nullptr)
                          : Json(std::max(0., center - std::sqrt(std::max(0., lower_variance))))},
            {"upper", bound == "lower"
                          ? Json(nullptr)
                          : Json(std::max(0., center + std::sqrt(std::max(0., upper_variance))))}};
    };
    const auto difference = [&](int q, int r, double divisor) {
        const double vl = std::pow(f.g[q] * ms[q], 2) + std::pow(f.h[r] * ms[r], 2) +
                          f.cross_g[q][r] * ms[q] * ms[r];
        const double vu = std::pow(f.h[q] * ms[q], 2) + std::pow(f.g[r] * ms[r], 2) +
                          f.cross_h[q][r] * ms[q] * ms[r];
        return interval((ms[q] - ms[r]) / divisor, vl / (divisor * divisor),
                        vu / (divisor * divisor));
    };
    const auto positive_sum = [&](const std::array<double, 4> &weights) {
        double center = 0, vl = 0, vu = 0;
        for (int q = 0; q < 4; ++q) {
            center += weights[q] * ms[q];
            vl += std::pow(f.g[q] * weights[q] * ms[q], 2);
            vu += std::pow(f.h[q] * weights[q] * ms[q], 2);
        }
        return interval(center, vl, vu);
    };
    Json result{{"level", confidence["level"]},
                {"bound", bound},
                {"method", "Minitab_MLS_variance_components"},
                {"variance_components", Json::object()}};
    auto &out = result["variance_components"];
    const int residual = interaction ? 3 : 2;
    out["EV"] = {
        {"lower", bound == "upper" ? Json(nullptr) : Json((1 - f.g[residual]) * ms[residual])},
        {"upper", bound == "lower" ? Json(nullptr) : Json((1 + f.h[residual]) * ms[residual])}};
    out["Operator"] = difference(1, 2, double(i * k));
    out["PV"] = difference(0, 2, double(j * k));
    if (interaction) {
        out["OperatorPart"] = difference(2, 3, double(k));
        const double center = (ms[1] + (i - 1) * ms[2] - i * ms[3]) / (i * k);
        const double vl = std::pow(f.g[1] * ms[1], 2) + std::pow(f.g[2] * (i - 1) * ms[2], 2) +
                          std::pow(f.h[3] * i * ms[3], 2) + i * f.cross_g[1][3] * ms[1] * ms[3] +
                          i * (i - 1) * f.cross_g[2][3] * ms[2] * ms[3] +
                          (i - 1) * f.positive_cross * ms[1] * ms[2];
        const double vu = std::pow(f.h[1] * ms[1], 2) + std::pow(f.h[2] * (i - 1) * ms[2], 2) +
                          std::pow(f.g[3] * i * ms[3], 2) + i * f.cross_h[1][3] * ms[1] * ms[3] +
                          i * (i - 1) * f.cross_h[2][3] * ms[2] * ms[3];
        out["AV"] = interval(center, vl / std::pow(i * k, 2), vu / std::pow(i * k, 2));
        out["GRR"] = positive_sum({0, 1. / (i * k), double(i - 1) / (i * k), double(k - 1) / k});
        out["TV"] = positive_sum(
            {1. / (j * k), 1. / (i * k), double(i * j - i - j) / (i * j * k), double(k - 1) / k});
    } else {
        out["OperatorPart"] = {{"lower", bound == "upper" ? Json(nullptr) : Json(0.)},
                               {"upper", bound == "lower" ? Json(nullptr) : Json(0.)}};
        out["AV"] = out["Operator"];
        out["GRR"] = positive_sum({0, 1. / (i * k), double(i * k - 1) / (i * k), 0});
        out["TV"] =
            positive_sum({1. / (j * k), 1. / (i * k), double(i * j * k - i - j) / (i * j * k), 0});
    }
    return result;
}
} // namespace msa
