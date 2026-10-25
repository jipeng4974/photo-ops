# TODO：focus-stack 下一阶段优先事项

本文只记录当前最重要的 focus-stack 后续工作，不重复完整设计。优先级含义：

- **P0**：继续扩展功能前应先处理；会直接影响核心正确性。
- **P1**：当前 MVP 可运行，但要成为实际长期使用的摄影工具，需要尽快完成。

---

## P0-2：把 registration 从 coarse initialization 提升到 subpixel refinement

### Change

保留当前 SIFT + mutual matching + RANSAC similarity 作为粗配准，但不要把它作为最终对齐结果。

建议流程：

```text
SIFT + RANSAC similarity
        ↓
coarse transform
        ↓
higher-resolution luminance / gradient proxy
        ↓
subpixel direct alignment refinement
        ↓
full-resolution-equivalent residual check
```

优先考虑 similarity 参数上的 ECC / correlation refinement；暂不引入 dense optical flow。

### Why

当前 registration 允许最高约 `1.5 proxy px` residual，再按 proxy step 放大到 full-resolution。对 6000 px 左右宽的照片、默认约 1024 px proxy，误差上限可能对应数个 full-res pixels。

focus stacking 对这种误差远比 panorama stitching 敏感：树枝、草叶、建筑轮廓、山脊、星芒等高频边缘即使只错位 1–2 px，也容易出现 double edge、halo 或局部变软。

SIFT/RANSAC 很适合找到正确几何 basin，但 sparse feature matching 本身不等于最终 subpixel alignment。后续 focus/fusion 都假设同一 `(x,y)` 已大致对应同一个现实位置，因此 registration quality 是整个算法的上限。

### Acceptance

- 保留 similarity transform：translation + rotation + uniform scale。
- refinement 在更高分辨率 proxy 上完成，不把高自由度 local warp 作为默认方案。
- 使用 dense/gradient-based post-registration metric，而不是只看 feature RANSAC residual。
- 对高信息区域达到接近 subpixel 的 full-resolution-equivalent residual；目标量级可先按约 `0.3–0.7 px` 验证，而不是把该数值当永久硬阈值。
- 新增真实或 synthetic regression，覆盖 focus breathing + 微小平移/旋转。

### Non-goals

- 不因为追求 pixel-perfect alignment 默认加入 optical flow / local mesh warp。
- 不允许 registration 为了降低 residual 而明显扭曲建筑、山脊或枝条几何。

---

## P1-1：实现 RAW float pipeline 与 `raw_sequence_context`

### Change

让 focus-stack 真正支持 Canon CR3 bracket，而不是要求用户先外部开发成 TIFF/JPEG。

目标路径：

```text
CR3 mosaic
  ↓
black/white normalization
  ↓
float demosaic
  ↓
camera RGB
  ↓
XYZ
  ↓
wide-gamut scene-linear RGB
  ↓
focus-stack
```

整组 bracket 先建立 immutable `raw_sequence_context`，固定并共享：

- white balance；
- black/white normalization；
- camera color transform；
- sensor / geometry assumptions；
- exposure normalization policy。

### Why

RAW 不是一张普通 RGB 图，而是 sensor measurements + camera metadata。独立逐帧开发会让 WB、black level、normalization 或其他处理产生微小差异，这些差异会污染：

- registration；
- motion detection；
- focus evidence；
- fusion/color consistency。

当前单图 LibRaw 路径经过 16-bit linear-sRGB compatibility intermediate；它适合单张处理，但不应成为 RAW bracket 的永久工作空间。scene-linear float + wide gamut 才能尽量保留负值、HDR/highlight headroom 与 sRGB gamut 外颜色。

### Acceptance

- CR3 bracket 共享同一个 sequence context，禁止逐帧 auto-WB / auto-brightening。
- internal working representation 使用明确的 scene-linear wide-gamut RGB；当前规划可继续用 linear Rec.2020 / D65。
- 保留 signed / HDR float，不在算法入口前无谓裁到 `[0,1]`。
- metadata/context mismatch 明确报错，不静默“各自开发后再 stack”。
- 用真实 Canon bracket 验证跨帧颜色一致性、highlight retention 与 focus-stack 结果。

---

## P1-2：处理 defocus / occlusion boundary

### Change

针对前景/背景深度边界增加专门的 defocus-aware fusion policy，而不是只依赖普通 focus score + multiband blend + motion fallback。

先从较保守的方法开始：

- 利用 focus-frame 顺序/来源建立粗 depth ordering；
- 检测 focus-label transition 与强边缘的重合；
- 在 foreground boundary 附近限制被 foreground bokeh 污染的 background contribution；
- 尝试 mask erosion/dilation 或 asymmetric boundary preference；
- 保留 reference fallback 作为安全策略。

### Why

景深合成最难的问题之一不是 registration，而是 defocus PSF 在深度边界处跨越真实几何边界。

例如前景树枝虚焦时，其模糊圆会扩散到后方天空/山体区域。因此背景对焦帧中的“背景像素”可能已经被虚化前景污染，简单逐像素选最清晰 source 或对称 multiband blend 都可能产生：

- halo；
- ghost edge；
- double contour；
- foreground transparent-looking rim；
- reference fallback 导致局部仍然虚焦。

这不是普通 scene motion，所以不能完全交给现有低频 RGB motion detector 处理。

### Acceptance

- 构造/收集典型 foreground-against-background fixtures：枝叶对天空、栏杆对远景、岩石对山景等。
- 明确区分 motion 与 defocus-boundary inconsistency。
- 在这些边界附近减少 halo/ghosting，同时不牺牲几何真实性。
- 不为了修复边界默认引入 unrestricted local warp。

---

## P1-3：优化 full-resolution 性能，同时保留 bounded-memory 架构

### Change

先 profile，再减少重复 warp、focus-score 计算和 scratch I/O；不要一开始直接上 SIMD/GPU。

当前 `fuse_tile()` 为了 bounded memory，会先遍历各 source 得到 base weight 总和，再第二遍重新 warp / 计算 weight / 建 pyramid。可以考虑：

- 在预算允许时缓存一个 tile 的 N 个 warped sources 或 base weights；
- memory planner 根据 budget 在 cache 与 two-pass streaming 之间选择；
- 避免重复 `focus_score()` / smoothing；
- profile 后再决定 tile-level parallelism、SIMD 或 GPU 是否值得。

### Why

当前架构的内存行为很好：不会常驻 N 张 full-res RGB，scratch 和输出也有明确预算与 atomic publishing。但已验证的 7 × 2048×1536 stack 约需 49 秒；Canon R8 full-resolution 是约 24 MP，实际用户流程很容易进入分钟级。

慢不会让结果错误，所以不属于 P0；但如果一次常规 bracket 需要等待过久，这个工具即使算法正确也会逐渐失去实际使用价值。

### Acceptance

- 先提供 stage-level profiling：decode / scratch I/O / warp / focus score / pyramid / output。
- 优化后仍保持显式 memory budget，不允许为了速度退回 `vector<full_res_image>` 常驻全部帧。
- 在代表性的 R8 分辨率和 3–8 帧 bracket 上记录 wall time、peak RSS、scratch bytes。
- 若增加并行，worker workspace 必须计入 memory planner。
- correctness regressions（tile vs whole-domain、motion fallback、identity、atomic output）继续通过。

---

## 推荐实施顺序

```text
P0-2 precise registration
        ↓
P1-1 RAW sequence pipeline
        ↓
P1-2 defocus / occlusion boundary
        ↓
P1-3 performance optimization
```

三项核心摄影问题分别对应：

1. **Registration**：不同 frame 的同一坐标是否真的对应同一个现实位置？
2. **RAW sequence context**：这些对应位置的数字 RGB 是否具有一致的颜色/曝光语义？
3. **Defocus-aware fusion**：坐标和颜色都一致后，哪些 frame 在深度边界处真正提供有效清晰信息？

性能优化放在正确性之后，但在 full-resolution Canon workflow 稳定前完成。

## 实施记录（2026-10-25）

- **P0-2 / v1.1.0**：已接入预算内的高分辨率四参数 direct refinement；新增
  dense gradient 指标与 fractional translation / 正反 breathing、rotation 回归。
  具体语义与验收边界见 [focus-stack](focus-stack.md)。
- **P1-1 / v1.2.0**：已实现 signed float Bayer RAW → XYZ → Rec.2020、immutable
  sequence context、metadata/身份检查与 float32 ICC TIFF 输出；generated DNG 与
  真实 Canon / R8 RAW/CRAW 解码回归通过，三帧 Canon 原尺寸合成已完成。R8 自摄
  bracket 的颜色／摄影质量验收仍待素材，不能由第三方单张替代。
- P1-2、P1-3 继续；九组 synthetic branches / railings / rocks × defocus 尺度已记录
  原始失败基线，后续先处理 boundary 再 profile/cache。
