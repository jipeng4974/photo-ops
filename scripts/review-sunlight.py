#!/usr/bin/env python3
"""Run the landscape fixtures and build a local, attributed before/after review page."""
import argparse
import html
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def comparison(before, after):
    return f'''<div class="viewer"><div class="comparison">
<img src="{after}" alt="After"><img class="before" src="{before}" alt="Before"></div>
<label>After ← <input type="range" min="0" max="100" value="50"
oninput="this.closest('.viewer').querySelector('.before').style.clipPath=
'inset(0 '+(100-this.value)+'% 0 0)'"> → Before</label></div>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/linux/x86_64/debug/photo-ops")
    parser.add_argument("--output", type=Path, default=ROOT / "build/sunlight-review")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "tests/data/landscape-manifest.json").read_text())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "index.html").exists():
        raise SystemExit("Review already exists; choose a fresh --output directory")
    sections = []
    summary = []
    for entry in manifest["files"]:
        source = ROOT / "tests/data/downloads" / entry["path"]
        name = source.stem
        subprocess.run([str(args.binary.resolve()), "sunlight-enhance", str(source),
                        "--half-size", "-o", str(output / (name + ".tif")),
                        "--diagnostics", str(output / name)], check=True)
        report = json.loads((output / name / "report.json").read_text())
        summary.append({"source": entry["source_page"], "fixture": entry["path"], **report})
        title = html.escape(entry["purpose"])
        attribution = html.escape(entry["source"])
        source_url = html.escape(entry["source_page"], quote=True)
        license_url = html.escape(entry["license_url"], quote=True)
        decisions = {
            "enhanced": "已增强",
            "insufficient_color_evidence": "保留原图：亮物体缺少足够的颜色证据",
            "no_coherent_sunlight": "保留原图：没有可信的受光候选",
            "weak_illumination_evidence": "保留原图：候选照明证据不足",
            "protected_pixels": "保留原图：选区像素受到高光／范围保护",
            "strength_zero": "保留原图：strength 为 0",
        }
        state = decisions.get(report.get("decision"),
                              "已增强" if report["applied"] else "保留原图")
        detail = ""
        if (output / name / "detail-before.jpg").exists():
            detail = ("<h3>选区局部 · 与全图相同曝光</h3>" +
                      comparison(name + "/detail-before.jpg", name + "/detail-after.jpg"))
        whole = comparison(name + "/before.jpg", name + "/after.jpg")
        sections.append(f'''<section><h2>{title}</h2>
<p>{state} · 区域 {100*report['selected_area_fraction']:.1f}% ·
置信度 {report['heuristic_confidence']:.2f} · 两图共享预览曝光
+{report['preview_exposure_ev']:.2f} EV</p>
<p>实际选区相对色度 {(report.get('mean_relative_chroma_gain',1)-1)*100:+.1f}% ·
亮度 {report.get('mean_luminance_change_ev',0):+.3f} EV。
数值为 mask 加权、通过像素保护之后的测量，不是理论上限。</p>
{whole}
{detail}
<p><a href="{name}/mask.jpg">Mask JPEG</a> ·
<a href="{name}/mask.tif">Soft mask TIFF</a> ·
<a href="{name}/report.json">JSON</a> · <a href="{name}.tif">16-bit TIFF</a></p>
<p class="credit">{attribution} · <a href="{source_url}">原帖</a> ·
<a href="{license_url}">许可</a>。派生图：半尺寸 RAW 开发、局部增强及共享预览曝光。</p>
</section>''')
    page = '''<!doctype html><html lang="zh"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sunlight enhancement review</title><style>
body{font:16px/1.6 system-ui;background:#181a1c;color:#e8e4df;max-width:1100px;
margin:40px auto;padding:0 20px}a{color:#e7be7c}section{margin:50px 0}
h1,h2{line-height:1.3}.comparison{position:relative;line-height:0;max-width:100%}
.comparison img{width:100%;max-height:80vh;object-fit:contain;background:#101112}
.comparison .before{position:absolute;inset:0;height:100%;clip-path:inset(0 50% 0 0)}
input{width:60%;vertical-align:middle}.credit{font-size:13px;color:#aaa}
</style><h1>Sunlight-enhance · 风景回归检查</h1>
<p>拖动滑块比较同曝光的 before / after；下方局部对照使用同一曝光。
没有自动去雾或全局调色。亮物体不等于受光主体：没有合适候选时应保留原图。</p>
''' + "\n".join(sections) + "</html>\n"
    (output / "index.html").write_text(page)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"review: {output / 'index.html'}")


if __name__ == "__main__":
    main()
