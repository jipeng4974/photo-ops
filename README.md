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

基础层已实现：C++26 named modules、xmake、move-only float32 image、strided
views、color transforms、Gaussian pyramid、diagnostics 与离线 deterministic tests。
CR3 / TIFF / JPEG 输入，16-bit TIFF 默认输出，可选 float32 EXR 与 JPEG。
当前 CLI 提供 `inspect` / `convert` / `sunlight-enhance`；focus-stack 算法尚未实现。
C++ 源码使用 LLVM clang-format + 96 列与声明间空行；可恢复错误用 expected，
调用违约用原生 C++26 contracts。

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

没有全局 xmake 时先运行 `scripts/bootstrap-xmake.sh`，然后把
`$PWD/.tools/install/bin` 加入 PATH。当前 canonical compiler 为 GCC 16.2.x。

- [C++ 风格、format 任务与 expected/contracts](docs/cpp-style.md)
- [sunlight-enhance 的算法、保护和已知限制](docs/sunlight-enhance.md)
- [Module graph 与格式契约](docs/architecture.md)
- [已下载测试集与风景样片候选](docs/fixtures.md)
- [工具链安装、版本与实际验证环境](docs/toolchain.md)

内部固定为 float32 linear-sRGB / D65。TIFF/JPEG 经过 ICC 或显式 sRGB 假设转换，
无法撤销先前 tone mapping；RAW 暂经 LibRaw 16-bit linear 中间结果。TIFF/JPEG
输出不自动 tone-map，负值与高光越界会裁剪；EXR 保留 float 范围。详见格式契约。

## License

Intended license direction: **GPL-3.0-or-later**.
