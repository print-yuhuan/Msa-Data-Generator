"""Check the actual v1.0.7 interaction curves and histogram bars against the Origin reference."""
import argparse
import json
from pathlib import Path
from xml.etree import ElementTree


def y_scale(panel):
    """Calibrate from the numeric axis labels and their horizontal tick marks."""
    lines = panel.findall(".//{*}line")
    ticks = []
    for text in panel.findall(".//{*}text"):
        if text.get("text-anchor") != "end":
            continue
        try:
            value = float(text.text)
        except (TypeError, ValueError):
            continue
        x, y = float(text.get("x")), float(text.get("y"))
        marks = [float(line.get("y1")) for line in lines
                 if line.get("y1") == line.get("y2") and
                 x < float(line.get("x1")) < float(line.get("x2")) <= x + 12]
        match = min(marks, key=lambda v: abs(v - y))
        if abs(match - y) > float(text.get("font-size")):
            raise ValueError("Numeric label is detached from its tick")
        ticks.append((value, match))
    (low, bottom), (high, top) = min(ticks), max(ticks)
    units = (high - low) / (top - bottom)
    return lambda y: low + (y - bottom) * units


def interaction_error(panel, reference):
    scale = y_scale(panel)
    parts = reference["parts"]
    labels = panel.findall(".//{*}text")
    x = {int(t.text): float(t.get("x")) for t in labels
         if t.get("text-anchor") == "middle" and t.text and t.text.isdecimal()}
    lines = panel.findall(".//{*}line")
    errors = []
    for name, means in reference["interaction_means"].items():
        legend = next(t for t in labels if t.text == "操作员 " + name)
        lx, ly = float(legend.get("x")), float(legend.get("y"))
        keys = [line for line in lines if line.get("y1") == line.get("y2") and
                0 < lx - float(line.get("x2")) < 12 and abs(ly - float(line.get("y2"))) < 8]
        if len(keys) != 1:
            raise ValueError(f"Expected one line key for operator {name}")
        for i, (a, b) in enumerate(zip(parts, parts[1:])):
            segment, = [line for line in lines if line.get("stroke") == keys[0].get("stroke") and
                        abs(float(line.get("x1")) - x[a]) < .01 and
                        abs(float(line.get("x2")) - x[b]) < .01]
            for k, attr in ((i, "y1"), (i + 1, "y2")):
                errors.append(abs(scale(float(segment.get(attr))) - means[k]))
    if max(errors) > .0001:
        raise ValueError(f"Plotted interaction means differ from Origin by {max(errors)}")
    return max(errors)


def histogram_counts(panel, count):
    scale = y_scale(panel)
    bars = []
    for rect in panel.findall(".//{*}rect"):
        if not rect.get("fill") or rect.get("fill", "").lower() in ("white", "#ffffff"):
            continue
        y, h = float(rect.get("y")), float(rect.get("height"))
        if abs(scale(y + h)) < .001:
            bars.append((float(rect.get("x")), float(rect.get("width")), scale(y)))
    start, width, _ = min(bars)
    counts = [0] * count
    for left, w, frequency in bars:
        index = (left - start) / width
        if (abs(index - round(index)) > .001 or not 0 <= round(index) < count or
                abs(w - width) > .01 or abs(frequency - round(frequency)) > .001):
            raise ValueError("Unexpected bin position, width or frequency")
        counts[round(index)] = round(frequency)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("svg", type=Path, help="Unedited crossed_anova_combined.svg")
    parser.add_argument("origin", type=Path, help="origin-analysis.json")
    args = parser.parse_args()
    ref = json.loads(args.origin.read_text(encoding="utf-8"))
    panels = ElementTree.parse(args.svg).getroot().findall("{*}svg")
    if len(panels) != 7:
        raise ValueError("Expected all seven default panels")
    error = interaction_error(panels[5], ref)
    expected = ref["matched_histogram"]["counts"]
    counts = histogram_counts(panels[6], len(expected))
    if counts != expected or sum(counts) != ref["records"]:
        raise ValueError(f"Expected Origin frequencies {expected}, got {counts}")
    print(json.dumps({"interaction_points": len(ref["parts"]) * len(ref["operators"]),
                      "max_svg_mean_error": error, "aligned_histogram_counts": counts,
                      "records": sum(counts)}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
