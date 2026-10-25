# focus-stack：性能测量与有界 tile cache

v1.4 在现有 `stack_focus` report 中增加 profiling，并按预算缓存**当前 tile**。
保持单个 fusion worker，不引入 SIMD／GPU，也不驻留整组 full-resolution RGB。

## 先定位耗时

在优化前冻结带 instrumentation 的 release binary，再测量同一输入／参数。
七帧 PCB 的 57.83 s 中，fusion 为 43.86 s；其内部 focus score 20.30 s、pyramid
13.88 s、warp 5.28 s。scratch read/write 约 1.05 s。优先消除重复计算有直接证据，
此处没有理由先增加 GPU 或多个 tile worker。

`focus_stack_report.profile` 即使未输出 diagnostics 也可通过 C++ API 读取；启用
`--diagnostics` 时，`report.json` 包含相同字段：

- 外层 wall stages：preflight（RAW unpack metadata/context 检查也在此）、decode
  （含 proxy 生成／spool）、registration、analysis、fusion、output、diagnostics。
- 内层累计时间：scratch read/write、warp、focus score、pyramid。它们包含在外层
  stage 中，warp 也包含自己的 scratch read，**不能全部相加**。
- 工作量：decoded frames、tiles、warp calls、focus evaluations、实际 tile cache 峰值。
- 字节：scratch 累计读／写、private run directory 的实际逻辑文件总量峰值、最终
  TIFF 文件大小。逻辑文件大小不是文件系统实际占用 blocks，也不是 OS page cache。

时钟为 `steady_clock`；I/O 计时包含 adapter 内的打开、读写与分配，并非纯介质延迟。
spool 直到退出才删除，因此 fusion 完成时所有文件的大小之和就是本次逻辑文件峰值。
`total_seconds` 不含最后 JSON 的序列化／发布与 scratch 清理；外部 wall 包括进程
启动／退出。diagnostics 在最终照片成功发布后生成，JSON 最后发布；诊断失败返回
明确的“完整照片已发布”路径，单文件 atomic 不等于多文件事务。

## 预算与计算减少

第一遍 warp／focus weight 同时生成 base weight 总和。若当前预算容纳，则保留
每个 source 的 warped RGB（12 bytes/pixel）和 base weight（4 bytes/pixel）；
第二遍直接移入 pyramid，避免再算 warp / focus。预算不足保留 two-pass streaming。

```text
Q = proxy pixels; T = (tile + 2*halo)^2; N = frames
usable = 0.8 * memory_budget
streaming reserve = 256*Q + 512*T + 128 MiB
cached reserve    = 256*Q + max(512, 384+16*N)*T + 128 MiB
cache capacity   = 16*N*T
```

planner 先用原有 streaming 规则选择 tile／proxy，再决定 cache 能否容纳，不通过
强制缩小 tile 追求 cache。`N<=8` 时 cache 在原 512T reserve 内；较大 N 的显式
`16NT` 成本不可省略。report 分别给出名义容量、是否采用、边缘 tile 真实分配峰值。
RAW scratch 预检同时保留 float32 最终文件的 12P bytes，不再沿用 rendered 的 6P。
这是保守工程估计，不是 OS RSS 限制或任意 codec 的严格内存上界。

其余优化保持数学顺序与颜色语义：

- hot image/view、颜色数学和 context accessors 在 named module 中显式 `inline`；
  模块内函数不能依赖传统 class body 的隐式 inline 假设。contracts 仍启用。
- 私有 smoothing、focus、pyramid、warp 内循环预先借用 row spans，避免每像素重复
  view 构造／contract 检查；纵向滑动和按行访问，保留每列求和顺序。RAW mosaic／green
  同样在已校验的 owned buffer 范围内借用 storage／stride，公式与 orientation 不变。
- coherent map 没有任何 label transition 时，不计算无证据的 defocus 模型；各 source
  motion 仍逐个合并，不把相同文件名或重复输入作为跳过 photographic pipeline 的捷径。

## 可复现运行

先构建 release，再运行测量脚本。脚本不修改输入，拒绝已有测量目录；复制 executable
到新目录并记录 SHA-256，避免后续重新构建改变 binary 身份。Linux `os.wait4` 获取
这一次 child 的 peak RSS／CPU 时间，不使用累计多个 children 的 `getrusage` 峰值。

```bash
scripts/configure.sh -m release --exr=n --sanitize=n -y
xmake -r -j4
python scripts/benchmark-focus.py tests/data/downloads/pcb/pcb_*.jpg \
  --output build/measure-pcb --memory-budget-mib 512
python scripts/benchmark-focus.py tests/data/downloads/rawpixls/6585-eos-r8-raw.CR3 \
  --repeat 3 --output build/measure-r8-3
python scripts/benchmark-focus.py tests/data/downloads/rawpixls/6585-eos-r8-raw.CR3 \
  --repeat 8 --output build/measure-r8-8
```

默认 tile=256、proxy 长边=1024、预算=2048 MiB。scratch 默认放新测量目录的
`scratch/`；可传已存在的 `--scratch-directory`。24 MP 多帧应选有足够空间的本地
磁盘，具体文件估计见 native report。每次输出 summary.json、native diagnostics、
TIFF、stdout/stderr 与 binary snapshot。所有原图下载／许可见 [fixtures](fixtures.md)。

## 本机测量（2026-10-26）

Intel i7-9750H、Arch 系统依赖、GCC 16.2、release、EXR 关闭，单次 before/after。
日常桌面环境未锁 CPU 频率或清 OS cache；这些是代表性实测，不是统计置信区间。
PCB 为七帧 2048×1536 真实 focus sequence；R8 为 **6022×4024 同一张 RAW 重复**，
仅用于 full-resolution 资源／identity，不能替代真实 3–8 帧 bracket 的画质验收。

| workload | wall before → after | speedup | RSS before → after (MiB) | scratch peak (MiB) |
| --- | ---: | ---: | ---: | ---: |
| PCB 7 帧 | 57.83 → 26.69 s | 2.17× | 160.03 → 160.80 | 356.25 |
| R8 3 次重复 | 215.43 → 85.29 s | 2.53× | 550.27 → 550.40 | 1209.50 |
| R8 8 次重复 | 597.61 → 222.90 s | 2.68× | 550.44 → 550.31 | 2763.14 |

各阶段／内核耗时（秒，before → after；内层不可重复相加）：

| workload | decode | analysis | fusion | focus (nested) | pyramid (nested) |
| --- | ---: | ---: | ---: | ---: | ---: |
| PCB 7 帧 | 2.42 → 2.21 | 5.87 → 3.22 | 43.86 → 16.17 | 20.30 → 5.02 | 13.88 → 5.07 |
| R8 3 次重复 | 23.73 → 7.16 | 7.08 → 1.31 | 177.22 → 66.96 | 76.85 → 17.22 | 57.48 → 22.79 |
| R8 8 次重复 | 70.90 → 25.37 | 25.87 → 2.31 | 489.56 → 184.72 | 230.39 → 55.47 | 137.60 → 60.01 |

所有三组输出的解码样本完全一致（最大差值 0）。PCB warp calls 672 → 336，
R8 三帧 2304 → 1152、八帧 6144 → 3072；scratch 写入／峰值不变，
读取量因省去第二次 warp 降低。RSS 主要由单帧 decode 决定，优化没有增加整组驻留。
测量后仅补正 RAW 最终输出的 free-space reserve（6P → 12P）并更新版本号，
没有再改图像计算路径；记录中保留测量 binary 的 SHA-256。

验证通过：cache 与 streaming 在不同预算下逐 RGB 样本一致（32 帧 identity、24 帧
交错 near/far）；tile 与全域、focus planes、motion/reference fallback、defocus／
三深度重叠、signed/HDR RAW、context mismatch、atomic output 等回归继续通过。
上述真实 PCB 与 R8 workload 另做逐行 TIFF 解码比较，确保性能优化没有改变样本。
完整可检查数值与 pixel SHA-256 见 [测量记录](performance-v140.json)。

真实 R8 自摄多焦点风景的耗时和画质，仍按 [素材计划](data_prep.md) 待采集后补充。

额外用公开 Canon 999 帧 macro 的稀疏 `000/499/998` 子集检查了更大的焦点跨度：
coarse registration 因 `frame_000` 少于 8 个 mutual matches 保守拒绝，没有 final
TIFF。该跨度缺少共享清晰证据，不能通过 `--aligned` 猜几何或下调门槛把它冒充成功
合成；三个连续前缀能运行也不意味着跨越整场景景深的三张图足够。此限制与输入
manifest 一起留在测量记录中，后续需更连续的焦点覆盖／真实 bracket 验证。

最终验证：release / EXR 关闭的完整 C++ 与 CLI／工具链 Python 回归通过，opt-in
真实 R8 RAW／CRAW 和 Canon 相邻帧 context／颜色一致性检查通过；clean debug +
EXR + ASan/UBSan 构建、完整 C++ 与 CLI／工具链 Python 回归通过，未发现 sanitizer
错误。可选 sunlight 实拍 Python 检查未启用（其余离线检查通过）。源码格式与本地
文档链接检查通过。
