#pragma once

#include "core.hpp"
#include <cmath>

namespace msa::detail {
struct Tolerance {
    double width;
    const char *source;
};

inline Tolerance effective_tolerance(const Json &analysis, const Json &specification,
                                     std::optional<double> reference_mean = std::nullopt) {
    const auto explicit_tolerance = analysis.find("tolerance");
    if (explicit_tolerance != analysis.end() && !explicit_tolerance->is_null())
        return {explicit_tolerance->get<double>(), "interval"};

    const auto &lsl = specification["lsl"], &usl = specification["usl"];
    if (!lsl.is_null() && !usl.is_null())
        return {usl.get<double>() - lsl.get<double>(), "two_sided"};
    if (reference_mean && !lsl.is_null())
        return {2 * std::abs(lsl.get<double>() - *reference_mean), "lower_only"};
    if (reference_mean && !usl.is_null())
        return {2 * std::abs(usl.get<double>() - *reference_mean), "upper_only"};
    return {0, "none"};
}
} // namespace msa::detail
