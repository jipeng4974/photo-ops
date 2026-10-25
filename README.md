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

## Build

Requires Linux x86-64, GCC 16.2.x, xmake 3.1.2+, and OpenCV 5.

```bash
sudo pacman -S --needed gcc pkgconf clang libraw libtiff libjpeg-turbo lcms2 opencv
scripts/configure.sh -m release -y
xmake -j4
```

If xmake is missing, run `scripts/bootstrap-xmake.sh` and add
`$PWD/.tools/install/bin` to `PATH` first.

## Usage

```bash
xmake run photo-ops inspect input.CR3
xmake run photo-ops convert input.CR3 -o decoded.tif
xmake run photo-ops sunlight-enhance input.CR3 -o enhanced.tif --diagnostics build/sunlight
xmake run photo-ops focus-stack near.CR3 middle.CR3 far.CR3 -o stacked-linear.tif
```

`focus-stack` accepts compatible Bayer CR3/DNG brackets with one frozen RAW context
and writes signed float32 linear Rec.2020/D65 ICC TIFF. Consistently developed
TIFF/JPEG sequences retain 16-bit sRGB TIFF output. RAW and rendered inputs cannot
be mixed. See [RAW support and limits](docs/raw-pipeline.md).

Focus-stack reports include stage timings and bounded tile-cache statistics; see
[performance measurements](docs/performance.md).

Existing outputs are never overwritten. Run `xmake run photo-ops --help` for all options.

## API

Version **1.4.0** keeps two photographic entry points: `enhance_sunlight(...)` and
`stack_focus(...)`.

**Breaking change:** `align_focus_stack(...)` is no longer exported. Call
`stack_focus(...)` and inspect the returned report's `frames[i].alignment` instead.

## License

Intended license direction: **GPL-3.0-or-later**.
