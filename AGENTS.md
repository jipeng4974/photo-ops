# AGENTS.md

This repository contains opinionated computational-photography operators for landscape photography.

The project is intentionally narrow: encode recurring photographic judgments as deterministic, inspectable, high-quality image-processing operators rather than build a general photo editor.

## Documentation language

Write project documentation primarily in Chinese by default. Keep `README.md` and
`AGENTS.md` fully in English. Technical terms, identifiers, commands, and code examples
may remain in their original language within Chinese documentation.

## Versioning and Git workflow

For every completed update, including documentation changes, bug fixes, and minor
or major features:

- Bump the project's `major.minor.patch` version in `xmake.lua`. Use a patch bump
  for fixes and documentation or maintenance changes, a minor bump for compatible
  new functionality, and a major bump for incompatible changes.
- Commit the update with the new version number in the commit subject and a
  moderately detailed description of what changed and why in the commit body.
  Include relevant validation results and any material limitations.
- Push the commit to `origin` on the current branch.

## Read first

Before making architectural or algorithmic changes, read:

- `docs/design.md` — product goals, algorithms, failure modes, and MVP scope.
- `docs/toolchain.md` — canonical C++ toolchain and coding philosophy.
- `docs/raw-pipeline.md` — wide-gamut RAW and immutable sequence-context design.
- `docs/focus-stack.md` — registration proxies and memory-budgeted full-resolution fusion.

Use Omarchy/Arch system packages as the canonical dependency path; Homebrew is
explicitly optional. Use stock clang-format; do not restore a custom C++ lexical
formatting pass. Honor the RAW/context and memory-budget contracts when extending
`photo_ops.focus_stack`. The current single-frame linear-sRGB decoder is a
compatibility path, not a stable working-color ABI or a sequence-safe decoder.
Publish output files atomically without overwriting existing paths.

If implementation and documentation disagree, do not silently reinterpret the design. Prefer a small explicit documentation update together with the code change.

## Product principles

1. **Automatic by default; inspectable and surgically correctable on failure.**
2. Prefer interpretable computational-photography decisions over opaque aesthetic transformation.
3. Preserve scene structure and photographic plausibility before chasing aggressive visual impact.
4. Emit useful diagnostics and confidence information for ambiguous operations.
5. Start with the simplest algorithm that can solve the observed problem. Add complexity only after real images justify it.
6. Do not turn this project into a Lightroom/Photoshop replacement.

The initial operators are:

- `focus-stack` — depth-of-field stacking with progressively stronger registration, spatially regularized focus selection, and multi-scale fusion.
- `sunlight-enhance` — illumination-aware enhancement of coherent sunlit regions when luminance contrast is strong but chromatic separation is weak.

## Canonical programming stack

This is a **C++26-first, module-native** codebase.

- Compiler: recent GCC, using the canonical pinned version documented in `docs/toolchain.md`.
- Build system: xmake.
- Language mode: C++26.
- Platform priority: Linux x86-64.
- Project-owned public/internal APIs should normally use named C++ modules.

Do not downgrade new project code to a header-centric C++20 style merely because it is more familiar.

At the same time, this project is **module-native, not module-purist**. Third-party C and C++ libraries may remain header-based and should be isolated behind narrow adapters or implementation boundaries.

## Modern C++ style

Prefer:

- named modules;
- value semantics;
- RAII;
- explicit ownership;
- strong types;
- `std::span` / `std::mdspan` and other vocabulary types when they improve interfaces;
- concepts when they clarify genuine constraints;
- ranges when they improve readability;
- compile-time structure over unnecessary runtime abstraction;
- small cohesive types and free functions over inheritance-heavy object hierarchies;
- `noexcept` and const-correctness where semantically appropriate.

Avoid:

- owning raw pointers;
- `shared_ptr` without actual shared ownership;
- inheritance/interfaces without real runtime polymorphism;
- factory/manager/service abstractions added only for architectural ceremony;
- macros where the language can express the same intent cleanly;
- header-only architecture merely for familiarity;
- legacy metaprogramming when modern language facilities are clearer;
- language-feature tourism: do not use a C++26 feature merely to prove the project is C++26.

Use new language/library features when they make the code more direct, safer, or more expressive.

## Discarded return values

Do not mark getters, mathematical functions, accessors, transformations, or
ordinary value-returning functions `[[nodiscard]]`. Prefer type-level
`[[nodiscard]]` when the obligation belongs to the result type itself rather
than to one particular producer function. Do not repeat it on functions
returning such a type.

Keep `result<T>` as an alias of `std::expected<T, error>`: the pinned GCC/libstdc++
already marks `std::expected` (including its void specialization) `[[nodiscard]]`.
Do not add an expected wrapper or inheritance layer just to attach an attribute.
Verify this diagnostic when upgrading the toolchain; it is a validated library
property, not an assumption about every standard-library implementation.

## Modules

Prefer a logical module graph along these lines:

```text
photo_ops.raw
photo_ops.image
photo_ops.color
photo_ops.pyramid
photo_ops.focus_stack
photo_ops.sunlight
photo_ops.diagnostics
```

Keep registration and fusion together in `photo_ops.focus_stack`; registration
currently serves focus stacking alone. Split out a reusable registration module
only when an actual second consumer justifies it. The initial implementation
accepts consistently developed rendered TIFF/JPEG sequences; RAW brackets remain
gated on the float/context pipeline, without a fallback to per-frame development.

Exact partitioning should follow implementation needs; do not create tiny modules for every file.

A clean dependency direction is more important than maximizing module count.

Third-party integration should look conceptually like:

```text
third-party API
    ↓
thin adapter / implementation boundary
    ↓
project-native types and named modules
```

Do not let types such as `cv::Mat` become the core project representation merely because OpenCV is convenient at one boundary.

Treat `import std;` separately from project named modules. Use it only when the pinned GCC/libstdc++/xmake combination is known to support it reliably. Do not make the build fragile for aesthetic purity.

## Image-processing architecture

The core pipeline should use high-bit-depth / floating-point representations internally where appropriate.

Prefer project-owned image views and buffers with explicit:

- dimensions;
- channel/pixel semantics;
- color-space assumptions;
- ownership/lifetime;
- stride/layout.

Do not hide expensive copies. Image-processing APIs should make ownership and mutability obvious.

Color-space conversions and tone-related assumptions must be explicit. In particular, do not accidentally run mathematically linear operations on gamma-encoded data unless the algorithm intentionally requires that representation.

## RAW and color handling

Canon `.CR3` is a primary input format, likely through LibRaw or another well-supported RAW decoder.

For focus stacking, prefer scene-linear or otherwise well-defined high-bit-depth intermediates before aggressive tone mapping, sharpening, or denoising.

Keep RAW decoding, demosaicing/color conversion, and output encoding separate from photographic operators so they can be tested independently.

## Dependency policy

Dependencies are allowed when they provide difficult, mature functionality, but keep the core small.

Likely candidates include:

- LibRaw for RAW decoding;
- LittleCMS for color management;
- libtiff and/or OpenEXR for high-bit-depth output;
- OpenCV selectively for mature feature detection, robust geometry, or optical flow;
- Eigen only if its use materially simplifies numerical code.

Do not adopt OpenCV as the project's universal image abstraction.

Simple primitives such as pyramids, convolution, gradients, masks, OKLab transforms, and multi-band blending are good candidates for project-native implementations when doing so keeps behavior explicit and testable.

Do not reimplement complex mature algorithms such as SIFT or dense optical flow merely for purity unless there is a concrete reason.

## Algorithm policy

### `focus-stack`

Use progressively stronger registration:

1. translation / similarity transform;
2. homography when residual error justifies it;
3. constrained local mesh warp / optical flow only when necessary.

Prefer small residual misalignment over geometrically implausible warping.

Do not use per-pixel sharpness `argmax` as the final focus map. Focus selection should be spatially coherent and fusion should avoid seams and halos.

Moving foliage/water/people should reduce confidence or trigger a coherent-source fallback rather than produce double edges.

### `sunlight-enhance`

Do not implement this as "find bright pixels and increase saturation".

The target is a large/coherent/plausibly sunlit region where illumination structure is strong but colorfulness does not reinforce it sufficiently.

Prefer perceptual chroma manipulation (for example in OKLab) with strong naturalness constraints:

- enhance existing color evidence;
- do not invent strong chroma in neutral surfaces;
- use warmth subtly;
- protect highlights;
- preserve atmospheric depth rather than globally removing haze;
- avoid HDR-like halos or edge exaggeration.

## Learned methods

Do not introduce a neural network merely because one could solve the task.

The initial bias is toward classical, interpretable computer vision and image processing because:

- the operators encode specific photographic judgments;
- diagnostics matter;
- reproducibility matters;
- the project should remain lightweight;
- classical methods provide a strong baseline and expose actual failure modes.

Learned components are welcome later if experiments show a clear advantage that justifies their complexity.

## Diagnostics

Operators should expose enough information to understand failures.

Examples include:

- registration residuals and transform parameters;
- focus/source maps;
- soft masks;
- confidence maps;
- detected motion regions;
- before/after previews;
- structured JSON reports.

Diagnostics should be optional in normal runs but easy to enable in development.

## Testing

Prefer small deterministic tests for mathematical/image primitives and regression fixtures for photographic behavior.

When fixing a real-image failure:

1. preserve a minimal reproducible fixture if licensing/storage permits;
2. characterize the failure numerically or visually;
3. add a regression test or diagnostic assertion;
4. then change the algorithm.

Do not tune thresholds to one photograph without checking nearby cases.

## Performance

Correctness and photographic quality come first, but this is a native image-processing project and avoidable copies or pathological memory behavior should not accumulate.

Before micro-optimizing:

- establish a representative workload;
- measure;
- identify the actual bottleneck.

SIMD, parallelism, cache-aware tiling, and GPU acceleration can be introduced where measurements justify them. Keep scalar/reference paths when they materially help testing and correctness.

## Scope discipline

When implementing an MVP, resist solving hypothetical future problems.

For example:

- start focus stacking with similarity registration before adding dense warps;
- start sunlight enhancement with classical region analysis before semantic segmentation;
- do not build a GUI until actual failure modes demonstrate that CLI + diagnostics + small overrides are insufficient.

A narrow, understandable operator that works reliably on the author's photographs is more valuable than a broad framework with weak behavior.
