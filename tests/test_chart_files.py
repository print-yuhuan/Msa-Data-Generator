"""回读实际导出文件；PDF 由 PyMuPDF 渲染，图片由 Pillow 解码。"""

import argparse
import json
from pathlib import Path
from xml.etree import ElementTree

import fitz
from PIL import Image, ImageChops


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.directory / "chart-regression.json").read_text(encoding="utf-8-sig"))
    checks = []
    pdf_count = 0
    svg_panels = []
    for item in manifest["files"]:
        path = Path(item["path"])
        try:
            expected_size = (item["width"], item["height"])
            if item["format"] == ".pdf":
                pdf_count += 1
                with fitz.open(path) as document:
                    require(document.page_count == 1, "组合图必须为单页")
                    page = document[0]
                    expected_points = [length * 72 / item["dpi"] for length in expected_size]
                    require(abs(page.rect.width - expected_points[0]) < 0.002 and
                            abs(page.rect.height - expected_points[1]) < 0.002, "PDF 页面尺寸错误")
                    images = page.get_images(full=True)
                    require(len(images) == 1, "当前 PDF 应包含一幅组合栅格图")
                    require(images[0][2:4] == expected_size, "PDF 内嵌图片尺寸错误")
                    pixmap = page.get_pixmap(matrix=fitz.Matrix(item["dpi"] / 72, item["dpi"] / 72), alpha=False)
                    require(abs(pixmap.width - expected_size[0]) <= 1 and
                            abs(pixmap.height - expected_size[1]) <= 1, "PDF 渲染尺寸错误")
                    image = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
                    # 物理页面高度经过三位小数取整，渲染像素可比原图多一行。
                    if path.name == "single.pdf":
                        pixel = image.getpixel((50, 60))
                        require(pixel[0] > 220 and pixel[1] < 50 and pixel[2] < 50, "PDF 内容方向或位置错误")
                    render_path = path.with_name(path.stem + "_pdf_preview.png")
                    image.save(render_path)
            elif item["format"] == ".svg":
                root = ElementTree.parse(path).getroot()
                ns = "{http://www.w3.org/2000/svg}"
                require(root.tag == ns + "svg", "缺少 SVG 根节点")
                size = tuple(int(root.get(key)) for key in ("width", "height"))
                require(size == expected_size, "SVG 尺寸错误")
                panels = root.findall(ns + "svg")
                require(len(panels) == item["panels"], "组合 SVG 的面板数错误")
                require(len({(p.get("x"), p.get("y")) for p in panels}) == len(panels),
                        "SVG 面板互相覆盖")
                ids = [elem.attrib["id"] for elem in root.iter() if "id" in elem.attrib]
                require(len(set(ids)) == len(ids), "SVG 出现重复的裁剪路径名称")
                require(root.find(".//" + ns + "image") is None, "SVG 意外使用了位图")
                for panel in panels:
                    require(panel.find(".//" + ns + "line") is not None or
                            panel.find(".//" + ns + "rect") is not None,
                            "面板缺少矢量图元")
                if len(panels) > 1:
                    require(all(list(panel.iter(ns + "text")) for panel in panels),
                            "组合图未保留可编辑文字")
                with fitz.open(path) as document:
                    pixmap = document[0].get_pixmap(alpha=False)
                    image = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
                svg_panels.append(len(panels))
            else:
                with Image.open(path) as source:
                    source.load()
                    require(source.size == expected_size, "图片尺寸与清单不符")
                    image = source.convert("RGB")
                    if "A" in source.getbands():
                        require(source.getchannel("A").getextrema() == (255, 255), "图表包含透明背景")
                    dpi = source.info.get("dpi")
                    require(dpi and abs(dpi[0] - item["dpi"]) < 1, "图片 DPI 缺失或不符")
            require(min(image.getpixel((0, 0))) > 245, "外边距不是白色")
            require(ImageChops.difference(image, Image.new("RGB", image.size, "white")).getbbox(),
                    "图像内容为空")
            checks.append({"name": path.name, "passed": True, "width": image.width, "height": image.height})
        except Exception as error:
            checks.append({"name": path.name, "passed": False, "error": str(error)})
    checks.append({"name": "已验证单图与两种分析方法的组合 PDF", "passed": pdf_count >= 3})
    checks.append({"name": "已验证单图与两种分析方法的七图组合 SVG",
                   "passed": 1 in svg_panels and svg_panels.count(7) >= 2})
    failed = sum(not item["passed"] for item in checks)
    report = {"passed": failed == 0, "count": len(checks), "failed": failed, "checks": checks,
              "pdf_decoder": "PyMuPDF", "image_decoder": "Pillow", "real_network": False}
    (args.directory / "chart-file-decoding.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"图表文件实际解码检查：{len(checks)} 项，失败 {failed} 项。")
    for item in checks:
        if not item["passed"]:
            print(item["name"] + "：" + item.get("error", "缺少必需输出"))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
