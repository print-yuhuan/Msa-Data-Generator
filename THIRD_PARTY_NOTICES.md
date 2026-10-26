# 第三方组件与许可声明

本项目的原创代码、文档和自有图标采用 [MIT License](LICENSE)，作者为 **print-yuhuan**，项目仓库为 [Msa-Data-Generator](https://github.com/print-yuhuan/Msa-Data-Generator)。第三方代码、字体及相关资源保留各自的版权声明和许可证，不因项目采用 MIT 而变更。第三方衍生文件中的适用条款同样保留。

## 组件清单

依赖源码随仓库提供，构建时无需下载。版本、来源和资源校验值见 [dependencies.lock.json](dependencies.lock.json)；纳入仓库的范围见 [第三方源码说明](third_party/README.md)。

| 组件 | 固定版本或来源 | 用途 | 适用许可证 |
| --- | --- | --- | --- |
| EUI-NEO | 0.6.1；上游提交 `1647709068e36834ab26da5a97ef4ad98aa34c9d` | 界面布局、输入、滚动、渲染与事件处理 | Apache-2.0 |
| GLFW | EUI-NEO 内置 3.4 | 窗口、键盘与 OpenGL 上下文 | zlib/libpng |
| FreeType | EUI-NEO 内置 2.13.3 | 字体栅格化 | FreeType License（本项目选择 FTL）及附带的组件许可 |
| libpng | 1.6.43 | PNG 图像解码 | libpng |
| zlib | 1.3.1 | 图像解码、完整字体的无损压缩与解压 | zlib |
| glad | 生成器 0.1.34 | OpenGL 函数加载 | MIT；Khronos 头文件保留其附带许可 |
| stb_image | 2.30 | 图像解码 | MIT（本项目选择该许可选项） |
| miniaudio | 源码随 EUI-NEO 上游提交固定 | 框架构建依赖；本程序未提供音频功能 | MIT No Attribution（本项目选择该许可选项） |
| yyjson | 0.12.0 | 框架依赖 | MIT |
| tray | 源码随 EUI-NEO 上游提交固定 | 随源码保留；本项目关闭托盘功能 | MIT |
| md4c | 源码随 EUI-NEO 上游提交固定 | 随源码保留；本项目关闭 Markdown 模块 | MIT |
| nlohmann/json | 3.12.0 | 配置与报告 JSON | MIT |
| Noto Sans CJK SC Regular | 2.004 | 内嵌于桌面主程序的完整中文字体 | SIL Open Font License 1.1 |

完整许可文本位于 [licenses/](licenses/) 和各组件的源码目录。`md4c` 的许可保留在 [源码目录](third_party/EUI-NEO/3rd/md4c/LICENSE.md)，该模块不参与本项目的发布构建。FreeType 的 FTL、BDF、PCF、HarfBuzz 衍生代码等相关声明均随 `licenses/` 保留。

## 版权声明

Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved.

Noto Sans CJK SC Regular 的原始字体元数据包含以下版权声明，内嵌时完整保留：

> © 2014-2021 Adobe (http://www.adobe.com/).

其他组件的作者、版权年份及许可正文以随附文件为准。

## 字体与资源

字体原文件与 OFL 正文位于 [resources/fonts/](resources/fonts/)。构建时使用仓库内的 C++ 辅助程序和 zlib，将完整字体无损分块压缩为 EXE 的 RCDATA 资源；不裁剪字形、布局表或 hinting 信息。

桌面主程序首次使用字体时，将其解码为与原 OTF 文件逐字节相同的共享内存，并校验 CRC32。此过程不依赖运行目录中的外部字体，也不释放临时字体文件。压缩仅改变存储表示，不改变字体内容或许可。Windows 运行包不包含外部 `resources/` 目录，但保留 `licenses/Noto-OFL.txt` 及相关声明。

程序使用本项目独立设计的应用图标，设计源与生成方法见 [图标说明](resources/icons/README.md)。发布包未包含 EUI-NEO 的演示音乐、图片集合或视频。

## 本地集成与分发

[EUI-NEO 集成补丁](patches/EUI-NEO-windows-integration.patch)记录了 Windows 资源编码、输入与滚动处理、内存字体注册、回退字体解析及文本度量缓存等改动。当前源码还包含焦点同步和首次显示重绘等集成修正；该补丁不是当前全部改动的完整差异，实际构建以仓库内源码为准。已应用的补丁无需重复执行。

当前开发版本另在输入组件增加密码掩码显示，用于 AI 密钥编辑。图表导出复用随仓库提供的 FreeType 与内嵌 Noto 字体；Windows 图像编码和网络请求使用系统接口，不新增第三方运行库。

文本渲染的本地修改还包含多页字形图集、有界 LRU 和 OpenGL 纹理页提交，解决固定图集满后缺字的问题；设计与回归范围见 [字体图集设计与验证](docs/字体图集设计与验证.md)。原字体内容、许可及 FreeType 栅格化参数保持不变。

本项目的运行包同时保留根目录 `LICENSE`、`NOTICE`、本说明和 `licenses/`。项目 MIT 许可全文另复制为 `licenses/MSA-Project-LICENSE.txt`；`NOTICE` 为项目来源说明，不对 MIT 增加限制。再分发时，应遵守 MIT 的版权与许可通知保留要求，并保留所使用第三方组件各自要求的声明与许可材料。

## 外部分析软件

Minitab、IBM SPSS Statistics、JMP、Origin 和 MATLAB 用于说明 CSV 导出预设及可选的统计对标环境。本项目不随包分发这些软件，也不代表其厂商认可或背书。

常规使用、数据生成、CSV 导出和 CTest 回归不要求安装上述软件。执行可选的原生软件对标时，需要自行安装并取得相应授权。已验证范围及差异见 [验证说明](docs/验证报告.md)和 [GUI 参数兼容与对标记录](docs/GUI参数兼容与对标进度.md)。
