# Design: Opinionated Computational-Photography Operators

## 摄影 API 边界

面向使用者的摄影操作保持两个入口：

```cpp
enhance_sunlight(...)
stack_focus(...)
```

内部配准、区域评分、mask refinement、多尺度融合可以持续演进，不把各阶段变成
需要使用者拼装的 API。必要的 controls、失败原因和可检查结果放在现有 options /
report 中；也不把 options 堆成算法开关清单。image / color / I/O 的工程接口继续按
职责存在，但不据此建一个通用 editor、pipeline builder 或另一套摄影调用方式。
新增摄影 operator 需要明确的实际用例与范围决定；不因为某个算法可以单独调用就
export 它，也不只为了测试而扩大公开边界。现有两个函数不必强行统一成抽象框架。

## v0.2 架构修正（2026-10-25）

进入 `photo_ops.focus_stack` 前，以以下契约为准：

- [RAW working pipeline 与 raw_sequence_context](raw-pipeline.md)：明确 camera-linear
  float → XYZ → linear Rec.2020/D65，保留负值／高光；先冻结整组 WB、black 与
  normalization，禁止逐帧自动开发。RAW 新路线仍待实现，不把现有 sRGB 视为永久 ABI。
  rendered-only 的 focus_stack 已可独立验证核心；该例外不放宽 RAW bracket 的门槛。
- [focus-stack 内存预算](focus-stack.md)：registration proxy + sequential decode/spool
  + tiled full-res fusion；单帧 decoder 峰值、halo、maps、scratch 与输出都计入预算。
- 输出现已使用同目录 temporary + atomic no-replace publish；每个文件独立完成，
  不是多文件事务，也不保证断电后的目录持久性。
- 依赖以 Omarchy/Arch system packages 为 canonical，brew 显式 opt-in；
  formatter 使用 stock clang-format，删除 custom lexical pass。

下文是产品算法方向；涉及全图融合的概念图不意味着可同时载入 N 张全尺寸图。
当前实现与未来设计的差异应显式保留，不能把待实现契约当作已实现能力。

## 当前已落地的兼容格式契约（2026-10-25）

输入以 CR3 为优先，并支持 TIFF、JPEG。内部使用项目自有 float32 linear RGB
buffer（当前旧路径明确为 linear-sRGB / D65，wide-gamut 迁移见上文）。默认输出为带 ICC 的 16-bit TIFF，
EXR 与 JPEG 为可选输出，Linear DNG 留待后续。详细支持范围、RAW 解码的
16-bit 中间结果限制，以及 rendered TIFF/JPEG 无法还原真实 scene radiance
的区别，见 [基础层架构](architecture.md)。

基础层已包含 module graph、xmake、image abstractions、格式检查、expected/contracts 与 tests。
CLI 提供 `inspect` / `convert` 和首版 `sunlight-enhance`；后者的实际算法、限制与
诊断见 [MVP 实现记录](sunlight-enhance.md)。v0.3.1 增加暗 RAW 的相对色度响应、
近中性亮材质／弱照明证据的保守 abstention，以及真实效果测量与局部对照。下文的完整 sunlight 设计仍包含未来改进，
`photo_ops.focus_stack` 已实现 rendered TIFF/JPEG 序列的配准、coherent selection、
运动 fallback 与 tiled multiband fusion；尚不接入 RAW bracket。实现边界与验收见
[focus-stack](focus-stack.md)。配准只服务本算子，暂不拆独立 registration/focus 模块。

`photo-ops` is a personal, programmatic landscape-photography toolkit.

It is not intended to replace Lightroom or Photoshop. Its purpose is to encode recurring photographic judgments as deterministic, inspectable operators that can usually run as:

```text
input → program → output
```

The two initial directions are:

1. `focus-stack` — automatic depth-of-field stacking from multiple RAW captures.
2. `sunlight-enhance` — illumination-aware enhancement of coherent sunlit regions when luminance contrast is strong but chromatic separation is weak.

The common philosophy is:

> Automatic by default, inspectable and surgically correctable on failure.

---

# 1. `focus-stack`

## Goal

Given multiple photographs of the same scene focused at different distances, produce one image whose perceptually important regions are all sharp.

Typical input conditions include:

- tripod + static scene + delayed shutter;
- minor tripod movement;
- handheld capture;
- focus breathing between frames;
- moderate parallax;
- small scene motion such as leaves, grass, water, clouds, or people.

The default workflow should require no interaction:

```bash
photo-ops focus-stack IMG_*.CR3 -o stacked.tif
```

Manual intervention should exist only as an escape hatch for ambiguous or failed regions.

## Non-goals

The first version does not need to be:

- a Photoshop-like brush editor;
- a general-purpose panorama stitcher;
- a neural rendering system;
- a dense-optical-flow-first solution;
- a full RAW developer or color-grading application.

The primary objective is robust stacking.

## Core pipeline

```text
CR3 inputs
   ↓
RAW decode
   ↓
scene-linear / high-bit-depth working representation
   ↓
alignment proxy generation
   ↓
global registration
   ↓
optional progressively stronger registration
   ↓
focus / sharpness estimation
   ↓
spatially regularized focus map
   ↓
soft multi-scale fusion
   ↓
artifact / confidence analysis
   ↓
crop invalid borders
   ↓
high-bit-depth output
```

## RAW handling

Prefer stacking before aggressive tone mapping, sharpening, and denoising.

Conceptually:

```text
CR3
 ↓
RAW decode
 ↓
linear RGB
 ↓
alignment + focus analysis + fusion
 ↓
high-bit-depth output
 ↓
Lightroom / Darktable / other final grading
```

Alignment can be estimated on reduced-resolution grayscale proxies while transforms are applied to full-resolution data.

Avoid using heavily sharpened or tone-mapped intermediates for focus estimation.

## Registration strategy

Registration is likely to be harder than focus estimation.

Increase registration complexity progressively rather than immediately allowing highly flexible deformation:

```text
Stage 1: translation / similarity transform
         translation + rotation + scale

Stage 2: homography
         stronger camera-pose changes

Stage 3: constrained local mesh warp / optical flow
         only when residual misalignment remains significant
```

Use the simplest transform that achieves acceptable residual error.

Dense local deformation should be conservative because excessive warp can distort:

- architecture;
- tree trunks;
- mountain ridges;
- horizons;
- starbursts;
- thin branches.

A small amount of residual misalignment is often preferable to geometrically implausible warping.

## Focus breathing

Changing focus distance can slightly alter effective magnification or field of view.

Even a static tripod capture can therefore require small scale correction.

Do not assume that a static camera implies pixel-identical framing.

## Parallax

Handheld motion or unstable tripod movement can create depth-dependent displacement. A single homography may then be insufficient because foreground and background objects move differently.

Parallax should trigger one of:

- constrained local registration;
- lower confidence in affected regions;
- conservative fallback behavior.

The system should prefer preserving plausible geometry over forcing exact correspondence.

## Focus estimation

For each frame \(i\), estimate a local focus score:

\[
F_i(x,y)
\]

Candidate classical measures include:

- Laplacian energy;
- local gradient energy;
- wavelet/high-frequency energy;
- multi-scale frequency response.

A learned estimator can be evaluated later, but is not required for the MVP.

A naïve selection:

\[
D(x,y)=\arg\max_i F_i(x,y)
\]

is not sufficient because it produces noisy frame switching and texture seams.

## Spatial regularization

The selected source frame should vary smoothly over space except at true depth boundaries.

Conceptually:

\[
E(D)=\sum_p C(p,D_p)+\lambda\sum_{p,q}S(D_p,D_q)
\]

where:

- \(C\) represents local focus evidence;
- \(S\) penalizes implausibly fragmented selections.

The desired result is a coherent focus/depth map rather than pixel-level salt-and-pepper switching.

## Fusion

Prefer soft blending instead of hard pixel selection.

Conceptually:

\[
I(x,y)=\sum_i w_i(x,y)I_i(x,y), \qquad \sum_i w_i(x,y)=1
\]

Candidate methods:

- Laplacian-pyramid blending;
- multi-band blending;
- smoothly feathered focus masks.

Important goals:

- avoid seams;
- avoid halos;
- preserve texture;
- preserve local contrast;
- prevent double edges.

## Motion handling

Landscape stacks may contain moving:

- foliage;
- grass;
- waves;
- reflections;
- clouds;
- people;
- birds.

The system should detect temporally inconsistent regions and lower confidence there.

Possible strategies:

- choose one representative source frame for a moving region;
- exclude inconsistent frames locally;
- prefer temporal consistency over maximum sharpness;
- mark the region for optional user override.

## Confidence and diagnostics

The program should output more than the final photograph when diagnostics are enabled.

Possible outputs:

```text
stacked.tif
focus_map.exr
alignment_confidence.exr
fusion_confidence.exr
diagnostics.json
```

Example diagnostic structure:

```json
{
  "frames": 7,
  "reference": "IMG_2034.CR3",
  "registration": {
    "max_displacement_px": 13.7,
    "scale_variation": 0.0043,
    "confidence": 0.992
  },
  "stack": {
    "low_confidence_fraction": 0.0017,
    "moving_regions": 4
  }
}
```

A stack can optionally be classified by difficulty:

```text
A — static tripod
B — tripod / minor motion
C — handheld
D — strong parallax or scene motion
```

## Optional overrides

Do not build a full interactive editor initially.

Small explicit overrides are enough:

```bash
--reference IMG_2034.CR3
--reject IMG_2036.CR3
--disable-local-warp
--prefer IMG_2034.CR3 --region <mask-or-region-id>
```

The default path should remain fully automatic.

## MVP

Recommended first implementation:

```text
CR3 input
 ↓
LibRaw decode
 ↓
linear float image
 ↓
reduced-resolution alignment proxy
 ↓
feature matching + RANSAC
 ↓
similarity transform
 ↓
multi-scale focus measure
 ↓
regularized focus map
 ↓
Laplacian-pyramid fusion
 ↓
16-bit TIFF
```

Only add homography, local mesh warping, motion rejection, and learned components when real photographs demonstrate a need.

---

# 2. `sunlight-enhance`

## Goal

Enhance landscape photographs where luminance contrast between sunlit and shadowed regions is already strong, but chromatic contrast is weak.

This often occurs because of:

- atmospheric haze;
- distance;
- weather;
- scattering;
- weak surface chroma;
- camera response;
- lighting conditions.

The desired result is not global saturation.

The intended perceptual effect is:

> Areas already visibly illuminated by direct sunlight should also exhibit plausible increases in colorfulness, warmth, local luminance, and visual prominence.

A concise formulation is:

> Restore or strengthen illumination-induced perceptual color separation.

The operator should make sunlight feel vivid without making the image look filtered, orange, or HDR-like.

## Human editing behavior being automated

The manual workflow being encoded is approximately:

1. identify a large, spatially coherent sunlit region;
2. place a soft local mask over it;
3. increase color separation;
4. slightly increase colorfulness;
5. slightly increase brightness;
6. slightly warm the local illuminant;
7. optionally restore a small amount of local contrast lost to haze.

The software should infer this automatically.

## Important distinction

Do **not** simply detect bright pixels.

The desired region is more like:

```text
bright
+ spatially coherent
+ sufficiently large
+ locally salient
+ plausibly sunlit
+ often surrounded by darker regions
```

Bright content that should not automatically be treated as target sunlight includes:

- bright sky;
- specular reflections;
- tiny white objects;
- isolated highlights;
- clipped areas;
- artificial lights.

## Sunlight likelihood map

Estimate a soft map:

\[
P_{\text{sun}}(x,y)\in[0,1]
\]

using some combination of:

- luminance;
- local luminance contrast;
- region size;
- spatial coherence;
- compactness;
- texture;
- chroma;
- scene context;
- edge structure;
- atmospheric cues.

A useful high-level pipeline is:

```text
linear / lightly rendered image
   ↓
multi-scale luminance analysis
   ↓
candidate bright-region segmentation
   ↓
connected-region analysis
   ↓
region scoring
   ↓
dominant plausible sunlit region(s)
   ↓
soft mask refinement
```

The final mask should have natural boundaries and smooth falloff.

## Colorfulness enhancement

Prefer perceptual chroma manipulation over naïve RGB saturation.

For example in OKLab:

\[
(L,a,b)
\]

with chroma:

\[
C=\sqrt{a^2+b^2}
\]

Apply:

\[
C'=C(1+kM)
\]

where:

- \(M\) is the sunlight mask;
- \(k\) is the enhancement strength.

Hue should remain approximately constant:

\[
h=\operatorname{atan2}(b,a)
\]

The central principle is:

> Enhance existing color evidence; do not invent color.

Low-chroma surfaces should not suddenly become vividly colored.

A more conservative form can use:

\[
C'=C+kM\,g(C)
\]

where \(g(C)\) suppresses gain for nearly neutral colors.

## Color contrast rather than global saturation

The key signal is the relationship between luminance separation and chromatic separation.

Let:

\[
\Delta L=L_{\text{sunlit}}-L_{\text{shadow}}
\]

and:

\[
\Delta C=C_{\text{sunlit}}-C_{\text{shadow}}
\]

The target case is:

```text
strong ΔL
weak ΔC
```

This means illumination structure is already visible, but colorfulness does not reinforce it enough perceptually.

Enhancement strength may therefore depend on a discrepancy such as:

\[
k\propto\max(0,\Delta L-\alpha\Delta C)
\]

The stronger the luminance contrast and the weaker the chromatic separation, the more compensation is justified.

## Warmth

Apply only a small local illuminant shift inside the sunlight mask.

Conceptually:

\[
T'=T+\Delta T\,M
\]

Warmth is a secondary cue, not the main mechanism.

Avoid turning sunlit regions orange.

## Luminance enhancement

Do not add a constant exposure offset across the whole mask.

Use luminance-dependent gain:

\[
\Delta L(x,y)=M(x,y)\,g(L)
\]

where \(g(L)\):

- gives modest lift to mid/high tones;
- decreases near clipping;
- protects highlight detail.

The operator should reinforce sunlight, not blow highlights.

## Atmospheric haze

Distant sunlit objects can exhibit:

- reduced local contrast;
- reduced chroma;
- blue/cyan contamination.

A mild local haze compensation may be useful inside the sunlight mask.

However:

> Preserve atmosphere; do not remove it.

The desired result is that haze remains visible while the region touched by sunlight regains color and life.

Avoid aggressive global-dehaze behavior.

## Naturalness constraints

Treat naturalness as a first-class constraint.

Protect against:

- excessive saturation;
- orange/yellow color casts;
- haloing;
- clipped highlights;
- artificial local contrast;
- HDR-like edge emphasis;
- inconsistent color between neighboring surfaces;
- color invention in neutral materials.

## Suggested pipeline

```text
CR3
 ↓
RAW decode / linear working image
 ↓
scene analysis
 ↓
illumination segmentation
 ↓
sunlight likelihood map
 ↓
dominant sunlit-region selection
 ↓
estimate luminance separation
 ↓
estimate chromatic separation
 ↓
estimate atmospheric attenuation
 ↓
adaptive chroma enhancement
 ↓
small warmth adjustment
 ↓
small luminance lift
 ↓
optional local haze compensation
 ↓
naturalness checks
 ↓
high-bit-depth output
```

## Diagnostics

The operator should be able to explain what it detected and changed.

Example:

```text
Sunlit region detected: 18.4% of image

Luminance contrast: strong
Chromatic contrast: weak
Atmospheric attenuation: moderate

Applied:
  perceptual chroma: +11%
  warmth: +180 K equivalent
  luminance: +0.12 EV
  local contrast: +4%
```

Useful debug outputs:

```text
sunlight_mask.exr
sunlight_candidates.png
before_after_preview.jpg
diagnostics.json
```

## MVP

Recommended first version:

```text
RAW / high-bit-depth RGB input
 ↓
convert an analysis copy to a perceptual color space
 ↓
multi-scale luminance segmentation
 ↓
connected-component scoring
 ↓
choose one dominant sunlit blob
 ↓
soft-mask generation
 ↓
OKLab chroma expansion
 ↓
small luminance lift
 ↓
small warm shift
 ↓
highlight protection
 ↓
output
```

Do not begin with a semantic segmentation network unless classical heuristics prove insufficient.

The first goal is to formalize the existing manual editing rule, not to solve arbitrary scene understanding.

---

# 3. Shared architecture

Both operators should share infrastructure where practical.

Possible logical structure:

```text
src/
  raw/
    decode
    color_management

  image/
    buffer
    view
    pyramid
    filtering
    masks
    warping

  registration/
    features
    ransac
    transforms
    confidence

  focus_stack/
    focus_measure
    focus_map
    fusion
    motion_detection

  sunlight/
    region_detection
    illumination_model
    chroma_adjustment
    haze_compensation

  diagnostics/
    debug_images
    metrics
    json_report

  cli/
```

The actual source tree should follow the module architecture rather than mirror this sketch mechanically.

Both operators should:

- work in high bit depth;
- preserve reproducibility;
- expose diagnostics;
- use deterministic defaults where practical;
- keep algorithmic stages separable;
- avoid destructive rendering decisions early in the pipeline.

---

# 4. Product philosophy

This project is best understood as a collection of:

> opinionated computational-photography operators

Each operator should encode a narrow photographic judgment that is repetitive enough to automate.

Potential command shape:

```bash
photo-ops focus-stack IMG_*.CR3 -o stacked.tif
photo-ops sunlight-enhance IMG_1234.CR3 -o IMG_1234.sun.tif
```

Possible future operators may include:

```text
atmosphere-restore
shadow-color-balance
highlight-guidance
```

but only when they correspond to a clear, repeatedly observed photographic editing pattern.

The overarching design principle is:

> Prefer interpretable image-processing decisions over opaque aesthetic transformation.
