#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace msa {
// Non-overlapping partials and final half-even correction, as in math.fsum.
// Compile without fast-math; all operations below must round to binary64.
double accurate_sum(const std::vector<double> &values) {
    std::vector<double> partials;
    for (double x : values) {
        if (!std::isfinite(x))
            throw std::overflow_error("求和包含非有限中间值");
        std::size_t i = 0;
        for (double y : partials) {
            if (std::abs(x) < std::abs(y))
                std::swap(x, y);
            double hi = x + y, yr = hi - x, lo = y - yr;
            if (!std::isfinite(hi))
                throw std::overflow_error("准确求和超出有限数值范围");
            if (lo)
                partials[i++] = lo;
            x = hi;
        }
        partials.resize(i);
        if (x)
            partials.push_back(x);
    }
    double hi = 0, lo = 0;
    std::size_t n = partials.size();
    if (n) {
        hi = partials[--n];
        while (n) {
            double x = hi, y = partials[--n];
            hi = x + y;
            lo = y - (hi - x);
            if (lo)
                break;
        }
    }
    if (n && ((lo < 0 && partials[n - 1] < 0) || (lo > 0 && partials[n - 1] > 0))) {
        double y = lo * 2, x = hi + y;
        if (y == x - hi)
            hi = x;
    }
    return hi;
}
std::string fixed(double value, int dp) {
    if (!std::isfinite(value))
        throw std::invalid_argument("无法格式化非有限数字");
    std::array<char, 512> buf{};
    auto r =
        std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed, dp);
    if (r.ec != std::errc{})
        throw std::runtime_error("数值格式化失败");
    return {buf.data(), r.ptr};
}
double parse_number(const std::string &text) {
    double v = 0;
    const char *start = text.data();
    if (!text.empty() && text[0] == '+')
        ++start;
    auto r = std::from_chars(start, text.data() + text.size(), v, std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(v))
        throw ConfigError("必须填写有限数字：" + text);
    return v;
}
double grid_bound(double value, int dp, bool lower) {
    // Decimal(str(value)) rounding: operate on shortest decimal digits, not
    // ceil(value*10^dp), which wrongly moves e.g. 0.07 to 0.08.
    char buf[64];
    auto r = std::to_chars(buf, buf + 64, value, std::chars_format::general);
    if (r.ec != std::errc{})
        throw ConfigError("网格边界无效");
    std::string s(buf, r.ptr);
    bool neg = s[0] == '-';
    if (neg)
        s.erase(0, 1);
    int exponent = 0;
    auto ep = s.find_first_of("eE");
    if (ep != s.npos) {
        exponent = std::stoi(s.substr(ep + 1));
        s.resize(ep);
    }
    auto dot = s.find('.');
    int decimal = int(dot == s.npos ? s.size() : dot) + exponent;
    if (dot != s.npos)
        s.erase(dot, 1);
    int cut = decimal + dp;
    if (cut >= int(s.size()))
        return value;
    bool remainder = false;
    for (int i = std::max(0, cut); i < int(s.size()); ++i)
        remainder |= s[i] != '0';
    std::string integer = cut > 0 ? s.substr(0, cut) : "0";
    if (remainder && ((lower && !neg) || (!lower && neg))) {
        int i = int(integer.size()) - 1;
        while (i >= 0 && integer[i] == '9')
            integer[i--] = '0';
        if (i < 0)
            integer.insert(0, "1");
        else
            ++integer[i];
    }
    return parse_number((neg ? "-" : "") + integer + "e-" + std::to_string(dp));
}
double ev_divisor(int o, int r, int p) {
    if (o * p > 20)
        return r == 2 ? 1.12838 : 1.69257;
    switch (o * p) {
    case 10:
        return r == 2 ? 1.16014 : 1.71573;
    case 12:
        return r == 2 ? 1.15490 : 1.71189;
    case 14:
        return r == 2 ? 1.15115 : 1.70914;
    case 15:
        return r == 2 ? 1.14965 : 1.70804;
    case 16:
        return r == 2 ? 1.14833 : 1.70708;
    case 18:
        return r == 2 ? 1.14613 : 1.70547;
    case 20:
        return r == 2 ? 1.14437 : 1.70419;
    default:
        throw ConfigError("研究设计无修正常数");
    }
}
double av_divisor(int o) { return o == 2 ? 1.41421 : 1.91155; }
double pv_divisor(int p) {
    static const double d[]{2.48124, 2.67253, 2.82981, 2.96288, 3.07794, 3.17905};
    return d[p - 5];
}
Json calculate(const Matrix &matrix, const Json &cfg) {
    int o = cfg["design"]["operators"], r = cfg["design"]["replicates"], p = cfg["design"]["parts"];
    if (o < 2 || o > 3 || r < 2 || r > 3 || p < 5 || p > 10)
        throw ConfigError("支持 2/3 人、2/3 次、5～10 件");
    if (matrix.size() != std::size_t(o * r))
        throw std::invalid_argument("矩阵行数不符");
    std::vector<double> values;
    Matrix x = matrix;
    for (const auto &row : matrix) {
        if (row.size() != std::size_t(p))
            throw std::invalid_argument("矩阵列数不符");
        for (double v : row) {
            if (!std::isfinite(v))
                throw std::invalid_argument("所有测量值必须是有限数字");
            values.push_back(v);
        }
    }
    double base = values[0];
    std::vector<double> shifted;
    for (auto &row : x)
        for (auto &v : row) {
            v -= base;
            shifted.push_back(v);
        }
    std::vector<double> ranges, am, pm;
    for (int i = 0; i < o; ++i) {
        std::vector<double> v;
        for (int j = 0; j < r; ++j)
            for (double q : x[i * r + j])
                v.push_back(q);
        am.push_back(accurate_sum(v) / (p * r));
        for (int k = 0; k < p; ++k) {
            double low = x[i * r][k], high = low;
            for (int j = 1; j < r; ++j) {
                low = std::min(low, x[i * r + j][k]);
                high = std::max(high, x[i * r + j][k]);
            }
            ranges.push_back(high - low);
        }
    }
    for (int k = 0; k < p; ++k) {
        std::vector<double> v;
        for (const auto &row : x)
            v.push_back(row[k]);
        pm.push_back(accurate_sum(v) / (o * r));
    }
    auto span = [](const std::vector<double> &v) {
        auto b = std::minmax_element(v.begin(), v.end());
        return *b.second - *b.first;
    };
    double ev = (accurate_sum(ranges) / (o * p)) / ev_divisor(o, r, p),
           a = span(am) / av_divisor(o);
    if (!std::isfinite(ev * ev) || !std::isfinite(a * a))
        throw std::overflow_error("方差计算超出有限数值范围");
    double av = std::sqrt(std::max(0., a * a - ev * ev / (p * r))), grr = std::hypot(ev, av),
           pv = span(pm) / pv_divisor(p), tv = std::hypot(grr, pv);
    Json anova;
    if (cfg["analysis"]["method"] == "crossed_anova") {
        anova = crossed_anova(matrix, cfg);
        const auto &sd = anova["stddev"];
        ev = sd["EV"];
        av = sd["AV"];
        grr = sd["GRR"];
        pv = sd["PV"];
        tv = sd["TV"];
    }
    if (!std::isfinite(ev) || !std::isfinite(av) || !std::isfinite(grr) || !std::isfinite(pv) ||
        !std::isfinite(tv))
        throw std::overflow_error("统计计算超出有限数值范围");
    const auto &s = cfg["specification"];
    const auto &g = cfg["generation"];
    Json m = {{"EV", ev}, {"AV", av}, {"GRR", grr}, {"PV", pv}, {"TV", tv}};
    if (!anova.is_null())
        m["anova"] = std::move(anova);
    for (auto pair : std::vector<std::pair<std::string, double>>{
             {"EV_pct", ev}, {"AV_pct", av}, {"GRR_pct", grr}})
        m[pair.first] = tv ? Json(100 * pair.second / tv) : Json(nullptr);
    m["PT_pct"] = nullptr;
    if (!s["lsl"].is_null() && !s["usl"].is_null()) {
        const double pt = 100 * cfg["analysis"]["study_multiplier"].get<double>() * grr /
                          (s["usl"].get<double>() - s["lsl"].get<double>());
        if (!std::isfinite(pt))
            throw std::overflow_error("P/T 超出有限数值范围，请检查规格与尺度");
        m["PT_pct"] = pt;
    }
    // Python integers have unbounded range. Store finite ndc as a JSON number;
    // usual studies fit int64, very large finite ratios remain exact as doubles.
    if (grr) {
        double n = std::max(1., std::floor(1.41 * pv / grr));
        if (!std::isfinite(n))
            throw std::overflow_error("ndc 超出有限数值范围");
        m["ndc"] = n < 9223372036854775808.0 ? Json(static_cast<std::int64_t>(n)) : Json(n);
    } else
        m["ndc"] = nullptr;
    m["mean"] = base + accurate_sum(shifted) / double(values.size());
    auto bounds = std::minmax_element(values.begin(), values.end());
    m["minimum"] = *bounds.first;
    m["maximum"] = *bounds.second;
    int below = 0, above = 0;
    for (double v : values) {
        if (!s["lsl"].is_null() && v < s["lsl"].get<double>())
            ++below;
        if (!s["usl"].is_null() && v > s["usl"].get<double>())
            ++above;
    }
    m["below_spec"] = below;
    m["above_spec"] = above;
    m["below_amount"] =
        s["lsl"].is_null() ? 0 : std::max(0., s["lsl"].get<double>() - *bounds.first);
    m["above_amount"] =
        s["usl"].is_null() ? 0 : std::max(0., *bounds.second - s["usl"].get<double>());
    m["center_error"] = g["center"].is_null()
                            ? Json(nullptr)
                            : Json(m["mean"].get<double>() - g["center"].get<double>());
    apply_analysis_options(m, cfg);
    return m;
}
Json checks(const Json &m, const Json &c) {
    Json s = Json::object();
    for (auto key : metric_keys) {
        auto b = acceptance(c, key);
        const auto &v = m.at(key);
        s[key] =
            !b ? Json(nullptr)
               : Json(!v.is_null() && b->first <= v.get<double>() && v.get<double>() <= b->second);
    }
    return s;
}
bool qualified(const Json &m, const Json &c) {
    for (const auto &item : m.items())
        if (item.value().is_number() && !std::isfinite(item.value().get<double>()))
            return false;
    if (m["TV"].get<double>() <= 0)
        return false;
    for (const auto &v : checks(m, c))
        if (v == false)
            return false;
    return c["specification"]["compliance"] != "strict" ||
           (m["below_spec"] == 0 && m["above_spec"] == 0);
}
Json Result::as_json() const {
    return {{"matrix", matrix},  {"metrics", metrics},   {"config", config},
            {"seed", seed},      {"attempts", attempts}, {"elapsed_seconds", elapsed_seconds},
            {"version", version}};
}
} // namespace msa
