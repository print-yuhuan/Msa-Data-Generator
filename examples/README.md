# 配置示例

示例适用于 v1.0.0，均使用 `schema_version=1`。GUI 与 CLI 共用字段及校验规则。业务参数含义、类型及默认值见[配置参考](../docs/配置参考.md)，命令行覆盖方式见[使用说明](../docs/使用说明.md#cli)。

| 文件 | 用途 |
|---|---|
| [默认配置.json](默认配置.json) | 与内置初始业务设置一致：3 人×3 次×10 件、4 位小数、默认宽表、同名追加序号；规格限和参考 TV_SD 为空，开始生成前需填写有效规格或尺度 |
| [ANOVA 示例配置.json](ANOVA示例配置.json) | 平衡交叉双随机效应 ANOVA 示例，交互项自动判断，显著性水平 α=0.05 |
| [示例配置.json](示例配置.json) | 可直接生成的 3 人×3 次×10 件示例；规格 9～11、倍数 6、生成种子 123，五项指标使用目标值约束，输出带列号和人员标记的默认宽表 |
| [Minitab 随机运行配置.json](Minitab随机运行配置.json) | 沿用同一示例生成参数，改为 Minitab 长表、操作员内部随机化、随机化操作员顺序、排序种子 321 及 StdOrder |

在项目根目录使用以下命令；仅写入指定的 build 测试目录，不修改示例配置：

```powershell
.\build\Release\msa_cli.exe --config ".\examples\示例配置.json" --output ".\build\example-wide.csv"
.\build\Release\msa_cli.exe --config ".\examples\Minitab随机运行配置.json" --output ".\build\example-minitab.csv" --report ".\build\example-minitab.json"
```

显式路径已存在时应更换路径或添加`--overwrite`。省略`--output`则使用配置中的输出目录、文件名模板和同名策略。空/相对输出目录以用户默认配置目录为基准；每次 CLI 正常执行都会生成研究，相同生成种子、参数、版本且搜索未被超时截断时可复现。

两个种子互相独立：`generation.seed`控制测量值，`output.run_order.seed`控制行序。只比较运行顺序时应使用同一份已生成数据；可重复的同组实测入口为`tests/test_minitab_run_order.ps1`，详见[专项报告](../docs/2026-10-25_同组数据三种运行顺序Minitab对标.md)。

配置可省略具有内置默认值的字段，加载时自动补齐。示例显式列出字段，方便查看和编辑；编码由 CSV 格式归一化：默认宽表为`utf-8-sig`，五种长表为`utf-8`。

ANOVA 示例可由 CLI 的 `--config` 读取。若用于 GUI 启动，先备份个人配置，再将其内容保存到 GUI 默认配置位置；界面“载入示例”使用程序内置示例，并非打开任意 JSON 文件。模型、报告和验证范围见 [ANOVA 说明](../docs/ANOVA设计与五软件对标.md)。
