# sunlight-enhance：可运行的 classical MVP

当前实现先建立可解释、可测试的基础，不宣称已能可靠理解所有风景光线。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/landscape-manifest.json
xmake run photo-ops sunlight-enhance INPUT.CR3 -o enhanced.tif \
  --half-size --strength 1 --diagnostics build/sunlight-diagnostics
python scripts/review-sunlight.py  # 三张样片的本地 HTML 滑块对照，含署名与 JSON
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
   的区域的 gain。选择一个 dominant region；confidence 太低时无修改。
5. 对 analysis mask 羽化、双线性提升到原尺寸，再按像素亮度约束边缘泄漏。
6. 在 OKLab 内增加**已有** chroma，unit strength 的理论增益上限 18%；
   近中性像素的增益和暖色位移趋于零。少量 luminance lift（unit strength
   最高 0.06 EV）与微小 warmth 都受 mask 和 highlight headroom 限制。
7. 负值／HDR 越界像素原样保留；已经在 gamut 内的像素按整个 RGB 改变量缩小
   步长以避免 channel clipping。没有全局 tone map、dehaze、锐化或局部 edge boost。

当前 soft mask 表示**选中的区域权重**，不等于每个像素都必然改变：中性、负值、
HDR 与临近 clipping 的像素还会受到 naturalness guards。
`confidence` 是可检查的区域分数，不是校准过的“阳光概率”。

## 输出与诊断

`--diagnostics DIR` 写：

- `before.jpg` / `after.jpg`：同一个自动 preview exposure，最多 +4 EV，只用于
  检查，实际 `enhanced.tif` 不应用这层曝光；共享曝光记录在 JSON 中。
- `mask.tif`：全分辨率三通道灰色 mask，16-bit sRGB + ICC，可直接查看；读回线性
  RGB 才是 mask 权重，不能把 gamma 编码的 TIFF 整数直接当权重。
- `report.json`：是否执行、候选数、区域占比、heuristic confidence、亮度分离、
  chroma deficit 和理论 gain 上限。

主输出与诊断文件拒绝覆盖；诊断写失败通过 result 返回，可能已经产生部分诊断
文件，需要清理目录或换新目录后重试。算法 API 借用输入，并明确分配输出 image
与 mask；诊断额外复用一个全幅 preview buffer，没有不透明的隐式 clone。

## 回归与真实图像

离线 synthetic tests 覆盖连贯受光色块、zero-strength identity、阴影不变、
中性不生色、HDR 保护、平光、微小高光、蓝天／白天空、蓝天内云块、底部水平反射
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

首次目视检查发现秋林的倒影被优先选中、亮云也可误判；先保留数值／视觉诊断，
加入对应 synthetic regression，再增加连通天空与水平 glint 保护。
真实图像中的水面、雾、浅色岩石、雪、复杂云层和与 sky 相连的山脊仍可能误判。
下一轮优先改善 region selection / confidence，而不是直接增加 saturation 强度。

首轮收紧规则后的检查结果（2026-10-25，half-size CR3）：秋林选中右侧受光枝叶，
区域 6.2%、heuristic confidence 0.33，不再优先选中底部倒影；山湖选中前景
雪／地面附近约 3.8%，confidence 0.47，仍不应视为可靠的语义阳光识别；
蓝天亮云场景没有可信候选，保留工作像素。HTML 滑块对照生成在
`build/sunlight-review/index.html`，数值结果在同目录 `summary.json`。

工程验证：C++ foundation + sunlight 回归、七项 Python format/CLI tests、
debug + EXR + ASan/UBSan，以及 release 关闭 EXR 的 clean build 均已通过。
contracts 的最终 build 语义为 `quick_enforce`，相关 GCC 限制见 C++ 风格文档。
