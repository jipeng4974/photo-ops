# v0.2 RAW 工作色彩与序列契约

本文约束 `photo_ops.focus_stack` 的 RAW bracket 接入。
v1.2 已接入支持范围内的 Bayer CR3 / DNG float 路径与 immutable context。
`decode_raw` / `convert` / `sunlight-enhance` 仍保留单帧 LibRaw 16-bit linear-sRGB
兼容路线；它们与新的 `decode_raw_frame(path, context, index)` 明确分开。

## v1.2 实现与边界

- `rgb_f32` 只表示存储，`working_image` 必须携带 `color_encoding`：primaries、
  D65、linear transfer、scene/rendered origin 与 normalization provenance。
  旧 `linear_rgb` 的 sRGB 语义没有变化；focus-stack 内部使用前者加显式 encoding。
- metadata 预检需要顺序 `open_file` + `unpack`，因为实测 Canon R5 II 的 black
  在 unpack 后才从 0 变成 2048；每次释放 mosaic，绝不同时持有整组像素。
  unpack 前先按 raw geometry / 文件大小验证保守预算，受检后才允许解包。
- 首版只支持 square-pixel、regular 2×2 RGB Bayer、integer sensor mosaic、
  scalar + per-channel black、三通道 daylight characterization。X-Trans、空间
  black 模型、已应用 WB、四通道颜色矩阵等明确拒绝，不退回 16-bit development。
- 开发直接读取 LibRaw **未调用 `dcraw_process()` 的 raw sensor samples**，float
  扣 black，以共享 `(white-black)` 分母归一，应用归一到 G1 的共享 WB 一次。
  采用 native float directional green interpolation，再插值 R/G、B/G 色差。
  这是为避免 integer demosaic 的裁剪而实现的窄 Bayer adapter；不宣称等同于 AHD
  或已解决所有 demosaic zipper / false color。边界 mirror 保持 CFA phase。
- 颜色使用 LibRaw `rgb_cam`（WB-balanced camera→linear sRGB）与 sRGB→XYZ
  矩阵组合成 camera→XYZ/D65，再转 Rec.2020。`cam_xyz` 虽然字段名易误解，实际为
  DNG 风格反方向 matrix，不能直接当 camera→XYZ。采用固定 daylight 表征与
  sensor diagonal WB，当前没有双 illuminant profile 插值或额外重复 CAT/WB。
  参见 [LibRaw 维护者说明](https://www.libraw.org/node/2099) 与
  [矩阵构造源码](https://github.com/LibRaw/LibRaw/blob/0.22.0/src/utils/utils_dcraw.cpp)。
- context 固定 reference、WB、black/white、geometry/CFA/orientation、exposure、
  camera characterization、decoder/算法版本与有序输入身份。ISO、shutter、aperture
  只容许 1e-6 相对 metadata 浮点误差；black/white/矩阵严格匹配。WB metadata 差异
  可报告，开发一律用共享 reference gains。支持 API 显式 WB；CLI 默认 camera WB。
- context fingerprint 和逐输入身份使用明确标记的 FNV-1a 64-bit 内容摘要，供本次
  private spool 的 provenance / 变化检测，**不是加密摘要或跨运行 cache**；fixture
  manifest 仍使用 SHA-256。unpack 前后校验输入身份，不复用过期 scratch。
- RAW 输出为 **float32 linear Rec.2020 ICC TIFF**，保留负值和 >1 headroom；
  `write_working_tiff_rows` / `read_working_tiff` 要求编码和 provenance，并校验 ICC
  与声明的 primaries / linear transfer 一致。旧 reader/writer 拒绝误解释这些样本。
  没有把 Rec.2020 送进旧 EXR writer；新的 wide-gamut EXR 输出仍未加入。
- TIFF/JPEG 输入仍为 rendered linear-sRGB，输出沿用 16-bit sRGB ICC TIFF。
  混合 RAW/rendered、混用 camera/mode/exposure 均失败。diagnostics 包括冻结上下文、
  每帧原始 WB metadata、sensor saturation fraction、signed range 与负值比例；
  before/after JPEG 只是显式转到 sRGB 并裁剪的 preview，不回流 fusion。

以下保留完整设计契约；当前仅实现上述明确范围，真实自摄 R8 brackets 的验收仍待
[data preparation](data_prep.md) 所述拍摄素材。已下载的 R8 RAW/CRAW 是 CC0 单张，
可验证 decoder 与重复输入性能，不能替代 focus bracket / 风景遮挡验收。

## 工作色彩不再由三个 float 的布局隐含决定

长期 RAW 路线固定为：

```text
sensor samples + camera calibration
  → black subtraction / shared normalization / shared white balance
  → demosaic to camera-linear float
  → explicit camera characterization → XYZ
  → float32 linear Rec.2020 / D65 working RGB
  → registration / focus / fusion
  → explicit output color conversion and rendering
```

Rec.2020 primaries / D65 是第一条 wide-gamut working 路线，不是 PQ、HLG 或
显示 tone curve。它也不覆盖所有相机颜色：保留有符号、超过 1 的 float，
禁止在 working 转换和 fusion 中 clip 或做 gamut mapping。单位是以序列共享
参考白标定的相对信号，不宣称绝对辐射度。相机表征包含所用矩阵／profile、
校准 illuminant、白平衡与色适应策略及其版本；白平衡不能在 profile 中重复应用。
先验证 decoder 的 camera-linear float 输出是否保留负值／高光、通道排列、
黑电平扣除阶段和隐含缩放，再决定调用哪层 LibRaw API；不能把
`output_color` 换一个整数、或把已裁剪的 16-bit bitmap 转 float 就称为完成。
若当前 decoder 无法给出可验证路径，返回 unavailable，不静默退回 16-bit。

当前 `linear_rgb` **明确仍指 linear-sRGB / D65**。它和 `decoded_image` 没有
跨版本稳定 ABI 承诺；不能把 Rec.2020 样本直接塞入同一个类型并继续调用现有
Rec.709 luminance / sRGB↔OKLab 函数。迁移时使用 project-owned `rgb_f32`
存储加必需的 `color_encoding`（primaries、white point、transfer、scene/rendered
来源、normalization provenance），组成 `working_image` / 对应借用 view。
`image<Pixel>` 继续只负责布局与所有权。wide-gamut focus_stack 公共接口必须携带
这个 encoding 或约束到明确的 working 类型；不允许裸 RGB 猜色彩空间。
这些类型已在 v1.2 落地；旧 linear-sRGB API 继续拒绝误标 Rec.2020，focus-stack 通过
带 encoding 的 working_image 路径处理 wide-gamut。

- 亮度使用该 working RGB→XYZ 矩阵的 Y 行，不能复用 Rec.709 系数。
- OKLab 经显式 working→XYZ D65→OKLab 转换；不得先裁到 sRGB。
- TIFF/JPEG rendered 输入保留来源标记，逆 transfer/ICC 不能恢复场景辐射度。
- TIFF/JPEG 输出显式转到目标 ICC，之后才选择 gamut mapping、tone curve 与量化。
  旧 writer 的 sRGB/clipping 行为保留并说明；wide-gamut 输入需新接口，拒绝误标。
- EXR 写实际 working chromaticities；cache、JSON 与输出必须记录 encoding。
  预览可用 sRGB，但不把预览结果回流到 fusion。

## raw_sequence_context：先扫描，再冻结，再开发

序列处理分两遍：先只收集 metadata 并检查兼容性，再以不可变 context 开发每帧。
reference 默认是输入顺序中间帧；之后的 registration reference 选择不能隐式
重算颜色上下文。显式更换颜色 reference 需要重建 context 和缓存。

| context 内容 | 冻结规则 |
| --- | --- |
| 身份与几何 | 相机型号／sensor mode、CFA、raw/active area、crop/orientation、bit depth；不同模式拒绝混用 |
| WB | 从明确 reference 的有效 camera multipliers 读取，归一到 green；也允许显式用户 multipliers；所有帧复用，禁用逐帧 camera/auto WB |
| black | 固定每通道 black、所需 spatial black 模型与校准策略；确认 decoder 扣除顺序，禁止重复扣除；metadata 不兼容则拒绝或要求明确重校准 |
| normalization | 固定 reference 的 white/saturation levels 与 `white-black` 分母、共享曝光 gain；禁止按每帧观测最大值、直方图或 auto-brightness 归一化 |
| color | camera characterization、适应策略、linear Rec.2020/D65 encoding 及版本 |
| processing | demosaic、坏点／降噪选项、orientation、decoder 版本、half/full-res 策略 |
| provenance | 有序输入身份、reference、所有冻结值、检查结果与稳定 context fingerprint |

black 不是“所有相机永远用一个常数”。若 sensor 需要逐行／光学黑校正，必须
定义共享模型及允许的测量参数；初版不支持的模型应拒绝。元数据中的逐帧 black、
WB 和 white 都记录为原始证据，不再隐式覆盖 context。WB 的 EXIF 差异可以仅报告，
实际开发仍使用共享 multipliers；无有效 reference WB 时需用户指定，不能猜。

默认 fixed policy 要求曝光时间、ISO、标称光圈一致（有理数等价和已声明的 metadata
精度容差除外），不按 metadata 微小误差补 gain。v1.6 提供显式 shutter_bracket
RAW engineering policy，仅允许 shutter 改变；共享 reference shutter / frame shutter
归一到同一信号尺度，ISO／aperture 仍须一致。不能按图像亮度自行拉齐。black/white 的容差必须来自 decoder
metadata 语义与 fixture 验证，在报告中列明；初版没有已验证容差时用严格匹配。
对每帧额外报告 raw 饱和比例、负值、normalization 后范围与有效区域。
不因某一帧饱和而改变全组的 scaling。

未来接口的职责是 `prepare_raw_sequence(metadata, options) → result<context>`，
再由 `decode_raw_frame(path, const context&)` 使用完全一致的开发参数。
context 只拥有小型校准数据和清单，**不持有整个 bracket 的像素**。
metadata 错误与不兼容返回 typed error；不应触发 contracts。

## 实现顺序与验收门槛

1. 给 camera-linear float adapter 建立数值 fixture，确认 black、WB、scaling 的
   实际执行阶段；包括负值、超过 1、饱和、非 sRGB gamut 的样本。
2. 实现有 encoding 的 working image 和 Rec.2020↔XYZ 变换；测试白点、矩阵
   往返、亮度与 OKLab，证明没有隐式裁剪或双重色适应。
3. 实现 context metadata preflight 与 decode，测试帧间 WB metadata 扰动不改变
   共享开发参数；black/曝光/相机模式不兼容被拒绝。
4. proxy 与 full-res 必须使用同一 context fingerprint；cache key 包括输入身份、
   decoder/算法版本、encoding 与参数。过期缓存拒绝，不仅按文件名复用。
5. 再接 focus-stack 的真实序列；当前 `convert` / `sunlight-enhance` 的旧路径
   可以继续用，但不得冒充新路线已经实现。

## v1.2 验证记录（2026-10-25）

- debug + EXR + ASan/UBSan clean build 与全部 C++ 回归通过；release / EXR 关闭
  clean build、C++ 回归及 opt-in 真实 RAW 回归通过；Python 12 项通过（1 项 opt-in
  sunlight 跳过）。module consumer 检查同步新增 raw 依赖，继续验证内部 registration
  helpers 不 export 和 expected 的 discard 诊断。
- generated Bayer DNG：共享 WB 使不同 camera-WB metadata 的相同 sensor samples
  得到完全相同像素；black/white、camera/color/exposure mismatch、篡改输入与错误
  context index 拒绝；3/6/8 orientation 与 native 图逐像素一致。neutral reference
  等于已知 `(304-64)/(1023-64)`，避免重复 WB / black / normalization。
- Rec.2020 白点、signed/HDR XYZ 与 OKLab 往返、float32 TIFF 精确往返、ICC
  声明矛盾、stream callback failure 和 no-overwrite 回归通过。
- 真实 R8 RAW / CRAW 单张的 opt-in 解码通过；真实 Canon R5 II 三个相邻 focus
  frames 的全局色度一致性检查通过。这些测试不等同于用户自摄 R8 bracket 验收。
- 三帧 5118×3412 Canon CR3 完成 full-resolution 合成：wall 485.63 s、peak child
  RSS 523708 KiB，预算估计 1307332692 bytes，scratch estimate 1037332632 bytes。
  最终 float TIFF 范围约 `[-0.00483, 1.47979]`，未做 gamut clipping。运行有同机
  build/test 负载，属于 correctness / resource 基线，非独占性能比较。
- 该三帧是很长 macro stack 的连续前缀；目视检查只有极小焦平面片段，不能声称
  已恢复整个场景景深。性能、defocus boundary 与风景素材验收继续分别推进。

## v1.6：shutter-only context 与 sensor support

为 `todo2` 的 HDR 实现增加两项 RAW 工程契约；本次尚未新增 HDR photographic operator。
普通 `stack_focus` 仍选择 `raw_exposure_policy::fixed`，不同曝光继续拒绝。

- `raw_sequence_options.exposure_policy = shutter_bracket` 显式选择 AEB；reference
  的 WB、black／white、camera matrix、geometry／CFA 仍共享不变，ISO／aperture
  不一致继续失败。每帧按 `reference.shutter / frame.shutter` 缩放 float 工作 RGB，
  reference 为 1；最大允许距 reference ±16 EV，超限／非法数值明确报错。
- exposure policy 和有序 metadata 被加入 v2 context fingerprint。存储相同 signed／
  HDR 语义；scope 是相对 reference exposure 的信号，不是绝对辐射度或自动 tone map。
- `decode_raw_frame(..., raw_decode_options{.sensor_support=true})` 可返回与 RGB 同向的
  `image<raw_sensor_range>`。minimum／maximum 是 **pre-WB** 的
  `(raw-black)/(white-black)` 在完整 demosaic 7×7 support 中的极值。因插值的色差
  依赖相邻 green，而 green 又依赖 ±2 samples，不能只看中心像素或 3×3。
- 饱和判断不从 Rec.2020 通道是否大于 1 反推；WB／矩阵后的 HDR 数值与 sensor
  clipping 是两件事。support 也提供暗部 signal floor 的保守输入，但不是校准 SNR。
- support 是 opt-in、8 bytes/output-pixel。两个 separable min/max pass 使用有界
  全帧临时 buffer，并在 RGB 输出分配前依次释放；相对原单帧 decode reserve 增加
  `8*output_pixels`。该成本在 unpack 前用 immutable context 的 budget 检查。
  仍只解码一帧，不持有 N 张 full-resolution 图。

release / EXR 关闭 clean build、完整 C++ 与 22 项 Python 回归（1 项可选 sunlight
实拍检查跳过）通过。新增 deterministic DNG 检查 gain、signed/HDR、pre-WB extrema、
7×7 saturation 扩张、orientation、policy identity、ISO／aperture／极端 shutter 拒绝，
以及 fixed policy 仍拒绝真实不同 shutter，容许的微小 metadata 差异不触发 gain。
真实 R8 AEB 质量验收仍待对应 bracket；已有单张不能替代。

真实 R8 RAW 与 CRAW 单张的 full-resolution sensor-support 检查也已 opt-in 通过，
输出尺寸与 RGB 一致、采样极值有限且有序；Canon 相邻三帧颜色／context 和原有
完整 C++ 回归一并通过。这仍不代表 AEB radiometric merge 的真实素材验收。
