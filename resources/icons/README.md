# MSA 程序图标

本项目独立设计的标识：蓝色圆角底、三列白色测量数据、一枚青色新增数据块。与界面主色一致，不包含文字、纹理、细线、阴影或模糊滤镜。

- `app-icon.svg`：64 × 64 矢量设计源。
- `app-icon.ico`：16、20、24、32、40、48、64、128、256 像素九档原生尺寸；每档先对齐像素边界，再处理圆角抗锯齿。小尺寸使用 32 位位图和透明遮罩，256 像素使用 PNG。
- `app-icon-*.png`：对应尺寸的透明 PNG。
- [明暗背景与实际尺寸预览](../../docs/app-icon-preview.png)。

Windows 的主程序、CLI 和各测试 EXE 均嵌入资源组 `1` 和 `GLFW_ICON`，供资源管理器与程序窗口使用。程序使用本项目的完整 ICO 资源；GUI 中文字体和窗口清单同样内嵌，运行包不携带外部 resources 目录，也不复制 EUI 演示资源与默认图标。界面左上角按同一几何设计绘制，随显示缩放对齐像素。

修改 SVG 后，在项目根目录执行 `python scripts/generate-icons.py` 重新生成 ICO、PNG 和预览图；该设计脚本需要 Pillow 10.1 及以上，正常编译和运行程序不需要 Python。若修改图形结构，同时更新 `src/gui.cpp` 的 `brand.mark` 绘制。

## Windows 快捷方式与图标缓存

创建快捷方式时，将目标指向实际的`msa_generator.exe`，工作目录设为运行目录，图标位置使用该 EXE 本身（索引 0）。无需另行提供 ICO 或 resources 目录；开发者如选择源码中的独立 ICO 制作其他快捷方式，应自行保留相应图标文件。

如果资源管理器不同列表视图或快捷方式显示不一致，先检查目标和图标路径是否存在、是否指向同一份文件，再参照[Microsoft 图标缓存排查说明](https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/application-shortcuts-show-blank-icons)处理系统缓存。移动运行目录后，应同步修改快捷方式的目标、工作目录及独立图标路径。

程序使用方式、构建文件用途和验证方法见[文档导航](../../docs/README.md)。
