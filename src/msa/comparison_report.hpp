#pragma once
#include "core.hpp"

namespace msa {
void write_comparison_report(const std::filesystem::path &path, const Result &study,
                             StudySource source, const std::string &source_name,
                             ExistingFile policy = ExistingFile::Preserve);
} // namespace msa
