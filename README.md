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

Design-stage / early implementation.

The first implementation target is a minimal, testable `focus-stack` pipeline before progressively adding more difficult registration and scene-motion handling.

## License

Intended license direction: **GPL-3.0-or-later**.
