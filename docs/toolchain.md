# Toolchain and C++ Engineering Philosophy

## 当前可复现基线（2026-10-25）

机器可读记录在 [`toolchain.json`](../toolchain.json)。当前构建限定 **GCC 16.2.x**，
实际验证版本为 Arch GCC 16.2.1 20260810、xmake 3.1.2、C++26、Linux x86-64。
这取代下文早期的示例版本。xmake 最低版本为 3.1.2，升级仍需重新验证
clean build、incremental build 与跨 target module consumers。当前显式关闭
`build.c++.modules.std`；标准库用 global module fragment 中的 headers，
项目 API 使用 named modules。

优先通过 Homebrew 管理依赖：

```bash
brew bundle --file=Brewfile
scripts/configure.sh -m debug --exr=y -y
xmake -j4
xmake test
```

`Brewfile` 是安装清单，不会冻结 formula 版本；`configure.sh` 优先选择
brew 的 `gcc-16` / `g++-16` 与 pkg-config 目录，GCC 系列不符会直接报错。
可用 `CC` / `CXX` 显式选择编译器。更新 GCC series 需同步 xmake 检查、
toolchain.json 与验证记录，不自动退回旧语言标准。

本次会话没有发现 brew 或预装 xmake，因此实际验证使用现有系统 GCC 和
系统影像库，并从固定 release 源码构建了项目本地 xmake，未更改系统包或
Omarchy 配置。brew 路径尚未在此环境实测。没有全局 xmake 时：

```bash
scripts/bootstrap-xmake.sh     # 源码 archive 有 SHA-256 校验
export PATH="$PWD/.tools/install/bin:$PATH"
scripts/configure.sh -m debug --exr=y -y
xmake -j4
xmake test
```

基础依赖为 LibRaw、libtiff、libjpeg-turbo **3.2+**（ICC API）、LittleCMS；
OpenEXR 由 `--exr=y` 启用，默认关闭。xmake 只通过 pkg-config 查找系统／brew
库，不自动下载或编译另一套影像依赖。实际验证版本见 toolchain.json。
LibRaw 的 CR3 支持也依赖具体相机型号，不能只依据扩展名保证全部相机支持。

验证配置：

```bash
# 内存与未定义行为检查
scripts/configure.sh -m debug --exr=y --sanitize=y -y
xmake -r -j4
xmake test
python scripts/smoke-fixtures.py

# 验证不依赖可选 EXR 的 release 路径
scripts/configure.sh -m release --exr=n --sanitize=n -y
xmake -r -j4
xmake test
```

切换 sanitizer、编译模式或依赖版本后建议 `xmake -r` 重建模块与 consumers；
不要复用另一组 flags 生成的 BMI。完成后可切回 debug / EXR 的开发配置。

初始基础层验证记录（风格／sunlight 重构前）：debug + EXR clean build 和 188 项断言通过；同配置启用
ASan/UBSan 后 188 项断言与 11 个真实输入 smoke 全部通过；release 关闭 EXR
后 clean build 与 185 项断言通过，动态链接列表不含 OpenEXR/Imath。
完整分辨率 CR3 解码得到 5118×3412、无 NaN/Inf。增量无改动构建通过。
工作目录最终回到 debug + EXR、关闭 sanitizer 的开发配置。

当前新增的工程约定见 [cpp-style.md](cpp-style.md)：clang-format LLVM / 96 列、
构建前检查、全局声明／定义间空行，及原生 contracts 编译／链接 flags。
`Brewfile` 增加 `llvm` 提供 formatter；仍使用 GCC 编译项目。当前库与 CLI 已使用
`std::expected` 返回可恢复失败。`tests/test_format.py` 与 `tests/test_cli.py`
提供七项离线 Python 回归；C++ tests 加入 contracts 和 sunlight 行为回归。
重构后的 debug + EXR + ASan/UBSan clean build 与 C++／Python tests 已通过。

依赖许可证方向：LibRaw 提供 LGPL-2.1/CDDL 双许可，可选择兼容的 LGPL
路径；LittleCMS 与 libtiff 为 MIT 风格；libjpeg-turbo 含 IJG/BSD/zlib
许可；OpenEXR 为 BSD-3-Clause。当前动态链接它们，不把第三方源码复制到
project modules。依赖许可不适用于外部照片，fixture 的来源与条件另见
[fixtures.md](fixtures.md)。

`photo-ops` intentionally uses a cutting-edge native C++ stack.

The canonical direction is:

```text
C++26
+ recent GCC
+ xmake
+ named modules
+ Linux x86-64
```

This is a deliberate architectural choice, not a requirement to use every new language feature.

---

# 1. Why C++26

This project sits naturally in native C++ territory:

- RAW decoding;
- high-bit-depth image buffers;
- SIMD-friendly numerical kernels;
- pyramids, convolutions, warps, and blending;
- feature matching and geometric estimation;
- color transforms;
- mature native imaging libraries;
- explicit control over memory layout and copies.

C++ provides strong interoperability with existing imaging libraries while allowing a clean, modern, strongly typed internal architecture.

The codebase should exploit the modern subset of the language rather than inherit legacy C++ conventions by default.

The project has no requirement to support old compilers, old Linux distributions, or legacy ABI consumers.

That makes it a good environment for aggressive but disciplined use of current C++.

---

# 2. Module-native, not module-purist

Project-owned code should prefer named C++ modules.

A reasonable logical module graph is:

```text
photo_ops.raw
photo_ops.image
photo_ops.color
photo_ops.pyramid
photo_ops.registration
photo_ops.focus
photo_ops.sunlight
photo_ops.diagnostics
```

The exact partitioning should follow cohesion and dependency direction rather than aesthetics alone.

The project should **not** attempt to force third-party C/C++ libraries into modules.

Instead, isolate them behind narrow implementation/adaptation boundaries.

Conceptually:

```text
LibRaw / OpenCV / lcms2 / libtiff / OpenEXR
                     ↓
             thin integration layer
                     ↓
      project-native types and modules
```

This avoids leaking third-party abstractions throughout the codebase.

For example, `cv::Mat` should not become the universal image type merely because OpenCV is useful for feature detection or geometric estimation.

---

# 3. `import std;`

Treat standard-library modules separately from project named modules.

The policy is:

- aggressively use project named modules;
- use `import std;` only when the pinned GCC/libstdc++/xmake combination is known to support it reliably;
- do not make the build fragile for aesthetic purity;
- avoid clever header-unit usage unless it solves a concrete problem.

If the standard-library module path is unstable in a particular toolchain release, conventional standard-library includes at the integration boundary are acceptable.

---

# 4. Compiler policy

Use a recent GCC release as the canonical compiler.

"GCC latest" should mean:

> actively track recent GCC releases, while pinning one canonical version for reproducible development and CI.

Do not let the project silently float to whatever compiler package happens to be installed on a machine that day.

When upgrading the canonical compiler, make the migration explicit and review module/build behavior as part of the change.

The repository should eventually record the canonical version in a machine-readable or clearly documented form.

For example:

```text
Canonical development toolchain:
- GCC 16.x
- xmake 3.x
- Linux x86-64
- C++26
```

The exact versions should be updated as the project evolves.

---

# 5. Build system: xmake

xmake is the preferred build system.

Reasons:

- concise project definitions;
- good ergonomics for modern C++ projects;
- direct support for C++ module dependency analysis;
- less build-script ceremony than a conventional CMake setup;
- suitable for a personal research/engineering project that controls its own environment.

A minimal shape may eventually look like:

```lua
set_project("photo-ops")
set_languages("c++26")

add_rules("mode.debug", "mode.release")

target("photo_ops")
    set_kind("static")
    add_files("src/**.cpp")
    add_files("src/**.cppm")
```

This is illustrative, not a frozen build specification.

---

# 6. Preferred C++ style

The desired style is modern, direct, and low-ceremony.

Prefer:

- value semantics;
- RAII;
- strong types;
- explicit ownership;
- non-owning views for image data where appropriate;
- `std::span` / `std::mdspan` when they make layout and extents clear;
- concepts when they express real constraints;
- ranges when they improve readability;
- `[[nodiscard]]`, `noexcept`, and const-correctness when semantically justified;
- compile-time composition over unnecessary runtime indirection;
- small cohesive APIs;
- free functions where object identity is unnecessary.

Avoid:

- owning raw pointers;
- blanket `shared_ptr` usage;
- inheritance-heavy architecture without real runtime polymorphism;
- factories/managers/services added as generic architecture patterns;
- macros when ordinary language facilities suffice;
- header-only design merely because it is familiar;
- legacy metaprogramming when modern constructs are clearer;
- using new language features simply to showcase them.

The project should feel like "modern C++ done intentionally," not a museum of language features.

---

# 7. Core image abstractions

The project should own its important image abstractions.

Core types should make explicit:

- dimensions;
- channel semantics;
- pixel format;
- color-space assumptions;
- mutability;
- ownership/lifetime;
- stride and layout when relevant.

A future interface might resemble:

```cpp
export module photo_ops.image;

export namespace photo_ops {

enum class pixel_format {
    linear_rgb_f32,
    oklab_f32,
    gray_f32,
};

template <class Pixel>
class image_view;

}
```

The exact API is intentionally not fixed yet.

The important property is that the project does not allow dependency-specific image types to define the architecture.

---

# 8. Third-party dependencies

Dependencies are acceptable when they provide mature functionality that is not interesting to reimplement.

Likely candidates:

- **LibRaw** — RAW decoding, including Canon `.CR3` support;
- **LittleCMS** — color management;
- **libtiff** — TIFF I/O;
- **OpenEXR** — optional floating-point diagnostic/intermediate output;
- **OpenCV** — selectively for mature CV algorithms such as SIFT, robust geometry, or optical flow;
- **Eigen** — only if it materially simplifies numerical code.

Dependency policy:

> use mature libraries for hard infrastructure, but do not allow them to become the project's conceptual API.

Simple computational-photography primitives are often worth implementing directly when they are small and central to the behavior of the operator.

Examples:

- Gaussian/Laplacian pyramids;
- gradients;
- local focus measures;
- mask feathering;
- OKLab transforms;
- multi-band blending;
- simple region statistics.

Conversely, avoid reimplementing complex, mature algorithms merely for purity.

Examples:

- SIFT;
- robust general-purpose feature matching;
- sophisticated dense optical flow.

---

# 9. Performance philosophy

This is a native image-processing project, so performance matters, but optimization should remain evidence-driven.

Priority order:

1. photographic correctness;
2. deterministic and understandable behavior;
3. clean ownership and data flow;
4. measurement;
5. optimization.

Potential optimization tools include:

- SIMD;
- multithreading;
- cache-aware tiling;
- reduced-resolution analysis passes;
- zero-copy views;
- optional GPU acceleration.

Avoid hidden full-frame copies in hot paths.

When practical, retain simple scalar/reference implementations for mathematical primitives because they help testing and validate optimized paths.

---

# 10. Interoperability

C and conventional C++ APIs are welcome at the boundary.

The preferred pattern is:

```text
external library
      ↓
small adapter
      ↓
project-native module API
```

This keeps the computational-photography core coherent and makes it easier to replace a dependency later.

The project should not sacrifice useful ecosystem access in pursuit of module purity.

---

# 11. Reproducibility

Because compiler module support evolves quickly, reproducibility matters more than usual.

The project should eventually pin or document:

- GCC version;
- xmake version;
- dependency versions where compatibility is sensitive;
- build mode and relevant flags;
- CI image/container/toolchain where practical.

Toolchain upgrades should be explicit changes rather than invisible environmental drift.

---

# 12. Licensing direction

The intended license direction is **GPL-3.0-or-later** unless changed explicitly later.

Rationale:

- the algorithms and implementation are themselves a core part of the project's value;
- strong copyleft fits a personal research/engineering tool better than a permissive embedding-oriented library license;
- generated photographs are outputs of the program and are not intended to become GPL-licensed merely because the program is GPL.

When source files receive SPDX identifiers, prefer:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

Third-party dependencies must be checked for license compatibility before adoption.

---

# 13. Summary

The stack is intentionally opinionated:

```text
C++26
recent pinned GCC
xmake
named modules first
value semantics
RAII
strong types
minimal dependencies
explicit ownership
minimal runtime polymorphism
clean third-party boundaries
```

The goal is not to maximize novelty.

The goal is to use a modern native stack that produces clear, high-performance, aesthetically coherent code for computational photography.

## 重构后的验证补充（2026-10-25）

release / EXR 关闭的 clean build、C++ foundation + sunlight 回归、七项 Python
format/CLI 回归均通过；debug + EXR + ASan/UBSan 也通过。最终采用
`-fcontract-evaluation-semantic=quick_enforce` 兼容本机 GCC modules 优化构建，
不使用关闭 contracts 的 workaround。工作目录回到 debug + EXR、关闭 sanitizer。
预期 SIGABRT 子进程设置 `PR_SET_DUMPABLE=0`，重跑后 systemd-coredump 没有
新增 photo-ops-tests 事件；独立的 Omarchy 用户服务 drop-in 精确忽略该程序通知，
记录在 `~/CHANGES.md`，不属于项目源码配置。
