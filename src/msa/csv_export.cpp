#include "core.hpp"
#include <algorithm>
#include <mutex>
#include <numeric>
#include <random>

namespace msa {
struct ExportStudyState {
    std::once_flag once;
    std::uint64_t seed = 0;
};
std::shared_ptr<ExportStudyState> make_export_study_state() {
    return std::make_shared<ExportStudyState>();
}
namespace {
struct Design {
    int operators, trials, parts;
    explicit Design(const Json &config)
        : operators(config.at("design").at("operators")),
          trials(config.at("design").at("replicates")), parts(config.at("design").at("parts")) {}
    int index(int op, int trial) const { return (op - 1) * trials + trial - 1; }
    int standard(int op, int part, int trial) const {
        return ((trial - 1) * parts + part - 1) * operators + op;
    }
    void check(const Matrix &matrix) const {
        if (int(matrix.size()) != operators * trials)
            throw ConfigError("测量矩阵行数与人员、测量次数不符");
        for (const auto &row : matrix)
            if (int(row.size()) != parts)
                throw ConfigError("测量矩阵列数与产品件数不符");
    }
};
std::uint64_t study_seed(const Result &result, const Json &order) {
    if (!order.at("seed").is_null())
        return order.at("seed").get<std::uint64_t>();
    if (!result.export_study)
        throw ConfigError("研究的运行顺序状态不可用");
    auto state = result.export_study;
    std::call_once(state->once, [state] {
        // Independent entropy and engine; never touches the generation RNG.
        std::random_device random;
        state->seed = ((std::uint64_t(random()) << 32) ^ random()) & 0x7FFFFFFFFFFFFFFFULL;
        if (!state->seed)
            state->seed = 1;
    });
    return state->seed;
}
template <class T> void shuffle(std::vector<T> &items, std::mt19937_64 &random) {
    // Explicit rejection-sampled Fisher-Yates, stable across standard libraries.
    for (std::size_t n = items.size(); n > 1; --n) {
        const auto bound = std::uint64_t(n), threshold = (std::uint64_t(0) - bound) % bound;
        std::uint64_t value;
        do {
            value = random();
        } while (value < threshold);
        std::swap(items[n - 1], items[std::size_t(value % bound)]);
    }
}
} // namespace
Json effective_run_seed(const Result &result, const Json &output) {
    if (output.at("format") == "default" || output.at("run_order").at("mode") == "standard")
        return nullptr;
    return study_seed(result, output.at("run_order"));
}
std::vector<MeasurementRecord> export_records(const Result &result, const Json &raw) {
    const auto cfg = normalize_config(result.config), output = normalize_output(raw);
    const Design d(cfg);
    d.check(result.matrix);
    std::vector<MeasurementRecord> records;
    for (int trial = 1; trial <= d.trials; ++trial)
        for (int part = 1; part <= d.parts; ++part)
            for (int op = 1; op <= d.operators; ++op)
                records.push_back({op, part, trial, d.standard(op, part, trial),
                                   result.matrix[d.index(op, trial)][part - 1]});
    const auto &order = output["run_order"];
    // Wide output always ignores run-order settings, including the optional seed.
    if (output["format"] == "default" || order["mode"] == "standard")
        return records;
    std::mt19937_64 random(study_seed(result, order));
    if (order["mode"] == "all") {
        shuffle(records, random);
        return records;
    }
    std::vector<int> operators(d.operators);
    std::iota(operators.begin(), operators.end(), 1);
    if (order["randomize_operators"].get<bool>())
        shuffle(operators, random);
    std::vector<MeasurementRecord> arranged;
    for (int trial = 1; trial <= d.trials; ++trial)
        for (const int op : operators) {
            std::vector<int> parts(d.parts);
            std::iota(parts.begin(), parts.end(), 1);
            shuffle(parts, random);
            for (const int part : parts)
                arranged.push_back(records[d.standard(op, part, trial) - 1]);
        }
    return arranged;
}
std::string export_csv_text(const Result &result, const Json &raw) {
    const auto cfg = normalize_config(result.config), output = normalize_output(raw);
    const Design d(cfg);
    d.check(result.matrix);
    const int dp = cfg["generation"]["decimal_places"];
    if (output["format"] == "default") {
        std::string text("\xef\xbb\xbf");
        for (int part = 1; part <= d.parts; ++part)
            text += ',' + std::to_string(part);
        text += "\r\n";
        for (int op = 1; op <= d.operators; ++op)
            for (int trial = 1; trial <= d.trials; ++trial) {
                text += char('A' + op - 1);
                for (const double value : result.matrix[d.index(op, trial)])
                    text += ',' + fixed(value, dp);
                text += "\r\n";
            }
        return text;
    }
    const bool standard = output["run_order"]["store_standard_order"];
    std::string text = standard ? "StdOrder,Operator,Part,Trial,Measurement\r\n"
                                : "Operator,Part,Trial,Measurement\r\n";
    for (const auto &record : export_records(result, output)) {
        if (standard)
            text += std::to_string(record.standard_order) + ',';
        text += char('A' + record.operator_number - 1);
        text += ',' + std::to_string(record.part) + ',' + std::to_string(record.trial) + ',';
        text += fixed(record.measurement, dp) + "\r\n";
    }
    return text;
}
} // namespace msa
