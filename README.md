# photo-ops

Opinionated computational-photography operators for landscape photography.

`photo-ops` is a personal, programmatic toolkit for automating recurring photographic edits without turning them into opaque one-click filters.

Initial directions:

- **`focus-stack`** — automatic depth-of-field stacking from multiple Canon RAW captures, with conservative registration, focus-map regularization, multi-scale fusion, and confidence diagnostics.
- **`sunlight-enhance`** — illumination-aware enhancement of coherent sunlit regions when luminance contrast is strong but chromatic separation is weak.

The project philosophy is:

> Automatic by default, inspectable and surgically correctable on failure.

and:

> Prefer interpretable image-processing decisions over opaque aesthetic transformation.

## Stack

The intended implementation stack is deliberately modern and native:

```text
C++26
recent pinned GCC
xmake
named C++ modules
Linux x86-64
```

Project code is module-native but not module-purist: mature C/C++ imaging libraries are welcome behind narrow adapter boundaries.

See:

- [`docs/design.md`](docs/design.md) — operator goals, algorithms, failure modes, diagnostics, and MVP scope.
- [`docs/toolchain.md`](docs/toolchain.md) — C++26/module/xmake engineering choices and dependency policy.
- [`AGENTS.md`](AGENTS.md) — instructions and architectural constraints for coding agents.

## Status

The foundation is implemented: C++26 named modules, xmake, move-only float32 images,
strided views, color transforms, Gaussian pyramids, diagnostics, and offline
deterministic tests. Inputs include CR3, TIFF, and JPEG; output defaults to 16-bit
TIFF, with optional float32 EXR and JPEG. The CLI currently provides `inspect`,
`convert`, `sunlight-enhance`, and `focus-stack`. Focus stacking currently accepts
consistently developed TIFF/JPEG sequences and writes 16-bit TIFF; RAW brackets
remain gated on the planned float decoder and shared sequence context.
C++ sources use LLVM clang-format with a 96-column target and blank lines between
logical sections. Formatting uses stock clang-format without a custom lexical pass.
Recoverable errors use `expected`; caller violations use native
C++26 contracts.

```bash
sudo pacman -S --needed gcc pkgconf clang libraw libtiff libjpeg-turbo lcms2 openexr opencv
scripts/configure.sh -m debug --exr=y -y
xmake -j4
xmake test
xmake format-check
python -m unittest discover -s tests -p 'test_*.py'
xmake run photo-ops focus-stack near.tif middle.tif far.tif -o stacked.tif \
  --memory-budget-mib 2048 --diagnostics build/focus-diagnostics
xmake run photo-ops inspect INPUT.CR3 --half-size
xmake run photo-ops convert INPUT.CR3 -o decoded.tif
python scripts/fetch-fixtures.py
python scripts/smoke-fixtures.py
python scripts/fetch-fixtures.py --manifest tests/data/landscape-manifest.json
xmake run photo-ops sunlight-enhance INPUT.CR3 -o enhanced.tif \
  --half-size --strength 1 --diagnostics build/sunlight-diagnostics
python scripts/review-sunlight.py
```

If xmake is not installed globally, run `scripts/bootstrap-xmake.sh` first, then add
`$PWD/.tools/install/bin` to PATH. The canonical compiler is GCC 16.2.x.
Omarchy/Arch system packages are the canonical dependency path. Homebrew is
optional: install from `Brewfile` and explicitly select
`PHOTO_OPS_DEPS=brew` when configuring.

Documentation is primarily in Chinese; `README.md` and `AGENTS.md` are fully in English.

- [C++ style, formatting tasks, and expected/contracts](docs/cpp-style.md)
- [sunlight-enhance algorithm, safeguards, and known limitations](docs/sunlight-enhance.md)
- [RAW working color pipeline and sequence context design](docs/raw-pipeline.md)
- [Memory-budgeted focus-stack design](docs/focus-stack.md)
- [Module graph and image format contracts](docs/architecture.md)
- [Downloaded test datasets and landscape fixture candidates](docs/fixtures.md)
- [Toolchain installation, versions, and validated environment](docs/toolchain.md)

Version 0.2 defines a wide-gamut RAW route through camera-linear float to linear
Rec.2020 / D65, with shared sequence white balance, black calibration, and
normalization. This route and the sequence context are designed but not yet
implemented. Registration and fusion now live together in `photo_ops.focus_stack`;
its rendered-input path uses disk spooling, bounded proxies, coherent selection,
motion fallback, and tiled multiband fusion. RAW integration follows the original
prerequisites. The
current compatibility representation remains float32 linear-sRGB / D65, without
a stable working-color ABI promise. TIFF/JPEG inputs are
converted using ICC profiles or an explicit sRGB assumption; prior tone mapping
cannot be undone. RAW decoding currently passes through LibRaw's 16-bit linear
intermediate. TIFF/JPEG output does not apply automatic tone mapping and clips
negative and over-range highlight values; EXR preserves the floating-point range.
Image and JSON outputs are published atomically from same-directory temporary
files and never overwrite existing paths. This is per-file atomic visibility,
not a multi-file transaction or a power-loss durability guarantee. See the image
format contracts for details.

## License

Intended license direction: **GPL-3.0-or-later**.
