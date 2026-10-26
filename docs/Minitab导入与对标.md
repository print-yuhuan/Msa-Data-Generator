# Minitab 导入、运行顺序与结果核对

本程序支持交叉量具 R&R 的 Xbar-R 与 ANOVA 测量数据。对标时将**同一组已生成数据**交给 Minitab 重新计算，统一方法、研究倍数和规格口径。已实测 Minitab 22.6；原始数据和详细结果见[同组数据专项报告](2026-10-25_同组数据三种运行顺序Minitab对标.md)与[全面测试报告](2026-10-25_全面测试与Minitab对标.md)。

## 推荐导入流程

1. 在“选项 → 导出设置 → CSV 导出格式”选择“Minitab”，设置运行顺序、可选排序种子与 StdOrder。已有生成结果时直接导出，无需重新生成。
2. 在 Minitab 打开 CSV，使用英文逗号分隔、首行列名；Operator 为人员字母，其他列为数字。所有长表统一 UTF-8 无 BOM、CRLF，保持配置的小数位。
3. 进入“统计 → 质量工具 → 量具研究 → 交叉量具 R&R”（英文界面为 Stat → Quality Tools → Gage Study → Gage R&R Study (Crossed)）。
4. 按列名选择：部件=Part，操作员=Operator，测量数据=Measurement。**分析方法与本程序一致，选择 Xbar 和 R 或 ANOVA**。
5. 研究变异倍数与程序一致，默认 6；比较 P/T 时使用相同双侧规格限或容差 USL−LSL。
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

%Study Var 采用标准差比例，%Contribution 采用方差比例，数值含义不同。P/T 优先使用独立分析容差，其次使用双侧规格宽度；仅有单侧规格时使用规格限到参考均值距离的两倍，距离为零时不可计算。无规格且未设置独立分析容差时，P/T 不可计算。对标须统一这些口径，不应将单侧结果直接与双侧容差比较。满足设定生成约束也不等于测量系统已达到某项质量判定标准。

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
| 不随机化 / `standard` | Trial→Part→Operator |
| 全部随机化 / `all` | 打乱全部完整记录，允许人员、部件和 Trial 跨组混排 |
| 操作员内部随机化 / `within_operator` | 逐 Trial、逐操作员，每个 Trial＋Operator 内独立打乱部件 |

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
| 数据生成种子 | 高级搜索 / `--seed` | 测量值及生成搜索 |
| 排序种子 | 运行顺序 / `--run-seed` | 独立重排完整测量记录 |

同一结果切换三种顺序，直接修改导出设置后重复导出即可。生成种子留空后点击“重新生成”，得到的是另一项研究，其统计值可能变化。跨 CLI 进程复现应固定生成种子及排序种子，并保持生成参数、运行配置、版本和搜索条件一致。CLI 每次正常执行都会生成研究。

排序种子留空时，在该研究内首次需要随机排列时自动确定；同一研究、相同运行配置导出到五种软件时复用同一排列。修改运行模式或随机人员开关会改变排序条件。相同排序种子不要求与 Minitab 的逐行随机结果完全相同；三种模式的语义依据 Minitab 的[随机化选项](https://support.minitab.com/zh-cn/minitab/help-and-how-to/quality-and-process-improvement/measurement-system-analysis/how-to/gage-study/create-gage-r-r-study-worksheet/perform-the-analysis/specify-the-randomization-options/)和[工作表随机化说明](https://support.minitab.com/zh-cn/minitab/help-and-how-to/quality-and-process-improvement/measurement-system-analysis/supporting-topics/gage-r-r-and-wheeler-s-emp-studies/worksheet-randomization-for-a-gage-r-r-study-or-a-wheeler-s-emp-study/)。

## 已通过的同组实测

只生成一次 3 人×3 次×10 件数据（生成种子 123，排序种子 321），连续导出三份，再实际导入 Minitab。每份 90 条，无丢失、重复，Trial/StdOrder 逐条正确，Measurement 回读 double 精确相等；按 StdOrder 恢复标准序后完整记录的 SHA-256 相同。

21 项统计对比全部通过。三种顺序间最大差约 2.81×10⁻¹³个百分点，ndc 精确一致。改变记录顺序可能引入浮点求和末位差异；不应据此改写测量值或放宽用户生成约束。详见[完整报告及三份 CSV](2026-10-25_同组数据三种运行顺序Minitab对标.md)。

## 重复执行实测

需要本机已安装、许可可用的 Minitab 桌面版及已构建的`msa_export_tests.exe`：

```powershell
cmake --build build --config Release --target msa_export_tests
powershell.exe -NoProfile -NonInteractive -STA -ExecutionPolicy Bypass -File .\tests\test_minitab_run_order.ps1
```

脚本通过测试程序的`--same-study`入口只生成一次，再使用同一个 Result 导出三次；不会以三次 CLI 生成为替代。可用`-Config`指定其他有效配置，`-BuildDirectory`指定构建目录。输出保存在新建的`build/minitab-same-study-时间/`，最新目录写入`build/latest-minitab-same-study.txt`。

批量 240 组实测入口为`tests/test_minitab.ps1`，支持`-BuildDirectory`和可选`-CaseLimit`；结果保存在`build/minitab-live-时间/`。常规 CTest 不依赖 Minitab，日常生成和导出也不需要安装 Minitab。命令与 COM 参考见 Minitab 官方[会话命令](https://support.minitab.com/en-us/minitab/session-command-help/)和[自动化接口](https://support.minitab.com/en-us/minitab/media/pdfs/notranslate/Minitab_Automation.pdf)。

该批量脚本当前只向 Minitab 传入双侧容差，因此单侧 P/T 和独立分析容差的 P/T 明确记为 `not_compared`，不算作这些指标的外部对标通过；没有规格及独立分析容差时记为 `not_applicable`。每例的 `pt_comparison` 和汇总的 `pt_comparisons` 保留比较范围与原因。`tests/test_minitab_policy.ps1` 可在不安装或启动 Minitab 的情况下，使用 CLI 实际结果回归这些判据及双侧不一致拒绝分支；这项本地回归不能替代软件实测。

## ANOVA 与其他软件

ANOVA 默认交互项自动判断、α=0.05；保留/合并策略及模型须一致。JMP 使用 EMS 及随机效应；SPSS 使用 Type III 随机效应 VARCOMP；MATLAB 使用`anovan`的`random=[1 2]`；Origin 验证原生 ANOVA 接口及工作表回读。详细公式、27 组实测和限制见[ANOVA 设计与五软件对标](ANOVA设计与五软件对标.md)。既有两份 Xbar-R 报告保留其原方法与被测哈希。
