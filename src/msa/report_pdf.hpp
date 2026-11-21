#pragma once
#ifdef _WIN32
#include <filesystem>
#endif
#include <string>

namespace msa {
std::string print_report_pdf(const std::string &html);
#ifdef _WIN32
namespace detail {
std::string print_report_pdf_with_browser(const std::string &html,
                                          const std::filesystem::path &browser);
}
#endif
}
