# v0.2 RAW 工作色彩与序列契约

本文约束 `photo_ops.focus_stack` 的 RAW bracket 接入。
目前先实现已统一开发的 rendered TIFF/JPEG 合成，不经过 RAW decoder；这不放宽
以下 RAW float/context 门槛。**wide-gamut float 开发与
`raw_sequence_context` 尚未实现**；当前 `decode_raw` 仍为单帧 LibRaw 16-bit
linear-sRGB 兼容路径，不应作为 focus bracket 的开发接口。

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
`image<Pixel>` 继续只负责布局与所有权。future wide-gamut focus_stack 公共接口必须携带
这个 encoding 或约束到明确的 working 类型；不允许裸 RGB 猜色彩空间。
这些是设计中的类型名；当前 rendered focus_stack 接口显式限制为已有
linear-sRGB/D65 类型，不能塞入 Rec.2020 样本。

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

曝光时间、ISO、标称光圈必须一致（有理数等价和已声明的 metadata 精度容差除外）。
不一致的曝光 bracket 默认拒绝；未来如引入显式 EV 校正，应写入 context 并说明
噪声／饱和差异，不能按图像亮度自行拉齐。black/white 的容差必须来自 decoder
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
