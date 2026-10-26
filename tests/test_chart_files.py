"""回读实际导出文件；PDF 由 PyMuPDF 渲染，图片由 Pillow 解码。"""

import argparse
import json
from pathlib import Path

import fitz
from PIL import Image, ImageChops


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.directory / "chart-regression.json").read_text(encoding="utf-8-sig"))
    checks = []
    pdf_count = 0
    for item in manifest["files"]:
        path = Path(item["path"])
        try:
            expected_size = (item["width"], item["height"])
            if item["format"] == ".pdf":
                pdf_count += 1
                with fitz.open(path) as document:
                    assert document.page_count == 1, "组合图必须为单页"
                    page = document[0]
                    expected_points = [length * 72 / item["dpi"] for length in expected_size]
                    assert abs(page.rect.width - expected_points[0]) < 0.002
                    assert abs(page.rect.height - expected_points[1]) < 0.002
                    images = page.get_images(full=True)
                    assert len(images) == 1, "当前 PDF 应包含一幅组合栅格图"
                    assert images[0][2:4] == expected_size, "PDF 内嵌图片尺寸错误"
                    pixmap = page.get_pixmap(matrix=fitz.Matrix(item["dpi"] / 72, item["dpi"] / 72), alpha=False)
                    assert abs(pixmap.width - expected_size[0]) <= 1
                    assert abs(pixmap.height - expected_size[1]) <= 1
                    image = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
                    # 物理页面高度经过三位小数取整，渲染像素可比原图多一行。
                    if path.name == "single.pdf":
                        pixel = image.getpixel((50, 60))
                        assert pixel[0] > 220 and pixel[1] < 50 and pixel[2] < 50, "PDF 内容方向或位置错误"
                    render_path = path.with_name(path.stem + "_pdf_preview.png")
                    image.save(render_path)
            else:
                with Image.open(path) as source:
                    source.load()
                    assert source.size == expected_size, "图片尺寸与清单不符"
                    image = source.convert("RGB")
                    if "A" in source.getbands():
                        assert source.getchannel("A").getextrema() == (255, 255), "图表包含透明背景"
                    dpi = source.info.get("dpi")
                    assert dpi and abs(dpi[0] - item["dpi"]) < 1, "图片 DPI 缺失或不符"
            assert min(image.getpixel((0, 0))) > 245, "外边距不是白色"
            assert ImageChops.difference(image, Image.new("RGB", image.size, "white")).getbbox(), "图像内容为空"
            checks.append({"name": path.name, "passed": True, "width": image.width, "height": image.height})
        except Exception as error:
            checks.append({"name": path.name, "passed": False, "error": str(error)})
    checks.append({"name": "已验证单图与两种分析方法的组合 PDF", "passed": pdf_count >= 3})
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
