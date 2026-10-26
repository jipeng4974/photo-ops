# scene-linear RAW HDR merge

v1.7 增加 `photo_ops.hdr` / `merge_hdr(...)`，处理同一焦点、固定 ISO／aperture／
名义焦距的 shutter-only Bayer CR3／DNG AEB。它合并对同一辐射量的多次测量，
输出带 linear Rec.2020/D65 ICC 的 **signed float32 TIFF**；不估计 JPEG CRF，
不做 tone mapping、局部对比度、锐化或显示风格化。

```bash
xmake run photo-ops hdr short.CR3 middle.CR3 long.CR3 -o radiance.tif \
  --memory-budget-mib 2048 --scratch-directory /path/to/scratch \
  --diagnostics build/hdr-review
xmake run photo-ops inspect radiance.tif
xmake run photo-ops inspect build/hdr-review/confidence.tif
```

输入顺序由调用者提供，2..32 帧，至少两种不同 shutter。默认 reference 是按真实
shutter 排序后的中间曝光，`--reference INDEX` 可显式选择输入索引；不假设输入按
EV 排序或正好相差整数 stop。`--aligned` 是调用者的几何断言，不是失败后的自动
identity fallback。普通 `focus-stack` 的 flat RAW 输入继续拒绝不同曝光。

## RAW 与 radiometric scale

全部输入先顺序 inspect/unpack，冻结一个 `raw_sequence_context`，采用显式
`shutter_bracket` policy。WB、black／white、camera matrix、sensor geometry／CFA、
ISO、aperture、已知 lens／nominal focal length 都共享；不兼容整组失败。
未提供的 lens／focal metadata 保留为空／0，不能据此证明拍摄时没有变焦。

每帧 scene-linear RGB 乘 `reference.shutter / frame.shutter`，单位是 reference
曝光下的相对信号，不是绝对物理辐射度。曝光关系来自 metadata，不按直方图拉齐。
原始 camera multipliers 的差异不改变冻结的 reference WB。相对 reference 最大
支持 ±16 EV，ISO／aperture／focal length 不用 gain 偷偷补偿。

```cpp
import photo_ops.hdr;
auto result = photo_ops::merge_hdr(paths, "radiance.tif", options);
```

另一个同名 overload 接受 `const raw_sequence_context&` 和有序的 context indices，
用于显式 capture groups 复用**一个**全局 RAW context。该 overload 保留 context 的
WB 与 reference exposure scale；HDR 几何 reference 可为组内另一帧，不能因此
重建颜色 context。没有公开 `exposure_align` / `radiance_fuse` 等摄影阶段。

## 饱和、暗部与融合

可靠性来自 pre-WB sensor support，而不是 developed Rec.2020 是否超过 1。
每像素的 minimum／maximum 覆盖 Bayer demosaic 的完整 7×7 footprint；任何
参与插值的 CFA channel 接近饱和，都会降低**整个 RGB measurement**的权重，
避免只替换一个色彩通道造成 hue 偏移。

第一版使用可检查的工程噪声近似：

```text
L = min_channel(white-black)
g = reference_shutter / frame_shutter
shadow_quality    = clamp((sensor_min*L - 2)/14, 0, 1)
highlight_quality = clamp((0.995-sensor_max)/0.095, 0, 1)
quality           = min(shadow_quality, highlight_quality)
variance          = g² * (16/L² + max(sensor_max,0)/L)
weight            = quality / variance
HDR RGB           = sum(weight * exposure_normalized_RGB) / sum(weight)
```

这倾向采用最长且可靠的曝光改善阴影，并从短曝光恢复高光。`4 counts` read-noise
和 signal-dependent variance 是**未经过相机标定的启发式**，不是 camera-specific
SNR 模型、统计置信概率或最优估计承诺。没有坏点表／完整传感器噪声校准；极端坏点、
读出非线性、flare、快门误差等仍可能污染结果，需由真实 bracket 驱动后续改进。

若所有权重为 0，选择最长未饱和 measurement；若全部饱和，选择最短曝光并标为
unrecoverable clipping。保留该 measurement 的 signed／HDR 数值，不把未知高光
“补出”细节，不把低于 black 的负值强制设为 0。

## 配准与 motion / deghost

复用 focus-stack 的四参数 similarity：SIFT／mutual matching／固定 RNG RANSAC
初始化，再做高分辨率 direct refinement。HDR proxy 使用共同尺度上的
`log1p(16*max(Y,0))`，不让曝光倍数本身成为配准目标。证据不足明确失败；
不默认加入 homography、local mesh 或 optical flow。

coarse motion analysis 先以可靠的 reference 为 anchor；reference 不可靠处可用其他
可靠曝光建立比较 anchor。只在有可靠信号的区域比较 exposure-normalized RGB，
结合相对差与传播过颜色矩阵的工程 noise guard。饱和 measurement 还提供 pre-WB
信号下界：若另一个可靠 measurement 显著低于该下界，属于 temporal inconsistency，
不能因为 reference 已 clipped 就把这类运动忽略。

候选 mask 膨胀两格 proxy，划分 connected components；逐曝光统计 component 的
可靠覆盖与质量，选**同一曝光来源**覆盖整片运动区域，reference 在相同质量时优先。
最终复制该 source 的配准 RGB；不能跨时间逐像素挑“最清晰／最亮”而拼出双影。
若最佳来源也 clipped／低信号，相关 maps 如实反映。proxy 可能漏掉很细的运动、
相似颜色／光照变化；motion map 是保守启发式，不是光流或语义分割。

## 有界内存与原子输出

一次只 decode 一帧 RGB + sensor support，写 private measurement spool 后释放。
coarse／fine proxies、component maps 与 tile workspace 都有 preflight reserve，
不常驻 N 张 full-resolution RGB。

```text
P = full-resolution pixels; Q = coarse proxy pixels; R = refinement pixels
T = tile_size²; N = exposures; usable = 0.8*budget
RAW decode  = max_frame(raw_decode_reserve + 8*P)
registration= max(512*Q,128*R) + 128 MiB
analysis    = 192*Q + 128 MiB
fusion      = 192*Q + (192+4*N)*T + 128 MiB
peak report = max(stages)/0.8
```

tile 不需要 pyramid halo；HDR 运动标签在全域 proxy 上先确定，pixel fusion 本身
局部独立。每个 tile 只读取一张 source ROI／warp，同时保存 `4*N*T` bytes 的 weight
maps 供 contribution 统计；RGB 不随 N×P 常驻。预算不足缩 proxy／tile，单帧 decode
无法容纳则失败。该模型是保守工程估计，仍不是 OS RSS 硬限制。

融合写 separate RGB／scalar spools，随后流式编码；scratch 空间估计包含 private
spools、照片和可选地图的发布峰值。短写／空间耗尽为 typed I/O error。完整 TIFF
atomic no-replace 发布后，才输出 diagnostics；`report.json` 最后发布。诊断失败返回
完整照片已发布的路径。各文件独立 atomic，不是多文件事务或断电持久性保证。

## Diagnostics

- `confidence.tif`：full-resolution float32 numeric quality。静态融合为可用 source 的
  最大 quality；deghost 为所选 source 的 quality；全无可靠测量时为 0。
- `deghost.tif`：full-resolution coherent fallback 区域，0／1。
- `clipping.tif`：该处没有可恢复未饱和 measurement，或 deghost 所选 source clipped。
- `low-snr.tif`：所有覆盖测量的 minimum signal 都低于 floor，或所选 deghost
  source 的 minimum signal 低于 16 counts。不是经过标定的 SNR 数字。
- `deghost-source.tif`：proxy-resolution source **index**；仅在 deghost 区域有选择意义，
  未覆盖边缘仍可回退 reference。它不是普通融合的逐像素唯一来源。
- `report.json`：共享 RAW calibration／lens／focal metadata、每帧 shutter、gain、EV、
  真实 weighted contribution fraction、sensor saturation、alignment、最终数值范围、
  clipping／low-signal／deghost 比例与 stage profiling／scratch／输出字节。

numeric maps 无 ICC／transfer function，携带显式 semantic + context fingerprint，
使用 `read_scalar_tiff` 读取；不能当 sRGB 图像 inverse-transfer 一次。`inspect`
可查看 working TIFF 与 scalar map，`convert` 不会隐式把 HDR 做 tone map 后写 JPEG。

计时使用 steady clock。decode 包含 proxy／spool，total 不含最后 JSON 和清理；
spool payload 读写计数不含小型 manifest／公共 TIFF 编码开销，实际文件峰值另计。

## 数值回归与验收边界

独立 DNG sensor simulator 从已知辐射量生成 -2／0／+2 EV，先按 shutter 采样，
再加入 count noise、量化和 sensor clipping；不使用 HDR 实现生成“期望答案”。
其中一组 shadow MAE 为 `2.13228e-5`（最短曝光 `4.46612e-4`）；highlight MAE 为
`5.50473e-4`（clipped middle `0.600316`）。另覆盖单 channel clipping 的三种 camera
颜色、signed／>1 输出、全部 clipped、caller 顺序、不同全局 context exposure 单位、
原子输出／诊断失败／预算、整数与 fractional-warp tile 等价性。

移动目标分别覆盖正常亮度／reference clipped、step=1／4 proxy。新增 clipped-motion
regression 先复现了忽略饱和 reference 的漏检，再加入 sensor lower-bound 检查；
整片目标保留同一可靠曝光。高信息 AEB geometry fixture 的 breathing／fractional
translation corner error 约 `0.00037–0.00150 full-res px`，是受控 synthetic 测量，
不能当作所有实拍图的精度承诺。

数值参考记录见 [hdr-baseline-v170.json](../tests/data/hdr-baseline-v170.json)。

**真实自摄 R8 AEB 和风景画质仍待素材**。R8 RAW／CRAW 单张及相邻同曝光 Canon
帧只能验证 decode／context，不能替代 radiometric AEB acceptance。

参考原则：[Debevec／Malik 1997](https://www.pauldebevec.com/Research/HDR/debevec-siggraph97.pdf)
讨论 radiance reconstruction 与曝光／饱和；[HDR+ 2016](https://www.hdrplusdata.org/hdrplus.pdf)
展示 noise-aware burst merge／reference robustness 的意义。本实现是窄范围、可检查的
RAW AEB baseline，没有复现这些论文的完整系统或引入其显示映射流程。

验证配置：release / EXR 关闭的完整 C++ 回归通过；clean debug + EXR + ASan/UBSan
构建后，HDR、RAW、focus 三组回归与全部 24 项 Python 检查通过（1 项可选 sunlight
实拍检查跳过），无 sanitizer 错误。原有 defocus 全套本轮在 release 中验证。

v1.8 分开明亮 clipping 与低信号：全部饱和不自动等于 low-SNR。增加 bright-clipped／
dark-below-black 回归；该诊断修复不改变 HDR RGB／weight。HDR 现由 matrix 在同一
context 下复用，见 [focus × exposure](focus-exposure.md)。
