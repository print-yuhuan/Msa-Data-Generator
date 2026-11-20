# Minitab 导入、运行顺序与结果核对

本程序支持交叉量具 R&R 的 Xbar-R 与 ANOVA 测量数据。对标时将**同一组已生成数据**交给 Minitab 重新计算，统一方法、研究倍数和规格口径。复现入口以 Minitab 22.6 的统计口径为参考。

## 推荐导入流程

1. 在“选项 → 文件 → 数据布局”的“导出布局”选择“Minitab”，按需设置运行顺序、种子与 StdOrder。已有生成结果时可直接导出。
2. 在 Minitab 打开 CSV，使用英文逗号分隔、首行列名；Operator 为人员字母，其他列为数字。所有长表统一 UTF-8 无 BOM、CRLF，保持配置的小数位。
3. 进入“统计 → 质量工具 → 量具研究 → 交叉量具 R&R”（英文界面为 Stat → Quality Tools → Gage Study → Gage R&R Study (Crossed)）。
4. 按列名选择：部件=Part，操作员=Operator，测量数据=Measurement。**分析方法与本程序一致，选择 Xbar 和 R 或 ANOVA**。
5. 研究变异倍数与程序一致，默认 6。比较 P/T 时匹配独立分析容差或规格限。只有一个规格限时按[单侧规格 P/T](#单侧规格-pt)设置。
6. 比较下表对应项；Trial 及 StdOrder 用于追溯，不作为响应或额外因子。Minitab 按操作员与部件识别重复测量。

| 本程序 | Minitab Xbar-R 输出 |
|---|---|
| GRR_SD | Total Gage R&R 的 StdDev |
| EV_SD | Repeatability 的 StdDev |
| AV_SD | Reproducibility 的 StdDev |
| PV_SD / TV_SD | Part-to-Part / Total Variation 的 StdDev |
| GRR/TV%、EV/TV%、AV/TV% | 相应行的%Study Var |
| P/T% | Total Gage R&R 行的%Tolerance |
| ndc | Number of Distinct Categories |
| 研究变异 | 配置倍数×StdDev |

%Study Var 采用标准差比例，%Contribution 采用方差比例。P/T 优先使用独立分析容差，其次使用双侧规格宽度或单侧等效容差。规格限和独立分析容差均未设置时，P/T 不可计算。统计结果需按实际研究的质量判据解读。

## 单侧规格 P/T

只有一个规格限时，按 `P/T（%）= 100 × k × GRR_SD ÷ (2 × |参考均值 − 规格限|)` 计算。k 为研究变异倍数。参考均值默认取全部测量值的算术平均值，程序也允许指定历史均值。距离为零时 P/T 不可计算。

将原始测量数据导入 Minitab，保持相同的研究变异倍数。直接传入单侧规格限时，Minitab 以样本均值计算距离，并在计算中使用半个研究变异。若需与指定了历史均值的程序输出核对，可将程序所用的参考均值换算为等效容差宽度，并通过 `TOLER` 传入。进行此项对标时，Minitab 的 `%Tolerance` 对应程序的 P/T。 [Minitab 单侧规格计算说明](https://support.minitab.com/en-us/minitab/help-and-how-to/quality-and-process-improvement/measurement-system-analysis/supporting-topics/gage-r-r-and-wheeler-s-emp-studies/calculation-of-percent-tolerance-with-a-one-sided-tolerance/)

例如长表含 StdOrder、规格下限为 2、研究变异倍数为 6，导入后可在 Minitab 会话命令窗格执行以下命令。列 C34 的首行为总量具 R&R 的 `%Tolerance`。复核等效容差时，将 `LSL 2` 替换为 `TOLER` 加上由样本均值换算的容差宽度。

```text
GAGERR;
  PARTS C3;
  OPERS C2;
  RESPONSE C5;
  XBAR;
  STUDYVAR 6;
  SSTDEV C30;
  SDISTINCT C31;
  SSTUDYVAR C32 C33;
  LSL 2;
  STOLERANCE C34.
```

Minitab 22.6 COM 已对 24 组仅有下限的研究完成两种口径的核对，共 1,125 条测量记录。原生单侧输入和等效容差输入在这 24 组得到相同的全精度 P/T，与程序的最大绝对误差为 1.563194 × 10⁻¹³ 个百分点。 [数据范围与复核结果](验证报告.md#minitab-com-对标结果)

## CSV 列和运行顺序

未勾选标准序时：

```csv
Operator,Part,Trial,Measurement
```

勾选后：

```csv
StdOrder,Operator,Part,Trial,Measurement
```

Operator 为 A/B/C 等人员标记，Part 从 1 开始，Trial 为原始重复测量编号。CSV 行序就是实际运行顺序，导入时无需为统计计算再次排序。

| 模式 | 规则 |
|---|---|
| 标准顺序 / `standard` | Trial→Part→Operator |
| 全部随机化 / `all` | 打乱全部完整记录，允许人员、部件和 Trial 跨组混排 |
| 操作员内随机化 / `within_operator` | 逐 Trial、逐操作员，每个 Trial＋Operator 内独立打乱部件 |

“随机化操作员顺序”仅第三种模式生效，每项研究随机确定一次人员顺序，各 Trial 沿用；随机选择也可能恰好抽到原人员顺序。StdOrder 记录随机化前的标准序号，范围为 1～N，N=人员×次数×件数：

```text
StdOrder = ((Trial−1)×件数 + Part−1)×人数 + 人员编号
A=1，B=2，C=3
```

例如 3 人×3 次×10 件时，StdOrder=62 始终对应 B、Part1、Trial3，即使移到 CSV 第 2 行，Trial 仍是 3。随机化只移动整条记录，Measurement、Trial 及字段绑定关系保持不变。

“默认”格式为带列号和人员标记的宽表，不能完整表达逐条运行顺序，相关选项禁用。用于上述列映射时请选择任一长表格式；已有宽表需先堆叠成对应测量记录。五种软件格式共用相同长表与排序实现，其名称表示排版预设；其他软件应用的实际导入不在 Minitab 对标验证范围内。

## 两个种子与同一份数据

| 种子 | GUI 位置 / CLI 参数 | 控制内容 |
|---|---|---|
| 数据生成种子 | 选项 → 研究 → 生成参数 / `--seed` | 测量值及生成搜索 |
| 运行顺序种子 | 导出 → 运行顺序 / `--run-seed` | 独立重排完整测量记录 |

同一结果切换三种顺序，直接修改导出设置后重复导出即可。生成种子留空后点击“重新生成”，得到的是另一项研究，其统计值可能变化。跨 CLI 进程复现应固定生成种子及排序种子，并保持生成参数、运行配置、版本和搜索条件一致。CLI 每次正常执行都会生成研究。

排序种子留空时，在该研究内首次需要随机排列时自动确定；同一研究、相同运行配置导出到五种软件时复用同一排列。修改运行模式或随机人员开关会改变排序条件。相同排序种子不要求与 Minitab 的逐行随机结果完全相同；三种模式的语义依据 Minitab 的[随机化选项](https://support.minitab.com/zh-cn/minitab/help-and-how-to/quality-and-process-improvement/measurement-system-analysis/how-to/gage-study/create-gage-r-r-study-worksheet/perform-the-analysis/specify-the-randomization-options/)和[工作表随机化说明](https://support.minitab.com/zh-cn/minitab/help-and-how-to/quality-and-process-improvement/measurement-system-analysis/supporting-topics/gage-r-r-and-wheeler-s-emp-studies/worksheet-randomization-for-a-gage-r-r-study-or-a-wheeler-s-emp-study/)。

## 同组数据核对

使用生成种子 123、排序种子 321，只生成一次 3 人 × 3 次 × 10 件数据，再按三种运行顺序分别导出并导入 Minitab。逐份核对 90 条记录的 Trial、StdOrder 和测量值，按 StdOrder 恢复标准顺序后比较完整记录的 SHA-256。

分别核对 GRR、EV、AV、PV 和 TV 的标准差、研究变异、%Study Var 与 %Tolerance，再核对 ndc，共 21 项。三种顺序只改变记录行序，浮点求和允许在[比较判据](验证报告.md#统计判据与基准)内出现末位差异，ndc 要求精确相等。

## 重复执行实测

需要本机已安装、许可可用的 Minitab 桌面版及已构建的`msa_export_tests.exe`：

```powershell
cmake --build build --config Release --target msa_export_tests
powershell.exe -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File .\tests\test_minitab_run_order.ps1
```

脚本通过测试程序的`--same-study`入口只生成一次，再使用同一个 Result 导出三次；不会以三次 CLI 生成为替代。可用`-Config`指定其他有效配置，`-BuildDirectory`指定构建目录。输出保存在新建的`build/minitab-same-study-时间/`，最新目录写入`build/latest-minitab-same-study.txt`。

批量 240 组实测入口为`tests/test_minitab.ps1`，支持`-BuildDirectory`和可选`-CaseLimit`；结果保存在`build/minitab-live-时间/`。常规 CTest 不依赖 Minitab，日常生成和导出也不需要安装 Minitab。命令与 COM 参考见 Minitab 官方[会话命令](https://support.minitab.com/en-us/minitab/session-command-help/)和[自动化接口](https://support.minitab.com/en-us/minitab/media/pdfs/notranslate/Minitab_Automation.pdf)。

批量脚本通过双侧容差复核 P/T。它将需要单侧口径或独立分析容差的 P/T 标为 `not_compared`，将无规格且无独立分析容差的 P/T 标为 `not_applicable`。每例的 `pt_comparison` 和汇总的 `pt_comparisons` 保留比较范围与原因。原批次留出的 24 组单侧规格已按上文单独补测。`tests/test_minitab_policy.ps1` 使用 CLI 输出核对分类和容差判据，运行时无需安装 Minitab。

## ANOVA 与其他软件

ANOVA 默认以 α=0.05 自动判断交互项。对比时应统一模型和交互项保留策略。JMP 使用随机效应 EMS，SPSS 使用 Type III 随机效应 VARCOMP，MATLAB 使用 `anovan` 的 `random=[1 2]`，Origin 使用原生 ANOVA 接口及工作表回读。详见[ANOVA 设计与五软件对标](ANOVA设计与五软件对标.md)。
