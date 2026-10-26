# focus-stack：实现、边界与内存预算

v0.3 实现单一 named module **`photo_ops.focus_stack`**：v1.7 起，
配准 adapter 由本模块和 HDR 两个实际 consumer 共享为内部 named module
`photo_ops.detail.registration`；没有公开额外摄影阶段或独立 focus operation。
public 摄影入口为 `stack_focus(paths, output, options)`；similarity 数值通过
report 检查，registration helper 不 export。v1.0 移除原先公开的
`align_focus_stack`，调用方迁移到 `stack_focus` 的返回 report。

## 当前可运行范围

```bash
xmake run photo-ops focus-stack near.tif middle.tif far.tif -o stacked.tif \
  --memory-budget-mib 2048 --scratch-directory /tmp \
  --diagnostics build/focus-diagnostics
```

输入为**同尺寸、已一致开发**的 RGB TIFF/JPEG（沿用现有 reader 的布局与 ICC 限制），
输出为流式 16-bit、sRGB ICC、Deflate TIFF。`--reference INDEX` 为从 0 开始的输入
索引，默认 `N/2`；顺序由调用者指定，不根据文件名重新排序。已经配准的序列可以
显式 `--aligned` 跳过几何估计；默认不会在配准失败时悄悄假设 identity。

v1.2 新增共享 `raw_sequence_context` 的 Bayer CR3 / DNG 路径；先顺序 unpack 取得
完整校准、冻结 context，再顺序 float develop。RAW 输出为 **float32 linear Rec.2020 /
D65 ICC TIFF**，保留 signed/HDR；rendered 输入仍输出 16-bit sRGB TIFF。内部使用
`rgb_f32` + 必需的 `color_encoding`，Y 按 working primaries 计算，不能将 Rec.2020
误传给旧 `linear_rgb` API。完整 sensor / calibration 支持范围见 [RAW 契约](raw-pipeline.md)。

```bash
xmake run photo-ops focus-stack near.CR3 middle.CR3 far.CR3 -o stack-linear.tif \
  --memory-budget-mib 2048 --diagnostics build/raw-stack
```

RAW 与 rendered 不能混合，曝光／black／camera mode 不兼容整组拒绝。reference 默认
中间帧，颜色 context 一旦冻结不再由 registration 重算。`--aligned` 只跳过 registration，
不跳过 metadata/context 校验。streaming JPEG/EXR 最终输出仍未加入。

## v1.3 Defocus / occlusion boundary

在 coherent focus-label transition 附近，以 shared-edge 梯度方向、受限 blur-model
fit 与低频颜色一致性区分静态 defocus spread 和 motion。边界使用一个可解释的
coherent source；多 source 重叠时保留已有 focus prior，真实 motion 仍 veto 并
精确回退 reference。新增 `defocus-boundary.tif` / `boundary-source.tif` 与 boundary
fraction。模型、独立 fixture 和限制见 [defocus-boundaries](defocus-boundaries.md)。

## v1.1 精配准

SIFT/RANSAC 保留为 coarse initialization。顺序解码时另外写入更高分辨率的
亮度 proxy，步长初始为 `max(1, coarse_step/2)`，根据预算可增加；解码结束并释放
全尺寸图像后，再逐帧执行四参数 similarity direct refinement。三档 Gaussian
尺度（3 / 1.5 / 0.75 proxy px）与 Huber loss 抑制局部失焦／遮挡的不一致，固定
迭代上限、line search 与 correction bound 防止离开 coarse basin；不引入 affine
shear、homography 或 local warp。

报告保留 `residual_px`（coarse feature RMS，full-res 单位），另列
`dense_residual_px`、`dense_correlation`、`refinement_samples`、`refined` 和
`refinement_step`。dense residual 为 robust photometric error / gradient energy
换算的 full-resolution-equivalent 位移量，**不是已知真值的二维坐标误差**；失焦、
噪声和颜色变化也会使它增大。目标 0.3–0.7 px 是评估量级，不作为所有真实场景的
永久硬门槛。证据不足、病态 Hessian、低相关或过大修正明确失败。

box proxy 使用中心坐标共轭；最外侧不足一块的 proxy 不用于 direct refinement。
同一 synthetic Gaussian-spot 场景中，正向 scale=1.015 / rotation=.015 /
translation=(3,-2) 与反向 scale=.997 / rotation=-.002 / translation=(.31,-.27)
均要求全图采样点几何误差 <0.15 full-res px，并检查重复运行确定性。
真实 PCB 与更多失焦／风景案例的 dense residual 应独立解读，不能由 synthetic
测试推断所有真实图像都达到 0.15 px。

## 已实现的处理过程

1. rendered 先读 header；RAW 先预算检查再顺序 unpack 校准。检查 2..256 帧、
   同尺寸、32..65536 每边、最多 200 MP；
   输入文件上限 1 GiB，JPEG header 路径额外限定 256 MiB。预算和 scratch 预检后
   顺序解码，一次仅持有一张全尺寸 float32 RGB。输入在运行中需保持不变。
2. 写入 private run directory 的原生 RGB spool，同时生成 box-average RGB/Y
   proxies；本轮不做跨运行 cache/reuse，不依赖未完成缓存的 fingerprint。
3. OpenCV 5 SIFT（2000 features）、0.75 ratio + mutual matches、固定 RNG 的
   RANSAC similarity。至少 8 个 inliers、35% consensus，proxy RMS <=1.5 px，
   scale 在 `[0.9,1.1]`、rotation <=0.1 rad、translation norm <= proxy 长边的 25%。
   reference/完全相同 proxy 使用 identity；其他证据不足的帧整组失败并报告路径。
   这些是 conservative MVP 限制，homography/local flow 未实现。
4. 相似变换 source→reference 通过 box-proxy 的像素中心共轭到 full-res；alignment
   report 的 translation/residual 均为 full-res pixels。最后不足一个 box 的边缘块
   只是近似同一 sampling center；不把这种边缘证据当作高精度标定。
5. 在 reference proxy 全域计算平滑的 Laplacian/gradient energy，按帧写 score spool；
   三次同步 Potts regularization 后形成 coherent labels，confidence 是 best/second
   score 的相对差，不是校准概率。没有 N×P score volume 或逐像素 argmax 最终图。
6. 用低频 **RGB** 差异检测候选 motion（`abs(delta_channel) > 0.06 + 0.2*abs(ref)`），
   膨胀两个 proxy pixels；整片用 reference，包括等亮度但不同颜色的运动。
   这是保守启发式，不能识别所有水面／叶片运动，也可能把开发差异当作 motion。
7. 全尺寸 tile+halo 从 source spool 读取有界反投影 ROI。每帧计算 full-res focus
   evidence、平滑权重，并使用 coarse label/confidence 作软 prior（最低仍为 0.2）。
   先顺序扫描得到 base weight 总和；预算允许时缓存当前 tile 的 warped RGB / base
   weight，否则保留 two-pass 重读路径。归一化后建立 4-level Gaussian weights /
   Laplacian RGB，逐帧累加、逐层归一并重建。
8. halo=64、tile origin 对齐 8，真实图像边缘才 replicate，tile 只提交 interior。
   motion 区域最终精确还原 reference；未被所有 source 覆盖的边缘也使用 reference，
   保留 reference 的完整尺寸，**首版不自动 crop**。因此边缘可能不具备全景深。
9. 融合结果先落 disk spool，再用 `write_tiff_rows` 一行一行编码，atomic no-replace
   发布。正常成功／失败清理本次 scratch；SIGKILL 后 private run directory 可残留。

同目录 diagnostics 提供 proxy-resolution `source-map.tif`、`confidence.tif`、
`motion.tif`、sRGB/clipped `before.jpg` / `after.jpg` 及最后发布的 `report.json`。source-map 为 regularized coarse label，
灰度线性值 `index/(N-1)`，**不是 full-res blend 的逐像素唯一来源**。map 使用带
sRGB ICC 的可视 TIFF，读取 scalar 时需逆 transfer。JSON 包括 reference、变换、
match/inlier/residual、source 选择比例、motion/low-confidence 比例及预算估计。
最终照片先完整发布，再生成 diagnostics；`report.json` 最后发布并包含 output 时间。
文件独立 atomic；多文件诊断与最终照片不是整体事务。diagnostic 阶段失败时返回
typed error，明确指出完整照片已发布的位置；不会删除这张成功照片。

## 当前预算模型与限制

`--memory-budget-mib` 默认 2048、最少 512；`--tile-size` 默认 256（允许 32..4096，
向下对齐 8），`--proxy-long-edge` 默认 1024（32..2048）。超预算按阶段缩小 tile 或
proxy（registration 超限不靠缩 tile 解决）；无法容纳单帧 decoder 或最小工作集则失败，不降低输出分辨率。

```text
P = full-resolution pixel count
Q = proxy pixel count
T = (tile_size + 2*64)^2
usable = 0.8 * budget
single_frame_decode_rendered = 24*P + largest_input_file_bytes + 128 MiB
single_frame_decode_RAW = 40*raw_sensor_pixels + input_file_bytes + 256 MiB
R = refinement proxy pixel count
registration = max(512*Q, 128*R) + 128 MiB
streaming_fusion = 256*Q + 512*T + 128 MiB
cached_fusion = 256*Q + max(512, 384+16*N)*T + 128 MiB
coherent_maps_and_fusion = cached_fusion if it fits usable, otherwise streaming_fusion
reported_peak = max(these three stages) / 0.8
scratch_estimate = 12*(N+1)*P + N*(36*Q + 4*R) + output_bytes
output_bytes = 6*P (rendered) or 12*P (RAW)
```

spool、coarse labels 和 metadata 外，不常驻 N 张 full-resolution RGB。缓存限于
当前含 halo 的 tile：`16*N*T` bytes（RGB 12 + base weight 4）；只在 reserve 可容纳时
启用，不通过自动缩小 tile 强求缓存。没有新增并行 worker；实际峰值与缓存容量写入
profile。计时定义与实测见 [performance](performance.md)。
512 bytes/tile-pixel 的保守 reserve 覆盖 source ROI、score/blur、base mask 总和、
当前 source pyramids、所有 level 的 RGB numerator/gray denominator 与重建 buffers；
不是实际分配常量。128 MiB 固定 reserve 覆盖 codec/OpenCV/allocator 工作区的估计，
并非第三方库的严格上界；RSS 还含动态库、线程和 OS page cache。
预算是 preflight 工程估计，**不是 OS RSS 限制或任意相机 RAW 解码保证**。
RAW 使用保守 reserve，需随代表相机／sensor mode 的实测继续校准。scratch 空间检查不是预分配承诺，
磁盘中途写满仍走 typed I/O error；不发布 partial final image。

下面保留原始预算设计中进一步的验收目标。与当前范围有差别时以上实现记录为准；
更多 sensor/空间 black 模型、精细 motion/local warp、streaming EXR 和 45–60 MP 性能标定仍属于后续工作。

## 预算按阶段计算，不同时持有 N 张全尺寸图

令 P 为全尺寸像素数，N 为帧数，Q 为 proxy 像素数，T 为含 halo 的 tile 像素数。
RGB float32 为 12P bytes；完整 Gaussian/Laplacian pyramid 约为原图的 4/3。
例如 60 MP 一帧约 687 MiB，8 帧约 5.36 GiB；仅 RGB pyramids 已约 7.15 GiB，
还没有 focus scores、warp、masks 和 decoder scratch。因此禁止以
`vector<image<...>>` 载入完整 bracket 后再设计 fusion。

已提供 `--memory-budget-mib`（默认 2048 MiB）和 `--scratch-directory`。预算覆盖项目分配、decoder／codec scratch、feature/RANSAC
工作区和并发 worker，不只统计 RGB buffer。预算不是 OS 级 RSS 硬限制；
allocator、库内部开销和文件页缓存可能额外占用，需记录实测 peak RSS 和余量。
第三方 decoder 的未知峰值不能被记作零，也不能保证任意相机都能在 2 GiB 解码。

各阶段分开 preflight，采用：

```text
peak_estimate = max(decode_stage, registration_stage, fusion_stage, output_stage)
decode_stage = decoder_reserve + one_decoded_frame + spool_write_buffer
registration_stage = reference_proxy + current_proxy + features + matcher_scratch
                     + bounded_global_maps
fusion_stage = shared_maps + workers * per_tile_workspace + bounded_tile_cache
               + output_strip_buffer + safety_margin
```

用受检整数运算计算 bytes，溢出和不合理尺寸返回错误。首版单 worker，默认留
至少 20% 预算作为 safety margin；decoder reserve 由代表相机实测并记录，缺失时
要求显式保守 reserve 或先完成测量。先降低 proxy/tile 尺寸和并发，仍不够则在
发布任何结果之前失败；不能静默 swap 或改为低分辨率输出。

## 1. metadata / sequential decode / scratch

建立 immutable `raw_sequence_context` 后，顺序开发一帧、生成 proxy、写 full-res
float scratch、释放 decoder 与全帧 buffer，再处理下一帧。LibRaw 可能仍需
整帧开发；**tiled fusion 不等于 tiled RAW decode**。decode 阶段的单帧峰值若
超过预算必须明确报错，不能声称 tile 大小可解决它。

scratch 使用可按区域读取的 project-owned float32 RGB tiled store，携带尺寸、
encoding、context/input fingerprint 与有效性 metadata。首版允许不压缩以减少
解码工作区；不要误用现有只读 unsigned TIFF adapter 充当 float tile reader。
store 可以用简单索引文件实现，或窄 adapter 接成熟格式，选择以真实 I/O 测量为准。
不使用全文件 mmap 来宣称已控制 resident memory。

磁盘预算至少计入 `12*N*P` bytes 的 RGB spool，加 proxy、maps、tile index 和
临时最终输出；60 MP × 8 仅 RGB 约 5.76 GB（十进制）。检查可用空间与路径，
写失败停止并清理本次临时文件；不要删其他任务的 cache。用 private run directory，
成功或正常失败后按保留策略清理，异常退出后留下可识别的目录供恢复／清理。
缓存是可重建数据，不会以 partial file 标记为 complete。

## 2. registration proxy

最长边初始上限 2048，按预算可再缩小；从同一 working/context 计算线性 Y，
特征检测需要的 contrast normalization 仅用于 proxy，不能影响融合像素。
顺序保留 reference 和当前 proxy，限制 keypoint 数和 matcher 候选，禁止无界
N² dense distance matrix。全组只常驻 transforms、残差摘要和有限分辨率的 maps；
proxy 可放 scratch 按需读，不要求 N 张同时常驻。

`photo_ops.focus_stack` 内部 registration adapter 接 project-owned gray views、proxy↔full-res 坐标映射
和显式 workspace budget，返回 source→reference similarity、inliers、residual、
有效域与 confidence。必须记录 resize/crop/orientation、像素中心约定，并在
full-res 反向采样时正确求逆；不能简单把 proxy 的 translation 原样用于原图。
先实现 translation/similarity + robust matching，真实残差证明有必要才升级 homography。
local mesh/flow 不进入第一版，未来的局部位移场同样必须计入预算。

## 3. 全局 coherent evidence，再 full-res tiled fusion

- 在有限分辨率的统一 reference grid 上做跨帧 focus/motion evidence 和空间
  正则化。按帧扫描或使用 scratch-backed score volume，不持有 N×P float scores。
  coarse labels、confidence 和 motion regions 必须全局一致；motion 区域指定一个
  coherent source，不能在每个 tile 独立挑来源。
- full-res 以 reference 网格分 tile，先把 tile+halo 的边界反投影到各 source，
  加 resampling footprint，再从 spool 取所需 source tiles。过大包围盒分块读，
  不为一个 tile 分配整张 warped source；超出预算就缩 tile 或报错。
- coarse map 作为约束，在 tile+halo 内计算 full-res 多尺度 focus evidence，
  做固定迭代数的局部 refinement，得到软权重。首版可按帧重读 tile，或把局部
  scores/weights 放 scratch；每种方案必须把 N 因子计入预算。不能为省内存退化为
  逐像素 sharpness argmax，也不能未经验证只保留 proxy top-1 来源。
- 跨 source 顺序累加各 pyramid level 的 `weight * Laplacian` 与 weight sum，
  float32 RGB accumulator + float32 denominator；归一后重建。每次仅保留当前
  source 的 warp/score/weight pyramid，不能保留 N 个 RGB pyramids。
  无有效权重时采用全局指定的可靠 source 或标记 invalid，不能除以零。
- tile 内部重建完成才把不含 halo 的 interior 写到 disk-backed 输出。最终 writer
  需支持 strips/tiles，不使用现有必须传入全图 view 的 `write_image` 冒充流式输出。
  diagnostics 同样分块／降采样，禁止最后生成一套 N×P debug maps。

单 worker 的预算表必须逐项列出 warped RGB、focus scratch、weights、所有 pyramid
levels、accumulators、inverse-warp read cache、重建 buffer 和输出 buffer。
粗略例子：含 halo 的 tile 边长 1280，若这些数组合计按保守 128 bytes/pixel
预留，约需 200 MiB；这是规划余量，不是已测得的实现峰值。实际实现按活跃数组
生命周期精算，不能拿这个常数当保证。coarse maps 或 N 增长也要单独 preflight。

## 4. halo、跨 tile 一致性与输出

halo 由完整依赖半径推导：focus stencil、blur/downsample、pyramid expand、mask
平滑、局部 refinement 的迭代传播都要计入。半径 2 的 Gaussian 经 L 次下采样，
输入尺度依赖半径为 `2*(2^L-1)`；Laplacian 重建还有 expand 路径，不能只算一次。
warp 插值 footprint 在 source 侧另算；禁止拍脑袋固定 16/32 px halo。
若某算法需要全局收敛，放到 coarse/global 阶段或改为有界方法，不能靠有限 halo
假装与全图相同。

pyramid 的采样相位以全局原点固定，tile origin 对齐 `2^L`；halo 不能随 tile 改变
downsample 相位。只有真实图像边界使用边界扩展，tile 边界从相邻真实像素取样。
只输出 interior；改变 tile 大小、处理顺序与 worker 数，不应产生可见接缝。
最终 crop 来自全局 transform validity，不按 tile 各自决定。

最终编码先写目标同目录临时文件，完成编码、校验和关闭后用 atomic no-replace
发布（沿用 `photo_ops.output`）；没有完整结果时不出现最终文件。多个 diagnostics
文件各自 atomic，JSON manifest 最后发布作为完成标记；这不是整个目录的事务。
进程被 SIGKILL 时可留下临时文件，但不能留下看似完成的最终图像。

## 5. 按顺序验收

1. RAW float / context 契约通过后，以 synthetic shifts/rotation/scale 检查 registration
   在 proxy/full-res 坐标中的方向、残差与 deterministic behavior，再跑真实静态序列。
2. 用小图 full-frame reference fusion 对照 tiled 版本，覆盖奇数尺寸、极端 warp、
   真边界、跨 tile 深度／motion 边界和多个 tile 大小，检查数值误差、seams/halos。
3. 低预算用更多 tile 仍输出同尺寸、相同允许误差的结果；不足以容纳一个最小 tile
   或单帧 decoder 时提前失败。用 N 加倍测试证明 RGB 常驻内存不随 N×P 增长。
4. 代表 45–60 MP bracket 实测 decode/fusion peak RSS、scratch 峰值与耗时，
   校准预算；disk-full、codec failure、目标冲突测试不得产生 partial final output。
5. 再扩展真实风景的 foliage/water/parallax 回归；宏观 CR3 fixture 不代表风景已通过。

## v0.3 验证记录（2026-10-25）

- debug + EXR + ASan/UBSan clean build 与 C++ 回归通过；release / EXR 关闭的
  clean build 与 C++ 回归通过。两配置的全部 10 项 Python tests 通过。
- synthetic similarity 恢复 scale=1.015、rotation=0.015 rad、translation=(3,-2)，
  并验证 step=2 的 proxy→full-res 像素中心共轭与重复运行稳定性。
- 两个不同焦平面恢复各自细节；tile=32 与整域参考的所有红通道像素差小于
  0.0002（含 193×129 奇数边界）。等亮度但不同颜色的 motion fixture 触发
  reference fallback，静态 flat stack 保持三通道数值。
- 2→4 帧重复输入的预算估计保持不变，scratch 估计增加；100 MP header fixture
  在解码前因预算失败。streaming TIFF callback 中途报错不发布 final，并清理 temp。
- 真实 PCB 七帧（manifest 固定来源，2048×1536）release / 512 MiB 预算：
  wall time 48.75 s，peak child RSS 118492 KiB（约 115.7 MiB），estimated peak
  280 MiB，scratch estimate 332.25 MiB。自动 proxy_step=4、tile=256；
  full-res residual 约 1.91–2.98 px，估计 scale 0.9806–1.0167；motion 候选约 10.1%。
  本机 review artifacts 在 `build/focus-review-v030/`（不提交图片和产物）。

视觉对照可看到 USB 接口与远处散热片的细节共同恢复；针脚和复杂遮挡边界仍有
局部 ghosting/softening，不能据此宣称已经解决 macro occlusion 或风景 motion。
首版 similarity 残差、低频 motion heuristic 和多尺度 mask 都有适用范围；
本轮未增加 homography、local flow 或调参到单张照片，也未做 45–60 MP 风景评测。

## v1.1 验证记录（2026-10-25）

release / EXR 关闭的 clean build、C++ 全套回归通过；Python 12 项通过（其中真实
sunlight opt-in 1 项跳过）。精配准 synthetic 的采样点 full-res 坐标误差 <0.15 px。
七帧 2048×1536 PCB 在 512 MiB 预算下完成，wall 123.40 s、peak child RSS
124844 KiB、scratch estimate 370409472 bytes；运行时同机还执行了 debug 对照与
测试，**该耗时不是独占负载的性能比较**。fine proxy step=2，coarse step=4。
真实 PCB 的 dense gradient-equivalent residual 为 2.42–5.00 full-res px，相关性
.918–.986；其中含强失焦与遮挡，不满足理想静态高信息区域的 .3–.7 px 目标，
不能声称该 macro case 已实现全域 subpixel registration。后续需要按静态清晰区域
分开评估；指标与真实风景/R8 几何质量仍需更多 fixture 验证。

## v1.8 HDR-first matrix

显式 `focus_exposure_matrix` / CLI `--matrix` 在一个 RAW context 下先做 HDR，再复用
本模块的 coherent selection／defocus／fusion，保留 confidence／motion 等中间证据。
新增预算与 private intermediates／numeric maps 见 [矩阵契约](focus-exposure.md)。
flat RAW 继续要求同曝光，working TIFF 仅通过受检内部 HDR plane 路径进入。
