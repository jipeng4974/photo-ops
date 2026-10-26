# focus × exposure：显式矩阵与 HDR-first 合成

v1.8 将二维 capture group 接入现有 `stack_focus(...)`，没有新增第四个摄影操作。
第一版先沿 exposure 轴做 HDR，再沿 focus 轴合成，不是 joint 2D optimizer。

```json
{
  "version": 1,
  "focus_planes": [
    {"name": "near", "exposures": ["near_-2.CR3", "near_0.CR3", "near_+2.CR3"]},
    {"name": "far", "exposures": ["far_-2.CR3", "far_0.CR3", "far_+2.CR3"]}
  ]
}
```

```bash
photo-ops focus-stack --matrix capture.json -o stacked-hdr.tif \
  --memory-budget-mib 2048 --scratch-directory /path/to/scratch \
  --diagnostics build/matrix-review
```

C++ 使用 `focus_exposure_matrix` / `focus_exposure_plane` 值类型 descriptor，
调用同名 `stack_focus(matrix, output, options)`。flat focus brackets 继续使用原有
契约，不能把 flat AEB 序列静默解释成矩阵。

## 一个全局 context

- 2..32 个显式 focus planes，每组 2..16 exposures，必须矩形，总计不超过 256 张。
  名称唯一非空，原始文件不能复用。
- 每组实际 shutter grid 相同，比较 metadata，不根据文件名／列位置判断。列内顺序
  可不同，不要求整数 EV 或等间隔。ISO、aperture、已知 optics metadata 必须一致。
- 顺序 inspect 全部 RAW，再冻结一个 shutter-bracket context。默认 focus reference
  是 `F/2`，颜色／曝光 reference 为该组的 median-shutter exposure。`--reference`
  选择 focus plane，不是 flattened RAW index。
- 各组调用同一个 `merge_hdr(context, indices, ...)`，保留全局 WB、camera matrix 和
  reference-shutter scale。HDR 几何 reference 不触发颜色 context 重算。
- 跨 HDR planes 做 precise similarity registration，再做 coherent focus selection、
  defocus-aware multi-band fusion。最终输出 signed float32 linear Rec.2020/D65 ICC TIFF，
  不隐式 tone map。
- `--aligned` 同时断言两个轴已对齐；三脚架不自动等于没有 focus breathing。默认
  仍依赖图像证据配准，不在失败后猜 identity。

JSON 路径相对 manifest 实际目录解析，也支持显式绝对路径，不展开 shell 表达式。
CLI 的 JsonCpp 薄 adapter 限制文件 1 MiB、解析深度 64，拒绝 duplicate keys、未知
schema 字段、trailing content、comments、非有限数值和控制字符路径。分组不从时间、
文件名或 EV 猜测。

## HDR 证据进入 focus fusion

每个 HDR plane 的 RGB、confidence、deghost、clipping 和 low-signal 地图都保留相同
context fingerprint。core 顺序读一张 RGB，一次处理一个 scalar map，检查尺寸／语义／
范围后写 spool 并释放，不常驻 N 张 full-resolution 图。

confidence 使用保守 min pooling，flags 使用 max pooling；confidence warp 对参与
插值的样本取 minimum，避免将无效测量插值成有效。它影响三个环节：

1. 选择：focus score 降权，Potts 排除没有可靠测量的 source。平坦区没有 sharpness
   evidence 时，也不能让 clipped reference 胜出；全部无效则保留 reference。
2. 运动／边界：HDR deghost 是不能被 defocus fit 撤销的 motion veto，包含 reference
   自身的运动。跨 focus RGB motion 比较和 boundary preference 只使用可靠支持。
3. 融合：full-resolution base weight 使用 HDR quality。只设零权重仍可能让坏 RGB
   通过 Gaussian/Laplacian 邻域污染结果，因此用 tile 内可靠 source 补齐无效样本，
   coherent label 优先。这个 donor 只服务 pyramid support，不是最终 focus argmax。
   motion donor 保留 reference，全部无效处最终精确回退 reference。

缓存模式先收集 RGB + quality，再将 quality slot 复用为 base weight；streaming
增加一次 bounded pass。两条路径复用相同 HDR／focus 算法，没有第二套 radiometric
实现。

## 预算与发布

令 `P` 为全尺寸 pixels，`F` 为 focus planes，`Q/R` 为 coarse/fine pixels，
`T=(tile+2*64)²`。HDR stages 顺序执行，使用 [HDR 预算](hdr.md)。matrix focus reserve：

```text
working-plane decode = 32*P + largest_private_HDR_file + 128 MiB
streaming fusion     = 320*Q + 544*T + 128 MiB
cached fusion        = 320*Q + max(544,416+16*F)*T + 128 MiB
cache RGB + quality/weight = 16*F*T
```

这计入 donor、scalar warp 和额外 global maps；flat focus 保持原 256Q／512T 模型。
先选 streaming 可容纳的 tile，再决定是否 cache，没有新增 worker。它是保守工程
估计，不是 OS RSS 限制。

磁盘预检计算 active HDR spools、已保留 plane RGB／maps、focus spools、最终文件与
可选诊断 copy 的阶段峰值。没有预分配承诺，写满仍返回 typed I/O error。report
给出 HDR／metadata 时间、scalar warp 次数、retained bytes 和跨阶段 private logical
file peak；压缩文件大小不等于内存工作集。

中间 HDR RGB／地图只在 private directory 保存，成功／失败都清理。最终 TIFF
atomic no-replace 发布后才复制诊断，根 `report.json` 最后发布。前置失败没有 final
photo；后置诊断失败保留完整 photo 并报告路径。每个 artifact 独立 atomic，目录
不是事务，失败时可能有部分已完成诊断。

## Diagnostics

- 原有 focus source／confidence／motion／boundary maps 与 before/after previews。
- `hdr-plane-I/`：完整 HDR 数字地图和 report，可查看每张 RAW 的 exposure、gain、
  alignment、contribution／clipping 和全局 context。
- `hdr-confidence.tif`、`hdr-clipping.tif`、`hdr-low-snr.tif`：proxy-resolution coherent
  label 所选 plane 的 HDR 证据，不是最终 multiband RGB 的逐像素唯一来源。
- `hdr-deghost.tif`：共同几何下 exposure-axis motion veto union。
- `combined-confidence.tif`：focus margin × HDR quality，属于启发式，不是统计概率；
  motion fallback 的 margin 为 0。
- 根 JSON 的 `frames` 对应 focus planes，`hdr_planes` 引用组内原始曝光；不把前 F 张
  RAW 错标成 F 个 HDR planes。`working_minimum/maximum` 是编码前工作值范围。

numeric maps 无 ICC／transfer，使用 `read_scalar_tiff`。per-plane maps 为 full-resolution，
组合证据为 proxy-resolution。focus `selected_fraction` 与 HDR `contribution_fraction`
描述不同阶段，不能相乘后声称是最终 pyramid 的唯一原图使用比例。

## 回归与限制

2×3 sensor simulator 覆盖 shadow／midtone／highlight 三档亮度与近／远景细节，刻意
改变不同 focus plane 的 camera-WB metadata，以验证全局 context。按亮度归一的
focus MAE 为 `5.93439e-6`，单 focus HDR reference 为 `0.0494418`；比较目标为清晰
plane 的独立 HDR 输出，radiance 正确性另由 known-radiance HDR fixtures 验证。

另覆盖曝光顺序重排、focus 顺序反转与对应 reference、default 两轴配准、部分／全部
无效 plane、HDR motion veto、tile/whole-domain、metadata grid／reuse／schema 错误、
atomic output 和诊断失败。受控双轴几何 fixture 的最大 corner error 为
`0.0019399 full-res px`，回归门槛为 0.4 px，不是实拍精度承诺。

20 focus × 2 exposure stress 强制触发 streaming 和 cache，所有 RGB samples 完全
一致。独立负向构建仅关闭 invalid-sample donor，就会触发 partial clipping／pyramid
污染回归失败，保留处理有具体 failure 依据。见
[数值记录](../tests/data/matrix-baseline-v180.json)。

真实 R8 2×3 风景验收仍待素材。motion、parallax、严重 defocus 和细小 proxy 漏检仍
有边界，shared context 不会消除它们。未据此增加 joint optimizer 或 unrestricted warp。
工作值必须有限且满足 focus core 保守 magnitude limit（绝对 channel <=1e6）；超限
失败，不静默裁剪。
