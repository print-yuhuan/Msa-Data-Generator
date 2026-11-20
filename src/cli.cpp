#include "msa/core.hpp"
#include "msa/comparison_report.hpp"
#include "msa/license_text.hpp"
#include "msa/part_sampling.hpp"
#include "msa/project.hpp"
#include <algorithm>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace {
std::atomic<bool> cancelled{false};
void on_signal(int) { cancelled = true; }
enum class ValueType { Integer, Number, NullableInteger, NullableNumber, Boolean, Text, OperatorName, Target };
struct Option {
    const char *name, *path;
    ValueType type;
    const char *description;
};
const Option options[]{
    {"--operators", "/design/operators", ValueType::Integer, "操作员人数：2 或 3，默认 3"},
    {"--operator-a", "/design/operator_names/0", ValueType::OperatorName, "第一位操作员名称，留空使用 A"},
    {"--operator-b", "/design/operator_names/1", ValueType::OperatorName, "第二位操作员名称，留空使用 B"},
    {"--operator-c", "/design/operator_names/2", ValueType::OperatorName, "第三位操作员名称，3 人时使用，留空使用 C"},
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
    {"--require-normality", "/generation/require_normality", ValueType::Boolean,
     "要求近似正态（筛选最终测量值）：true / false（默认）"},
    {"--residual-model", "/generation/residual_model", ValueType::Text,
     "模拟误差模式：standard 标准（默认）/ varied 变化残差"},
    {"--part-sampling", "/generation/part_sampling", ValueType::Text, "部件基础尺寸的取样方案（默认取样：default）"},
    {"--scale-mode", "/generation/scale_mode", ValueType::Text,
     "波动尺度：auto 自动（默认）/ reference_tv_sd 参考总变异标准差"},
    {"--minimum-part-span-pct", "/generation/minimum_part_span_pct", ValueType::NullableNumber,
     "最低部件跨度占规格宽度的百分比（0 < 值 <= 100，须有双侧规格）或 null（默认）"},
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
     "数据输出目录；留空使用系统桌面，相对目录从桌面计算"},
    {"--filename", "/output/filename", ValueType::Text, "数据文件名模板，不含扩展名，支持 [变量]"},
    {"--file-type", "/output/file_type", ValueType::Text, "数据文件类型：csv（默认）/ xlsx"},
    {"--existing-file", "/output/existing_file", ValueType::Text,
     "同名文件处理：number 追加序号（默认）/ overwrite 覆盖"},
    {"--csv-format", "/output/format", ValueType::Text,
     "数据布局：default（默认）/ minitab / spss / jmp19 / origin / matlab"},
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
                         [--output PATH] [--report PATH] [--comparison-report PATH] [--overwrite]
  分析已有数据：msa_cli.exe --input PATH [--sheet NAME] [--config PATH | --no-config]
                           [业务参数] [--report PATH] [--comparison-report PATH] [--overwrite]
  重放生成报告：msa_cli.exe --replay-report PATH --output PATH [--overwrite]
  创建默认配置：msa_cli.exe --init [--config PATH]

基本参数：
  --config PATH   读取指定的 JSON 配置文件；文件必须存在（--init 除外）
  --no-config     不读取配置文件，仅使用内置默认值与命令行参数
  --output PATH   数据文件路径（.csv 或 .xlsx）；相对当前工作目录，不展开文件名变量
  --report PATH   保存包含最终配置、矩阵和统计结果的 JSON 报告
  --comparison-report PATH  保存同一矩阵的 Xbar-R / ANOVA 对照报告，支持 .html 和 .pdf
  --replay-report PATH  从新版生成报告还原原始数据文件，核对 SHA-256 后写入 --output
  --input PATH    分析 CSV 或 XLSX，自动识别文件中的操作员名称和人数，不能与 --output 合用
  --sheet NAME    按名称选择 XLSX 工作表；省略时按工作簿顺序读取第一张数据工作表
  --overwrite     允许覆盖数据文件和报告；优先于 --existing-file，与参数排列顺序无关
  --init          创建缺失的默认配置，保留已有文件；仅可与 --config 合用
  --help, -h      显示帮助，不读取配置文件
  --license       显示内置的项目许可证全文，不读取配置文件

快速开始（在 PowerShell 中可将 msa_cli.exe 写为 .\msa_cli.exe）：
  msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 --output data.xlsx
  msa_cli.exe --no-config --input data.xlsx --method crossed_anova --report analysis.json

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
        if (name == "--part-sampling") {
            for (const auto &method : msa::part_sampling_methods)
                std::cout << "      " << std::setw(16) << method.key << method.label << "  "
                          << method.description << '\n';
        }
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
随机取样（--part-sampling uniform_random）须同时指定 --lsl 和 --usl。
每件部件独立从整个规格区间抽取一个基础尺寸，供该部件的所有重复测量使用。
设置最低部件跨度须同时提供 LSL 和 USL。先计算舍入后每件部件的测量均值。
跨度百分比 = 100 × (最大部件均值 − 最小部件均值) / (USL − LSL)，该值须达到所设下限。
百分比目标允许误差的单位为百分点；ndc 的目标、区间和允许误差均为整数。
--require-normality true 对按小数位数舍入后的整批测量值应用 Anderson–Darling 正态性筛选。
采用独立正态样本的 5% 参考阈值，筛选用于控制模拟样本的分布形状。
--input 分析文件中的平衡交叉研究，自动读取操作员、部件数和重复测量次数。
带标记的宽表按操作员分组排列。长表有 StdOrder 时按标准序号确定人员顺序，
省略时按 Operator 首次出现的顺序列出。报告和图表使用文件中的名称。
纯数值宽表按位置使用 A/B/C。只有 6 行时，--replicates 2 指 3 人各测 2 次，
--replicates 3 指 2 人各测 3 次，省略时使用配置的重复次数（内置默认 3）。
导入结果独立于生成用的默认操作员。分析成功时，标准输出的 design 显示本次识别结果。

数据文件与运行顺序：
  default：带列号和操作员标记的宽表，左上角空白；按操作员、重复轮次排列。
  minitab/spss/jmp19/origin/matlab：统一四列 Operator,Part,Trial,Measurement。
  CSV 使用英文逗号、CRLF；宽表含 UTF-8 BOM，长表使用 UTF-8 无 BOM。
  XLSX 使用数值单元格和指定的小数位。两种文件均支持相同布局，宽表忽略运行顺序选项。
  standard：按 Trial -> Part -> Operator 排列。
  all：打乱全部完整记录，允许跨操作员、部件、Trial 混排。
  within_operator：逐 Trial、逐操作员，每个 Trial+Operator 内独立打乱部件。
  --randomize-operators true：仅 within_operator 生效；每项研究确定一次操作员顺序，各 Trial 沿用。
  --store-standard-order true：长表首列增加 StdOrder；行序为实际运行顺序，Trial 永不重编号。
  StdOrder = ((Trial-1)*部件数量+Part-1)*操作员人数+操作员编号，编号按研究设计中的位置从 1 开始。
  StdOrder 为完整的 1～总记录数排列；Trial 是原始重复测量编号，不是随机化后的行号。
  --seed 控制测量数据生成，--run-seed 独立控制排序；相同数据、运行配置和排序种子可复现排列。
  随机化时最终 JSON 报告的 export.effective_run_seed 保存实际排序种子；可跨进程重放。
  CLI 生成运行会重新生成研究；--input 只复算输入数据。GUI 仅修改导出设置时无需重新生成。

路径与保存：
省略 --output 时，按 --file-type 或配置添加 .csv / .xlsx 扩展名，使用配置的目录、文件名及同名策略。
显式 --output 按扩展名选择文件类型，以当前工作目录为基准；替换需 --overwrite。
--overwrite 始终优先于 --existing-file，不受参数先后顺序影响；报告覆盖也需要 --overwrite。
--output-directory 留空使用系统桌面，相对目录从桌面计算。--no-config 同样使用此规则。
--config、--input、--output、--report、--comparison-report 指定的相对文件路径从当前工作目录计算。
输入 CSV 上限为 4 MiB，XLSX 上限为 16 MiB，JSON 配置上限为 64 MiB。数据文件和报告须使用独立于本次输入配置的文件路径。
--report 包含完整最终配置、实际生成种子、矩阵、指标、尝试次数、耗时和版本。
生成报告另含数据导出清单 export（含 SHA-256）。重放从保存的矩阵还原，独立于搜索耗时。
长表支持带双引号的字段、按名称重排列和附加列，必需列为 Operator、Part、Trial、Measurement。
--input 报告标记 analysis_only=true，不具有数据生成种子；不要求输入数据满足指标约束。
输入、输出和报告须指向独立文件；报告保存失败时，已成功导出的数据会保留。

名称变量（命令行通过 output.filename 或 --filename 使用。显式 --output 按字面使用）：
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
  支持范围与已知差异见 docs/GUI参数兼容与对标.md。

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
  msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 --csv-format spss --run-order all --run-seed 321 --file-type xlsx --filename "MSA_[DATETIME]_[SEED]" --existing-file number
  对同一份 3 人 × 3 次 × 10 件数据进行 ANOVA 复算，输出 95% 双侧区间：
  msa_cli.exe --no-config --input data.csv --method crossed_anova --confidence true --confidence-level 95 --confidence-bound both --confidence-method mls --report anova.json

完整说明：docs/使用说明.md；配置字段：docs/配置参考.md；导入与核对：docs/Minitab导入与对标.md。
)";
}
msa::Json number_value(const std::string &text, bool integer) {
    if (!integer)
        return msa::parse_number(text);
    return msa::parse_integer(text);
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
void validate_option_field(const msa::Json::json_pointer &path, const msa::Json &value,
                           const msa::Json &config = nullptr) {
    auto single = msa::Json::object();
    if (config.contains("schema_version"))
        single["schema_version"] = config.at("schema_version");
    single[path] = value;
    msa::normalize_config(single);
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
        case ValueType::OperatorName:
            // Names are validated together with the final operator count and other names.
            return text;
        case ValueType::Target:
            value = target_value(text, std::string(option.path) == "/targets/ndc");
            break;
        }
        // Reuse the GUI's field validation, before any file access or generation.
        validate_option_field(msa::Json::json_pointer(option.path), value);
        return value;
    } catch (const std::exception &e) {
        throw std::invalid_argument(std::string(option.name) + ": " + e.what());
    }
}
enum class OutputUse { Configured, Explicit, Analysis };
bool unused_output_field(OutputUse use, std::string_view path) {
    if (path.rfind("/input/", 0) == 0 || path.rfind("/output/charts/", 0) == 0 ||
        path.rfind("/output/silent/", 0) == 0 || path.rfind("/output/comparison_report/", 0) == 0)
        return true;
    if (use == OutputUse::Analysis)
        return path.rfind("/output/", 0) == 0;
    return use == OutputUse::Explicit &&
           (path == "/output/directory" || path == "/output/filename" ||
            path == "/output/existing_file" || path == "/output/file_type");
}
void default_invalid_unused_output_fields(msa::Json &config, OutputUse use) {
    constexpr const char *fields[]{"/input/directory",
                                   "/input/filename",
                                   "/output/directory",
                                   "/output/filename",
                                   "/output/file_type",
                                   "/output/existing_file",
                                   "/output/format",
                                   "/output/encoding",
                                   "/output/run_order/mode",
                                   "/output/run_order/seed",
                                   "/output/run_order/randomize_operators",
                                   "/output/run_order/store_standard_order",
                                   "/output/silent/csv",
                                   "/output/silent/charts",
                                   "/output/silent/comparison_report",
                                   "/output/charts/directory",
                                   "/output/charts/filename",
                                   "/output/comparison_report/format",
                                   "/output/comparison_report/directory",
                                   "/output/comparison_report/filename"};
    const auto defaults = msa::default_config();
    for (const auto *field : fields) {
        if (!unused_output_field(use, field))
            continue;
        const msa::Json::json_pointer path(field);
        if (!config.contains(path))
            continue;
        // Retain valid report metadata. Full normalization still checks object shape and keys.
        try {
            validate_option_field(path, config.at(path), config);
        } catch (const msa::ConfigError &) {
            config.at(path) = defaults.at(path);
        }
    }
}
int run(const std::vector<std::string> &args) {
    std::filesystem::path config, output, report, input, comparison, replay;
    std::optional<std::string> sheet;
    bool init = false, overwrite = false, noConfig = false;
    OutputUse outputUse = OutputUse::Configured;
    std::vector<std::pair<const Option *, std::string>> pending;
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
            else if (a == "--sheet") {
                sheet = value();
                if (sheet->empty())
                    throw std::invalid_argument("--sheet 需要工作表名称");
            } else if (a == "--config" || a == "--output" || a == "--report" || a == "--input" ||
                     a == "--comparison-report" || a == "--replay-report") {
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
                else if (a == "--comparison-report")
                    comparison = p;
                else if (a == "--replay-report")
                    replay = p;
                else
                    report = p;
            } else {
                if (a == "--trials")
                    a = "--replicates";
                const auto *option = std::find_if(std::begin(options), std::end(options),
                                                  [&](const Option &o) { return a == o.name; });
                if (option == std::end(options))
                    throw std::invalid_argument("未知参数：" + a);
                pending.emplace_back(option, value());
            }
        }
        if (sheet && (input.empty() || msa::data_file_type(input) != "xlsx"))
            throw std::invalid_argument("--sheet 需要通过 --input 指定 XLSX 工作簿");
        outputUse = !input.empty()    ? OutputUse::Analysis
                    : !output.empty() ? OutputUse::Explicit
                                      : OutputUse::Configured;
        for (const auto &[option, text] : pending) {
            try {
                overrides.emplace_back(msa::Json::json_pointer(option->path),
                                       option_value(*option, text));
            } catch (const std::invalid_argument &) {
                if (!unused_output_field(outputUse, option->path))
                    throw;
            }
        }
        if (noConfig && !config.empty())
            throw std::invalid_argument("--no-config 与 --config 不能同时使用");
        if (init && (noConfig || !pending.empty() || !output.empty() || !report.empty() ||
                     !input.empty() || !comparison.empty() || !replay.empty() || overwrite))
            throw std::invalid_argument("--init 仅可与 --config 一起使用");
        if (!replay.empty() &&
            (output.empty() || !pending.empty() || !config.empty() || !input.empty() ||
             !report.empty() || !comparison.empty() || noConfig))
            throw std::invalid_argument("--replay-report 须与 --output 合用，可选 --overwrite");
        if (!init) {
            if (!input.empty() && !output.empty())
                throw std::invalid_argument("--input 仅分析已有数据，不能与 --output 合用");
            if (input.empty() && output.empty() && config.empty() && overrides.empty() && !noConfig)
                throw std::invalid_argument(
                    "请用 --config 指定配置模板导出，或用 --output 指定输出路径");
            const std::array destinations{output, report, comparison};
            for (const auto &p : destinations)
                if (!p.empty() && std::filesystem::exists(p) && !overwrite)
                    throw std::invalid_argument("文件已存在；另选路径或添加 --overwrite：" +
                                                p.u8string());
            for (std::size_t i = 0; i < destinations.size(); ++i) {
                const auto &p = destinations[i];
                if (p.empty())
                    continue;
                for (const auto &source : {input, replay})
                    if (!source.empty() && msa::same_file_path(p, source))
                        throw std::invalid_argument("输出路径不能覆盖输入文件：" + p.u8string());
                for (std::size_t j = 0; j < i; ++j)
                    if (!destinations[j].empty() && msa::same_file_path(p, destinations[j]))
                        throw std::invalid_argument("数据与报告须使用各自独立的文件路径");
            }
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
    std::vector<std::filesystem::path> committed;
    try {
        const auto policy = overwrite ? msa::ExistingFile::Replace : msa::ExistingFile::Preserve;
        if (!replay.empty()) {
            const auto manifest = msa::Json::parse(msa::read_text(replay, msa::config_read_limit));
            msa::replay_data(manifest, output, policy);
            std::cout << msa::Json{{"output", output.u8string()},
                                   {"sha256", manifest.at("export").at("sha256")}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (init) {
            if (config.empty())
                config = msa::config_path();
            msa::load_config(config);
            std::cout << "配置已准备；现有配置未覆盖。\n";
            return 0;
        }
        auto c = msa::Json::object();
        std::filesystem::path source_config;
        if (!noConfig) {
            const auto candidate = config.empty() ? msa::config_path() : config;
            if (!config.empty() || std::filesystem::exists(candidate)) {
                source_config = candidate;
                for (const auto &destination : {output, report, comparison}) {
                    if (!destination.empty() && msa::same_file_path(destination, source_config)) {
                        std::cerr << "输出路径不能覆盖本次使用的配置文件\n";
                        return 2;
                    }
                }
                c = msa::Json::parse(msa::read_text(source_config, msa::config_read_limit));
            }
        }
        if (!c.is_object())
            throw msa::ConfigError("配置必须是 JSON 对象");
        // Pointer assignment replaces complete constraints and preserves explicit nulls.
        const msa::Json::json_pointer names("/design/operator_names");
        for (const auto &[path, value] : overrides) {
            if (path.parent_pointer() == names && !c.contains(names))
                c[names] = msa::default_config().at(names);
            c[path] = value;
        }
        default_invalid_unused_output_fields(c, outputUse);
        if (outputUse == OutputUse::Explicit)
            c["output"]["file_type"] = msa::data_file_type(output);
        if (overwrite)
            c["output"]["existing_file"] = "overwrite";
        for (const auto &destination : {report, comparison}) {
            if (destination.empty())
                continue;
            if (std::filesystem::is_directory(destination))
                throw std::runtime_error("报告目标是目录：" + destination.u8string());
            auto parent = std::filesystem::absolute(destination).parent_path();
            while (!parent.empty() && !std::filesystem::exists(parent) &&
                   parent != parent.root_path())
                parent = parent.parent_path();
            if (!parent.empty() && !std::filesystem::is_directory(parent))
                throw std::runtime_error("报告父路径不是目录：" + parent.u8string());
        }
        if (!input.empty()) {
            auto imported = sheet ? msa::read_study_data(input, c, *sheet)
                                  : msa::read_study_data(input, c);
            msa::Result result;
            result.config = std::move(imported.config);
            result.matrix = std::move(imported.matrix);
            result.metrics = msa::calculate(result.matrix, result.config);
            auto analysis = result.as_json();
            analysis["analysis_only"] = true;
            analysis["seed"] = nullptr;
            analysis["checks"] = msa::checks(result.metrics, result.config);
            if (!report.empty()) {
                msa::atomic_text(report, analysis.dump(2) + '\n', policy);
                committed.push_back(report);
            }
            if (!comparison.empty())
                msa::write_comparison_report(comparison, result, msa::StudySource::ImportedData,
                                             input.filename().u8string() +
                                                 (imported.sheet_name.empty() ? "" : " / " + imported.sheet_name),
                                             policy);
            std::cout << msa::Json{{"input", input.u8string()},
                                   {"design", result.config.at("design")},
                                   {"metrics", result.metrics},
                                   {"checks", analysis["checks"]}}
                             .dump(2)
                      << '\n';
            return 0;
        }
        c = msa::normalize_config(c);
        std::signal(SIGINT, on_signal);
        auto result = msa::generate(c, &cancelled);
        msa::Json receipt;
        auto *receipt_output = report.empty() ? nullptr : &receipt;
        if (output.empty()) {
            auto settings = c["output"];
            if (overwrite)
                settings["existing_file"] = "overwrite";
            output = msa::export_configured_data(result, settings, std::chrono::system_clock::now(),
                                                report, source_config, comparison, receipt_output);
        } else
            msa::export_data(result, output, c.at("output"), policy, receipt_output);
        committed.push_back(output);
        if (!report.empty()) {
            auto generated = result.as_json();
            generated["export"] = std::move(receipt);
            msa::atomic_text(report, generated.dump(2) + '\n', policy);
            committed.push_back(report);
        }
        if (!comparison.empty())
            msa::write_comparison_report(comparison, result, msa::StudySource::Generated,
                                         output.filename().u8string(), policy);
        std::cout << msa::Json{{"output", output.u8string()},
                               {"seed", result.seed},
                               {"metrics", result.metrics}}
                         .dump(2)
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        for (const auto &path : committed)
            std::cerr << "已保存：" << path.u8string() << '\n';
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
