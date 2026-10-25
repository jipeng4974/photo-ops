# focus-stack：有内存预算的实现设计

本文件定义 v0.2 后 `photo_ops.registration` / `photo_ops.focus` 的实现边界，
不是已实现功能。先满足 [RAW 与序列契约](raw-pipeline.md) 的颜色／一致性门槛，
再接 bracket；不为规划先创建空模块。

## 预算按阶段计算，不同时持有 N 张全尺寸图

令 P 为全尺寸像素数，N 为帧数，Q 为 proxy 像素数，T 为含 halo 的 tile 像素数。
RGB float32 为 12P bytes；完整 Gaussian/Laplacian pyramid 约为原图的 4/3。
例如 60 MP 一帧约 687 MiB，8 帧约 5.36 GiB；仅 RGB pyramids 已约 7.15 GiB，
还没有 focus scores、warp、masks 和 decoder scratch。因此禁止以
`vector<image<...>>` 载入完整 bracket 后再设计 fusion。

计划提供 `--memory-budget-mib`（默认目标 2048 MiB）和 `--scratch-directory`。
它们尚未加入 CLI。预算覆盖项目分配、decoder／codec scratch、feature/RANSAC
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

`photo_ops.registration` 接 project-owned gray views、proxy↔full-res 坐标映射
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
