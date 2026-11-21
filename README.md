<div align="center">
  <h1>MSA 数据生成器</h1>
  <p>生成交叉量具 R&amp;R 模拟数据，或导入已有测量记录，在同一组数据上比较均值极差法与方差分析法。</p>
  <p><strong>Windows x64</strong> · v1.1.3 · <a href="LICENSE">MIT 许可证</a></p>
  <p>
    <a href="https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.1.3/MSA_Data_Generator_v1.1.3_Windows_x64.zip"><strong>下载 Windows x64</strong></a>
    · <a href="#快速开始">快速开始</a>
    · <a href="docs/README.md">文档中心</a>
  </p>
</div>

<p align="center">
  <a href="docs/images/ui-preview.png">
    <img src="docs/images/ui-preview.png" alt="MSA 数据生成器的数据页，左侧是生成设置与指标约束，右侧是复核摘要和指标表" width="960">
  </a>
</p>
<p align="center"><sub>v1.1.3 实机截图 · Xbar-R 指标复核 · 3 名操作员 × 3 次重复测量 × 10 个部件，共 90 条测量记录</sub></p>

## 快速开始

下载 [Windows x64 便携包][windows-zip]，解压后运行 `msa_generator.exe`。桌面程序内嵌中文字体和图标，图形环境需支持 **OpenGL 3.3 Core**。包内还包含命令行工具、示例配置、验证工具和许可证。[发布页][release] · [SHA-256 校验文件][checksum]

| 想完成的任务 | 操作 |
| --- | --- |
| 生成一组示例数据 | 在「数据」页点击「载入示例」，再点击「生成数据」。在右侧的「指标复核」对照目标和实际值，「数据预览」可逐行查看读数。 |
| 分析已有测量 | 在「图表」页点击「导入数据」选择 CSV 或 XLSX。具名数据沿用文件中的操作员，多工作表 XLSX 可选表。六行无名称的纯数值宽表会询问是 2 人各测 3 次，还是 3 人各测 2 次。 |
| 保存测量、图表或报告 | 「数据」页的「导出数据」可保存 CSV 或 XLSX。「图表」页可导出图表或 PDF、HTML 方法对比报告。默认弹出“另存为”，可在「选项 → 文件」分别设置目录和静默导出。 |

配置文件使用 `"schema_version": 2`，默认保存在 `%USERPROFILE%\MSA数据生成器\msa_generator.json`。首次配置可从随包示例开始。测量数据、图表和报告的默认目录为当前用户的 Windows 系统桌面。[配置参考](docs/配置参考.md) · [示例文件](examples/README.md)

> [!NOTE]
> 生成结果适用于方法学习、软件对标和流程验证。测量系统的实际判定须依据真实测量记录。

## 主要功能

| 能力 | 支持范围 |
| --- | --- |
| 研究与生成 | 2～3 名操作员、2～3 次重复测量、5～10 个部件，共 24 种平衡交叉设计。提供十种部件取样方案，可在上下规格限之间随机取样，也可选择分层随机、正态分位或双批取样等方案。可按需设置最低跨度和近似正态筛选。[取样设置](docs/使用说明.md#部件取样与跨度) |
| 统计复核 | 支持 **Xbar-R（均值极差法）** 与 **ANOVA（方差分析法）**。可设置 EV/TV、AV/TV、GRR/TV、P/T、ndc 的目标值、区间或上下限。 |
| 分析选项 | 支持研究变异倍数、独立分析容差和历史过程参数。ANOVA 另支持交互项设置、误分类概率和方差分量置信限。 |
| 图表 | 提供七类 MSA 图表，可在默认和 Origin 两种风格下编辑坐标、图例和数据点，支持操作员分面、控制限与单图或组合图导出。可选格式为 PNG、JPEG、PDF 和矢量 SVG。 |
| 方法对比报告 | 在同一份报告中查看 Xbar-R 和 ANOVA 的指标、均值图和极差图。导出 PDF 需要本机 Microsoft Edge 或 Google Chrome。[报告说明](docs/结果可视化.md#方法对比报告) |
| 数据文件 | 可导入和导出 CSV 或 Excel 工作簿（XLSX）。提供默认宽表及 Minitab、SPSS、JMP、Origin、MATLAB 五种长表预设。可设置运行顺序、独立排序种子、`StdOrder` 和名称变量。 |

数据生成、统计分析、图表绘制和文件导出均在本机完成。桌面程序和 CLI 共用统计内核与配置规则。统计口径及外部软件的参数差异见 [Minitab 导入与对标](docs/Minitab导入与对标.md)和 [ANOVA 对标记录](docs/ANOVA设计与五软件对标.md)。

## 命令行

在解压目录打开 PowerShell。下面的示例先生成 Minitab 长表和 JSON 研究报告，再从同一份测量记录导出方法对比报告。

```powershell
.\msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 `
  --csv-format minitab --run-order all --run-seed 321 --store-standard-order true `
  --output ".\data.csv" --report ".\result.json"

.\msa_cli.exe --no-config --input ".\data.csv" --lsl 9 --usl 11 `
  --comparison-report ".\方法对比.html"
```

`--seed` 控制测量值，`--run-seed` 控制数据行序。CLI 也支持分析已有 CSV 或 XLSX。运行 `.\msa_cli.exe --help` 查看选项，完整用法见 [CLI 使用说明](docs/使用说明.md#cli)和 [配置参考](docs/配置参考.md)。

## 文档导航

| 场景 | 文档 |
| --- | --- |
| 操作与配置 | [使用说明](docs/使用说明.md) · [配置参考](docs/配置参考.md) · [示例配置](examples/README.md) |
| 图表与导出 | [结果可视化](docs/结果可视化.md) · [Minitab 导入与对标](docs/Minitab导入与对标.md) · [Origin 图表对照及样例](docs/图表专项验证.md#originpro-2026b-实测) |
| 统计与验证 | [ANOVA 设计与五软件对标](docs/ANOVA设计与五软件对标.md) · [GUI 参数对标](docs/GUI参数兼容与对标.md) · [验证说明](docs/验证报告.md) |
| 开发与发布 | [界面设计规范](docs/界面设计规范.md) · [界面验收](docs/界面验收.md) · [源码打包规则](docs/源码打包规则.md) |

## 构建与验证

统计与文件读写代码位于 `src/msa/`，桌面与命令行入口位于 `src/gui.cpp` 和 `src/cli.cpp`。`tests/` 收录回归脚本、外部软件对标脚本和参考数据，`scripts/` 提供构建、打包及发布校验工具。

在 Windows x64 上使用 **Visual Studio C++ 工具链**、**Windows SDK** 和 **CMake 3.24+**。构建依赖随源码提供，版本与来源记录在 [依赖清单](dependencies.lock.json)。在仓库根目录用 PowerShell 构建 Release 并运行全部桌面回归。

```powershell
.\scripts\build.ps1 -BuildDirectory build -FullTest
```

可执行文件位于 `build/Release/`。图形回归需要可用的桌面 OpenGL 环境。运行 `.\scripts\package.ps1 -BuildDirectory build`，可在 `dist/` 生成 Windows x64 便携包和 SHA-256 校验文件。[验证说明](docs/验证报告.md)

单独在非 Windows 平台构建核心和 CLI 时，使用 `-DMSA_BUILD_GUI=OFF` 并安装系统的 OpenSSL Crypto、libarchive 与 libxml2 开发库。Windows 使用系统提供的 BCrypt 计算文件摘要。

## 许可证

© 2026 **print-yuhuan**。项目原创内容采用 [MIT 许可证](LICENSE)。第三方代码、字体及资源适用各自许可证。[作者与项目来源](NOTICE) · [第三方许可声明](THIRD_PARTY_NOTICES.md)

欢迎提交可复现的问题。反馈时可附上软件版本、研究设置、操作步骤和预期结果。[GitHub Issues](https://github.com/print-yuhuan/Msa-Data-Generator/issues)

[release]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/tag/v1.1.3
[windows-zip]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.1.3/MSA_Data_Generator_v1.1.3_Windows_x64.zip
[checksum]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.1.3/MSA_Data_Generator_v1.1.3_Windows_x64.zip.sha256
