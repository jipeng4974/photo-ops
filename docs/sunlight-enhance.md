# sunlight-enhance：可运行的 classical MVP

当前实现先建立可解释、可测试的基础，不宣称已能可靠理解所有风景光线。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/landscape-manifest.json
xmake run photo-ops sunlight-enhance INPUT.CR3 -o enhanced.tif \
  --half-size --strength 1 --diagnostics build/sunlight-diagnostics
python scripts/review-sunlight.py --output build/sunlight-review-new
# 生成三张样片的本地 HTML 滑块对照；输出目录需是新目录，保留原始对照
```

`--strength` 范围 `[0,2]`，默认 1；0 是逐像素精确无修改结果。
CR3、TIFF、JPEG 进入相同 float32 linear-sRGB / D65 算法，默认输出 16-bit TIFF；
也可按后缀写 JPEG 或（启用时）EXR。`--half-size` 只作用于 RAW decode。

## 算法与可检查的判断

1. 面积平均成最长边不超过 256 的 analysis grid。用相对 log2 luminance 避免
   RAW 开发亮度不同就完全改变阈值；小尺度平滑抑制单个亮点与噪声。
2. 使用两种尺度的 illumination，结合全局分位数、四连通区域、区域面积
   （1%–55%）、与较暗环境的 luminance 分离及局部对比选候选。
3. 从顶部连接的蓝色／明亮中性像素构造保守 sky exclusion，连接蓝天中的
   明亮云块；蓝色大区域另做保护。底部相连且具有明显水平条纹的亮区域
   作为疑似水面 glint 拒绝。它们都是可以失败的启发式，不是 semantic segmentation。
4. 比较区域与较暗半幅参考的 OKLab lightness/chroma 分离，并限制已经很彩色
   的区域的 gain。v0.3.1 拒绝平均 `C/L < 0.02` 的近中性亮区，并以
   `smoothstep(0.02,0.05,C/L)` 调整候选 confidence：颜色证据弱时需要更强的
   illumination 证据。不是拒绝浮冰后无条件挑下一个亮块；confidence 不够就无修改。
   这不能从单张照片证明材质／照明的区别，也会保守跳过真正受光的白色表面。
5. 对 analysis mask 羽化、双线性提升到原尺寸，再按像素亮度约束边缘泄漏。
6. 在 OKLab 内增加**已有** chroma，unit strength 的理论系数为
   `0.35*sqrt(chroma_deficit)`（最多 +35%），实际还受 mask、颜色证据与 headroom 限制。
   像素颜色证据使用相对色度 `C/L`，不再用绝对 C 把较暗 RAW 中的彩色枝叶当作中性。
   warmth 同样按 L 缩放；在未触及 high/low/gamut guards 时，曝光缩放不改变相对
   色度响应。luminance lift 在 unit strength 最多 +0.16 EV；仍是局部小幅调整。
7. 负值／HDR 越界像素原样保留；已经在 gamut 内的像素按整个 RGB 改变量缩小
   步长以避免 channel clipping。没有全局 tone map、dehaze、锐化或局部 edge boost。

当前 soft mask 表示**选中的区域权重**，不等于每个像素都必然改变：中性、负值、
HDR 与临近 clipping 的像素还会受到 naturalness guards。
`confidence` 是可检查的区域分数，不是校准过的“阳光概率”。`applied` 还需要
至少有实际像素改动；全部像素受保护时，不能把非空候选 mask 报成“已增强”。

## 输出与诊断

`--diagnostics DIR` 写：

- `before.jpg` / `after.jpg`：同一个自动 preview exposure，最多 +4 EV，只用于
  检查，实际 `enhanced.tif` 不应用这层曝光；共享曝光记录在 JSON 中。
- `detail-before.jpg` / `detail-after.jpg`：mask >0.5 的包围区域加边距，使用全图
  同一曝光；只有非空选区才生成。HTML 分别提供全图／局部滑块，不夸大差分或额外提亮。
- `mask.jpg`：方便直接查看的 mask；另有精确度更高的 `mask.tif`。
- `mask.tif`：全分辨率三通道灰色 mask，16-bit sRGB + ICC，可直接查看；读回线性
  RGB 才是 mask 权重，不能把 gamma 编码的 TIFF 整数直接当权重。
- `report.json`：decision、是否执行、候选数、近中性拒绝数、区域占比、heuristic
  confidence、亮度分离、chroma deficit 和理论 gain 上限；新增 **实际** mask 加权
  `mean_relative_chroma_gain`、`mean_luminance_change_ev` 与全图 `changed_pixel_fraction`。
  相对色度增益比较 `(C/L)_after/(C/L)_before`，避免把曝光提升本身算作 saturation。
  理论上限不是最终效果；所有像素保护和 gamut 约束之后再测量。

主输出与诊断文件拒绝覆盖；诊断写失败通过 result 返回，可能已经产生部分诊断
文件，需要清理目录或换新目录后重试。算法 API 借用输入，并明确分配输出 image
与 mask；诊断额外复用一个全幅 preview buffer，没有不透明的隐式 clone。

## 回归与真实图像

离线 synthetic tests 覆盖连贯受光色块、zero-strength identity、阴影不变、
中性亮材质不作为 illumination 证据、1/32 曝光缩放的相对色度响应、
已彩色区域的 gain 上限、完全受保护高光的精确 identity、HDR 保护、平光、微小高光、
蓝天／白天空、蓝天内云块、底部水平反射
条带，以及非法用户输入。CLI tests 额外验证 TIFF → operator → TIFF、diagnostics
文件、JSON 和错误边界，无需下载图像。

三张新风景 RAW 在 `tests/data/landscape-manifest.json` 固定来源和 SHA-256：

| 场景 | 作者／来源 | 检查重点 |
| --- | --- | --- |
| 阳光秋林与暗河道 | [Len Philpot, Into darkness?](https://discuss.pixls.us/t/into-darkness/54398) | 目标树叶与水面倒影的区分 |
| 山湖与积雪 | [Aliks, Western Himalayas mountain lake](https://discuss.pixls.us/t/western-himalayas-mountain-lake/44268) | 天空、雪高光、山体与湖面、距离感 |
| 蓝天、云与暗山地 | [Karl / kaha, Blue sky between mountains and clouds](https://discuss.pixls.us/t/blue-sky-between-mountains-and-clouds/48825) | 亮云误判和高动态范围 |

三篇原帖都提供 RAW 并标注 CC BY-SA；具体许可链接及署名随 manifest 保留。
照片不提交到源码；派生预览应保留原作者、来源、许可，并说明重新 RAW 开发、
半尺寸处理、局部增强及 preview exposure。它们不是 ground-truth mask。

真实样片回归是显式 opt-in（默认离线测试会 skip，不会自行联网下载）：

```bash
PHOTO_OPS_REAL_FIXTURES=1 python -m unittest discover -s tests -p 'test_sunlight_real.py'
```

该检查先核对 manifest hash，要求树选区有适度实际变化，而浮冰与云层 case 无修改。
不把这三张照片当作通用 sunlight benchmark；仍需检查 nearby synthetic cases 和曝光缩放。


首次目视检查发现秋林的倒影被优先选中、亮云也可误判；先保留数值／视觉诊断，
加入对应 synthetic regression，再增加连通天空与水平 glint 保护。
真实图像中的水面、雾、浅色岩石、雪、复杂云层和与 sky 相连的山脊仍可能误判。
不能只靠提高 saturation 修复 region selection；区域意义与实际编辑幅度需要分开验证。

v0.1 首轮收紧规则后的历史结果（2026-10-25，half-size CR3）：秋林选中右侧受光枝叶，
区域 6.2%、heuristic confidence 0.33，不再优先选中底部倒影；山湖选中前景
雪／地面附近约 3.8%，confidence 0.47，仍不应视为可靠的语义阳光识别；
蓝天亮云场景没有可信候选，保留工作像素。HTML 滑块对照生成在
`build/sunlight-review/index.html`，数值结果在同目录 `summary.json`。

工程验证：C++ foundation + sunlight 回归、七项 Python format/CLI tests、
debug + EXR + ASan/UBSan，以及 release 关闭 EXR 的 clean build 均已通过。
contracts 的最终 build 语义为 `quick_enforce`，相关 GCC 限制见 C++ 风格文档。


## v0.3.1：亮材质误选与暗 RAW 响应修正

用户检查指出两类问题：western-himalayas 的浮冰有可见亮度变化，但不是典型
受光主体；into-darkness 的 mask 选对了树，成片却几乎看不出增强。先保留
manifest 固定来源的真实 fixture 与旧 review，新增 synthetic 回归再修改算法。

测量旧版后发现 into-darkness 的理论 chroma 上限约 +5.5%，实际选区平均相对色度
只有 **+1.93%**，亮度 **+0.046 EV**。绝对 C 保护受 RAW 工作曝光影响，加上过低
增益，导致“选中了但几乎没改”。v2 改为相对颜色证据、相对 warmth 和上述温和增益。

同一 half-size decode、strength=1、同一 +3.066 EV 预览下，选区 mask 与旧版
**逐像素相同**，平均相对色度 **+9.23%**、亮度 **+0.121 EV**。新增局部对照可见
树叶颜色和亮度变化，背景不做全局处理；原有 HDR、近中性与 gamut 保护仍在。
western-himalayas 现在 `weak_illumination_evidence`、mask 为空、实际改动为 0；
云层 case 维持 `no_coherent_sunlight`、改动为 0。二者作为 abstention 负例，而不是
为了展示效果而强行寻找一个亮区。

本机新 review：`build/sunlight-review-v031-checked/index.html`，原 review 保留。
算法标记为 `sunlight-classical-v2`。照片及派生图仍不提交，署名与许可保留在页面。
单张图的 albedo/illumination 分离仍不完整；当前 heuristic 只表达何时有足够证据
运行这个窄算子，不宣称识别画面主体或理解所有光线。

v0.3.1 工程验证：debug + EXR + ASan/UBSan 和 release / EXR 关闭均完成 clean
build 与 C++ 回归；10 项离线 Python 回归通过，真实 RAW opt-in 检查的三个 case
通过。检查真实样片 SHA-256、原／新树 mask 的像素差为零、局部预览，以及生成
HTML 的本地资源链接。最终恢复 debug + EXR、关闭 sanitizer；未修改系统包。
