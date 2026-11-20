#include "core.hpp"
#include "analysis_detail.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>

namespace msa {
namespace {
// Explicit integer-to-uniform mapping and Fisher-Yates keep native sequences
// stable across STL implementations. This is deliberately NOT Python Random.
struct Random {
    std::mt19937_64 engine;
    explicit Random(std::int64_t seed) : engine(static_cast<std::uint64_t>(seed)) {}
    double unit() { return double(engine() >> 11) * 0x1.0p-53; }
    double uniform(double a, double b) { return a + (b - a) * unit(); }
    void shuffle(std::vector<double> &v) {
        for (std::size_t n = v.size(); n > 1; --n) {
            auto bound = std::uint64_t(n), threshold = (0 - bound) % bound;
            std::uint64_t x;
            do {
                x = engine();
            } while (x < threshold);
            std::swap(v[n - 1], v[std::size_t(x % bound)]);
        }
    }
};
double preferred(const Json &c, const std::string &k, double fallback) {
    auto &t = c["targets"][k];
    if (t.is_null())
        return fallback;
    if (t["mode"] == "target")
        return t["value"];
    if (t["mode"] == "between")
        return (t["min"].get<double>() + t["max"].get<double>()) / 2;
    return fallback;
}
void shuffle_and_center(std::vector<double> &seq, Random &rng) {
    rng.shuffle(seq);
    const double mean = accurate_sum(seq) / static_cast<double>(seq.size());
    for (auto &v : seq)
        v -= mean;
}
std::vector<double> profile(int n, Random &rng) {
    std::vector<double> seq{0, 1};
    for (int i = 2; i < n; ++i)
        seq.push_back(rng.uniform(.05, .95));
    shuffle_and_center(seq, rng);
    return seq;
}
std::vector<double> part_profile(int n, const std::string &sampling, Random &rng) {
    if (sampling == "default")
        return profile(n, rng);

    std::vector<double> seq;
    seq.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (sampling == "stratified") {
            seq.push_back(rng.uniform(static_cast<double>(i) / n, static_cast<double>(i + 1) / n));
            continue;
        }
        const double position = 2.0 * i / (n - 1) - 1;
        double value = position;
        if (sampling == "center_dense")
            value *= .5 + .5 * position * position;
        else if (sampling == "end_dense")
            value *= 2 - std::abs(position);
        seq.push_back(value);
    }
    // Xbar-R constructs the intended PV from a profile with unit range.
    const double span = seq.back() - seq.front();
    for (double &value : seq)
        value /= span;
    shuffle_and_center(seq, rng);
    return seq;
}
std::optional<std::pair<Matrix, double>>
normalized(const Json &c, const std::array<double, 6> &ratio, Random &rng, int attempt) {
    auto [el, eh, al, ah, gl, gh] = ratio;
    int o = c["design"]["operators"], r = c["design"]["replicates"], p = c["design"]["parts"];
    const auto &sampling = c["generation"]["part_sampling"].get_ref<const std::string &>();
    const auto &t = c["targets"];
    bool et = !t["EV_pct"].is_null(), at = !t["AV_pct"].is_null();
    double fallback =
        et && at ? std::hypot(preferred(c, "EV_pct", 0), preferred(c, "AV_pct", 0)) / 100 : .1;
    auto &nd = t["ndc"];
    if (!nd.is_null() && (nd["mode"] == "target" || nd["mode"] == "between") &&
        t["GRR_pct"].is_null())
        fallback = 1.41 / std::hypot(1.41, preferred(c, "ndc", 10) + .5);
    double nominal = std::min(gh, std::max(gl, preferred(c, "GRR_pct", fallback * 100) / 100));
    double g = attempt % 5 == 0 ? nominal : rng.uniform(gl, gh);
    double e2lo = std::max({el * el, g * g - ah * ah, 0.}),
           e2hi = std::min(eh * eh, g * g - al * al);
    // Preserve Python candidate construction guard; never used in qualification.
    if (e2lo > e2hi + 1e-15)
        return {};
    e2hi = std::max(e2lo, e2hi);
    double ep = preferred(c, "EV_pct", g * 70) / 100, ap = preferred(c, "AV_pct", g * 70) / 100;
    double e2 = et && at ? (ep * ep + g * g - ap * ap) / 2 : at ? g * g - ap * ap : ep * ep;
    e2 = attempt % 3 == 0 ? std::min(e2hi, std::max(e2lo, e2)) : rng.uniform(e2lo, e2hi);
    double ev = std::sqrt(e2), av = std::sqrt(std::max(0., g * g - e2)),
           pv = std::sqrt(std::max(0., 1 - g * g));
    if (c["analysis"]["method"] == "crossed_anova") {
        // 正交分解构造指定均方；仅ANOVA采用此分支，旧均值极差法的随机序列不变。
        auto scale_norm = [](std::vector<double> &v, double sumsq) {
            std::vector<double> squares;
            for (double x : v)
                squares.push_back(x * x);
            const double norm = std::sqrt(accurate_sum(squares));
            if (norm == 0 && sumsq > 0)
                throw std::runtime_error("ANOVA正交构造退化");
            const double factor = norm ? std::sqrt(sumsq) / norm : 0;
            for (double &x : v)
                x *= factor;
        };
        const int dfi = (o - 1) * (p - 1), dfe = o * p * (r - 1);
        const std::string mode = c["analysis"]["interaction"];
        double inter = mode == "exclude" ? 0 : av * av * rng.uniform(.15, .7);
        double msi = ev * ev + r * inter;
        if (mode == "auto" &&
            (inter == 0 || (ev > 0 && f_survival(msi / (ev * ev), dfi, dfe) >=
                                          c["analysis"]["interaction_alpha"].get<double>()))) {
            inter = 0;
            msi = ev * ev;
        }
        auto parts = part_profile(p, sampling, rng), operators = profile(o, rng);
        scale_norm(parts, (p - 1) * (msi + o * r * pv * pv) / (o * r));
        scale_norm(operators, (o - 1) * (msi + p * r * (av * av - inter)) / (p * r));
        Matrix interaction(o, std::vector<double>(p));
        for (auto &row : interaction)
            for (double &v : row)
                v = rng.uniform(-1, 1);
        for (auto &row : interaction) {
            const double mean = accurate_sum(row) / p;
            for (double &v : row)
                v -= mean;
        }
        for (int j = 0; j < p; ++j) {
            std::vector<double> col;
            for (int i = 0; i < o; ++i)
                col.push_back(interaction[i][j]);
            const double mean = accurate_sum(col) / o;
            for (int i = 0; i < o; ++i)
                interaction[i][j] -= mean;
        }
        std::vector<double> flat;
        for (const auto &row : interaction)
            for (double v : row)
                flat.push_back(v);
        scale_norm(flat, dfi * msi / r);
        Matrix matrix(o * r, std::vector<double>(p));
        std::vector<double> residuals;
        const bool varied = c["generation"]["residual_model"] == "varied";
        for (int i = 0; i < o; ++i)
            for (int j = 0; j < p; ++j) {
                const auto residual = profile(r, rng);
                // Each subgroup stays centered. The common normalization below preserves SSE.
                const double amplitude = varied ? rng.uniform(.4, 1.6) : 1.;
                for (double value : residual)
                    residuals.push_back(amplitude * value);
            }
        scale_norm(residuals, dfe * ev * ev);
        for (int i = 0; i < o; ++i)
            for (int j = 0; j < p; ++j)
                for (int k = 0; k < r; ++k)
                    matrix[i * r + k][j] =
                        parts[j] + operators[i] + flat[i * p + j] + residuals[(i * p + j) * r + k];
        return std::make_pair(matrix, g);
    }
    auto parts = part_profile(p, sampling, rng);
    for (auto &v : parts)
        v *= pv * pv_divisor(p);
    double arange = av > 1e-14 ? av_divisor(o) * std::sqrt(av * av + ev * ev / (p * r)) : 0;
    auto opers = profile(o, rng);
    for (auto &v : opers)
        v *= arange;
    std::vector<double> weights;
    for (int i = 0; i < o * p; ++i)
        weights.push_back(rng.uniform(.65, 1.35));
    double wm = accurate_sum(weights) / (o * p);
    Matrix matrix(o * r, std::vector<double>(p));
    for (int i = 0; i < o; ++i)
        for (int j = 0; j < p; ++j) {
            double er = ev * ev_divisor(o, r, p) * weights[i * p + j] / wm;
            auto residual = profile(r, rng);
            for (int k = 0; k < r; ++k)
                matrix[i * r + k][j] = parts[j] + opers[i] + residual[k] * er;
        }
    return std::make_pair(matrix, g);
}
double part_mean_span(const Matrix &matrix) {
    // Preserve the exact distance between two constant endpoint parts, even when
    // averaging repeated offsets relative to one common origin would round it down.
    using PartMean = std::pair<double, double>;
    const auto difference = [](const PartMean &a, const PartMean &b) {
        return accurate_sum({a.first - b.first, a.second, -b.second});
    };
    PartMean low{}, high{};
    std::vector<double> values(matrix.size());
    for (std::size_t part = 0; part < matrix.front().size(); ++part) {
        const double origin = matrix.front()[part];
        for (std::size_t row = 0; row < matrix.size(); ++row)
            values[row] = matrix[row][part] - origin;
        const PartMean mean{origin, accurate_sum(values) / static_cast<double>(values.size())};
        if (part == 0 || difference(mean, low) < 0)
            low = mean;
        if (part == 0 || difference(mean, high) > 0)
            high = mean;
    }
    return difference(high, low);
}
Matrix place_full_part_span(const Matrix &matrix, double scale, double lo, double hi, int dp) {
    // To attain the full width in strict mode, every reading at each extreme part
    // must be on the same limit. Interior parts still carry repeatability and AV.
    Matrix result = matrix;
    std::vector<double> means(matrix.front().size()), values(matrix.size());
    for (std::size_t j = 0; j < means.size(); ++j) {
        for (std::size_t i = 0; i < matrix.size(); ++i)
            values[i] = matrix[i][j];
        means[j] = accurate_sum(values) / static_cast<double>(values.size());
    }
    const auto [lowest, highest] = std::minmax_element(means.begin(), means.end());
    const double span = *highest - *lowest;
    for (std::size_t j = 0; j < means.size(); ++j) {
        if (means.begin() + j == lowest || means.begin() + j == highest) {
            for (auto &row : result)
                row[j] = means.begin() + j == lowest ? lo : hi;
            continue;
        }
        const double position = span > 0 ? (means[j] - *lowest) / span : .5;
        const double center = std::clamp(lo + (hi - lo) * position, lo, hi);
        double amplitude = scale;
        for (const auto &row : matrix) {
            const double deviation = row[j] - means[j];
            if (deviation > 0)
                amplitude = std::min(amplitude, (hi - center) / deviation);
            else if (deviation < 0)
                amplitude = std::min(amplitude, (lo - center) / deviation);
        }
        for (std::size_t i = 0; i < result.size(); ++i) {
            const double value = center + amplitude * (matrix[i][j] - means[j]);
            result[i][j] = parse_number(fixed(std::clamp(value, lo, hi), dp));
        }
    }
    return result;
}
bool uses_reference_scale(const Json &generation, const Json &specification) {
    return generation["scale_mode"] == "reference_tv_sd" ||
           specification["lsl"].is_null() || specification["usl"].is_null();
}
bool pt_excludes_historical_variation(const Json &c) {
    const auto pt = acceptance(c, "PT_pct");
    if (!pt || pt->first <= 0)
        return false;
    const auto &analysis = c["analysis"];
    const auto &historical_mean = analysis["historical_mean"];
    const auto reference_mean = historical_mean.is_null()
                                    ? std::optional<double>{}
                                    : std::optional<double>{historical_mean.get<double>()};
    const auto width =
        detail::effective_tolerance(analysis, c["specification"], reference_mean).width;
    if (!(width > 0))
        return false;
    const double historical_sd = analysis["historical_sd"], multiplier = analysis["study_multiplier"];
    // At equality, the analysis still uses history. Match P/T's computation order.
    const double largest_pt_using_history = 100 * (multiplier * historical_sd) / width;
    return pt->first > largest_pt_using_history;
}
std::optional<Matrix> scale_place(const Matrix &matrix, double grr, const Json &c, Random &rng,
                                  int attempt, double minimum_part_span) {
    auto &s = c["specification"];
    auto &g = c["generation"];
    const auto &analysis = c["analysis"];
    bool haslo = !s["lsl"].is_null(), hashi = !s["usl"].is_null(), both = haslo && hashi;
    double lo = haslo ? s["lsl"].get<double>() : -INFINITY,
           hi = hashi ? s["usl"].get<double>() : INFINITY;
    const bool strict = s["compliance"] == "strict";
    const int dp = g["decimal_places"];
    const double grid_lo = strict && haslo ? grid_bound(lo, dp, true) : lo,
                 grid_hi = strict && hashi ? grid_bound(hi, dp, false) : hi;
    double low = INFINITY, high = -INFINITY;
    for (auto &row : matrix)
        for (double v : row) {
            low = std::min(low, v);
            high = std::max(high, v);
        }
    double span = high - low;
    double center = g["center"].is_null() ? (both ? (lo + hi) / 2 : 0) : g["center"].get<double>();
    const bool explicit_tolerance = !analysis["tolerance"].is_null();
    const bool single = !both && (haslo || hashi) && !explicit_tolerance;
    const bool automatic_single_center =
        single && g["center"].is_null() && analysis["historical_mean"].is_null();
    double width = explicit_tolerance ? analysis["tolerance"].get<double>() : hi - lo;
    if (single) {
        const double reference = analysis["historical_mean"].is_null()
                                     ? center
                                     : analysis["historical_mean"].get<double>();
        width = 2 * std::abs((haslo ? lo : hi) - reference);
    }
    // A requested center is a preference. With a single limit at that center,
    // placement may move the sample mean and give it a nonzero P/T denominator.
    bool estimate_pt_scale = !automatic_single_center && width > 0;
    auto pt = acceptance(c, "PT_pct");
    double sl = 0, sh = INFINITY;
    if (pt && grr && estimate_pt_scale) {
        double factor = width / (100 * c["analysis"]["study_multiplier"].get<double>() * grr);
        sl = pt->first * factor;
        sh = pt->second * factor;
        if (single && strict && analysis["historical_mean"].is_null() && std::isfinite(sh)) {
            const double grid = haslo ? grid_lo : grid_hi;
            const double center_distance = haslo ? center - grid : grid - center;
            const double extent = haslo ? high : -low;
            const double farthest_reading = std::max(center_distance + sh * extent, sh * span);
            const double margin = 8 * std::numeric_limits<double>::epsilon() *
                                  std::max(std::abs(grid), std::abs(center));
            // Below half a grid step, every originally proposed reading rounds to the same
            // limit. Let the final measured P/T decide instead of forcing a constant sample.
            if (farthest_reading + margin < .5 * std::pow(10., -dp)) {
                estimate_pt_scale = false;
                sl = 0;
                sh = INFINITY;
            }
        }
    }
    if (strict && both && span)
        sh = std::min(sh, (hi - lo) / span);
    double preferred_span_scale = 0;
    const bool full_part_span = strict && both && g["minimum_part_span_pct"] == 100;
    const bool guide_part_span =
        !full_part_span && minimum_part_span > 0 && g["scale_mode"] == "auto";
    if (guide_part_span) {
        const double span_at_unit_scale = part_mean_span(matrix);
        if (span_at_unit_scale > 0) {
            // Rounding can move the difference of two part means by one output unit.
            const double output_unit = std::pow(10., -dp);
            sl = std::max(sl, (minimum_part_span - output_unit) / span_at_unit_scale);
            preferred_span_scale = minimum_part_span / span_at_unit_scale;
        }
        if (strict && span)
            sh = std::min(sh, (grid_hi - grid_lo) / span);
    }
    if (sl > sh)
        return {};
    const bool reference_scale = uses_reference_scale(g, s);
    double desired = reference_scale
                         ? (g["reference_tv_sd"].is_null() ? analysis["historical_sd"].get<double>()
                                                           : g["reference_tv_sd"].get<double>())
                         : (hi - lo) * .60 / std::max(span, 1e-12);
    auto &t = c["targets"]["PT_pct"];
    if (!t.is_null() && (t["mode"] == "target" || t["mode"] == "between") && grr &&
        estimate_pt_scale)
        desired = preferred(c, "PT_pct", 10) * width /
                  (100 * c["analysis"]["study_multiplier"].get<double>() * grr);
    if (guide_part_span)
        desired = std::max(desired, preferred_span_scale);
    double scale = std::min(sh, std::max(sl, desired));
    if (attempt % 5 != 0) {
        if (std::isfinite(sh) && sh > sl)
            scale = rng.uniform(std::max(sl, sh * .15), sh);
        else if (!pt) {
            scale *= rng.uniform(.7, 1.3);
            if (guide_part_span)
                scale = std::clamp(scale, sl, sh);
        }
    }
    if (!analysis["historical_sd"].is_null() && analysis["process_variation"] == "historical") {
        const double anchor = reference_scale && !g["reference_tv_sd"].is_null()
                                  ? g["reference_tv_sd"].get<double>()
                                  : analysis["historical_sd"].get<double>();
        // 双侧自动模式可按规格生成样本，报告仍使用配置的历史过程口径。
        const bool flexible = !reference_scale;
        const double limit = flexible && strict && span
                                 ? std::min(sh, (grid_hi - grid_lo) / span)
                                 : sh;
        if (anchor >= sl && anchor <= limit)
            scale = anchor;
        else if (!flexible || sl > limit)
            return {};
        else
            scale = std::min(scale, limit);
    }
    if (scale <= 0 || !std::isfinite(scale))
        return {};
    if (full_part_span)
        return place_full_part_span(matrix, scale, grid_lo, grid_hi, dp);
    if (automatic_single_center && pt && grr) {
        const double target = std::clamp(preferred(c, "PT_pct", 10), pt->first, pt->second);
        if (target <= 0)
            return {};
        const double distance =
            100 * analysis["study_multiplier"].get<double>() * grr * scale / (2 * target);
        center = haslo ? lo + distance : hi - distance;
    }
    if (strict) {
        const double al = grid_lo - scale * low, ah = grid_hi - scale * high;
        if (al > ah)
            return {};
        center = std::min(ah, std::max(al, center));
    }
    Matrix result = matrix;
    for (auto &row : result)
        for (auto &v : row)
            v = parse_number(fixed(center + scale * v, dp));
    return result;
}
double adjusted_normality_statistic(const Matrix &matrix) {
    std::vector<double> values;
    values.reserve(matrix.size() * matrix.front().size());
    const double origin = matrix.front().front();
    for (const auto &row : matrix)
        for (double value : row)
            values.push_back(value - origin);
    std::sort(values.begin(), values.end());

    const double count = static_cast<double>(values.size());
    const double mean = accurate_sum(values) / count;
    double sum_squares = 0;
    for (double value : values) {
        const double deviation = value - mean;
        sum_squares += deviation * deviation;
    }
    const double normal_scale = std::sqrt(2 * (sum_squares / (count - 1)));
    if (!(normal_scale > 0) || !std::isfinite(normal_scale))
        return INFINITY;

    double tail_sum = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double lower = (values[i] - mean) / normal_scale;
        const double upper = (values[values.size() - 1 - i] - mean) / normal_scale;
        tail_sum += (2.0 * static_cast<double>(i) + 1) *
                    (std::log(.5 * std::erfc(-lower)) + std::log(.5 * std::erfc(upper)));
    }
    const double statistic = -count - tail_sum / count;
    return statistic * (1 + .75 / count + 2.25 / (count * count));
}
// Stephens' fitted-normal reference is used as a shape criterion for grouped measurements.
constexpr double normality_threshold = .752;
std::pair<double, double> score(const Json &m, const Json &c) {
    double total = 0;
    for (auto k : metric_keys) {
        const auto &t = c["targets"][k];
        if (t.is_null() || (t["mode"] != "target" && t["mode"] != "between") || m[k].is_null())
            continue;
        auto b = *acceptance(c, k);
        double width = std::max(b.second - b.first, std::string(k) == "ndc" ? 1. : 1e-6);
        double v = (m[k].get<double>() - preferred(c, k, 0)) / width;
        total += v * v;
    }
    double error = m["center_error"].is_null() ? 0 : std::abs(m["center_error"].get<double>());
    error = parse_number(fixed(error, c["generation"]["decimal_places"].get<int>() + 4));
    return !c["generation"]["center"].is_null() && c["generation"]["priority"] == "center"
               ? std::make_pair(error, total)
               : std::make_pair(total, error);
}
} // namespace
Result generate(const Json &raw, const std::atomic<bool> *cancel, Progress progress) {
    Json c = validate_request(raw);
    std::int64_t seed;
    if (c["generation"]["seed"].is_null()) {
        std::random_device rd;
        seed = std::int64_t(((std::uint64_t(rd()) << 32) ^ rd()) & 0x7fffffffffffffffULL);
    } else
        seed = c["generation"]["seed"];
    Random rng(seed);
    auto ratio = ratios(c);
    Json construction = c;
    const auto &analysis = c["analysis"];
    const auto &generation = c["generation"];
    const auto &specification = c["specification"];
    const bool require_normality = generation["require_normality"].get<bool>();
    const double minimum_part_span =
        generation["minimum_part_span_pct"].is_null()
            ? 0.
            : std::max(std::numeric_limits<double>::denorm_min(),
                       (generation["minimum_part_span_pct"].get<double>() / 100) *
                           (specification["usl"].get<double>() - specification["lsl"].get<double>()));
    const bool with_optional_reports =
        analysis["method"] == "crossed_anova" &&
        (analysis["confidence"]["enabled"] == true ||
         (analysis["misclassification"] == true &&
          (!specification["lsl"].is_null() || !specification["usl"].is_null())));
    Json screening = c;
    if (with_optional_reports) {
        // 置信区间和误分类概率不参与候选筛选与评分。
        screening["analysis"]["confidence"]["enabled"] = false;
        screening["analysis"]["misclassification"] = false;
    }
    if (!analysis["historical_sd"].is_null() && analysis["process_variation"] == "historical" &&
        uses_reference_scale(generation, specification) && !generation["reference_tv_sd"].is_null() &&
        !pt_excludes_historical_variation(c)) {
        // 历史SD定义报告分母，参考TV_SD定义数据本身的尺度；保留两项独立含义。
        const double factor =
            analysis["historical_sd"].get<double>() / generation["reference_tv_sd"].get<double>();
        for (auto &value : ratio)
            value *= factor;
        for (int index : {1, 3, 5})
            ratio[index] = std::min(1., ratio[index]);
        if (ratio[0] > ratio[1] || ratio[2] > ratio[3] || ratio[4] > ratio[5])
            throw InfeasibleError("参考数据尺度无法容纳历史过程口径的测量变异约束。");
        for (const auto *key : {"EV_pct", "AV_pct", "GRR_pct"}) {
            auto &target = construction["targets"][key];
            if (!target.is_object())
                continue;
            for (const auto *field : {"value", "min", "max", "tolerance_pp"})
                if (target.contains(field))
                    target[field] = target[field].get<double>() * factor;
        }
        // ndc 已在原始比例约束中合并；构造阶段不再按研究部件重新解释其偏好。
        construction["targets"]["ndc"] = nullptr;
    }
    auto start = std::chrono::steady_clock::now();
    auto seconds = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::optional<Result> best;
    std::pair<double, double> best_score{};
    int found = -1, attempts = 0;
    const auto check_cancelled = [cancel] {
        if (cancel && cancel->load())
            throw CancelledError("已取消，未导出数据。");
    };
    int rejected_construction = 0, rejected_placement = 0, rejected_indicators = 0,
        rejected_span = 0, rejected_normality = 0, accepted = 0;
    const auto consider = [&](int attempt) {
        auto n = normalized(construction, ratio, rng, attempt);
        if (!n) {
            ++rejected_construction;
            return;
        }
        auto matrix = scale_place(n->first, n->second, c, rng, attempt, minimum_part_span);
        if (!matrix) {
            ++rejected_placement;
            return;
        }
        auto m = calculate(*matrix, screening);
        if (!qualified(m, c)) {
            ++rejected_indicators;
            return;
        }
        if (minimum_part_span > 0 && part_mean_span(*matrix) < minimum_part_span) {
            ++rejected_span;
            return;
        }
        if (require_normality && !(adjusted_normality_statistic(*matrix) <= normality_threshold)) {
            ++rejected_normality;
            return;
        }
        ++accepted;
        auto sc = score(m, c);
        if (!best || sc < best_score) {
            best = Result{std::move(*matrix), std::move(m), c, seed};
            best_score = sc;
        }
        if (found < 0)
            found = attempt;
    };
    std::string stop_reason = "max_attempts";
    const int max_attempts = generation["max_attempts"];
    const double timeout = generation["timeout_seconds"];
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        check_cancelled();
        if (seconds() >= timeout) {
            stop_reason = "timeout";
            break;
        }
        if (found >= 0 && attempt - found > 120) {
            stop_reason = "refinement_limit";
            break;
        }
        attempts = attempt + 1;
        consider(attempt);
        if (progress && attempt % 100 == 0)
            progress(attempt + 1, seconds(), best.has_value());
        if (best && best_score.first == 0 && best_score.second == 0) {
            stop_reason = "optimal_score";
            break;
        }
    }
    if (progress)
        progress(attempts, seconds(), best.has_value());
    check_cancelled();
    Json diagnostics{{"stop_reason", stop_reason},
                     {"attempts", attempts},
                     {"seed", seed},
                     {"accepted", accepted},
                     {"rejected", {{"construction", rejected_construction},
                                   {"placement", rejected_placement},
                                   {"indicators", rejected_indicators},
                                   {"part_span", rejected_span},
                                   {"normality", rejected_normality}}}};
    if (!best) {
        const char *hint = require_normality && minimum_part_span > 0
                               ? "尚未找到同时满足规格、指标、最低部件跨度和近似正态要求的数据。请尝试"
                                 "降低最低部件跨度或调整指标、小数位、正态筛选和搜索预算。"
                           : minimum_part_span > 0
                               ? "尚未找到同时满足规格、指标和最低部件跨度的数据。请尝试降低最低部件跨度"
                                 "或调整指标、小数位和搜索预算。"
                           : require_normality
                               ? "尚未找到同时满足生成约束和近似正态要求的数据。请调整指标约束、小数位或"
                                 "搜索预算，也可关闭“要求近似正态”。"
                               : "尚未找到满足条件的数据（不代表已证明无解）。请调整指标误差、小数位"
                                 "、规格或搜索限制。";
        const std::string reason = stop_reason == "timeout" ? "达到搜索时间上限" : "达到搜索次数上限";
        throw SearchError(reason + "，已尝试 " + std::to_string(attempts) + " 次。" +
                              "淘汰计数：构造 " + std::to_string(rejected_construction) +
                              "，缩放放置 " + std::to_string(rejected_placement) +
                              "，规格及指标 " + std::to_string(rejected_indicators) +
                              "，部件跨度 " + std::to_string(rejected_span) + "，正态筛选 " +
                              std::to_string(rejected_normality) + "。" + hint + "种子：" +
                              std::to_string(seed),
                          diagnostics);
    }
    if (with_optional_reports) {
        best->metrics = calculate(best->matrix, c);
        check_cancelled();
    }
    best->attempts = attempts;
    const double span = part_mean_span(best->matrix);
    diagnostics["part_mean_span"] = span;
    diagnostics["part_mean_span_pct"] = nullptr;
    if (!specification["lsl"].is_null() && !specification["usl"].is_null()) {
        const double width = specification["usl"].get<double>() - specification["lsl"].get<double>();
        const double percentage = 100 * (span / width);
        if (std::isfinite(percentage))
            diagnostics["part_mean_span_pct"] = percentage;
    }
    const double statistic = adjusted_normality_statistic(best->matrix);
    diagnostics["anderson_darling"] = {
        {"adjusted", std::isfinite(statistic) ? Json(statistic) : Json(nullptr)},
        {"threshold", normality_threshold},
        {"required", require_normality}};
    best->search_diagnostics = std::move(diagnostics);
    best->elapsed_seconds = seconds();
    return *best;
}
} // namespace msa
