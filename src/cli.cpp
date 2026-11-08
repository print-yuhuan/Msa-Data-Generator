#include "msa/core.hpp"
#include "msa/license_text.hpp"
#include "msa/project.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <iomanip>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace {
std::atomic<bool> cancelled{false};
void on_signal(int) { cancelled = true; }
enum class ValueType { Integer, Number, NullableInteger, NullableNumber, Boolean, Text, Target };
struct Option {
    const char *name, *path;
    ValueType type;
    const char *description;
};
const Option options[]{
    {"--operators", "/design/operators", ValueType::Integer, "操作员人数：2 或 3，默认 3"},
    {"--replicates", "/design/replicates", ValueType::Integer,
     "每位操作员对每件部件的重复测量次数：2 或 3，默认 3；别名 --trials"},
    {"--parts", "/design/parts", ValueType::Integer, "部件数量：5～10，默认 10"},
    {"--method", "/analysis/method", ValueType::Text,
     "crossed_xbarr 均值极差法（默认）/ crossed_anova 方差分析法"},
    {"--interaction", "/analysis/interaction", ValueType::Text,
     "部件×操作员交互项：auto 自动（默认）/ include 保留 / exclude 合并"},
    {"--interaction-alpha", "/analysis/interaction_alpha", ValueType::Number,
     "交互项检验显著性水平：0 < alpha < 1，默认 0.05；仅 auto 使用"},
    {"--study-multiplier", "/analysis/study_multiplier", ValueType::Number,
     "研究变异的标准差倍数 k：0.000001～100，默认 6"},
    {"--analysis-tolerance", "/analysis/tolerance", ValueType::NullableNumber,
     "分析容差宽度：1e-12～2e12 或 null；优先用于 P/T，默认 null"},
    {"--historical-sd", "/analysis/historical_sd", ValueType::NullableNumber,
     "历史过程标准差：1e-12～1e12 或 null，默认 null"},
    {"--historical-mean", "/analysis/historical_mean", ValueType::NullableNumber,
     "历史过程均值：-1e12～1e12 或 null；默认 null，使用测量均值"},
    {"--process-variation", "/analysis/process_variation", ValueType::Text,
     "过程变异来源：historical 历史标准差（默认）/ study 研究数据"},
    {"--misclassification", "/analysis/misclassification", ValueType::Boolean,
     "误分类概率：true / false（默认）；仅 ANOVA 且至少有一个规格限时生效"},
    {"--confidence", "/analysis/confidence/enabled", ValueType::Boolean,
     "ANOVA 方差分量及派生指标置信区间：true / false（默认）"},
    {"--confidence-level", "/analysis/confidence/level", ValueType::Number,
     "置信水平百分数：0.01～99.99，默认 95；MLS 另有适用范围限制"},
    {"--confidence-bound", "/analysis/confidence/bound", ValueType::Text,
     "区间类型：both 双侧（默认）/ lower 单侧下限 / upper 单侧上限"},
    {"--confidence-method", "/analysis/confidence/method", ValueType::Text,
     "区间算法：mls（默认，Minitab）/ satterthwaite（JMP EMS）"},
    {"--lsl", "/specification/lsl", ValueType::NullableNumber, "下规格限：-1e12～1e12 或 null"},
    {"--usl", "/specification/usl", ValueType::NullableNumber, "上规格限：-1e12～1e12 或 null"},
    {"--compliance", "/specification/compliance", ValueType::Text,
     "规格符合性：strict 严格（默认）/ relaxed 宽松，允许超出规格限"},
    {"--center", "/generation/center", ValueType::NullableNumber,
     "数据中心：-1e12～1e12 或 null；默认 null，自动确定"},
    {"--priority", "/generation/priority", ValueType::Text,
     "生成优先级：center 中心优先（默认）/ indicators 指标优先"},
    {"--scale-mode", "/generation/scale_mode", ValueType::Text,
     "波动尺度：auto 自动（默认）/ reference_tv_sd 参考总变异标准差"},
    {"--reference-tv-sd", "/generation/reference_tv_sd", ValueType::NullableNumber,
     "参考总变异标准差 TV_SD：1e-12～1e12 或 null，默认 null"},
    {"--decimal-places", "/generation/decimal_places", ValueType::Integer,
     "测量值小数位数：0～10，默认 4；生成与导出使用同一精度"},
    {"--seed", "/generation/seed", ValueType::NullableInteger,
     "数据生成随机种子：有符号 64 位整数或 null；默认 null，自动选择"},
    {"--max-attempts", "/generation/max_attempts", ValueType::Integer,
     "候选搜索次数上限：1～1000000，默认 5000"},
    {"--timeout-seconds", "/generation/timeout_seconds", ValueType::Number,
     "搜索时间上限：0.01～3600 秒，默认 30"},
    {"--percentage-tolerance-pp", "/target_defaults/percentage_tolerance_pp", ValueType::Number,
     "百分比目标默认允许误差：0～1000 百分点，默认 0.1"},
    {"--ndc-tolerance", "/target_defaults/ndc_tolerance", ValueType::Integer,
     "可区分类别数 ndc 的默认允许误差：0～1000000 整数，默认 0"},
    {"--ev", "/targets/EV_pct", ValueType::Target, "重复性 EV/TV% 约束：SPEC，默认 none"},
    {"--av", "/targets/AV_pct", ValueType::Target, "再现性 AV/TV% 约束：SPEC，默认 none"},
    {"--grr", "/targets/GRR_pct", ValueType::Target, "总量具 R&R 的 GRR/TV% 约束：SPEC，默认 none"},
    {"--pt", "/targets/PT_pct", ValueType::Target, "精度与容差比 P/T% 约束：SPEC，默认 none"},
    {"--ndc", "/targets/ndc", ValueType::Target,
     "可区分类别数 ndc 约束：SPEC，默认 none；数值和允许误差须为整数"},
    {"--output-directory", "/output/directory", ValueType::Text,
     "输出目录；空字符串或相对路径以默认配置目录为基准"},
    {"--filename", "/output/filename", ValueType::Text, "CSV 文件名模板，支持 [变量]"},
    {"--existing-file", "/output/existing_file", ValueType::Text,
     "同名文件处理：number 追加序号（默认）/ overwrite 覆盖"},
    {"--csv-format", "/output/format", ValueType::Text,
     "CSV 格式：default（默认）/ minitab / spss / jmp19 / origin / matlab"},
    {"--run-order", "/output/run_order/mode", ValueType::Text,
     "standard 标准顺序（默认）/ all 全部随机化 / within_operator 操作员内随机化"},
    {"--randomize-operators", "/output/run_order/randomize_operators", ValueType::Boolean,
     "随机化操作员顺序：true / false（默认）；仅 within_operator 生效"},
    {"--run-seed", "/output/run_order/seed", ValueType::NullableInteger,
     "运行顺序随机种子：1～9223372036854775807 或 null，默认 null"},
    {"--store-standard-order", "/output/run_order/store_standard_order", ValueType::Boolean,
     "导出标准运行序号 StdOrder：true / false（默认），仅用于长表"},
};
void help() {
    std::cout << "MSA 数据生成器 CLI " << msa::version
              << "\n交叉量具 R&R：Xbar-R（均值极差法）与 ANOVA（方差分析法）\n作者："
              << msa::project_author << "\n项目仓库：" << msa::project_repository << "\n项目许可："
              << msa::project_license
              << "\n允许商业使用、修改和分发；分发副本或实质部分时须保留版权声明与许可声明。\n"
                 "软件按现状提供，不作担保；第三方组件适用各自许可证。\n"
                 "许可全文：--license 或 LICENSE；随包副本：licenses/MSA-Project-LICENSE.txt。\n"
                 "项目信息：NOTICE；第三方许可：THIRD_PARTY_NOTICES.md 与 licenses/。\n"
              << R"(
用法：
  生成并导出：msa_cli.exe [--config PATH | --no-config] [业务参数]
                         [--output PATH] [--report PATH] [--overwrite]
  分析已有数据：msa_cli.exe --input CSV [--config PATH | --no-config]
                           [业务参数] [--report PATH] [--overwrite]
  创建默认配置：msa_cli.exe --init [--config PATH]

基本参数：
  --config PATH   读取指定的 JSON 配置文件；文件必须存在（--init 除外）
  --no-config     不读取配置文件，仅使用内置默认值与命令行参数
  --output PATH   CSV 文件路径（含文件名）；相对当前工作目录，不展开文件名变量
  --report PATH   保存包含最终配置、矩阵和统计结果的 JSON 报告
  --input CSV     仅分析已有宽表或长表，不生成、导出或改写 CSV；不能与 --output 合用
  --overwrite     允许覆盖 CSV 和报告；优先于 --existing-file，与参数排列顺序无关
  --init          创建缺失的默认配置，保留已有文件；仅可与 --config 合用
  --help, -h      显示帮助，不读取配置文件
  --license       显示内置的项目许可证全文，不读取配置文件

快速开始（在 PowerShell 中可将 msa_cli.exe 写为 .\msa_cli.exe）：
  msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 --output data.csv
  msa_cli.exe --no-config --input data.csv --method crossed_anova --report analysis.json

第一条命令生成默认 3 人 × 3 次 × 10 件研究，第二条分析同一文件。
默认配置没有规格限和参考尺度；只执行 --init 后还须补齐这些生成条件。
直接运行且不提供任何参数不会生成文件，返回退出码 2。

业务参数：每项均须提供 VALUE，支持 --name VALUE 和 --name=VALUE。
以下“默认”均指内置默认值；已有配置会覆盖它，命令行又优先于配置。
枚举值区分大小写；true/false 为显式开关，null 清空可选数值。
)";
    for (const auto &option : options) {
        const std::string name = option.name;
        if (name == "--operators")
            std::cout << "\n研究设计：\n";
        else if (name == "--method")
            std::cout << "\n分析方法与交互项：\n";
        else if (name == "--study-multiplier")
            std::cout << "\n研究变异、容差与历史过程：\n";
        else if (name == "--confidence")
            std::cout << "\n置信区间（仅 ANOVA）：\n";
        else if (name == "--lsl")
            std::cout << "\n规格限与生成范围：\n";
        else if (name == "--center")
            std::cout << "\n生成与搜索：\n";
        else if (name == "--percentage-tolerance-pp")
            std::cout << "\n指标约束：\n";
        else if (name == "--output-directory")
            std::cout << "\n输出与文件命名：\n";
        else if (name == "--run-order")
            std::cout << "\n运行顺序（长表）：\n";
        std::cout << "  " << std::left << std::setw(29) << option.name << option.description
                  << '\n';
    }
    std::cout << R"(
指标约束 SPEC（替换整个旧约束，不继承该指标原有的允许误差或区间）：
  none                  不约束
  target:VALUE[:ERROR]   目标值；省略 ERROR 使用默认允许误差，0 表示精确目标
  between:MIN:MAX        闭区间（包含上下限）
  at_most:VALUE          不高于（包含边界）
  at_least:VALUE         不低于（包含边界）
  例如：--ev target:10:0.1 --av at_most:10 --grr between:10:20 --ndc at_least:5
  EV 目标 10、允许误差 0.1 百分点表示接受 9.9%～10.1%，不是相对误差 0.1%。

配置优先级与生成条件：
优先级：内置默认值 < 配置文件 < 命令行；重复参数最后一次生效。
--config 与 --no-config 互斥；含空格的路径和文件名模板用引号包围。
省略参数保留配置值；null 是清空可选数值，不能用空字符串代替。
未指定 --config 时读取 %USERPROFILE%\MSA数据生成器\msa_generator.json；
该文件不存在则使用内置默认值，不自动创建。普通运行不保存命令行覆盖。
生成时，严格规格至少需要一个规格限；P/T 约束需要规格限或分析容差。
缺少双侧规格或选择参考尺度时，须指定参考 TV_SD 或历史标准差。
百分比目标允许误差的单位为百分点；ndc 的目标、区间和允许误差均为整数。
--input 仅分析，不要求满足生成条件；操作员人数、重复次数与部件数量仍须与 CSV 一致。

CSV 与运行顺序：
  default：带列号和操作员标记的宽表，左上角空白，UTF-8 BOM；按操作员、重复轮次排列。
  minitab/spss/jmp19/origin/matlab：统一四列 Operator,Part,Trial,Measurement，UTF-8 无 BOM。
  所有格式使用英文逗号、CRLF、配置的小数位；默认宽表忽略运行顺序选项。
  standard：按 Trial -> Part -> Operator 排列。
  all：打乱全部完整记录，允许跨操作员、部件、Trial 混排。
  within_operator：逐 Trial、逐操作员，每个 Trial+Operator 内独立打乱部件。
  --randomize-operators true：仅 within_operator 生效；每项研究确定一次操作员顺序，各 Trial 沿用。
  --store-standard-order true：长表首列增加 StdOrder；CSV 行序为实际运行顺序，Trial 永不重编号。
  StdOrder = ((Trial-1)*部件数量+Part-1)*操作员人数+操作员编号，A=1、B=2、C=3。
  StdOrder 为完整的 1～总记录数排列；Trial 是原始重复测量编号，不是随机化后的行号。
  --seed 控制测量数据生成，--run-seed 独立控制排序；相同数据、运行配置和排序种子可复现排列。
  运行顺序种子为 null 时，在当前研究内复用自动选择的种子；跨进程复现应固定两个种子。
  CLI 生成运行会重新生成研究；--input 只复算输入数据。GUI 仅修改导出设置时无需重新生成。

路径与保存：
省略 --output 时，使用最终配置的输出目录、文件名变量及同名策略。
显式 --output 不使用上述路径、命名和同名策略；替换需 --overwrite。
--overwrite 始终优先于 --existing-file，不受参数先后顺序影响；报告覆盖也需要 --overwrite。
--output-directory 的空值或相对路径以用户配置目录为基准；--no-config 不改变此规则。
--config、--input、--output、--report 的相对路径均以当前工作目录为基准。
--report 包含完整最终配置、实际生成种子、矩阵、指标、尝试次数、耗时和版本。
--input 报告标记 analysis_only=true，不具有数据生成种子；不要求输入数据满足指标约束。
CSV 和报告不可同路径，输入 CSV 也不能与报告同路径；报告失败时可能已有成功 CSV。

文件名变量（仅配置 filename / --filename 展开，显式 --output 按字面使用）：
  方括号表示变量且区分大小写；括号外原样保留。[[ 和 ]] 输出字面量方括号。
  月份 [MM]，时/分/秒 [H]/[M]/[S]；[MS] 是三位毫秒，[TIMESTAMP] 是 Unix 秒数。
)";
    for (const auto &variable : msa::filename_variables()) {
        std::cout << "  [" << variable.name << ']';
        if (variable.alias && *variable.alias)
            std::cout << " / [" << variable.alias << ']';
        std::cout << "  " << variable.description << '\n';
    }
    std::cout << R"(
  日期时间变量使用导出时刻；研究参数、指标和 [SEED] 使用已生成结果，中心留空取实际均值。
  未设置的规格限、不可计算的指标输出 NA。
  百分比文件名变量保留两位小数、不带百分号；未知变量或非法文件名会报错。

分析方法与指标含义：
  crossed_xbarr：均值极差法，依据极差和均值估计重复性、再现性与部件间变异。
  此方法不单独估计部件×操作员交互作用；不输出 ANOVA 表、置信区间或误分类概率。
  crossed_anova：平衡交叉双随机效应模型，使用 EMS（期望均方）估计方差分量。
  auto 按完整模型交互项 P 值判断：P >= alpha 时合并其平方和、自由度到重复性误差；否则保留。
  include 始终保留交互项，exclude 始终合并；负方差分量逐项置零。
  AV 包含操作员及保留的交互分量；GRR 包含重复性和再现性。
  EV、AV、GRR、PV、TV 均表示标准差（1 sigma）；研究变异 = k × 对应标准差。
  EV/TV%、AV/TV%、GRR/TV% 为标准差比，与 %Contribution（方差贡献率）不同。
  ndc = max(1, floor(1.41 × PV/GRR))；GRR 为零时不可计算。
  metrics.anova 保存完整及最终 ANOVA 表、交互项判定、方差分量、标准差及百分比。

容差、历史过程与置信区间：
  P/T% = 100 × k × GRR / 容差宽度；独立 --analysis-tolerance 优先，其次使用 USL-LSL。
  独立分析容差只用于计算，不代替控制生成范围的规格限。
  仅有单侧规格时：P/T% = 100 × (k/2) × GRR / abs(规格限-均值)。
  均值默认来自测量数据，可用 --historical-mean 指定；分母为零时 P/T 不可计算。
  指定历史标准差且 process-variation=historical 时，若历史 SD >= GRR，
  则 TV=历史 SD、PV=sqrt(TV²-GRR²)；否则保留研究估计。
  process-variation=study 始终保留研究 TV/PV；指定历史 SD 后同时计算 %Process。
  误分类概率仅用于 ANOVA 且至少有一个规格限，采用独立正态的真实部件和测量误差模型。
  概率输出为 0～1；单独的容差宽度不构成规格限。
  ANOVA 置信区间覆盖方差分量、标准差、研究变异，以及可计算的 P/T 和 %Process。
  mls 使用 Minitab MLS 近似；satterthwaite 使用 Satterthwaite 有效自由度近似。
  MLS 的最低适用水平取决于操作员人数和单/双侧类型，超出范围时拒绝计算并提示下界。
  需要更低置信水平时，请显式选择 satterthwaite；程序不会自动更换估计方法。
  重复性采用卡方区间；单侧区间未指定的一侧、不可可靠计算的边界输出 null。
  暂不提供 %Contribution、%Study Var 和 ndc 的比值置信区间。

与其他软件核对：
  使用相同完整数据与交叉模型；Part、Operator、Measurement 对应部件、操作员和测量值。
  ANOVA 统一随机效应、交互项策略、alpha 与 EMS 估计；不直接比较固定效应或 REML 默认结果。
  统一研究变异倍数、规格/容差、历史参数与置信区间算法；百分比比较 %Study Var，
  P/T 比较总量具 R&R 的 %Tolerance。不同软件的单侧规格和历史误分类口径可能不同。
  Trial 与 StdOrder 仅用于记录追溯，不作为额外因子；行序改变只允许浮点末位计算差异。
  相同排序种子不保证与 Minitab 产生相同排列。满足生成约束不等于测量系统合格。
  支持范围与已知差异见 docs/GUI参数兼容与对标进度.md。

退出状态与排错：
  0  成功（--input 的约束复核未满足不改变该退出码）
  1  配置读取、生成、文件读写或执行过程错误；生成被 Ctrl+C 取消
  2  参数错误，或预检查发现显式输出/报告文件已存在且未允许覆盖
  超时或搜索次数耗尽不表示数学上无解；先检查约束、规格、尺度和小数位，再增加搜索预算。
  成功生成/分析时标准输出为 JSON，错误写入标准错误；--init、--help、--license 输出文本。

更多示例：
  创建配置文件（再编辑规格或参考尺度）：
  msa_cli.exe --init --config study.json
  按配置生成，并导出包含标准运行序号的长表：
  msa_cli.exe --config "examples\示例配置.json" --operators 2 --csv-format minitab --run-order within_operator --randomize-operators true --run-seed 321 --store-standard-order true --report result.json
  不读取配置，固定两个随机种子并自动处理同名文件：
  msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 --csv-format spss --run-order all --run-seed 321 --filename "MSA_[DATETIME]_[SEED].csv" --existing-file number
  对同一份 3 人 × 3 次 × 10 件数据进行 ANOVA 复算，输出 95% 双侧区间：
  msa_cli.exe --no-config --input data.csv --method crossed_anova --confidence true --confidence-level 95 --confidence-bound both --confidence-method mls --report anova.json

完整说明：docs/使用说明.md；配置字段：docs/配置参考.md；导入与核对：docs/Minitab导入与对标.md。
)";
}
msa::Json number_value(const std::string &text, bool integer) {
    if (!integer)
        return msa::parse_number(text);
    std::int64_t value;
    const auto *begin = text.data();
    if (!text.empty() && text.front() == '+')
        ++begin;
    const auto parsed = std::from_chars(begin, text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("必须是 int64 范围内的整数");
    return value;
}
msa::Json target_value(const std::string &text, bool ndc) {
    if (text == "none")
        return nullptr;
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = text.find(':', begin);
        fields.push_back(text.substr(begin, end == text.npos ? end : end - begin));
        if (end == text.npos)
            break;
        begin = end + 1;
    }
    const auto &mode = fields.front();
    msa::Json target{{"mode", mode}};
    if (mode == "between" && fields.size() == 3) {
        target["min"] = number_value(fields[1], ndc);
        target["max"] = number_value(fields[2], ndc);
    } else if ((mode == "target" && (fields.size() == 2 || fields.size() == 3)) ||
               ((mode == "at_most" || mode == "at_least") && fields.size() == 2)) {
        target["value"] = number_value(fields[1], ndc);
        if (fields.size() == 3)
            target[ndc ? "tolerance" : "tolerance_pp"] = number_value(fields[2], ndc);
    } else
        throw std::invalid_argument(
            "约束须为 none、target:值[:误差]、between:下限:上限、at_most:值或 at_least:值");
    return target;
}
msa::Json option_value(const Option &option, const std::string &text) {
    try {
        msa::Json value;
        switch (option.type) {
        case ValueType::NullableInteger:
        case ValueType::NullableNumber:
            if (text == "null") {
                value = nullptr;
                break;
            }
            [[fallthrough]];
        case ValueType::Integer:
        case ValueType::Number:
            value = number_value(text, option.type == ValueType::Integer ||
                                           option.type == ValueType::NullableInteger);
            break;
        case ValueType::Boolean:
            if (text != "true" && text != "false")
                throw std::invalid_argument("须为 true 或 false");
            value = text == "true";
            break;
        case ValueType::Text:
            value = text;
            break;
        case ValueType::Target:
            value = target_value(text, std::string(option.path) == "/targets/ndc");
            break;
        }
        // Reuse the GUI's field validation, before any file access or generation.
        auto single = msa::Json::object();
        single[msa::Json::json_pointer(option.path)] = value;
        msa::normalize_config(single);
        return value;
    } catch (const std::exception &e) {
        throw std::invalid_argument(std::string(option.name) + ": " + e.what());
    }
}
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b) {
    auto ca = std::filesystem::weakly_canonical(a), cb = std::filesystem::weakly_canonical(b);
#ifdef _WIN32
    return CompareStringOrdinal(ca.c_str(), -1, cb.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return ca == cb;
#endif
}
int run(const std::vector<std::string> &args) {
    std::filesystem::path config, output, report, input;
    bool init = false, overwrite = false, noConfig = false;
    std::vector<std::pair<msa::Json::json_pointer, msa::Json>> overrides;
    try {
        for (std::size_t i = 1; i < args.size(); ++i) {
            auto a = args[i];
            const auto equals = a.find('=');
            const auto inlineValue = equals != a.npos;
            const auto suffix = inlineValue ? a.substr(equals + 1) : std::string{};
            if (inlineValue)
                a.resize(equals);
            auto value = [&]() -> std::string {
                if (inlineValue)
                    return suffix;
                if (i + 1 == args.size() || args[i + 1].rfind("--", 0) == 0 || args[i + 1] == "-h")
                    throw std::invalid_argument("参数缺少值：" + a);
                return args[++i];
            };
            if (inlineValue && (a == "--help" || a == "-h" || a == "--init" || a == "--overwrite" ||
                                a == "--no-config" || a == "--license"))
                throw std::invalid_argument("此参数不接受值：" + a);
            if (a == "--help" || a == "-h") {
                help();
                return 0;
            }
            if (a == "--license") {
                std::cout << msa::project_license_text;
                return 0;
            }
            if (a == "--init")
                init = true;
            else if (a == "--overwrite")
                overwrite = true;
            else if (a == "--no-config")
                noConfig = true;
            else if (a == "--config" || a == "--output" || a == "--report" || a == "--input") {
                auto text = value();
                if (text.empty())
                    throw std::invalid_argument("参数路径不能为空：" + a);
                auto p = std::filesystem::u8path(text);
                if (a == "--config")
                    config = p;
                else if (a == "--input")
                    input = p;
                else if (a == "--output")
                    output = p;
                else
                    report = p;
            } else {
                if (a == "--trials")
                    a = "--replicates";
                const auto *option = std::find_if(std::begin(options), std::end(options),
                                                  [&](const Option &o) { return a == o.name; });
                if (option == std::end(options))
                    throw std::invalid_argument("未知参数：" + a);
                overrides.emplace_back(msa::Json::json_pointer(option->path),
                                       option_value(*option, value()));
            }
        }
        if (noConfig && !config.empty())
            throw std::invalid_argument("--no-config 与 --config 不能同时使用");
        if (init && (noConfig || !overrides.empty() || !output.empty() || !report.empty() ||
                     !input.empty() || overwrite))
            throw std::invalid_argument("--init 仅可与 --config 一起使用");
        if (!init) {
            if (!input.empty() && !output.empty())
                throw std::invalid_argument("--input仅分析已有CSV，不能与--output合用");
            if (!input.empty() && !report.empty() && same_path(input, report))
                throw std::invalid_argument("输入CSV与报告路径不能相同");
            if (input.empty() && output.empty() && config.empty() && overrides.empty() && !noConfig)
                throw std::invalid_argument(
                    "请用 --config 指定配置模板导出，或用 --output 指定输出路径");
            for (auto p : {output, report})
                if (!p.empty() && std::filesystem::exists(p) && !overwrite)
                    throw std::invalid_argument("文件已存在；另选路径或添加 --overwrite：" +
                                                p.u8string());
            if (!output.empty() && !report.empty() && same_path(output, report))
                throw std::invalid_argument("CSV 与报告路径不能相同");
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
    try {
        if (init) {
            if (config.empty())
                config = msa::config_path();
            msa::load_config(config);
            std::cout << "配置已准备；现有配置未覆盖。\n";
            return 0;
        }
        auto c = msa::Json::object();
        if (!noConfig) {
            const bool explicitConfig = !config.empty();
            if (config.empty())
                config = msa::config_path();
            if (explicitConfig || std::filesystem::exists(config))
                c = msa::Json::parse(msa::read_text(config));
        }
        if (!c.is_object())
            throw msa::ConfigError("配置必须是 JSON 对象");
        // Pointer assignment replaces complete constraints and preserves explicit nulls.
        for (const auto &[path, value] : overrides)
            c[path] = value;
        if (overwrite)
            c["output"]["existing_file"] = "overwrite";
        c = msa::normalize_config(c);
        if (!input.empty()) {
            msa::Result result;
            result.config = c;
            result.matrix = msa::parse_export_csv(msa::read_text(input), c);
            result.metrics = msa::calculate(result.matrix, c);
            auto analysis = result.as_json();
            analysis["analysis_only"] = true;
            analysis["seed"] = nullptr;
            analysis["checks"] = msa::checks(result.metrics, c);
            if (!report.empty())
                msa::atomic_text(report, analysis.dump(2) + '\n',
                                 overwrite ? msa::ExistingFile::Replace
                                           : msa::ExistingFile::Preserve);
            std::cout << msa::Json{{"input", input.u8string()},
                                   {"metrics", result.metrics},
                                   {"checks", analysis["checks"]}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        std::signal(SIGINT, on_signal);
        auto result = msa::generate(c, &cancelled);
        const auto policy = overwrite ? msa::ExistingFile::Replace : msa::ExistingFile::Preserve;
        if (output.empty()) {
            auto settings = c["output"];
            if (overwrite)
                settings["existing_file"] = "overwrite";
            output = msa::export_configured_csv(result, settings, std::chrono::system_clock::now(),
                                                report);
        } else
            msa::export_csv(result, output, policy);
        if (!report.empty())
            msa::atomic_text(report, result.as_json().dump(2) + '\n', policy);
        std::cout << msa::Json{{"output", output.u8string()},
                               {"seed", result.seed},
                               {"metrics", result.metrics}}
                         .dump(2)
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        auto n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        s.pop_back();
        args.push_back(s);
    }
    return run(args);
}
#else
int main(int argc, char **argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
