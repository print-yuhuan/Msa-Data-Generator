"""用 SciPy 独立核对置信限分位数及误分类积分；不替代五软件 GUI 实测。"""
import argparse
import json
import math
import pathlib
import subprocess

from scipy.integrate import quad
from scipy.stats import chi2, norm


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-directory", required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.output_directory).resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []

    def analyze(name, options):
        report = output / (name + ".json")
        command = [args.cli, "--no-config", "--method", "crossed_anova", "--input", args.input,
                   "--report", str(report), "--overwrite"] + options
        completed = subprocess.run(command, capture_output=True, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(completed.stderr)
        return json.loads(report.read_text(encoding="utf-8-sig"))["metrics"]

    def check(name, actual, expected, absolute=2e-11, relative=2e-8):
        if not math.isclose(actual, expected, abs_tol=absolute, rel_tol=relative):
            raise AssertionError(f"{name}: {actual!r} != {expected!r}")
        checks.append(dict(name=name, actual=actual, expected=expected,
                           absolute_difference=abs(actual - expected)))

    for model in ("include", "exclude"):
        # 现有MLS平方根近似仅在系数非负的有效域内使用；低水平由C++拒绝回归覆盖。
        for level in (70, 90, 95, 99):
            for bound in ("both", "lower", "upper"):
                name = f"ci-{model}-{level}-{bound}"
                result = analyze(name, ["--interaction", model, "--lsl", "9", "--usl", "11",
                    "--confidence", "true", "--confidence-level", str(level),
                    "--confidence-bound", bound])
                residual = next(row for row in result["anova"]["table"]
                                if row["source"] == "Repeatability")
                tail = (1 - level / 100) / (2 if bound == "both" else 1)
                interval = result["confidence"]["variance_components"]["EV"]
                if bound != "upper":
                    check(name + "-lower", interval["lower"],
                          residual["ss"] / chi2.ppf(1 - tail, residual["df"]))
                if bound != "lower":
                    check(name + "-upper", interval["upper"],
                          residual["ss"] / chi2.ppf(tail, residual["df"]))

    # 按独立均方的抽样方差计算有效自由度，使用SciPy非整数自由度分位数复核。
    for model in ("include", "exclude"):
        for level in (50, 90, 95, 99):
            for bound in ("both", "lower", "upper"):
                name = f"satterthwaite-{model}-{level}-{bound}"
                result = analyze(name, ["--interaction", model, "--confidence", "true",
                    "--confidence-level", str(level), "--confidence-bound", bound,
                    "--confidence-method", "satterthwaite"])
                means = [row["ms"] for row in result["anova"]["full_table"][:4]]
                degrees = [row["df"] for row in result["anova"]["full_table"][:4]]
                if model == "exclude":
                    means[2] = (means[2]*degrees[2]+means[3]*degrees[3])/(degrees[2]+degrees[3])
                    degrees[2] += degrees[3]
                # 共同CSV设计：10部件、3人员、3次重复。
                weights = {"Operator": (0, 1/30, -1/30, 0),
                           "PV": (1/9, 0, -1/9, 0)}
                if model == "include":
                    weights.update(EV=(0,0,0,1), OperatorPart=(0,0,1/3,-1/3),
                        AV=(0,1/30,9/30,-1/3), GRR=(0,1/30,9/30,2/3),
                        TV=(1/9,1/30,17/90,2/3))
                else:
                    weights.update(EV=(0,0,1,0), AV=weights["Operator"],
                        GRR=(0,1/30,29/30,0), TV=(1/9,1/30,77/90,0))
                tail = (1-level/100)/(2 if bound=="both" else 1)
                for key, coefficients in weights.items():
                    terms = [c*m for c,m in zip(coefficients, means)]
                    center = sum(terms)
                    effective = center**2/sum(t*t/d for t,d in zip(terms,degrees))
                    limits = result["confidence"]["variance_components"][key]
                    if bound != "upper":
                        check(name+"-"+key+"-lower", limits["lower"],
                              center*effective/chi2.ppf(1-tail,effective))
                    if bound != "lower":
                        check(name+"-"+key+"-upper", limits["upper"],
                              center*effective/chi2.ppf(tail,effective))

    cases = [(9, 11, None), (9.5, 10.5, None), (None, 10, None), (10, None, None),
             (None, 15, None), (9, 11, .5), (9.5, None, .5), (None, 10.5, .5)]
    for number, (lower, upper, historical) in enumerate(cases):
        options = ["--lsl", str(lower) if lower is not None else "null",
                   "--usl", str(upper) if upper is not None else "null",
                   "--misclassification", "true"]
        if historical is not None:
            options += ["--historical-sd", str(historical), "--process-variation", "study"]
        result = analyze(f"classification-{number}", options)["misclassification"]
        mean, part, gage = result["mean"], result["part_sd"], result["gage_sd"]
        lo = -math.inf if lower is None else (lower - mean) / part
        hi = math.inf if upper is None else (upper - mean) / part

        def interval(a, b):
            if a >= 0:
                return norm.sf(a) - norm.sf(b)
            if b <= 0:
                return norm.cdf(b) - norm.cdf(a)
            return 1 - norm.sf(b) - norm.cdf(a)

        def integrate_region(a, b, reject=False):
            mass = interval(a, b)
            if not mass:
                return 0.
            # 归一化积分同时覆盖概率极小的条件事件，避免绝对容差掩盖条件误差。
            def f(z):
                value = mean + part * z
                l = -math.inf if lower is None else (lower - value) / gage
                u = math.inf if upper is None else (upper - value) / gage
                probability = (norm.cdf(l) + norm.sf(u)) if reject else interval(l, u)
                return math.exp(norm.logpdf(z) - math.log(mass)) * probability
            value = quad(f, max(-40., a), min(40., b), epsabs=1e-12, epsrel=1e-10,
                         limit=500)[0]
            return mass * value

        bad = norm.cdf(lo) + norm.sf(hi)
        good = interval(lo, hi)
        accepted = integrate_region(-math.inf, lo) + integrate_region(hi, math.inf)
        rejected = integrate_region(lo, hi, True)
        check(f"classification-{number}-bad-accepted", result["bad_accepted"], accepted)
        check(f"classification-{number}-good-rejected", result["good_rejected"], rejected)
        if bad:
            check(f"classification-{number}-accept-given-bad", result["accept_given_bad"],
                  accepted / bad)
        if good:
            check(f"classification-{number}-reject-given-good", result["reject_given_good"],
                  rejected / good)
    (output / "reference-comparison.json").write_text(
        json.dumps(dict(passed=True, checks=checks), ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(checks)} SciPy 分位数和误分类参考检查通过")


if __name__ == "__main__":
    main()
