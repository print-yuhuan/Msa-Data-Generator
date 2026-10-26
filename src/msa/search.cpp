#include "core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
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
std::vector<double> profile(int n, Random &rng) {
    std::vector<double> seq{0, 1};
    for (int i = 2; i < n; ++i)
        seq.push_back(rng.uniform(.05, .95));
    rng.shuffle(seq);
    double mean = accurate_sum(seq) / n;
    for (auto &v : seq)
        v -= mean;
    return seq;
}
std::optional<std::pair<Matrix, double>>
normalized(const Json &c, const std::array<double, 6> &ratio, Random &rng, int attempt) {
    auto [el, eh, al, ah, gl, gh] = ratio;
    int o = c["design"]["operators"], r = c["design"]["replicates"], p = c["design"]["parts"];
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
        auto parts = profile(p, rng), operators = profile(o, rng);
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
        for (int i = 0; i < o; ++i)
            for (int j = 0; j < p; ++j) {
                const auto residual = profile(r, rng);
                residuals.insert(residuals.end(), residual.begin(), residual.end());
            }
        scale_norm(residuals, dfe * ev * ev);
        for (int i = 0; i < o; ++i)
            for (int j = 0; j < p; ++j)
                for (int k = 0; k < r; ++k)
                    matrix[i * r + k][j] =
                        parts[j] + operators[i] + flat[i * p + j] + residuals[(i * p + j) * r + k];
        return std::make_pair(matrix, g);
    }
    auto parts = profile(p, rng);
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
std::optional<Matrix> scale_place(const Matrix &matrix, double grr, const Json &c, Random &rng,
                                  int attempt) {
    auto &s = c["specification"];
    auto &g = c["generation"];
    const auto &analysis = c["analysis"];
    bool haslo = !s["lsl"].is_null(), hashi = !s["usl"].is_null(), both = haslo && hashi;
    double lo = haslo ? s["lsl"].get<double>() : -INFINITY,
           hi = hashi ? s["usl"].get<double>() : INFINITY;
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
    auto pt = acceptance(c, "PT_pct");
    double sl = 0, sh = INFINITY;
    if (pt && grr && !automatic_single_center) {
        double factor = width / (100 * c["analysis"]["study_multiplier"].get<double>() * grr);
        sl = pt->first * factor;
        sh = pt->second * factor;
    }
    if (s["compliance"] == "strict" && both && span)
        sh = std::min(sh, (hi - lo) / span);
    if (sl > sh)
        return {};
    double desired = g["scale_mode"] == "reference_tv_sd" || !both
                         ? (g["reference_tv_sd"].is_null() ? analysis["historical_sd"].get<double>()
                                                           : g["reference_tv_sd"].get<double>())
                         : (hi - lo) * .60 / std::max(span, 1e-12);
    auto &t = c["targets"]["PT_pct"];
    if (!t.is_null() && (t["mode"] == "target" || t["mode"] == "between") && grr &&
        !automatic_single_center)
        desired = preferred(c, "PT_pct", 10) * width /
                  (100 * c["analysis"]["study_multiplier"].get<double>() * grr);
    double scale = std::min(sh, std::max(sl, desired));
    if (attempt % 5 != 0) {
        if (std::isfinite(sh) && sh > sl)
            scale = rng.uniform(std::max(sl, sh * .15), sh);
        else if (!pt)
            scale *= rng.uniform(.7, 1.3);
    }
    if (!analysis["historical_sd"].is_null() && analysis["process_variation"] == "historical") {
        scale = g["scale_mode"] == "reference_tv_sd" && !g["reference_tv_sd"].is_null()
                    ? g["reference_tv_sd"].get<double>()
                    : analysis["historical_sd"].get<double>();
        if (scale < sl || scale > sh)
            return {};
    }
    if (scale <= 0 || !std::isfinite(scale))
        return {};
    if (automatic_single_center && pt && grr) {
        const double target = std::clamp(preferred(c, "PT_pct", 10), pt->first, pt->second);
        if (target <= 0)
            return {};
        const double distance =
            100 * analysis["study_multiplier"].get<double>() * grr * scale / (2 * target);
        center = haslo ? lo + distance : hi - distance;
    }
    int dp = g["decimal_places"];
    if (s["compliance"] == "strict") {
        double al = haslo ? grid_bound(lo, dp, true) - scale * low : -INFINITY,
               ah = hashi ? grid_bound(hi, dp, false) - scale * high : INFINITY;
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
    if (!analysis["historical_sd"].is_null() && analysis["process_variation"] == "historical" &&
        generation["scale_mode"] == "reference_tv_sd" && !generation["reference_tv_sd"].is_null()) {
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
    for (int attempt = 0; attempt < c["generation"]["max_attempts"].get<int>(); ++attempt) {
        if (cancel && cancel->load())
            throw CancelledError("已取消，未导出数据。");
        if (seconds() >= c["generation"]["timeout_seconds"].get<double>())
            break;
        // Check before candidate construction so continue cannot skip the budget.
        if (found >= 0 && attempt - found > 120)
            break;
        attempts = attempt + 1;
        auto n = normalized(construction, ratio, rng, attempt);
        if (!n)
            continue;
        auto matrix = scale_place(n->first, n->second, c, rng, attempt);
        if (!matrix)
            continue;
        auto m = calculate(*matrix, c);
        if (qualified(m, c)) {
            auto sc = score(m, c);
            if (!best || sc < best_score) {
                best = Result{*matrix, m, c, seed};
                best_score = sc;
            }
            if (found < 0)
                found = attempt;
        }
        if (progress && attempt % 100 == 0)
            progress(attempt + 1, seconds(), best.has_value());
    }
    if (progress)
        progress(attempts, seconds(), best.has_value());
    if (!best)
        throw SearchError("已尝试 " + std::to_string(attempts) +
                          " 次，尚未找到满足条件的数据（不代表已证明无解）。请调整指标误差、小数位"
                          "、规格或搜索限制。种子：" +
                          std::to_string(seed));
    best->attempts = attempts;
    best->elapsed_seconds = seconds();
    return *best;
}
} // namespace msa
