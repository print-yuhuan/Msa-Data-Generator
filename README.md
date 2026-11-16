<div align="center">
  <h1>MSA 数据生成器</h1>
  <p>生成交叉量具 R&amp;R 模拟数据，复核统计指标，导出测量记录。</p>
  <p><strong>Windows x64</strong> · v1.0.1 · <a href="LICENSE">MIT 许可证</a></p>
  <p>
    <a href="https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.0.1/MSA_Data_Generator_v1.0.1_Windows_x64.zip"><strong>下载 Windows x64</strong></a>
    · <a href="#快速开始">快速开始</a>
    · <a href="docs/README.md">文档中心</a>
  </p>
</div>

<p align="center">
  <a href="docs/ui-preview.png">
    <img src="docs/ui-preview.png" alt="MSA 数据生成器的数据页，左侧是生成设置与指标约束，右侧是复核摘要和指标表" width="960">
  </a>
</p>
<p align="center"><sub>数据页 · Xbar-R · 3 名操作员 × 3 次重复测量 × 10 个部件</sub></p>

## 快速开始

1. 下载 [Windows x64 便携包][windows-zip]，解压后双击 `msa_generator.exe`。
2. 在**数据**页点击**载入示例**，再点击**生成数据**。
3. 在右侧查看**指标复核**、**数据预览**和**分析详情**，或切换到**图表**页。
4. 在**选项 → 导出**选择格式与运行顺序，然后返回数据页点击**导出 CSV**。

桌面程序内嵌中文字体和图标，可免安装运行，图形环境需支持 **OpenGL 3.3 Core**。压缩包还包含 CLI、示例配置、回归验证工具和许可证。[发布页][release] · [SHA-256 校验文件][checksum]

> [!NOTE]
> 生成结果适用于方法学习、软件对标和流程验证。测量系统的实际判定须依据真实测量记录。

## 主要功能

| 能力 | 支持范围 |
| --- | --- |
| 研究与生成 | 2～3 名操作员、2～3 次重复测量、5～10 个部件，共 24 种平衡交叉设计。支持五种部件取样方案及可选最低跨度，可按需筛选近似正态的测量值。[取样设置](docs/使用说明.md#部件取样与跨度) |
| 统计复核 | 支持 **Xbar-R（均值极差法）** 与 **ANOVA（方差分析法）**。可设置 EV/TV、AV/TV、GRR/TV、P/T、ndc 的目标值、区间或上下限。 |
| 分析选项 | 支持研究变异倍数、独立分析容差和历史过程参数。ANOVA 另支持交互项设置、误分类概率和方差分量置信限。 |
| 图表 | 提供七类 MSA 图表，支持操作员分面、控制限、坐标与外观编辑，可导出 PNG、JPEG 或 PDF。 |
| CSV 导出 | 提供默认宽表及 Minitab、SPSS、JMP、Origin、MATLAB 五种长表预设。可设置运行顺序、独立排序种子、`StdOrder` 和文件名模板。 |

数据生成、统计分析、图表绘制和文件导出均在本机完成。桌面程序和 CLI 共用统计内核与配置规则。统计口径及外部软件的参数差异见 [Minitab 导入与对标](docs/Minitab导入与对标.md)和 [ANOVA 对标记录](docs/ANOVA设计与五软件对标.md)。

## v1.0.1 更新

- **生成设置** · 新增部件取样、最低部件跨度和近似正态筛选。沿用默认取样并关闭新增筛选时，原有固定种子可继续复现同一组测量值。
- **选项与反馈** · 补充规格、指标与分析选项的联动检查，修复随机化模式切换后运行顺序种子提示不刷新的问题。
- **帮助与对标** · 重整内置帮助，并补充 Minitab 22 单侧规格 P/T 的计算口径和复现步骤。

## 命令行

在解压目录打开 PowerShell，运行以下命令生成一份 Minitab 长表和 JSON 报告。

```powershell
.\msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 `
  --csv-format minitab --run-order all --run-seed 321 --store-standard-order true `
  --output ".\data.csv" --report ".\result.json"
```

`--seed` 控制测量值，`--run-seed` 控制 CSV 行序。CLI 也支持分析已有 CSV。运行 `.\msa_cli.exe --help` 查看选项，完整用法见 [CLI 使用说明](docs/使用说明.md#cli)和 [配置参考](docs/配置参考.md)。

## 文档导航

| 场景 | 文档 |
| --- | --- |
| 操作与配置 | [使用说明](docs/使用说明.md) · [配置参考](docs/配置参考.md) · [示例配置](examples/README.md) |
| 图表与导出 | [结果可视化](docs/结果可视化.md) · [Minitab 导入与对标](docs/Minitab导入与对标.md) |
| 统计与验证 | [ANOVA 设计与五软件对标](docs/ANOVA设计与五软件对标.md) · [GUI 参数对标](docs/GUI参数兼容与对标.md) · [验证说明](docs/验证报告.md) |
| 开发与发布 | [界面设计规范](docs/界面设计规范.md) · [界面验收](docs/界面验收.md) · [源码打包规则](docs/源码打包规则.md) |

## 构建与验证

在 Windows x64 上使用 **Visual Studio C++ 工具链**、**Windows SDK** 和 **CMake 3.24+**。构建依赖随源码提供，版本与来源记录在 [依赖清单](dependencies.lock.json)。

```powershell
cmake -S . -B build -A x64 -DMSA_BUILD_RENDER_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

可执行文件位于 `build/Release/`。图形回归需要可用的桌面 OpenGL 环境。运行 `.\scripts\package.ps1 -BuildDirectory build`，可在 `dist/` 生成 Windows x64 便携包和 SHA-256 校验文件。[验证说明](docs/验证报告.md)

## 许可证

© 2026 **print-yuhuan**。项目原创内容采用 [MIT 许可证](LICENSE)。第三方代码、字体及资源适用各自许可证。[作者与项目来源](NOTICE) · [第三方许可声明](THIRD_PARTY_NOTICES.md)

欢迎提交可复现的问题。反馈时可附上软件版本、研究设置、操作步骤和预期结果。[GitHub Issues](https://github.com/print-yuhuan/Msa-Data-Generator/issues)

[release]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/tag/v1.0.1
[windows-zip]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.0.1/MSA_Data_Generator_v1.0.1_Windows_x64.zip
[checksum]: https://github.com/print-yuhuan/Msa-Data-Generator/releases/download/v1.0.1/MSA_Data_Generator_v1.0.1_Windows_x64.zip.sha256
