# Defocus / occlusion boundary：v1.3 实现与验收

目标是在前景 bokeh 跨越真实遮挡边界时，保住清晰、不透明的前景几何，并限制被污染
背景帧的贡献。它位于 `stack_focus` 内部，默认自动运行，没有新的摄影 API 或 local warp。

## 从失败 fixture 开始

`tests/defocus.cpp` 以独立的 Gaussian image-formation renderer 构造：

```text
near-focused = sharp foreground + defocused background outside its sharp alpha
far-focused  = defocused foreground premultiplication + (1 - defocused alpha) * sharp background
ideal        = sharp foreground + sharp background outside sharp alpha
```

覆盖细枝对天空、栏杆对纹理远景、岩石边缘对山景式背景；sigma=2 / 4 / 7，385×257
奇数尺寸。原有 v1.2 在大 defocus 时会出现透明边、halo，并将部分静态 spread
误判为 motion。数值基线固定在 `tests/data/defocus-baseline-v120.json`，不提交图片。

## 保守的 shared-edge / blur-model evidence

在 regularized focus labels 的 transition 邻域内，逐 source/reference pair 检查：

1. 两帧是否包含方向一致的结构边缘：比较局部平均梯度的方向相关与强度比例，
   且候选 owner 必须在边缘邻域得到 regularized source labels 支持。
2. 较锐 source 经三个 box passes 的 PSF 近似后，能否更好解释另一帧；只比较
   radius=1 / 2 / 4 / 8 proxy pixels 的有限模型，拟合须明显优于原始差异。
3. 更低频颜色是否一致，避免把新增物体或曝光变化解释为 defocus。
4. 只有上述 evidence 与 focus-label transition 同时成立才扩展 boundary band，
   扩展宽度由拟合半径限定；每个像素仍须有局部 model improvement。
5. 多个合法 band 重叠时，以已有 coherent focus prior 与相对 focus evidence
   解决 source 竞争，避免按输入次序让第一帧占据整片远景。

不假定输入按 near→far 排列。默认由哪帧提供较锐结构边缘与可解释的 spread
推断局部 source preference；这不是完整、标定过的 depth reconstruction。

motion 判定仍逐 pair 保留原有低频 RGB evidence。某 pair 只能解释自己的 temporal
差异；第三帧的真实运动仍能 veto。最后 dilate motion，并在完整区域精确使用
reference。缺少重叠覆盖也使用 reference；不为降低 residual 扭曲图像几何。

## Full-resolution fusion

全局 boundary mask / source map 约束每个 tile 的软权重。在有较强 evidence 的
boundary 内恢复一个 coherent source，降低 multiband 重建把 bokeh 带回前景的风险。
较粗 proxy 的 alpha 通过固定全局像素中心插值，且不跨不同 boundary owners 混用
mask；避免块状 proxy 边缘。tile+halo 依赖半径不增加，boundary analysis 在全局完成。

额外 resident maps 只随 Q 增长，source RGB 仍按帧读取。预算将 global analysis
reserve 从 128Q 提到 256Q；aligned proxy / per-pair motion spool 另计 16NQ bytes，
总 coarse scratch 为 36NQ，加原有 4NR fine proxy。没有 N×P 的常驻 RGB volume。

## 可检查结果

- `defocus-boundary.tif`：实际启用的 soft guard，含 motion/uncovered veto。
- `boundary-source.tif`：coherent boundary source，线性灰度为 `index/(N-1)`；只有
  guard 非零处才有意义，与普通 coarse `source-map.tif` 含义不同。
- `motion.tif`：仍需 reference fallback 的区域，不把 defocus 与 motion 合成一张图。
- `defocus_boundary_fraction`：proxy guard >.05 的比例，不是 occlusion 的校准概率。
- `before.jpg` / `after.jpg`：sRGB preview，用于观察透明边与清晰度取舍。

## 已知限制

这是有限模型的 conservative heuristic，不是 PSF inversion 或任意遮挡的完美复原。
在 bokeh 污染处选择清晰前景帧，可能保留一小圈较软背景；宁可保住几何，也不凭空
合成不可见纹理。dense detail 中多个 band 可以覆盖较大面积，需要结合 source map
检查；没有证据时沿用普通融合，强运动／无覆盖处使用 reference。

shared-edge motion、复杂 parallax、非常大的 bokeh、细于 proxy 的前景和多层遮挡
仍有适用边界。synthetic tests 不代表真实风景、R8 枝叶/天空或微距全部通过。
真实 R8 专项采集仍遵循 [data_prep](data_prep.md)，不能把公开单张当作完整验收。

## v1.3 验证记录（2026-10-25）

线性红通道 boundary MAE（旧 v1.2 → v1.3）：

| 场景 | sigma=2 | sigma=4 | sigma=7 |
| --- | --- | --- | --- |
| 枝条 | .00532 → .00476 | .07410 → .00604 | .11160 → .00732 |
| 栏杆 | .01739 → .01727 | .06860 → .02069 | .11323 → .02571 |
| 岩石 | .01856 → .01841 | .03085 → .01648 | .07922 → .01664 |

九个静态案例均未再触发 motion fallback。另覆盖 exposure×.3、彩色前景／背景、
亮前景、near/far 输入反转、proxy step=4、tile/整域一致性、3 px 细枝运动，以及
第三帧运动叠加静态 defocus 的 veto。3 px moving branch 的 defocus guard 为零。

独立三焦平面 fixture 检查 overlapping bands；更强背景纹理与三个相邻相位中，
远景区域 MAE 约 .0197–.0231，中间焦平面为 .0724–.0770。来源支持与 focus prior
防止仅满足 blur fit 的中间焦平面占据整片远景；混合区域仍有保守的 reference softness。负对照禁用 owner-label 支持后，
该回归在 MAE .0638 处失败；源码恢复后完整回归通过。
release C++ 回归和全部
12 项 Python tests 通过（1 项 opt-in sunlight 跳过）。

七帧真实 PCB：wall 55.25 s、peak child RSS 161512 KiB、预算估计 325058560 bytes，
motion candidate 约 3.57%、guard 约 78.87%。该 dense-detail 场景的 band 大量重叠；
视觉核对与局部 focus evidence 发现并修复了最初按 pair 次序选择来源导致的远景
变软。当前针脚／复杂遮挡仍有局部 ghosting，不能声称 macro 或真实风景全部解决。
运行同时有 build/test 负载，不据此与旧耗时作独占性能比较；下一步单独 profile。
