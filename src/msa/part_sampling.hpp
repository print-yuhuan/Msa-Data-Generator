#pragma once
#include <array>
#include <string_view>

namespace msa {
enum class PartSampling {
    Default, UniformRandom, Stratified, Even, NormalQuantile, CenterDense, EndDense,
    LowDense, HighDense, TwoCluster
};

struct PartSamplingMethod {
    PartSampling mode;
    const char *key;
    const char *label;
    const char *description;
};

inline constexpr std::array<PartSamplingMethod, 10> part_sampling_methods{{
    {PartSampling::Default, "default", "默认取样", "两端各取一点，中间随机取样。"},
    {PartSampling::UniformRandom, "uniform_random", "随机取样",
     "每件部件的基础尺寸独立地在上下规格限之间均匀抽取。"},
    {PartSampling::Stratified, "stratified", "分层随机", "将区间等分，每段随机取一件。"},
    {PartSampling::Even, "even", "等距覆盖", "在基础尺寸区间内等距离布置部件。"},
    {PartSampling::NormalQuantile, "normal_quantile", "正态分位",
     "按正态分布等概率分段，取各段概率中点对应的尺寸。"},
    {PartSampling::CenterDense, "center_dense", "中心加密", "中心附近部件较密，向两端逐渐稀疏。"},
    {PartSampling::EndDense, "end_dense", "两端加密", "靠近两端的部件较密，中心附近较稀疏。"},
    {PartSampling::LowDense, "low_dense", "低值加密", "低值一侧部件最密，向高值一侧逐渐拉开间距。"},
    {PartSampling::HighDense, "high_dense", "高值加密", "高值一侧部件最密，向低值一侧逐渐拉开间距。"},
    {PartSampling::TwoCluster, "two_cluster", "双批取样",
     "在低值和高值区间各分层随机取约半数部件，模拟两批尺寸之间有明显间隔。"},
}};

inline const PartSamplingMethod *find_part_sampling(std::string_view key) {
    for (const auto &method : part_sampling_methods)
        if (method.key == key)
            return &method;
    return nullptr;
}
} // namespace msa
