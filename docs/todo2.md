# TODO2：HDR 与 focus × exposure 二维合成

> **状态：分阶段实施中。** v1.6 已提供 shutter-only RAW context 与 sensor-support 基础；v1.7 已提供 standalone HDR operator，v1.8 已接入显式二维 HDR-first 合成；实拍验收仍待素材。本页各项以末尾实施记录为准。

本文考虑两个相关但不同的目标：

1. **单独 HDR merge**：输入同一焦点、不同曝光的 AEB RAW 序列，输出 scene-linear HDR 图像；
2. **focus × exposure 二维合成**：输入多个焦平面、每个焦平面又包含多个曝光档位的 RAW 矩阵，同时扩展景深与动态范围。

术语上，本文的 **HDR merge** 指 radiometric merge，不默认包含局部 tone mapping 或“HDR 风格化”。最终显示映射应与 scene-linear 合成本身分离。

---

## P2-A：独立 HDR merge

### Change

新增明确的 photographic operator，例如：

```text
photo_ops.hdr
merge_hdr(...)
```

CLI 可对应：

```bash
photo-ops hdr exp_-2.CR3 exp_0.CR3 exp_+2.CR3 -o hdr.tif
```

优先支持 Canon RAW AEB bracket；rendered TIFF/JPEG 可后续按需求增加，不为第一版重新引入 camera response curve 估计。

### 推荐实现

```text
AEB RAW bracket
      ↓
shared raw_sequence_context
      ↓
signed float scene-linear working RGB
      ↓
exposure normalization
      ↓
small geometric alignment if needed
      ↓
per-pixel exposure reliability / motion validity
      ↓
radiometric HDR fusion
      ↓
float HDR TIFF / EXR
```

核心建议：

- 整组 AEB 使用同一个 RAW sequence context，沿用现有 signed float / Rec.2020-D65 路线；
- 从 EXIF / RAW metadata 验证曝光关系，优先假设摄影端固定 aperture、ISO、WB，仅改变 shutter；
- 把每帧归一到共同 scene-radiance scale，而不是把不同曝光直接做普通 alpha blend；
- RAW 已经是近似线性 sensor measurement，第一版不做传统 JPEG HDR 中的 CRF estimation；
- 每像素 exposure weight 应考虑：
  - highlight clipping / 饱和风险；
  - shadow noise floor / SNR；
  - 非有限值和坏点；
  - 与 reference 的 motion / temporal inconsistency；
- 一般倾向使用**最长且未饱和**的曝光获取 shadow SNR，同时从短曝光恢复高光；
- registration 使用 exposure-robust proxy（log luminance / gradient / normalized contrast），不让曝光差本身污染几何估计；
- 对移动树叶、水面、人等区域提供 conservative deghost：必要时选择一个 coherent reference exposure，而不是跨时间混合出重影；
- HDR merge 输出保持 scene-linear float；tone mapping / local contrast / display rendering 不塞进 merge 核心。

### Why

AEB 的本质不是“把三张不同亮度照片平均”，而是对同一场景辐射量的不同测量：长曝光 shadow SNR 好但更容易 clipping，短曝光保护 highlights 但暗部噪声更高。scene-linear RAW 下可以直接利用这种互补性，不必先生成 display-referred JPEG 再反推响应曲线。

### Acceptance

- 支持至少 3-frame RAW AEB（例如 `-2 / 0 / +2 EV`）；
- 固定 aperture / ISO 的 shutter bracket 能恢复 clipped highlight 与深阴影细节；
- 输出保留 signed / HDR float，不在 merge 后自动裁到 `[0,1]`；
- diagnostics 至少包含 exposure metadata、每档 contribution / clipping 信息、motion/deghost 区域和最终动态范围；
- synthetic radiance fixture + 真实 R8 AEB bracket 均有回归；
- HDR merge 本身不进行不可逆 tone mapping。

### Non-goals

- 第一版不做“HDR look”或局部 tone mapping；
- 不鼓励通过改变 aperture 形成 AEB，因为那会同时改变景深，使 exposure 轴与 focus 轴纠缠；
- 不为了 handheld HDR 默认引入 unrestricted local warp。

---

## P2-B：focus × exposure 二维矩阵合成

### Input model

典型输入不是一条 flat sequence，而是显式二维分组：

```text
                 Exposure
             -2      0      +2
Focus near    A0      A1      A2
Focus far     B0      B1      B2
```

对 R8 的实际拍摄，一个很自然的第一版就是：

```text
2 个摄影师手选焦平面 × 3 档 AEB = 6 张 CR3
```

每个焦平面按一次 AEB burst，再手动切到下一个关键焦点。输入分组必须显式提供，不能仅凭文件名、时间或 EV 自动猜测二维结构。

建议使用 manifest / grouped descriptor，例如：

```json
{
  "focus_planes": [
    {"name": "near", "exposures": ["near_-2.CR3", "near_0.CR3", "near_+2.CR3"]},
    {"name": "far",  "exposures": ["far_-2.CR3",  "far_0.CR3",  "far_+2.CR3"]}
  ]
}
```

CLI 更适合扩展现有 `focus-stack`：

```bash
photo-ops focus-stack --matrix bracket.json -o stacked-hdr.tif
```

而不是马上增加一批 `registration/exposure/focus` 低层公共 API。

### 第一版推荐：hierarchical scene-linear merge

不要一开始做完全 joint 2D optimizer。第一版建议：

```text
所有 RAW
  ↓
one shared sequence/context contract
  ↓
按 focus plane 分组
  ↓
每个焦平面内部做 scene-linear HDR merge
  ↓
H_near, H_mid, H_far + HDR confidence maps
  ↓
跨焦平面 precise registration
  ↓
defocus-aware focus selection / fusion
  ↓
scene-linear extended-DOF + HDR output
```

也就是：

> **先沿 exposure 轴合并，再沿 focus 轴合并。**

### 为什么优先 HDR-first

相比“每个曝光档先 focus-stack，再对多个 focus-stacked 结果做 HDR”，HDR-first 更自然：

- exposure merge 在同一焦平面内完成，几何/PSF 一致性更高；
- 可以先把不同曝光统一为同一个 radiometric representation，再做 focus evidence；
- exposure 轴被提前压缩，后续 focus-stack 不需要重复执行 N 次；
- focus-first 可能让 `-2/0/+2 EV` 三组各自得到略不同的 focus boundary / motion fallback，随后 HDR merge 会面对几何上不一致的中间结果；
- scene-linear HDR result 可以携带 per-pixel confidence，供 focus fusion 避免使用 clipping / low-SNR 区域。

但 HDR-first 不是数学上永远最优：曝光、focus、motion、occlusion 并非完全独立。因此保留 joint 方案作为后续研究方向，而不是第一版前置条件。

### 需要传播的中间信息

每个 focus plane 的 HDR merge 不应只输出 RGB，还应至少输出：

```text
radiance / working RGB
exposure confidence
clipping / low-SNR validity
motion/deghost map
```

然后 focus fusion 的 evidence 可以综合：

```text
F_f(p)   = focus evidence for focus plane f
E_f(p)   = HDR exposure reliability
M_f(p)   = motion validity
D_f(p)   = coherent depth/focus label
```

第一版不必把它们简单写成一个 `F * E * M` 权重；可以保持现有 coherent focus-label 逻辑，只在 evidence / fallback 时使用 exposure reliability 与 motion validity。

### 长期方向：真正 joint 2D fusion

只有在真实 2D fixtures 证明 staged HDR-first pipeline 存在系统性失败后，再考虑 joint model：

```text
I[f, e, p]
  ↓
exposure-normalized focus evidence
  ↓
coherent focus/depth labeling
  ↓
within-label exposure fusion
  ↓
occlusion / motion consistency
```

一个合理的 joint 思路是先确定 coherent focus/depth support，再只在该 support 内跨 exposure 做 radiometric fusion；不要让 exposure reliability 直接覆盖 foreground/background occlusion 关系。

避免演化成通用 graph/pipeline builder；这仍然是一个明确的 photographic operation。

### 拍摄约束

二维 bracket 要尽量保证两个轴正交：

- **Exposure 轴**：固定 aperture、ISO、WB、focal length，优先只变 shutter；
- **Focus 轴**：固定曝光策略，只改变 focus distance；
- 最好使用 tripod；
- R8 无法同时自动运行 AEB 与 Focus Bracketing，因此“手选 2 个关键焦平面，每个焦平面拍一组 AEB”正好是合理的数据采集方式；
- 场景运动仍是主要难点，capture 顺序和时间间隔需要记录。

### Diagnostics

建议 2D merge 至少可检查：

- focus source / depth plane map；
- exposure contribution map；
- clipping / low-SNR map；
- motion / deghost map；
- defocus-boundary / occlusion fallback；
- combined confidence；
- 每张输入的 exposure、focus group、alignment 与使用比例。

### Acceptance

- 先支持最典型的 `2 focus × 3 exposure` RAW matrix；
- 在静态高反差风景上同时优于单张 exposure 和单 focus plane：高光、阴影、前景、远景均有有效信息；
- staged HDR-first pipeline 复用 standalone HDR 与现有 focus-stack 能力，不复制两套算法；
- motion / clipping / defocus-boundary 的 diagnostics 可解释最终 fallback；
- 输出保持 scene-linear float HDR，显示 tone mapping 仍独立；
- 收集真实 R8 2×3 bracket fixture 后再判断是否值得进入 joint 2D optimization。

---

## 建议实施顺序

```text
P2-A standalone scene-linear HDR merge
        ↓
HDR diagnostics + R8 AEB fixtures
        ↓
focus-stack 接受 grouped exposure planes / matrix manifest
        ↓
P2-B staged HDR-first 2D fusion
        ↓
real-image failure analysis
        ↓
只有确有必要才做 joint 2D optimization
```

与 `docs/todo.md` 的关系：P1-2 defocus/occlusion 与 P1-3 performance 仍然是现有 focus-stack 的直接质量工作；standalone HDR 可以并行探索，但 2D focus × exposure 的正式质量验收应建立在 HDR merge 与 defocus-aware focus fusion 都足够稳定之后。

## API 原则

实现阶段建议只增加一个真正新的 photographic capability：

```cpp
merge_hdr(...)
```

二维 matrix 不需要再暴露 `exposure_align()`、`radiance_fuse()` 等公共阶段；优先扩展 `stack_focus(...)` 的 grouped input model / CLI manifest。若最终需要新的 public operation 名称，应在实现前同步更新 `AGENTS.md` 的 photographic API surface，而不是仅因为内部算法变复杂就扩张 API。

## 实施记录（2026-10-26）

- **v1.6.0 / RAW 基础**：显式 `shutter_bracket` policy 冻结 WB／calibration，按 metadata
  归一共享 exposure scale；opt-in pre-WB sensor-support extrema 覆盖完整 demosaic
  footprint，并在 unpack 前计入预算。fixed focus bracket 的曝光拒绝契约继续保留。
  细节与回归见 [RAW pipeline](raw-pipeline.md)。
- **v1.7.0 / standalone HDR**：新增 `photo_ops.hdr`、`merge_hdr` 与 `hdr` CLI，支持
  signed float radiometric AEB、shutter metadata 归一、sensor reliability／clipping、
  coherent component deghost、预算内 tile/spool 和数字地图／JSON 诊断。合成 radiance、
  三种 camera 色彩的 partial clipping、reference clipped 的运动与 subpixel AEB
  registration 回归见 [HDR](hdr.md)。真实 R8 AEB acceptance 仍待素材。
- **v1.8.0 / matrix**：grouped `stack_focus` 与 `--matrix` JSON 先逐 focus plane 调用
  同一 HDR operation，再复用 focus／defocus／fusion。所有 originals 共享一个
  context，HDR 可靠性与 motion 进入选择、pyramid support 和 fallback。见
  [focus × exposure](focus-exposure.md)。
- 工程路径已可运行；真实 R8 AEB／2×3 专项采集与风景验收仍未完成。joint 2D
  optimization 继续保留为研究方向，等待真实 failure evidence。
- **v1.8.1 / 回归修复**：新增独立解析式 Gaussian 细纹理 fixture，修复高亮静态失焦
  被跨 focus motion gate 误报的问题；要求强局部 blur fit 与相邻共享结构支持，
  保留真实运动与 HDR deghost veto。没有用调高全局 motion 阈值掩盖失败。

- **v1.9.0 / 检查工具**：提供独立 DNG radiance／Gaussian-defocus／noise fixture 的
  复现、两个单焦点 HDR 与矩阵的统一显示预览，以及 HDR／matrix per-child 资源记录。
  这些 synthetic artifacts 不算真实 R8 AEB／2×3 capture。
