# R8 / CR3 素材准备计划

> **状态：采集计划尚未完成；v1.5 已提供本地登记与校验工具。**
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

- 验证已实现的 float RAW pipeline；
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
  "exposure_time_seconds": 0.008,
  "capture": "tripod",
  "focus_order": ["near", "mid", "far"],
  "files": ["IMG_1234.CR3", "IMG_1235.CR3", "IMG_1236.CR3"],
  "scene_tags": ["focus-breathing", "occlusion"],
  "photographer": "填写实际拍摄者",
  "public": false,
  "license": "private",
  "canonical_regression": false,
  "expected_behavior": "保留枝条几何，同时改善背景清晰度且不引入双边缘",
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

首批自摄 RAW bracket 优先准备以下四组即可：

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

## v1.2 工程验证补充（不代表完成上述采集）

已增加固定 SHA-256 / CC0 provenance 的 R8 RAW 与 CRAW **单张**下载 manifest，
以及可 opt-in 的 float decoder 检查，见 [fixtures](fixtures.md)。它们可验证相机
模式与 full-resolution 资源模型，重复同一输入可用于性能／identity；不能被登记为
已采集的静态 breathing、遮挡、手持或广角 focus brackets。

目前数值契约由 deterministic Bayer DNG fixtures 覆盖；真实三帧 Canon R5 II
macro 连续前缀已完成 float/context 合成。风景质量与用户自摄 R8 bracket 的验收
仍需要按本文原计划提供素材。

## v1.5 本地登记工具

工具只读原图，生成小型 manifest；不复制、不上传 CR3，不从文件名猜焦点顺序，
也不把声明的拍摄条件当作已核实的 EXIF。模板默认是 private / exploration，
拍摄者和镜头留空，须填写后才能登记。

```bash
# 使用 repo 外的实际采集目录；不会覆盖已有 metadata.json
python scripts/prepare-r8-fixture.py init /path/to/r8-fixtures/scene-001
# 将原图保存在该目录，填写 metadata.json，包括显式有序 files / focus_order
python scripts/prepare-r8-fixture.py register /path/to/r8-fixtures/scene-001
python scripts/prepare-r8-fixture.py verify /path/to/r8-fixtures/scene-001/manifest.json
```

`files` 和 `focus_order` 的位置一一对应；同一次 focus bracket 应固定曝光时间、
ISO、光圈。`--frames near.CR3 middle.CR3 far.CR3` 也可提供顺序，但若 metadata 已有
`files`，两者必须一致。工具要求 2..256 个非空 CR3、明确 R8 型号、镜头／焦距／
曝光参数、tripod/handheld、场景标签与拍摄者，记录每个文件的 SHA-256、字节数、
焦点位置，以及 metadata 原始字节的 SHA-256。解码阶段还会独立检查真实相机与曝光
兼容性；这里只做登记，不验证 sensor 文件确实可解码。

manifest 可写到 repo 中独立的清单位置，原图保持在外部目录：

```bash
python scripts/prepare-r8-fixture.py register /path/to/r8-fixtures/scene-001 \
  --output tests/data/r8-scene-001-manifest.json
python scripts/prepare-r8-fixture.py verify tests/data/r8-scene-001-manifest.json \
  --directory /path/to/r8-fixtures/scene-001
```

工具拒绝父目录穿越、逃逸 symlink、重复路径／symlink alias、缺失／空文件和已有
输出（包括 dangling symlink）；manifest 使用同目录 temporary + atomic no-replace。
校验会发现原图内容／长度、metadata 或焦点顺序变化。修改拍摄记录后应创建新的
manifest，不能用覆盖旧清单隐藏 provenance 变更。manifest 中只有相对文件名，
不自动写入机器本地绝对路径；metadata 的自由文本仍由作者决定是否适合提交。

需要公开时，显式设置 `public: true`，填写 `license` 与 `license_url`；工具只验证
这些声明存在，不代替授权。升级 canonical 时，填写 `canonical_regression: true`
和具体 `expected_behavior`，再做 decoder / 摄影结果 review 与回归绑定。
manifest 的 `canonical_regression_declared` 只是作者声明，
`state: registered-not-decoder-validated` 与 `raw_decoder_validation: not-run` 表示
尚未通过 RAW 开发验证，不能据此宣布完成自摄 corpus。

实际开发／合成仍通过现有摄影入口：

```bash
xmake run photo-ops focus-stack /path/to/near.CR3 /path/to/middle.CR3 /path/to/far.CR3 \
  -o build/r8-scene-001-stack.tif --diagnostics build/r8-scene-001-review
```

先 `verify` 身份，再检查 context、registration、motion/boundary diagnostics、
高光与局部画质；把具体失败记录到 expected behavior / regression。manifest 校验
通过不能替代这些步骤。九项离线 Python 回归覆盖身份／顺序／元数据篡改、路径逃逸、
atomic publication 与声明校验；测试使用明确标注的 dummy bytes，不冒充真实 CR3。
