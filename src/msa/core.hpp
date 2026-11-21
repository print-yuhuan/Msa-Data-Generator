#pragma once
#include "nlohmann/json.hpp"
#include "msa/version.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
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
    Json diagnostics;
    SearchError(const std::string &message, const Json &details)
        : std::runtime_error(message), diagnostics(details) {}
};
struct CancelledError : std::runtime_error {
    using runtime_error::runtime_error;
};
struct FileExistsError : std::runtime_error {
    using runtime_error::runtime_error;
};
enum class ExistingFile { Replace, Preserve };
Json default_config();
std::array<std::string, 3> operator_names(const Json &design);
Json normalize_config(const Json &raw);
void require_config_version(const Json &config);
Json validate_request(const Json &raw);
Bounds acceptance(const Json &cfg, const std::string &key);
std::array<double, 6> ratios(const Json &cfg);
double accurate_sum(const std::vector<double> &values);
double grid_bound(double value, int dp, bool lower);
std::string fixed(double value, int dp);
double parse_number(const std::string &text);
std::int64_t parse_integer(const std::string &text);
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
    Json search_diagnostics;
    Json as_json() const;
};
struct FilenameVariable {
    const char *name, *alias, *description, *example;
};
const std::vector<FilenameVariable> &filename_variables();
void validate_filename_template(const std::string &name);
std::string normalize_filename_template(const std::string &name, const std::string &file_type);
enum class ExportBasenameKind { Chart, ComparisonReport, Data };
enum class StudySource { Generated, ImportedData };
std::string normalize_export_basename(const std::string &name, ExportBasenameKind kind,
                                      const std::string &extension = {});
std::string normalize_import_filename(const std::string &name);
inline constexpr char default_chart_basename[] = "MSA_Charts";
inline constexpr char default_report_basename[] = "MSA_Comparison";
struct ChartPreferences {
    int style = 0, format = 0, scope = 0, columns = 1, resolution = 1;
    std::array<bool, 7> included{true, true, true, true, true, true, false};
    // GUI choices use zero-based indices. Config files use stable keys, column counts and pixels.
    void write_to(Json &charts) const;
    static ChartPreferences read(const Json &charts);
};
Json normalize_output(const Json &output);
std::string render_study_basename(const std::string &name, ExportBasenameKind kind,
                                  const Result &study, StudySource source,
                                  std::chrono::system_clock::time_point when,
                                  const std::string &extension);
std::string render_import_basename(const std::string &name, const Json &options,
                                   const Result *active_study, StudySource source,
                                   std::chrono::system_clock::time_point when,
                                   const std::string &extension = {});
std::filesystem::path configured_data_path(const Result &result, const Json &output,
                                           std::chrono::system_clock::time_point when);
std::filesystem::path
export_configured_data(const Result &result, const Json &output,
                       std::chrono::system_clock::time_point when = std::chrono::system_clock::now(),
                       const std::filesystem::path &reserved_report = {},
                       const std::filesystem::path &source_config = {},
                       const std::filesystem::path &comparison_report = {}, Json *receipt = nullptr);
using Progress = std::function<void(int, double, bool)>;
Result generate(const Json &raw, const std::atomic<bool> *cancel = nullptr, Progress progress = {});
std::filesystem::path config_path();
std::filesystem::path desktop_directory();
std::filesystem::path resolve_output_directory(const std::string &directory);
// Data directories use the system desktop as their base. Explicit CLI file paths use the CWD.
std::filesystem::path configured_data_path(const Json &cfg);
void atomic_text(const std::filesystem::path &path, const std::string &bytes,
                 ExistingFile policy = ExistingFile::Replace);
bool same_file_path(const std::filesystem::path &a, const std::filesystem::path &b);
inline constexpr std::size_t csv_read_limit = 4 * 1024 * 1024;
inline constexpr std::size_t config_read_limit = 64 * 1024 * 1024;
std::string read_file_bytes(const std::filesystem::path &path, std::size_t max_bytes);
std::string read_text(const std::filesystem::path &path,
                      std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)());
void save_config(const Json &cfg, const std::filesystem::path &path = config_path());
Json load_config(const std::filesystem::path &path = config_path());
std::string csv_text(const Matrix &matrix, int dp);
Matrix parse_csv(const std::string &text);
std::string data_file_type(const std::filesystem::path &path);
using WorksheetSelector =
    std::function<std::optional<std::size_t>(const std::vector<std::string> &)>;
// Used only for six unlabeled numeric rows. A canceled choice cancels the import.
using ReplicateSelector = std::function<std::optional<int>()>;
struct ImportedData {
    Matrix matrix;
    std::string sheet_name;
    Json config;
};
ImportedData import_csv_study(const std::string &text, const Json &config);
std::optional<ImportedData> import_csv_study(const std::string &text, const Json &config,
                                             const ReplicateSelector &choose_replicates);
ImportedData read_study_data(const std::filesystem::path &path, const Json &config,
                             std::optional<int> replicates = std::nullopt);
ImportedData read_study_data(const std::filesystem::path &path, const Json &config,
                             const std::string &sheet_name,
                             std::optional<int> replicates = std::nullopt);
std::optional<ImportedData> read_study_data(const std::filesystem::path &path, const Json &config,
                                            const WorksheetSelector &selector,
                                            const ReplicateSelector &choose_replicates = {});
Matrix read_data(const std::filesystem::path &path, const Json &config);
Matrix read_data(const std::filesystem::path &path, const Json &config,
                 const std::string &sheet_name);
std::optional<ImportedData> read_data(const std::filesystem::path &path, const Json &config,
                                      const WorksheetSelector &selector);
struct MeasurementRecord {
    int operator_number, part, trial, standard_order;
    double measurement;
};
std::vector<MeasurementRecord> export_records(const Result &result, const Json &output);
std::string export_csv_text(const Result &result, const Json &output);
Json effective_run_seed(const Result &result, const Json &output);
std::string sha256(const std::string &bytes);
void replay_data(const Json &report, const std::filesystem::path &path, ExistingFile policy);
Json export_data(const Result &result, const std::filesystem::path &path,
                 ExistingFile policy = ExistingFile::Replace);
Json export_data(const Result &result, const std::filesystem::path &path, const Json &output,
                 ExistingFile policy = ExistingFile::Replace, Json *receipt = nullptr);
Matrix parse_export_csv(const std::string &text, const Json &config);
} // namespace msa
