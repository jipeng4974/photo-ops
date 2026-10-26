#!/usr/bin/env python3
"""Prepare a deterministic two-focus/three-shutter DNG review and optionally measure native merges.

Development-only NumPy/Pillow/tifffile dependencies; never part of native image processing.
This mathematical scene is not an R8 capture or an occlusion/handheld quality acceptance.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHUTTERS = ((1, 400), (1, 100), (1, 25))
SEED = 180026


def publish(path, payload):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.review-', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
            os.link(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def publish_json(path, value):
    publish(path, (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False)+'\n').encode())


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dng_header(shutter, width, height):
    # Reuse only independent test metadata, not any production RAW/merge helper.
    spec = importlib.util.spec_from_file_location('dng_fixture', ROOT/'tests/dng_fixture.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)/'template.dng'
        fixture.write_dng_fixture(path, shutter, 32, 32)
        data = bytearray(path.read_bytes())
    offset = struct.unpack_from('<I', data, 4)[0]
    count = struct.unpack_from('<H', data, offset)[0]
    pixel_offset = None
    for index in range(count):
        at = offset+2+12*index
        tag, kind, length, value = struct.unpack_from('<HHII', data, at)
        if tag == 273:
            pixel_offset = value
        replacements = {256: width, 257: height, 278: height, 279: width*height*2}
        if tag in replacements:
            if kind != 4 or length != 1:
                raise ValueError('unexpected independent DNG template geometry')
            struct.pack_into('<I', data, at+8, replacements[tag])
    if pixel_offset is None:
        raise ValueError('independent DNG template lacks pixel offset')
    return bytes(data[:pixel_offset])


def prepare(root, width, height, np, Image):
    y, x = np.ogrid[:height, :width]
    # Three radiance bands, fine texture and two depth regions; no camera motion.
    level = np.where(x < width/3, .004, np.where(x < 2*width/3, .12, 1.5))
    texture = (1 + .25*np.sin(x*2*np.pi/11) + .25*np.cos(y*2*np.pi/15)
               + .12*np.sin((x+y)*2*np.pi/7))
    truth = (level*texture).astype(np.float32)
    with io.BytesIO() as stream:
        np.save(stream, truth, allow_pickle=False)
        publish(root/'camera-signal-truth.npy', stream.getvalue())
    # Analytic Gaussian attenuation of each sinusoid (sigma=4); no resize aliasing.
    blurred_texture = (1 + .25*np.exp(-8*(2*np.pi/11)**2)*np.sin(x*2*np.pi/11)
                       + .25*np.exp(-8*(2*np.pi/15)**2)*np.cos(y*2*np.pi/15)
                       + .12*np.exp(-16*(2*np.pi/7)**2)*np.sin((x+y)*2*np.pi/7))
    blurred = (level*blurred_texture).astype(np.float32)
    near_support = np.clip((height*.5-y)/8+.5, 0, 1).astype(np.float32)
    generator = np.random.default_rng(SEED)
    planes, records = [], []
    for focus, support in zip(('near', 'far'), (near_support, 1-near_support)):
        scene = truth*support + blurred*(1-support)
        files = []
        for index, shutter in enumerate(SHUTTERS):
            signal = scene*(shutter[0]/shutter[1]/.01)*15871
            # Independent, reproducible read + signal-dependent noise in sensor code units.
            sigma = np.sqrt(3**2 + np.maximum(signal, 0)*.3)
            samples = np.clip(np.rint(512+signal+generator.normal(size=scene.shape)*sigma),
                              0, 16383).astype('<u2')
            path = root/f'{focus}-e{index}.dng'
            publish(path, dng_header(shutter, width, height)+samples.tobytes())
            files.append(path.name)
            records.append(dict(path=path.name, sha256=digest(path), bytes=path.stat().st_size,
                                focus=focus, shutter_seconds=shutter[0]/shutter[1]))
        planes.append(dict(name=focus, exposures=files))
    publish_json(root/'capture.json', dict(version=1, focus_planes=planes))
    provenance = dict(kind='synthetic-planar-focus-exposure-review', generator_version=1,
                      width=width, height=height, seed=SEED, numpy_version=np.__version__,
                      pillow_version=Image.__version__, camera='Independent Python fixture DNG',
                      not_real_r8_capture=True, geometry='identity; --aligned explicitly asserted',
                      noise='3-code read RMS plus sqrt(0.3 * signal_codes) Gaussian RMS',
                      scene='grayscale camera signal; three radiance bands; complementary planar defocus',
                      limitations=['analytic Gaussian defocus inside planar bands; not an occlusion simulator', 'not real sensor noise calibration',
                                   'no motion or geometry stress; those have separate native regressions'],
                      script_sha256=digest(Path(__file__)), fixture_writer_sha256=digest(ROOT/'tests/dng_fixture.py'),
                      truth_sha256=digest(root/'camera-signal-truth.npy'), files=records)
    provenance['capture_manifest_sha256'] = digest(root/'capture.json')
    publish(root/'review-generator.py.txt', Path(__file__).read_bytes())
    publish(root/'dng-fixture-writer.py.txt', (ROOT/'tests/dng_fixture.py').read_bytes())
    publish_json(root/'provenance.json', provenance)
    return planes, truth


def save_png(path, array, np, Image):
    image = Image.fromarray(np.rint(np.clip(array, 0, 1)*255).astype(np.uint8))
    with io.BytesIO() as stream:
        image.save(stream, format='PNG')
        publish(path, stream.getvalue())


def preview(rgb, np):
    # Explicit Rec.2020/D65 -> linear sRGB, fixed global display curve, sRGB OETF.
    rec2020_to_srgb = np.array([[1.660491, -.587641, -.072850],
                               [-.124550, 1.132900, -.008349],
                               [-.018151, -.100579, 1.118730]])
    linear = np.maximum(rgb@rec2020_to_srgb.T, 0)
    mapped = linear/(linear+.18)
    return np.where(mapped <= .0031308, 12.92*mapped, 1.055*mapped**(1/2.4)-.055)


def render(root, truth, matrix_only, np, Image, ImageDraw, tifffile):
    report = json.loads((root/'matrix/diagnostics/report.json').read_text())
    hdr = json.loads((root/'matrix/diagnostics/hdr-plane-0/report.json').read_text())
    xyz_to_rec2020 = np.array([[1.716651, -.355671, -.253366],
                              [-.666684, 1.616481, .015769],
                              [.017640, -.042771, .942103]])
    camera_to_xyz = np.array(hdr['raw_context']['camera_to_xyz']).reshape(3, 3)
    gains = np.array(hdr['raw_context']['white_balance'])[:3]
    neutral_response = xyz_to_rec2020@camera_to_xyz@gains
    h, w = truth.shape
    # Metrics exclude the synthetic band/depth transitions and demosaic image borders.
    yy, xx = np.ogrid[:h, :w]
    interior = ((xx >= 16) & (xx < w-16) & (yy >= 16) & (yy < h-16)
                & (abs(xx-w/3) > 16) & (abs(xx-2*w/3) > 16) & (abs(yy-h/2) > 16))
    target = truth[:, :, None]*neutral_response
    expected_preview = preview(target, np)
    save_png(root/'truth.png', expected_preview, np, Image)
    panels = [('Truth (mathematical camera signal)', 'truth.png')]
    measurements = {}
    runs = ['matrix'] if matrix_only else ['near-hdr', 'far-hdr', 'matrix']
    for name in runs:
        rgb = tifffile.imread(root/name/'stack.tif')
        errors = np.mean(abs(rgb-target), axis=2)
        regions = {'all': interior, 'near': interior & (yy < h/2), 'far': interior & (yy >= h/2),
                   'shadow': interior & (xx < w/3), 'highlight': interior & (xx >= 2*w/3)}
        metrics = {}
        for region, mask in regions.items():
            denominator = np.mean(abs(target[mask]))
            metrics[region+'_relative_mae'] = float(np.mean(errors[mask])/max(denominator, 1e-8))
        metrics.update(minimum=float(rgb.min()), maximum=float(rgb.max()),
                       tiff_sha256=digest(root/name/'stack.tif'))
        measurements[name] = metrics
        save_png(root/f'{name}.png', preview(rgb, np), np, Image)
        panels.append((name+' (same fixed display curve)', name+'.png'))
    scalar_names = ['hdr-confidence', 'hdr-clipping', 'hdr-low-snr', 'hdr-deghost', 'combined-confidence']
    for name in scalar_names:
        values = tifffile.imread(root/'matrix/diagnostics'/f'{name}.tif')
        save_png(root/f'{name}.png', values, np, Image)
    # A local contact sheet plus full-size PNGs; no display transform feeds the operator.
    thumbnails = []
    for caption, filename in panels:
        with Image.open(root/filename) as source:
            image = source.convert('RGB')
            image.thumbnail((640, 440))
        thumbnails.append((caption, image))
    canvas = Image.new('RGB', (1280, ((len(thumbnails)+1)//2)*480), '#20242a')
    draw = ImageDraw.Draw(canvas)
    for index, (caption, image) in enumerate(thumbnails):
        left, top = index % 2*640, index//2*480
        draw.text((left+8, top+8), caption, fill='white')
        canvas.paste(image, (left, top+32))
    with io.BytesIO() as stream:
        canvas.save(stream, format='PNG')
        publish(root/'contact-sheet.png', stream.getvalue())
    publish_json(root/'metrics.json', dict(kind='synthetic-planar-review-metrics',
                 note='Relative working-RGB MAE against known camera signal transformed by declared calibration; excludes transition margins. No fitted exposure or color correction.',
                 neutral_working_response=neutral_response.tolist(), measurements=measurements,
                 context_fingerprint=report['raw_context']['fingerprint']))
    publish(root/'review.md', ('# 合成二维合成检查\n\n'
            '这不是 R8 实拍。已明确断言 identity geometry；不覆盖遮挡、手持或真实 sensor noise。\n\n'
            '![固定显示映射对照](contact-sheet.png)\n\n'
            '所有 PNG 使用相同 Rec.2020 → sRGB、x/(x+0.18) 全局显示曲线和 sRGB OETF；'
            'float TIFF 保留原始 scene-linear 数值。此曲线不回流合成。\n\n'
            '查看 [metrics](metrics.json)、[provenance](provenance.json)、'
            '[matrix 资源记录](matrix/summary.json)、[native report](matrix/diagnostics/report.json)。'
            'HDR confidence／clipping／low-SNR／deghost 与 combined confidence PNG 为 [0,1] 线性灰度预览，'
            '原始数字地图仍为 TIFF。\n').encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--width', type=int, default=768)
    parser.add_argument('--height', type=int, default=512)
    parser.add_argument('--binary', type=Path, help='Run native merges; omit to only prepare fixtures')
    parser.add_argument('--matrix-only', action='store_true', help='Skip separate per-plane HDR measurements')
    parser.add_argument('--memory-budget-mib', type=int, default=2048)
    parser.add_argument('--tile-size', type=int, default=256)
    args = parser.parse_args()
    if args.width < 128 or args.height < 96 or args.width % 2 or args.height % 2:
        parser.error('dimensions must be even and at least 128 x 96')
    if args.width*args.height > 100_000_000:
        parser.error('review generator is limited to 100 MP')
    binary = args.binary.resolve(strict=True) if args.binary else None
    import numpy as np
    from PIL import Image, ImageDraw
    import tifffile
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    planes, truth = prepare(root, args.width, args.height, np, Image)
    if not binary:
        print(root/'capture.json')
        return
    runs = [] if args.matrix_only else [(focus['name']+'-hdr', ['--operator', 'hdr',
            *[str(root/name) for name in focus['exposures']]]) for focus in planes]
    runs.append(('matrix', ['--matrix', str(root/'capture.json')]))
    for name, descriptor in runs:
        subprocess.run([sys.executable, str(ROOT/'scripts/benchmark-focus.py'), *descriptor,
                        '--binary', str(binary), '--output', str(root/name), '--aligned',
                        '--memory-budget-mib', str(args.memory_budget_mib), '--tile-size', str(args.tile_size)], check=True)
    render(root, truth, args.matrix_only, np, Image, ImageDraw, tifffile)
    print(root/'review.md')


if __name__ == '__main__':
    main()
