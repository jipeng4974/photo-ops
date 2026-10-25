# 景深合成测试样片

2026-10-25 检索并核对原始来源。目标是焦点位置不同的**处理前序列**，
不把最终合成图当输入，也不把普通 HDR 曝光包围当作 focus bracket。

## 已接入的本地下载集

### Canon CR3：Johannes Sood / Focus Stack Sample

[数据集与作者说明](https://huggingface.co/datasets/jjjsood/focus-stack-sample)
提供 Canon EOS R5 Mark II + RF 100mm F2.8 L Macro 的 999 帧序列；
原始 CR3 使用 1.6× crop mode，5088×3392。许可证是 CC BY 4.0，署名
Johannes Sood。完整拍摄参数与许可随下载保留在 `sood/README.md`、`sood/LICENSE`。

首批只下载 `raw/frame_000.CR3` 到 `frame_002.CR3`，用于真实解码、相邻帧
registration 与 focus breathing 的起步检查。**前三帧只覆盖完整深度范围的一小段**，
不是可生成全景深结果的完整序列。后续 fusion 验证应扩展连续深度段，再考虑整个序列。

同时下载一张 `tiff/frame_000.tif` 验证 TIFF 输入。作者明确说明 TIFF 是
darktable 开发的 **8-bit sRGB**，未经对齐或修图；它不是 16-bit scene-linear
TIFF，不能用于验证 RAW 高光保留。`preview/stacked.jpg` 是最终合成结果，
preview 文件不作为序列输入。

数据集是微距，适合解码、breathing 和长序列压力测试，不覆盖风景中的
近景草叶／远山、视差、流水与天空运动。

**2026-10-25 目视复核：不再用前三帧作为主视觉验收样片。** 作者自己的
`preview/frame_000.jpg` 也几乎全虚，只有右下角极薄的一层清晰；作者的
`preview/stacked.jpg` 才能看到表面纹理。本项目解码图具有同样的失焦结构，
因此这项现象本身不能证明 CR3 解码坏了。先前只检查了读写成功与数值有限，
不足以说明选取的画面适合 photographic regression。保留它用于低纹理／
大面积失焦测试，不再把它当作清晰度、对齐或风景质量的主要依据。

### JPEG：Petteri Aimonen / focus-stack PCB examples

[上游 examples](https://github.com/PetteriAimonen/focus-stack/tree/master/examples/pcb)
中的 `pcb_001.jpg` 到 `pcb_007.jpg` 是轻量的真实 focus-stack 源序列，约 2.3 MB。
下载清单固定了仓库 commit，并保留 `LICENSE.md`。仓库使用 MIT；
图像没有独立许可声明，因此记录这一事实，不另行宣称 CC 授权。
目前仅作为用户主动下载的外部 fixture，不把图像直接提交到本仓库。
`expected.jpg` 是上游算法结果，不作为输入或独立 ground truth。

## 下载与可复现性

```bash
python scripts/fetch-fixtures.py                # 当前小集合，约 50 MB
python scripts/fetch-fixtures.py --group cr3    # 仅 CR3 + notices
python scripts/fetch-fixtures.py --group tiff
python scripts/fetch-fixtures.py --group jpeg
python scripts/smoke-fixtures.py               # 已下载图像的 inspect + RAW→TIFF→JPEG
```

`tests/data/manifest.json` 记录每个文件的固定 revision URL、SHA-256、来源与许可。
CR3/TIFF 的 hash 对照作者发布的 `SHA256SUMS`；JPEG 与 notices 的 hash
在固定 commit 上计算。下载后逐文件校验，已存在且正确的文件跳过。
图片保存在被 git 忽略的 `tests/data/downloads/`，不污染源码提交。
默认 C++ tests 自行生成微型 fixture，无需网络、照片或下载脚本。

## 补充 CR3：清晰单张与真实焦点序列

### raw.pixls.us：五个 CC0 解码／颜色案例

[原始目录](https://raw.pixls.us/) 是 darktable / RawTherapee 使用的 RAW
样本库。以下五个条目逐文件核对了 CC0 标记与上游 SHA-256；目录行保存在
`tests/data/rawpixls-provenance.json`，独立下载清单为
`tests/data/cr3-extra-manifest.json`。

| ID | 机型／格式 | 已目视检查的内容 | 用途 |
| --- | --- | --- | --- |
| 4611 | EOS R，CR3 | 前景植物、花蕾与虚化砖墙 | 细线、自然色、清晰／失焦边界 |
| 4694 | EOS R5，RAW，无 Dual Pixel | 苹果、柠檬、番茄等静物 | 表面细节、色彩、45 MP 解码 |
| 4698 | EOS R5，C-RAW，无 Dual Pixel | 同类果蔬静物 | 有损 RAW 压缩路径；不假定与 4694 像素相同 |
| 5634 | EOS R7，C-RAW | 紫色花朵与绿叶 | APS-C 解码、花瓣纹理与颜色 |
| 7882 | EOS R5 Mark II，C-RAW | 蓝色瓶子、番茄等静物，不同深度清晰度不同 | 新机型 C-RAW、焦平面边界 |

这些是**单张解码 fixtures，不是焦点包围序列**；相似 subject 或同机型不意味着
可直接 stack。工作流没有自动提亮／tone mapping，因此本项目预览可能明显
暗于相机 JPEG；这里用来检查结构与色彩，不以相机渲染亮度为像素级 ground truth。

### Play Raw：高反差秋林 CR3

Jonathan Bieler 的 [High contrast autumn forest scene](https://discuss.pixls.us/t/high-contrast-autumn-forest-scene/27793)
提供 `0V2A3171.CR3`，约 30 MB，作者帖明确标注 CC BY-SA 4.0。
已下载为 `playraw/27793-autumn-forest.CR3` 并加入同一补充 manifest。
树林、细枝、天空和深阴影适合风景纹理／动态范围／颜色处理检查，也可为之后
sunlight-enhance 提供输入；它仍然是单帧，不能验证 focus-stack。
下载照片或分享其派生预览时，保留 Jonathan Bieler 署名、来源和
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) 链接；本项目
生成的半尺寸 JPEG 是重新解码的预览，与原帖完成的调色不同。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/cr3-extra-manifest.json
python scripts/smoke-fixtures.py --manifest tests/data/cr3-extra-manifest.json
```

单独使用 opt-in manifest，避免默认轻量下载集突然变大。预览在被忽略的
`tests/data/downloads/previews/`，没有把照片提交到源码。

### Deptheos：46 帧蜘蛛 CR3 序列

[作者项目的 Example Images](https://github.com/ryanmitts/deptheos#example-images)
公开提供 [完整 RAW/CR3 ZIP](https://images.deptheos.com/spider1-cr3.zip)，
711,338,937 bytes，包含 `9U1A5301.CR3` 至 `9U1A5346.CR3` 46 帧，以及
macOS metadata；另有 JPEG 源序列和最终合成图。它是第二个真正可用的 CR3
focus-stack 来源，不是普通 burst 或 HDR 包围曝光。

为了减少下载，本地通过 HTTP Range 读取 ZIP central directory，取出连续
`5301`、`5302`、`5303`，再补中段 `5324` 与末段 `5346`。
每个文件通过 ZIP CRC32 验证并记录本地 SHA-256，见
`tests/data/deptheos-provenance.json`。文件在 `downloads/deptheos/`。
**五帧只是连续小段 + 深度跨度抽样，不能替代完整 46 帧作无空洞 fusion 验收。**
重现完整序列可以下载上述 ZIP，用标准 ZIP 工具提取 CR3；不要把 `__MACOSX/`
文件当作输入。选取帧的 SHA-256 可与本仓库 provenance 比较。

作者明确提供这些源文件供软件示例使用，但没有找到独立的照片开放许可证。
项目软件的 GPL-3.0 不能自动推定为外链照片的许可。本地用于评估，未加入
默认下载 manifest、未把照片放进源码，也不把它标记为 CC0。

本次补充验证：六个单帧 CR3 的 SHA-256 与 smoke checks 通过；蜘蛛五帧的
CRC32、SHA-256 与解码／读写 smoke 通过。已目视检查新预览：蜘蛛 `5301`
主要是前方腿和承载面清晰，`5324` 的脸部较清晰，`5346` 焦点移动到背部，
确实覆盖不同焦平面。预览未自动提亮，整体较暗。可复查已提取的蜘蛛子集：

```bash
python scripts/smoke-fixtures.py --manifest tests/data/deptheos-provenance.json
```

## 后续候选与使用边界

| 来源 | 适用性 | 当前处理 |
| --- | --- | --- |
| [Towards Real-World Focus Stacking](https://github.com/araujoalexandre/FocusStackingDataset) | 作者提供 94 个高分辨率 RAW bursts 与商业软件生成的 pseudo ground truth；适合算法研究 | README 未给出明确数据许可，不自动下载或再分发；pseudo ground truth 也不是无误差参考 |
| [Matthew Storer 风景教程](https://www.youtube.com/watch?v=9I7-EaPiyaY) | 教程声明有 free RAW 下载，主题正好是风景 focus stacking | 已找到候选，但未验证到稳定可下载文件与开放授权，不放入 manifest |
| [Helicon 官方 demo stacks](https://www.heliconsoft.com/helicon-focus-gallery/) | 官方提供多个处理前序列 | 页面用途条件指向测试 Helicon Focus、演示和营销，并要求保留版权声明，分享结果还需联系；未据此推定可自由用于本项目回归集 |

当前已验证可用的 CR3 是微距，尚未获得同时具备稳定直链与明确开放许可的
**风景 CR3 焦点包围序列**。此缺口应保留；后续优先加入作者自己的三到五帧
风景 CR3，以及同一场景的平静／有风两组，而不是以微距集替代风景质量验收。

## sunlight-enhance 的典型风景集

`tests/data/landscape-manifest.json` 增加三张可直接下载的 CR3：Len Philpot 的
阳光秋林／河道、Aliks 的喜马拉雅山湖、Karl / kaha 的蓝天亮云／暗山地。
它们分别提供受光树叶／倒影、积雪／大气深度、天空误判等检查场景，均保留
作者帖子和 CC BY-SA 许可链接、SHA-256；不把照片放进源码。
详见 [算法与实际检查](sunlight-enhance.md)。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/landscape-manifest.json
python scripts/review-sunlight.py
```

## v1.2 RAW float / R8 补充素材

`r8-single-manifest.json` / `r8-single-provenance.json` 固定 raw.pixls.us 的 R8
sample 6585（RAW Full Frame）和 6587（CRAW Full Frame），记录原始 repository row、
CC0 链接与 SHA-256。它们用于相机解码、signed float 范围和 24 MP 重复输入的
性能／identity 测试，**不是一组 focus bracket**。用户自摄 corpus 仍未采集。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/r8-single-manifest.json
PHOTO_OPS_RAW_FIXTURES="$PWD/tests/data/downloads" xmake test
```

opt-in C++ 测试也读取默认 manifest 中相邻的三帧 Canon R5 II CR3，检查同一 context
和全局色度一致性。该三帧是 999-frame macro 的连续前缀，景深跨度很小，不能用来
证明全场景已合成清晰。新增 `canon-depth-span-manifest.json` 固定 frame 000 / 499 /
998 的 RAW 身份，供探索更大深度跨度的失败与 conservative rejection；三帧抽样
同样不代表完整覆盖。来源继续为 Johannes Sood、CC BY 4.0，不提交原始图片。

```bash
python scripts/fetch-fixtures.py --manifest tests/data/canon-depth-span-manifest.json
```

生成的 Bayer DNG 数值 fixtures 位于 C++ 临时目录，控制 black、WB metadata、
exposure、orientation、negative / HDR sensor data，正常结束删除；不依赖网络照片。
