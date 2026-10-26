# HDR / focus × exposure 的复现与检查

工程实现见 [HDR](hdr.md) 与 [矩阵合成](focus-exposure.md)。本页说明如何检查数值、
资源与预览；合成 fixture 与真实 R8 摄影验收分开记录。

## 小图数值与预览

`review-focus-exposure.py` 是开发工具，需要单独 Python 环境中的 NumPy、Pillow、
tifffile、imagecodecs，不进入 native binary 或普通 CLI。先构建 release，使用该环境：

```bash
/path/to/review-env/bin/python scripts/review-focus-exposure.py \
  --output build/hdr-matrix-review --binary build/linux/x86_64/release/photo-ops
```

默认生成 768×512、2 focus × 3 shutter 的独立 Bayer DNG。三个相机信号区间
（约 0.004／0.12／1.5）包含细纹理，near/far 各提供一半清晰支持；失焦使用
sigma=4 的解析式 Gaussian 对各正弦分量的衰减，避免 resize aliasing。
噪声在量化／clipping 前加入：3-code read RMS，加 signal-dependent variance。
该模型不是 R8 profile，也不模拟遮挡 PSF、手持或运动。

输出目录必须不存在。工具记录固定 seed、依赖版本、DNG／truth／manifest SHA-256
和 generator／独立 DNG writer 源码快照，再依次运行两个 standalone HDR 和 matrix。
不传 `--binary` 时只生成输入；`--matrix-only` 跳过两个单焦点 HDR 对照。

检查入口是生成的 `review.md`、`contact-sheet.png` 与各全尺寸 PNG：

- 真值、near HDR、far HDR、matrix 使用相同显示变换。先显式 Rec.2020/D65 →
  linear sRGB，再用固定 `x/(x+0.18)` 全局曲线与 sRGB OETF。负显示通道裁到 0；
  没有为各图单独归一亮度，也不把 preview 送回 operator。
- 原始 TIFF 保留 float32 scene-linear 数值与 ICC；PNG 不能证明高光仍是 HDR。
- confidence、clipping、low-SNR、deghost、combined-confidence 的 PNG 是 [0,1]
  线性灰度预览；各 native scalar TIFF／JSON 保留原始语义和分辨率。
- `metrics.json` 比较 working RGB 与已知 camera signal 经过声明 calibration 的真值；
  排除图像、亮度带和焦平面过渡 margin，不拟合 exposure／颜色来降低误差。
  它包含 demosaic 与测量噪声误差，不能与 native suite 中以独立 HDR planes 为
  对照的 focus-selection MAE 直接比较。

工具本身可使用 full-image NumPy 数组，它的 RSS 不属于 native memory budget。
资源记录单独测 native child，不把 generator、preview renderer 或 Python parent
的内存峰值算成 operator 峰值。

## 原尺寸资源测量

```bash
/path/to/review-env/bin/python scripts/review-focus-exposure.py \
  --width 6022 --height 4024 --output build/hdr-matrix-review-24mp \
  --binary build/linux/x86_64/release/photo-ops --memory-budget-mib 2048
```

这是 **R8 分辨率的合成 DNG**，不是 R8 实拍；不把 24 MP 当作 camera metadata。
原尺寸图像仍明确断言 `--aligned`，配准精度由其他已知 transform fixtures 验证。

已有真实或外部输入可直接使用测量工具，不需要合成 generator 或 Python 影像依赖：

```bash
python scripts/benchmark-focus.py --operator hdr short.CR3 middle.CR3 long.CR3 \
  --output build/hdr-measure
python scripts/benchmark-focus.py --matrix /path/to/capture.json \
  --output build/matrix-measure
```

延续 [focus 性能工具](performance.md) 的 atomic summary、binary snapshot/SHA-256、
Linux `wait4` per-child RSS／CPU／wall 与 native stage reports。矩阵另记录 manifest
SHA-256，帧数从 native HDR-plane exposure reports 统计，不把两个焦平面误记成两张
原图。`--repeat` 只服务 flat focus 的 identity／资源 workload，不能伪装 AEB。
已有输出目录不会被覆盖。

## 验收边界

合成回归已覆盖 radiance、clipping、deghost、静态失焦、两个几何轴、预算内 cache／
streaming 与发布失败；v1.8.1 另保留首次失败及修复后的静态 Gaussian baseline。
真实 R8 focus／AEB／2×3 capture、枝条遮挡、真实噪声、手持 parallax 和运动风景
仍按 [素材计划](data_prep.md) 待拍摄与人工检查。需要先发现真实 staged failure，
才决定是否实现 joint 2D optimizer。

## 本机记录（2026-10-26）

Intel i7-9750H，Arch／GCC 16.2，release／EXR 关闭，tile=256、proxy 长边=1024、
预算 2048 MiB。冻结 v1.8.1 photographic core binary 后顺序运行，测量期间不并行
编译／跑测试；单次桌面测量，未锁 CPU 频率或清 OS cache。

| 6022×4024 合成 workload | wall（秒） | child RSS（MiB） | private file peak（MiB） |
| --- | ---: | ---: | ---: |
| near：3 exposure HDR | 33.06 | 730.06 | 2141.61 |
| far：3 exposure HDR | 34.71 | 730.21 | 2141.61 |
| 2 focus × 3 exposure matrix | 137.50 | 731.09 | 2516.27 |

matrix 逐组 HDR 后复用 2 张 working planes，RSS 没有因六张 originals 常驻而线性
增长。native planner 的保守 estimated peak 为 1764.37 MiB，不是 OS 强制限制；
磁盘 peak 含跨阶段保留的 HDR intermediates。矩阵读取／写入统计与每组 HDR
profile 分开报告，不能只把 root scratch counters 当成整个 capture 的累计 I/O。

原尺寸 matrix 相对已知 camera radiance 的 working-RGB MAE 为 **1.484%**，两个
单焦点 HDR 各约 **10.08%**；近／远区域都约 1.484%，shadow 约 3.362%。matrix
样本范围约 `[-0.000621, 2.60403]`，未裁到 [0,1]，静态场景 motion fraction 为 0。
768×512 独立检查的 matrix MAE 为 1.523%。这些数值来自该明确的数学场景，
不推断真实风景质量，也不把分辨率测量称为真实 R8 acceptance。

完整 input／truth／generator／binary／输出 hash、noise model、阶段耗时与质量
指标见 [冻结记录](../tests/data/hdr-matrix-review-v190.json)。本机 small／24 MP 的
`review.md`、contact sheet、full PNG、float TIFF、numeric maps 与原始 summary
保留在 `build/matrix-review-v190-final/` 和 `build/matrix-review-v190-24mp/`。
