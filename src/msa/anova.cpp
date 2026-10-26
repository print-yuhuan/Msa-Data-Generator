#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace msa {
namespace {
// 连分数计算正则不完全Beta；直接计算F分布上尾，避免1-CDF的消减误差。
double beta_fraction(double a, double b, double x) {
    constexpr double tiny = 1e-300;
    const auto guard = [tiny](double value) {
        return std::abs(value) < tiny ? std::copysign(tiny, value) : value;
    };
    double c = 1, d = 1 / guard(1 - (a + b) * x / (a + 1)), h = d;
    for (int m = 1; m <= 10000; ++m) {
        const double k = 2. * m;
        double aa = m * (b - m) * x / ((a + k - 1) * (a + k));
        d = 1 / guard(1 + aa * d);
        c = guard(1 + aa / c);
        h *= d * c;
        aa = -(a + m) * (a + b + m) * x / ((a + k) * (a + k + 1));
        d = 1 / guard(1 + aa * d);
        c = guard(1 + aa / c);
        const double delta = d * c;
        h *= delta;
        if (std::abs(delta - 1) <= 4 * std::numeric_limits<double>::epsilon())
            return h;
    }
    throw std::runtime_error("F分布概率计算未收敛");
}
double regularized_beta(double x, double a, double b) {
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    const double factor = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                                   a * std::log(x) + b * std::log1p(-x));
    const double value = x < (a + 1) / (a + b + 2) ? factor * beta_fraction(a, b, x) / a
                                                   : 1 - factor * beta_fraction(b, a, 1 - x) / b;
    return std::clamp(value, 0., 1.);
}
double centered_mean(const std::vector<double> &values) {
    const double origin = values.front();
    std::vector<double> offsets;
    for (double v : values)
        offsets.push_back(v - origin);
    return origin + accurate_sum(offsets) / values.size();
}
Json anova_row(const char *source, int df, double ss, double denominator = -1,
               int denominator_df = 0) {
    Json row{{"source", source}, {"df", df},     {"ss", ss},
             {"ms", ss / df},    {"f", nullptr}, {"p", nullptr}};
    if (denominator < 0)
        return row;
    const double ms = ss / df;
    // 0/0不可计算；正数/0的F为无穷，上尾概率为0。JSON不序列化无穷。
    if (denominator == 0) {
        if (ms > 0)
            row["p"] = 0.;
    } else {
        const double statistic = ms / denominator;
        row["f"] = std::isfinite(statistic) ? Json(statistic) : Json(nullptr);
        row["p"] = f_survival(statistic, df, denominator_df);
    }
    return row;
}
} // namespace

double f_survival(double f, int numerator_df, int denominator_df) {
    if (numerator_df <= 0 || denominator_df <= 0 || std::isnan(f) || f < 0)
        throw std::invalid_argument("F分布参数无效");
    if (std::isinf(f))
        return 0;
    if (f == 0)
        return 1;
    const double ratio = f * (double(numerator_df) / denominator_df);
    return regularized_beta(1 / (1 + ratio), denominator_df / 2., numerator_df / 2.);
}

Json crossed_anova(const Matrix &matrix, const Json &cfg) {
    const int o = cfg["design"]["operators"], r = cfg["design"]["replicates"],
              p = cfg["design"]["parts"];
    if (o < 2 || o > 3 || r < 2 || r > 3 || p < 5 || p > 10 || matrix.size() != std::size_t(o * r))
        throw ConfigError("ANOVA要求完整平衡交叉设计：2/3人、2/3次、5～10件");
    Matrix x = matrix;
    const double base = matrix.front().empty() ? 0 : matrix.front().front();
    for (auto &row : x) {
        if (row.size() != std::size_t(p))
            throw ConfigError("ANOVA矩阵列数不符");
        for (double &v : row) {
            if (!std::isfinite(v))
                throw ConfigError("ANOVA测量值必须是有限数字");
            v -= base;
        }
    }
    Matrix cells(o, std::vector<double>(p));
    std::vector<double> operators(o), parts(p), terms;
    for (int i = 0; i < o; ++i)
        for (int j = 0; j < p; ++j) {
            terms.clear();
            for (int k = 0; k < r; ++k)
                terms.push_back(x[i * r + k][j]);
            cells[i][j] = centered_mean(terms);
        }
    for (int i = 0; i < o; ++i)
        operators[i] = centered_mean(cells[i]);
    for (int j = 0; j < p; ++j) {
        terms.clear();
        for (int i = 0; i < o; ++i)
            terms.push_back(cells[i][j]);
        parts[j] = centered_mean(terms);
    }
    const auto squares = [](const std::vector<double> &values) {
        std::vector<double> offsets;
        for (double v : values)
            offsets.push_back(v - values.front());
        const double center = centered_mean(offsets);
        std::vector<double> squared;
        for (double v : offsets)
            squared.push_back((v - center) * (v - center));
        return accurate_sum(squared);
    };
    const double ssp = o * r * squares(parts), sso = p * r * squares(operators);
    // 先去掉共同的部件轮廓，再双中心化。完全相同的人员记录产生严格零交互，
    // 避免把均值舍入误差当成交互，不使用可能掩盖真实微小变异的绝对阈值。
    Matrix contrasts = cells;
    std::vector<double> row_means(o), column_means(p);
    for (int i = 0; i < o; ++i) {
        for (int j = 0; j < p; ++j)
            contrasts[i][j] -= cells[0][j];
        row_means[i] = centered_mean(contrasts[i]);
    }
    for (int j = 0; j < p; ++j) {
        terms.clear();
        for (int i = 0; i < o; ++i)
            terms.push_back(contrasts[i][j]);
        column_means[j] = centered_mean(terms);
    }
    const double grand_contrast = centered_mean(row_means);
    std::vector<double> interactions, errors;
    for (int i = 0; i < o; ++i)
        for (int j = 0; j < p; ++j) {
            const double v = (contrasts[i][j] - row_means[i]) - (column_means[j] - grand_contrast);
            interactions.push_back(r * v * v);
            for (int k = 0; k < r; ++k)
                errors.push_back(std::pow(x[i * r + k][j] - cells[i][j], 2));
        }
    const double ssi = accurate_sum(interactions), sse = accurate_sum(errors),
                 total = accurate_sum({ssp, sso, ssi, sse});
    const int dfp = p - 1, dfo = o - 1, dfi = dfp * dfo, dfe = o * p * (r - 1);
    const double msp = ssp / dfp, mso = sso / dfo, msi = ssi / dfi, mse = sse / dfe;
    auto full = Json::array(
        {anova_row("Part", dfp, ssp, msi, dfi), anova_row("Operator", dfo, sso, msi, dfi),
         anova_row("Part*Operator", dfi, ssi, mse, dfe), anova_row("Repeatability", dfe, sse),
         Json{{"source", "Total"},
              {"df", o * r * p - 1},
              {"ss", total},
              {"ms", nullptr},
              {"f", nullptr},
              {"p", nullptr}}});
    const auto interaction_p = full[2]["p"];
    const auto &settings = cfg["analysis"];
    const std::string mode = settings.value("interaction", std::string("auto"));
    const double alpha = settings.value("interaction_alpha", .05);
    const bool included = mode == "include" || (mode == "auto" && !interaction_p.is_null() &&
                                                interaction_p.get<double>() < alpha);
    const int residual_df = included ? dfe : dfe + dfi;
    const double residual = included ? mse : (sse + ssi) / residual_df;
    const double denominator = included ? msi : residual;
    const double op = std::max(0., (mso - denominator) / (p * r)),
                 inter = included ? std::max(0., (msi - mse) / r) : 0.,
                 part = std::max(0., (msp - denominator) / (o * r));
    Json table = full;
    if (!included)
        table = Json::array({anova_row("Part", dfp, ssp, residual, residual_df),
                             anova_row("Operator", dfo, sso, residual, residual_df),
                             anova_row("Repeatability", residual_df, sse + ssi), full[4]});
    Json components{{"EV", residual},
                    {"Operator", op},
                    {"OperatorPart", inter},
                    {"AV", op + inter},
                    {"GRR", residual + op + inter},
                    {"PV", part},
                    {"TV", residual + op + inter + part}};
    for (const auto &v : components)
        if (!std::isfinite(v.get<double>()))
            throw std::overflow_error("ANOVA方差分量超出有限数值范围");
    Json contribution = Json::object(), sd = Json::object(), sv = Json::object(),
         percent = Json::object(), tolerance = Json::object();
    const double tv = std::sqrt(components["TV"].get<double>()),
                 multiplier = settings["study_multiplier"];
    const auto &spec = cfg["specification"];
    const bool both = !spec["lsl"].is_null() && !spec["usl"].is_null();
    for (const auto &entry : components.items()) {
        const double value = entry.value(), sigma = std::sqrt(value);
        const double study = multiplier * sigma;
        const double pt =
            both ? 100 * study / (spec["usl"].get<double>() - spec["lsl"].get<double>()) : 0;
        if (!std::isfinite(study) || !std::isfinite(pt))
            throw std::overflow_error("ANOVA研究变异或P/T超出有限数值范围");
        sd[entry.key()] = sigma;
        sv[entry.key()] = study;
        contribution[entry.key()] =
            tv ? Json(100 * (value / components["TV"].get<double>())) : Json(nullptr);
        percent[entry.key()] = tv ? Json(100 * sigma / tv) : Json(nullptr);
        tolerance[entry.key()] = both ? Json(pt) : Json(nullptr);
    }
    return {{"interaction_mode", mode},
            {"interaction_alpha", alpha},
            {"interaction_p", interaction_p},
            {"interaction_included", included},
            {"full_table", full},
            {"table", table},
            {"variance_components", components},
            {"stddev", sd},
            {"study_variation", sv},
            {"contribution_pct", contribution},
            {"study_variation_pct", percent},
            {"tolerance_pct", tolerance}};
}
} // namespace msa
