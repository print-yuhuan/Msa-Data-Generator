#pragma once
#include "charts.hpp"

namespace msa {
void write_comparison_report(const std::filesystem::path &path, const Result &study,
                             StudySource source, const std::string &source_name,
                             ExistingFile policy = ExistingFile::Preserve,
                             std::optional<charts::Style> style = std::nullopt);
} // namespace msa
