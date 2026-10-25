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
`convert`, and `sunlight-enhance`; focus stacking is not yet implemented.
C++ sources use LLVM clang-format with a 96-column limit and blank lines between
declarations. Recoverable errors use `expected`; caller violations use native
C++26 contracts.

```bash
brew bundle --file=Brewfile
scripts/configure.sh -m debug --exr=y -y
xmake -j4
xmake test
xmake format-check
python -m unittest discover -s tests -p 'test_*.py'
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

Documentation is primarily in Chinese; `README.md` and `AGENTS.md` are fully in English.

- [C++ style, formatting tasks, and expected/contracts](docs/cpp-style.md)
- [sunlight-enhance algorithm, safeguards, and known limitations](docs/sunlight-enhance.md)
- [Module graph and image format contracts](docs/architecture.md)
- [Downloaded test datasets and landscape fixture candidates](docs/fixtures.md)
- [Toolchain installation, versions, and validated environment](docs/toolchain.md)

The internal representation is float32 linear-sRGB / D65. TIFF/JPEG inputs are
converted using ICC profiles or an explicit sRGB assumption; prior tone mapping
cannot be undone. RAW decoding currently passes through LibRaw's 16-bit linear
intermediate. TIFF/JPEG output does not apply automatic tone mapping and clips
negative and over-range highlight values; EXR preserves the floating-point range.
See the image format contracts for details.

## License

Intended license direction: **GPL-3.0-or-later**.
