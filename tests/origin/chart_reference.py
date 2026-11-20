"""Create the optional OriginPro 2026b benchmark from measurements.csv and study.json.

In a NEW Origin project, run this from the Script Window (use absolute paths):
  string msa_chart_folder$="D:/Msa-Data-Generator/build/v107-origin";
  run -pyf "D:/Msa-Data-Generator/tests/origin/chart_reference.py";
Generate the reference inputs with msa_cli.exe from v1.0.6:
  msa_cli.exe --no-config --lsl 9 --usl 11 --seed 123 --method crossed_anova
              --csv-format origin --output measurements.csv --report study.json
Run in OriginPro 2026b with its bundled originpro, PyOrigin and ocMath 10.3.5.
"""
import csv
import json
import math
from pathlib import Path
import runpy
from statistics import mean
import tempfile
import traceback

import originpro as op
import PyOrigin


COLORS = ("#2864A6", "#CE682F", "#16847D")


def series(layer, sheet, xcol, ycol, index, kind="linesymbol", size=7):
    plot = layer.add_plot(sheet, colx=xcol, coly=ycol, type=kind)
    plot.color = COLORS[index]
    plot.symbol_kind = index + 1
    plot.symbol_size = size
    return plot


def save_png(graph, path):
    graph.save_fig(str(path), width=1400)
    if not path.is_file():
        raise RuntimeError(f"Origin did not export {path}")


def native_anova(records, operators, parts, cli, multiplier):
    # Reuse the same pinned Origin 2026b native-engine adapter as the ANOVA tests.
    if not math.isclose(op.lt_float("@V"), 10.35, abs_tol=0.001):
        raise RuntimeError("This native ANOVA adapter requires Origin 2026b (10.35).")
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        manifest = scratch / "manifest.json"
        manifest.write_text(json.dumps({"cases": [{"id": "chart", "folder": tmp}]}))
        encoded = [[i, operators.index(o) + 1, p, t, v]
                   for i, (o, p, t, v) in enumerate(records, start=1)]
        (scratch / "origin-import.json").write_text(json.dumps({"data": encoded}))
        PyOrigin.LT_set_str("msa_manifest", str(manifest))
        runpy.run_path(str(Path(__file__).parents[1] / "anova" / "origin_engine.py"))
        native = json.loads((scratch / "origin.json").read_text())

    table = []
    for cli_row, native_row in zip(cli["anova"]["full_table"], (0, 1, 2, 4, 5)):
        ss, df, ms = native["full"][native_row]
        for actual, expected in ((df, cli_row["df"]), (ss, cli_row["ss"]), (ms, cli_row["ms"])):
            if expected is not None and not math.isclose(actual, expected, rel_tol=1e-8, abs_tol=1e-10):
                raise ValueError(f"Origin disagrees with CLI for {cli_row['source']}: {actual} vs {expected}")
        table.append(dict(source=cli_row["source"], df=df, ss=ss,
                          ms=ms if cli_row["ms"] is not None else None))
    interaction_p = native["interaction_p"]
    if not math.isclose(interaction_p, cli["anova"]["interaction_p"], rel_tol=1e-8, abs_tol=1e-10):
        raise ValueError(f"Unexpected native interaction p-value {interaction_p}")
    worksheet = op.new_sheet("w", lname="Origin two-factor ANOVA")
    for col, key in enumerate(("source", "df", "ss", "ms")):
        values = [row[key] for row in table if row[key] is not None]
        worksheet.from_list(col, values, lname=key.upper())
    worksheet.from_list(4, [interaction_p], lname="Interaction p")

    included = cli["anova"]["interaction_included"]
    selected = native["full"] if included else native["pooled"]
    repeats = len(records) // (len(operators) * len(parts))
    error_ms = selected[4][2]
    interaction_ms = selected[2][2] if included else error_ms
    operator_var = max(0, (selected[1][2] - interaction_ms) / (len(parts) * repeats))
    interaction_var = max(0, (interaction_ms - error_ms) / repeats)
    part_var = max(0, (selected[0][2] - interaction_ms) / (len(operators) * repeats))
    av = operator_var + interaction_var
    grr = error_ms + av
    total = grr + part_var
    tolerance = cli["study"]["effective_tolerance"]
    components = {}
    for name, var in (("GRR", grr), ("EV", error_ms), ("AV", av), ("PV", part_var)):
        metrics = {"contribution_pct": 100 * var / total,
                   "study_variation_pct": 100 * math.sqrt(var / total),
                   "tolerance_pct": 100 * multiplier * math.sqrt(var) / tolerance if tolerance else None}
        for key, value in metrics.items():
            if value is not None and not math.isclose(value, cli["study"]["components"][name][key],
                                                      rel_tol=1e-8, abs_tol=1e-10):
                raise ValueError(f"Unexpected {name} {key}: {value}")
        components[name] = metrics
    return {"full_model": table, "origin_interaction_p": interaction_p,
            "cli_interaction_p": cli["anova"]["interaction_p"], "components": components}


def operator_and_part_plots(folder, records, operators, parts):
    split = op.new_sheet("w", lname="Readings by operator")
    box = op.new_graph(lname="Distribution by operator", template="BOX")
    points = op.new_graph(lname="Readings by part", template="LINESYMB")
    for index, name in enumerate(operators):
        subset = [(p, v) for o, p, _, v in records if o == name]
        split.from_list(2 * index, [p for p, _ in subset], lname="Part", axis="X")
        col = 2 * index + 1
        split.from_list(col, [v for _, v in subset], lname=name, axis="Y")
        box_plot = box[0].add_plot(split, coly=col, type=206)
        box_plot.color = COLORS[index]
        series(points[0], split, col - 1, col, index, kind="scatter", size=5)
    averages = op.new_sheet("w", lname="Part means")
    averages.from_list(0, parts, lname="Part", axis="X")
    averages.from_list(1, [mean(v for _, p, _, v in records if p == part) for part in parts],
                       lname="Part mean", axis="Y")
    mean_plot = points[0].add_plot(averages, colx=0, coly=1, type="linesymbol")
    mean_plot.color = "#353D4C"
    mean_plot.symbol_kind = 5
    mean_plot.symbol_size = 6
    for graph, x_title, file in ((box, "Operator", "origin-operator-box.png"),
                                 (points, "Part", "origin-measurement-part.png")):
        layer = graph[0]
        layer.rescale()
        layer.axis("x").title = x_title
        layer.axis("y").title = "Measurement"
        if graph == box:
            layer.lt_exec("label -r legend;")
        else:
            layer.set_xlim(parts[0] - 1, parts[-1] + 1, 1)
            layer.lt_exec("legend -r;")
        save_png(graph, folder / file)


def control_plots(folder, records, operators, parts):
    cells = [[v for o, p, _, v in records if o == name and p == part]
             for name in operators for part in parts]
    repeats = len(cells[0])
    if repeats not in (2, 3) or any(len(values) != repeats for values in cells):
        raise ValueError("Expected a balanced 2- or 3-repeat crossed study.")
    a2, d4 = {2: (1.880, 3.267), 3: (1.023, 2.574)}[repeats]
    ranges = [max(values) - min(values) for values in cells]
    means = [mean(values) for values in cells]
    rbar, grand = mean(ranges), mean(means)
    limits = {}
    for key, values, cl, lcl, ucl, ytitle in (
        ("range", ranges, rbar, 0, d4 * rbar, "Subgroup range"),
        ("mean", means, grand, grand - a2 * rbar, grand + a2 * rbar, "Subgroup mean"),
    ):
        sheet = op.new_sheet("w", lname=f"{ytitle} control data")
        graph = op.new_graph(lname=f"{ytitle} control chart", template="LINESYMB")
        layer = graph[0]
        for i, name in enumerate(operators):
            first = i * len(parts)
            sheet.from_list(2 * i, list(range(first + 1, first + 1 + len(parts))), lname="Subgroup", axis="X")
            sheet.from_list(2 * i + 1, values[first:first + len(parts)], lname=name, axis="Y")
            series(layer, sheet, 2 * i, 2 * i + 1, i, size=5)
        base = 2 * len(operators)
        sheet.from_list(base, [1, len(values)], lname="Subgroup", axis="X")
        for i, (label, limit) in enumerate((("CL", cl), ("LCL", lcl), ("UCL", ucl)), start=1):
            sheet.from_list(base + i, [limit, limit], lname=label, axis="Y")
            line = layer.add_plot(sheet, colx=base, coly=base + i, type="line")
            line.color = "#27846C" if label == "CL" else "#9C5151"
        layer.rescale()
        layer.axis("x").title = "Subgroup (part within operator)"
        layer.axis("y").title = ytitle
        layer.lt_exec("legend -r;")
        save_png(graph, folder / f"origin-{key}-control.png")
        limits[key] = {"LCL": lcl, "CL": cl, "UCL": ucl}
    return limits


def component_chart(folder, components):
    sheet = op.new_sheet("w", lname="ANOVA variance components")
    names = ["GRR", "EV", "AV", "PV"]
    sheet.from_list(0, names, lname="Component", axis="X")
    graph = op.new_graph(lname="Gage variance components", template="COLUMN")
    layer = graph[0]
    for index, (key, label) in enumerate((("study_variation_pct", "Study variation"),
                                         ("contribution_pct", "Contribution"),
                                         ("tolerance_pct", "Tolerance"))):
        values = [components[name][key] for name in names]
        if all(v is not None for v in values):
            sheet.from_list(index + 1, values, lname=label, axis="Y")
            layer.add_plot(sheet, colx=0, coly=index + 1, type="column")
    layer.group()
    layer.rescale()
    layer.axis("x").title = "Variance component"
    layer.axis("y").title = "Percentage (%)"
    layer.lt_exec("legend -r;")
    legend = layer.label("Legend")
    legend.set_int("attach", 0)
    legend.set_float("x1", layer.xlim[0] + 0.30 * (layer.xlim[1] - layer.xlim[0]))
    legend.set_float("y1", layer.ylim[1] * 0.99)
    save_png(graph, folder / "origin-variance-components.png")


def origin_histogram(folder, measurements, records, aligned=False):
    title = "Distribution using program bins" if aligned else "Measurement distribution"
    source = measurements
    column = 3
    if aligned:
        values = [v for _, _, _, v in records]
        low, high = min(values), max(values)
        n = max(5, min(20, math.ceil(math.sqrt(len(values)))))
        width = (high - low) / n
        bounds = [low + i * width for i in range(n)] + [high]
        centers = [(a + b) / 2 for a, b in zip(bounds, bounds[1:])]
        measurements.activate()
        # Count in the imported worksheet. Cumulative counts below each interior
        # edge leave the rightmost observation in the last bin, as in the app.
        cumulative = [0] + [int(op.lt_float(f'countif(col(D), "<{x:.17g}")'))
                            for x in bounds[1:-1]] + [len(values)]
        aligned_counts = [b - a for a, b in zip(cumulative, cumulative[1:])]
        source = op.new_sheet("w", lname="Program bin boundaries and Origin counts")
        for i, (label, data) in enumerate((("Lower", bounds[:-1]), ("Upper", bounds[1:]),
                                           ("Center", centers), ("Count", aligned_counts))):
            source.from_list(i, data, lname=label)
        # Origin's native histogram uses [a,b) even for the last bin. Present
        # each frequency at its bin center so its plot preserves our closed top.
        column = 4
        source.from_list(column, [x for x, count in zip(centers, aligned_counts) for _ in range(count)],
                         lname="Bin centers weighted by frequency", axis="Y")
    histogram = op.new_graph(lname=title, template="Hist")
    distribution = histogram[0]
    bars = distribution.add_plot(source, coly=column, type=219)
    bars.color = "#427FB0"
    if aligned:
        distribution.lt_exec(f"set %C -hbs {width:.17g}; set %C -hbb {low:.17g}; set %C -hbe {high:.17g};")
    distribution.rescale()
    distribution.axis("x").title = "Measurement"
    distribution.axis("y").title = "Count"
    distribution.lt_exec("label -r legend;")
    save_png(histogram, folder / ("origin-histogram-aligned.png" if aligned else "origin-histogram.png"))
    histogram.activate()
    op.lt_exec("openBinWks nRecalculate:=1;")
    bins = op.find_sheet()
    if not bins or bins.name == measurements.name:
        raise RuntimeError("Origin did not return its histogram bin worksheet.")
    centers = bins.to_list(0)
    counts = list(map(int, bins.to_list(1)))
    step = centers[1] - centers[0]
    edges = [v - step / 2 for v in centers] + [centers[-1] + step / 2]
    if aligned:
        expected = [0] * n
        for value in values:
            expected[min(n - 1, int((value - low) / width))] += 1
        if counts != expected or any(not math.isclose(edge, low + i * width) for i, edge in enumerate(edges)):
            raise ValueError(f"Native histogram {counts} {edges}; expected {expected} ({low} to {high})")
    if sum(counts) != len(records):
        raise ValueError(f"Unexpected histogram bin counts {counts}")
    method = ("Origin Countif on raw values; native histogram of frequency-weighted bin centers; "
              "last bin includes maximum." if aligned else "Origin native histogram bin worksheet")
    return {"source": method, "centers": centers,
            "edges": edges, "counts": counts}


def make_reference(folder):
    csv_path = folder / "measurements.csv"
    with csv_path.open(encoding="utf-8-sig", newline="") as stream:
        exported = [(r["Operator"], int(r["Part"]), int(r["Trial"]), float(r["Measurement"]))
                    for r in csv.DictReader(stream)]
    study = json.loads((folder / "study.json").read_text(encoding="utf-8-sig"))
    cli = study["metrics"]
    measurements = op.new_sheet("w", lname="Imported measurements")
    measurements.from_file(str(csv_path), keep_DC=False)
    columns = [measurements.to_list(i) for i in range(4)]
    records = [(str(o), int(p), int(t), float(v)) for o, p, t, v in zip(*columns)]
    if records != exported:
        raise ValueError(f"Origin worksheet differs from CSV ({len(records)} rows).")
    operators = list(dict.fromkeys(r[0] for r in records))
    parts = sorted(set(r[1] for r in records))
    summary = op.new_sheet("w", lname="Part by operator means")
    summary.from_list(0, parts, lname="Part", axis="X")
    temp = op.new_sheet("w", lname="Mean calculation")
    chart_means = {}
    for col, name in enumerate(operators, start=1):
        part_means = []
        for part in parts:
            values = [v for o, p, _, v in records if o == name and p == part]
            temp.from_list(0, values)
            temp.activate()
            native = op.lt_float("mean(col(A))")
            if not math.isclose(native, mean(values), abs_tol=1e-12, rel_tol=0):
                raise ValueError(f"Incorrect native mean for {name}/{part}: {native}")
            part_means.append(native)
        summary.from_list(col, part_means, lname=name, axis="Y")
        chart_means[name] = summary.to_list(col)
    temp.get_book().destroy()

    interaction = op.new_graph(lname="Part by operator interaction", template="LINESYMB")
    layer = interaction[0]
    for i, _ in enumerate(operators):
        series(layer, summary, 0, i + 1, i)
    layer.rescale()
    layer.set_xlim(parts[0], parts[-1], 1)
    layer.axis("x").title = "Part"
    layer.axis("y").title = "Mean measurement"
    layer.lt_exec("legend -r;")
    save_png(interaction, folder / "origin-interaction.png")

    histogram = origin_histogram(folder, measurements, records)
    aligned = origin_histogram(folder, measurements, records, aligned=True)

    operator_and_part_plots(folder, records, operators, parts)
    limits = control_plots(folder, records, operators, parts)
    check = native_anova(records, operators, parts, cli, study["config"]["analysis"]["study_multiplier"])
    component_chart(folder, check["components"])
    check.update({"origin_version": op.lt_float("@V"), "records": len(records),
                  "operators": operators, "parts": parts, "csv_round_trip_exact": True,
                  "control_limits": limits, "interaction_means": chart_means,
                  "histogram": histogram, "matched_histogram": aligned})
    (folder / "origin-analysis.json").write_text(json.dumps(check, indent=2), encoding="utf-8")
    interaction.activate()
    if not op.save(str(folder / "Origin-reference.opju")):
        raise RuntimeError("Origin could not save the project.")


output = Path(PyOrigin.LT_get_str("msa_chart_folder"))
try:
    make_reference(output)
    PyOrigin.LT_set_var("msa_reference_ok", 1)
except Exception:
    (output / "origin-error.log").write_text(traceback.format_exc(), encoding="utf-8")
    raise
