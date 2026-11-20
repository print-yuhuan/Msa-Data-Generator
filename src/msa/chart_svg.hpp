#pragma once
#include "charts.hpp"
#include <ostream>

namespace msa::charts {
std::string xml_text(const std::string &value);
// prefix distinguishes text clipping paths when several scenes share a document.
void write_svg_items(std::ostream &out, const Scene &scene, const std::string &prefix);
} // namespace msa::charts
