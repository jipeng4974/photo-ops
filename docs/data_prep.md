# R8 / CR3 素材准备计划

> **状态：计划中，尚未完成。**
>
> 本文定义后续需要主动积累的 Canon EOS R8 / CR3 测试素材类型、组织方式与用途。
> 除非另有明确记录，不应把这里列出的场景理解为已经采集、已经纳入 regression，
> 或已经具备可公开下载的数据集。

## 目标

真实 R8 CR3 素材用于验证 `photo-ops` 在实际摄影条件下的行为，尤其服务于：

- focus-stack 的高精度 registration；
- RAW float / `raw_sequence_context`；
- defocus / occlusion boundary handling；
- motion fallback；
- full-resolution 性能与内存预算；
- 真实摄影 failure mode 的发现与回归。

目标不是收集大量随机风景照片，而是优先积累**有明确测试意义、容易暴露问题的 fixture**。

## 优先采集的 focus bracket

### 1. 静态三脚架 / focus breathing

**优先级：最高。**

建议：R8 + RF 85mm F2 Macro IS STM，三脚架、无风、延迟快门，只改变焦点位置。
每组约 3–7 张，从 near → middle → far。

用途：

- 分离纯 focus breathing 与相机位移；
- 验证 similarity transform；
- 验证后续 subpixel registration refinement；
- 测试 focus map 与 multiband fusion；
- 为 RAW sequence consistency 提供最干净的真实序列。

### 2. 强前后景遮挡 / defocus boundary

例如：

- 近处树枝 against 天空或远山；
- 栏杆 against 建筑；
- 花草 against 远景；
- 岩石边缘 against distant landscape。

用途：

- 暴露 foreground bokeh 对背景的污染；
- 测试 halo、double contour、ghosting、fuzzy rim；
- 驱动 P1 的 defocus / occlusion-aware fusion。

这类素材比普通大平面风景更有价值，因为它直接覆盖景深合成最困难的边界情况。

### 3. 轻微手持 bracket

保持构图基本一致，但允许小幅：

- translation；
- rotation；
- perspective / parallax variation。

用途：

- 验证 coarse SIFT/RANSAC initialization；
- 验证高分辨率 registration refinement；
- 评估 similarity transform 的适用边界；
- 发现何时需要明确失败，而不是错误融合。

首轮不需要追求极端手持位移。

### 4. 广角三脚架风景

建议：R8 + RF 14–35mm F4 L IS USM。

用途：

- 覆盖实际最常见的广角风景场景；
- 与 85mm Macro 的强 focus breathing case 形成对照；
- 验证建筑、山脊和大尺度场景的边缘几何；
- 验证高分辨率 tiled fusion。

## 第二阶段素材

### 5. 运动物体

例如：

- 风吹树叶；
- 草地；
- 水面；
- 云；
- 偶然经过的人或动物。

用途：验证 motion detection / coherent-source fallback，避免不同帧局部拼出双影或碎裂结构。

### 6. 高反差细边缘

例如：

- 细树枝 against bright sky；
- 电线；
- 建筑轮廓；
- 金属栏杆；
- 星芒／点状高光。

用途：对 subpixel registration 极为敏感，可快速暴露 1 px 以内的错位、halo 和重影。

### 7. 低纹理场景

例如：

- 雾；
- 平整墙面；
- 大面积天空；
- 低细节地面。

用途：验证 registration 在缺少 feature 时是否保守失败，而不是盲目接受 identity 或错误 transform。

### 8. 高动态范围 RAW

例如：

- 亮天空 + 暗地面；
- 逆光；
- 高亮云层 + 阴影树林；
- 局部接近 sensor clipping 的景观。

用途：

- 验证未来 float RAW pipeline；
- 验证 highlight headroom；
- 验证负值／超出 display gamut 的 working RGB；
- 验证 linear Rec.2020 / D65 等 wide-gamut working representation。

## 每组素材的建议规模

一般每组 focus bracket **3–7 张** 即可。

不追求大数据量；优先保证：

- 拍摄条件明确；
- focus 顺序明确；
- exposure / aperture / ISO 一致；
- 文件完整；
- 场景对应一个清楚的算法问题。

## 建议目录结构

原始 CR3 不直接提交到 Git 仓库。建议外部保存：

```text
r8-fixtures/
  scene-001-tripod-breathing/
    IMG_1234.CR3
    IMG_1235.CR3
    IMG_1236.CR3
    metadata.json

  scene-002-branch-occlusion/
    ...
```

repo 中只记录 manifest、hash、metadata 和 fixture 分类。

示例：

```json
{
  "camera": "Canon EOS R8",
  "lens": "RF85mm F2 Macro IS STM",
  "focal_length_mm": 85,
  "aperture": 4.0,
  "iso": 100,
  "capture": "tripod",
  "focus_order": ["near", "mid", "far"],
  "scene_tags": ["focus-breathing", "occlusion"],
  "notes": "Foreground branch against distant building; no intentional camera movement"
}
```

建议 manifest 额外记录：

- 原始文件名；
- SHA-256；
- 是否允许公开；
- 拍摄者 / 版权；
- fixture category；
- 是否 canonical regression；
- 已知 failure / expected behavior；
- 可选的 local/download location。

## Canonical fixtures 与 exploration corpus

建议把素材分成两层。

### Canonical fixtures

少量、稳定、长期维护，建议最终约 10–20 组。

每组应对应一个明确 regression 目的，例如：

```text
01 tripod-static-breathing
02 handheld-similarity
03 branch-sky-occlusion
04 foliage-motion
05 water-motion
06 high-dynamic-range
07 low-texture
08 architecture-fine-lines
```

这些 fixture 应有稳定 hash 和明确 provenance。

### Exploration corpus

日常摄影中积累更多真实 bracket，用于：

- 发现新 failure mode；
- 做人工视觉 review；
- 判断当前 heuristic 是否过拟合 canonical fixtures；
- 挑选未来值得升级为 canonical regression 的 case。

不要求 exploration corpus 全部进入仓库 manifest。

## 首批建议采集

在正式实现 RAW bracket 前，优先准备以下四组即可：

1. **R8 + RF85 F2 Macro，静态三脚架 focus breathing**；
2. **树枝 / 栏杆 against 远景，强 occlusion boundary**；
3. **轻微手持 focus bracket**；
4. **R8 + RF14–35 F4 L，普通三脚架广角风景 bracket**。

这四组已经能覆盖当前最重要的：

```text
registration precision
RAW sequence consistency
defocus / occlusion boundary
full-resolution fusion
```

## 数据进入项目的原则

- 不因为“是真实照片”就直接成为 regression fixture；先明确它对应什么 failure / behavior。
- 原始 CR3 默认保存在 repo 外；不要用普通 Git history 存几十 GB RAW。
- 自己拍摄的素材最适合长期维护，因为 provenance 和授权最清晰。
- 如果未来公开少量 fixture，单独记录许可与下载方式。
- 真实 fixture 不能取代 synthetic tests；二者应互补：synthetic case 锁定机制，真实照片验证摄影质量。

## 当前状态

**尚未完成任何上述 R8 专项采集计划。**

已有 repo 中的第三方 / 下载 fixture 与本文规划的“用户自行拍摄 R8 canonical corpus”是两套概念。
后续每完成一组采集，应更新 manifest / provenance，而不是只修改本文把计划描述成事实。
