#pragma once
#include "nlohmann/json.hpp"
#include "msa/version.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace msa {
using Json = nlohmann::json;
void apply_analysis_options(Json &metrics, const Json &cfg);
Json anova_confidence_intervals(const Json &anova, const Json &cfg);
void validate_mls_confidence(int operators, const Json &confidence);
double chi_square_quantile(double probability, double df);
using Matrix = std::vector<std::vector<double>>;
using Bounds = std::optional<std::pair<double, double>>;
inline constexpr std::array<const char *, 5> metric_keys{"EV_pct", "AV_pct", "GRR_pct", "PT_pct",
                                                         "ndc"};
struct ConfigError : std::invalid_argument {
    using invalid_argument::invalid_argument;
};
struct InfeasibleError : std::invalid_argument {
    using invalid_argument::invalid_argument;
};
struct SearchError : std::runtime_error {
    using runtime_error::runtime_error;
};
struct CancelledError : std::runtime_error {
    using runtime_error::runtime_error;
};
struct FileExistsError : std::runtime_error {
    using runtime_error::runtime_error;
};
enum class ExistingFile { Replace, Preserve };
Json default_config();
Json normalize_config(const Json &raw);
Json validate_request(const Json &raw);
Bounds acceptance(const Json &cfg, const std::string &key);
std::array<double, 6> ratios(const Json &cfg);
double accurate_sum(const std::vector<double> &values);
double grid_bound(double value, int dp, bool lower);
std::string fixed(double value, int dp);
double parse_number(const std::string &text);
double ev_divisor(int operators, int replicates, int parts);
double av_divisor(int operators);
double pv_divisor(int parts);
Json calculate(const Matrix &matrix, const Json &cfg);
double f_survival(double f, int numerator_df, int denominator_df);
Json crossed_anova(const Matrix &matrix, const Json &cfg);
Json checks(const Json &metrics, const Json &cfg);
bool qualified(const Json &metrics, const Json &cfg);
struct ExportStudyState;
std::shared_ptr<ExportStudyState> make_export_study_state();
struct Result {
    Matrix matrix;
    Json metrics, config;
    std::int64_t seed = 0;
    int attempts = 0;
    double elapsed_seconds = 0;
    // Copies of one study share its lazily chosen export-only random seed.
    std::shared_ptr<ExportStudyState> export_study = make_export_study_state();
    Json as_json() const;
};
struct FilenameVariable {
    const char *name, *alias, *description, *example;
};
const std::vector<FilenameVariable> &filename_variables();
void validate_filename_template(const std::string &name);
Json normalize_output(const Json &output);
std::string render_filename(const std::string &name, const Result &result,
                            std::chrono::system_clock::time_point when);
std::filesystem::path configured_csv_path(const Result &result, const Json &output,
                                          std::chrono::system_clock::time_point when);
std::filesystem::path
export_configured_csv(const Result &result, const Json &output,
                      std::chrono::system_clock::time_point when = std::chrono::system_clock::now(),
                      const std::filesystem::path &reserved = {});
using Progress = std::function<void(int, double, bool)>;
Result generate(const Json &raw, const std::atomic<bool> *cancel = nullptr, Progress progress = {});
std::filesystem::path config_path();
// GUI quick export: empty/relative directories are anchored to the user config
// directory, never the process working directory. CLI explicit paths are unchanged.
std::filesystem::path configured_csv_path(const Json &cfg);
void atomic_text(const std::filesystem::path &path, const std::string &bytes,
                 ExistingFile policy = ExistingFile::Replace);
std::string read_text(const std::filesystem::path &path);
void save_config(const Json &cfg, const std::filesystem::path &path = config_path());
Json load_config(const std::filesystem::path &path = config_path());
std::string csv_text(const Matrix &matrix, int dp);
Matrix parse_csv(const std::string &text);
Matrix read_csv(const std::filesystem::path &path);
struct MeasurementRecord {
    int operator_number, part, trial, standard_order;
    double measurement;
};
std::vector<MeasurementRecord> export_records(const Result &result, const Json &output);
std::string export_csv_text(const Result &result, const Json &output);
Matrix parse_export_csv(const std::string &text, const Json &config);
Json export_csv(const Result &result, const std::filesystem::path &path,
                ExistingFile policy = ExistingFile::Replace);
Json export_csv(const Result &result, const std::filesystem::path &path, const Json &output,
                ExistingFile policy = ExistingFile::Replace);
} // namespace msa
