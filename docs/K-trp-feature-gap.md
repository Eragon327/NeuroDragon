# K. TRP 0.5.3 对照：SR / NR 有什么功能，我们做哪些

> 2026-10-24。回答两个问题：**TRP 的 SR 和 NR 各有什么功能**、**我们已有 / 缺什么 / 缺的里哪些值得做、代价多少**。
>
> 依据：
> - TRP 源码 `D:\Documents\theosrp`。**工作树停在 0.4.5，全部结论按 `origin/main` = 0.5.3 读**——
>   用 `git show origin/main:<path>`，别在本机 working tree 里找 0.5.x 的代码。
> - 我们自己的实测：`docs/J`（SR）、`docs/D`（NR 事实与参数表）。
> - 本机数值：NR `1485×835` 实测 **3.5–3.8 ms**、100% 内部分辨率时 **8.75 ms**；
>   同帧引擎 DLSS **1.2–1.7 ms**；~82 fps ⇒ NR 约占 **29%** 帧预算。
>   游戏目录 `nvngx_dlss.dll` = **310.5.3**，NR 运行时 = **310.9.16**（266 MB）。
>
> 代价是估算，不是承诺；「能做」= 有现成管道，只差接线 + 实机验证。

## 0. 结论速览

**SR**：TRP 0.5.x 只在 UI 和截图上动过，SR 的实质能力从 0.4.5 起没变。值得抄的只有三件——
**RCAS 锐化**、**成品帧截图**、**mip LOD 偏置**；另有两个实验（换 DLSS 运行时、78% 档）。

**NR**：TRP 的更新主力就是 NR（5 pass、隔帧、对比、对象遮罩、曝光/宽窄/细节、稳态网格）。
我们缺的全在「运行时之外那一层」，最值得做的是**降本**：**输入缩放**和**隔帧**。
现在 NR 吃 29% 帧预算，这两条能把它砍到 1/4–1/2，而且我们的 gain 合成架构正好吃得下。

## 1. 读 TRP 源码的入口

| 想知道 | 看哪里 |
|---|---|
| SR 的面板行（模式/比例/预设/曝光/抖动/锐化） | `src/OverlayImagePanel.cpp` |
| SR 的运行时接入、锐化、直写输出 | `src/DLSSBackend.cpp`（`Evaluate` / `GetOptimalMipLodBias`） |
| 质量档 → 渲染尺寸的取值 | `src/DLSSBackend.h` 顶部注释、`src/NvidiaUpscalerConfiguration.h` |
| mip LOD 偏置怎么落到采样器 | `src/UpscalerSamplerHooks.cpp`（D3D11 重建 sampler，非 D3D12 思路） |
| 截图替换 | `src/EngineScreenshot.{h,cpp}` |
| NR 运行时参数契约（名字/类型/范围） | `src/FrameGen/NeuralRenderingRuntimeContract.h` |
| NR 调参默认值与范围 | `src/FrameGen/NeuralRenderingTuning.h` |
| NR 合成/曝光/输入缩放/隔帧 | `src/FrameGen/NeuralRenderingReconstruction.h`、`NeuralModelRate.h` |
| 每版改了什么 | `CHANGELOG.md`（`git show origin/main:CHANGELOG.md`） |

## 2. TRP 的 SR

### 2.1 功能清单（0.5.3）

| # | 功能 | TRP 做法 |
|---|---|---|
| 1 | 模式 DLSS / DLAA / XeSS | DLAA = 模式 3（分配槽 5）；XeSS 实验，AMD 自动走 |
| 2 | 渲染比例 5 档 | 33 UltraPerf / 50 Perf / 58 Bal / 67 Qual / **78 UltraQual**；DLAA = 原生 100% |
| 3 | 模型预设 | Driver / E / F / J / K(默认) / L / M，Apply 原地重建 |
| 4 | **锐化** | 自带 RCAS compute（`RCAS.hlsl` + 常量缓冲），开关 + 0–1 强度（默认 0.672），跑在 DLSS 之后 |
| 5 | **mip LOD 偏置** | `log2(render/display) − 1`（DLSS 编程指南公式）或手填；钩 D3D11 采样器重建，0.4.3 起只作用于游戏自身渲染 |
| 6 | 输出直写 | `DirectDLSSOutput` / `DirectRCASOutput`，省一次全屏拷贝，不合格自动回退 |
| 7 | 现场切换 | DLSS↔XeSS 在原地尺寸上 Apply；换比例/DLAA 仍需重启 |
| 8 | 尺寸预检 | 查 NGX 的 optimal/min/max + 宽高比，拒绝会失败的切换 |
| 9 | 抖动 | jitter 开关、phase count、开菜单时重置历史 |
| 10 | 自动曝光 | DLSS 创建 flag `AutoExposure` 开关（XeSS 保留游戏曝光） |
| 11 | 遥测 | 尺寸、输入格式、HDR 输入、jitter 相位、evaluate 成败计数、分阶段 GPU 时间 |
| 12 | **成品帧截图** | 0.5.3：钩引擎 `SaveRenderTargetToFile`，随后用**下一张真实帧**覆盖文件（有延迟，不精确） |
| 13 | HDR 输出 | SDR→HDR10 逆色调映射（与 SR 相邻，非核心） |
| 14 | 接管游戏 DRS | 钩 `SetDRS` 锁渲染比例，防游戏动态分辨率与 DLSS 打架 |
| 15 | NR ↔ SR 顺序 | NR 可在 DLSS 前或后 |

### 2.2 对照我们

**已对齐**

| TRP | 我们 |
|---|---|
| DLSS + DLAA(100%) | ✅ |
| 比例档 | ✅ 33/50/58/66/100（**缺 78%**，见 §5） |
| 模型预设 + 现场重建 | ✅ 六个 `Hint.Render.Preset` 全写，ctx 换槽重建（docs/J §6） |
| subrect / 只渲染子矩形 | ✅ |
| 锁渲染比例 | ✅ FrameBuilder 因子表 patch（等价于它的 DRS 接管） |
| 遥测 | 部分：尺寸/档位/flags/预设/动态范围/ctx；缺输入格式、jitter 相位、evaluate 成败 |
| NR 在 SR 之前 | ✅（我们只有这条）；「放大后」= 位置轴 After 档，2026-10-24 已排期，见 [N](N-nr-placement-plan.md) |

**可做（按性价比）**

| 功能 | 我们现状 | 代价 | 风险 / 说明 |
|---|---|---|---|
| ① RCAS 锐化 | **已实现（2026-10-24，见 [J §9](J-sr-implementation.md#L101-L131)），待实机** | 2–2.5 天 | 不能直抄：引擎 `Output` 是线性 HDR，RCAS 只对 [0,1] 有解，得先过 Reinhard 域 |
| ② 成品帧截图 | **已实现（2026-10-24，见 [J §10](J-sr-implementation.md#L139-L170)），待实机**：Present 里抓后缓冲 → WIC PNG，F2，不含面板 | 0.5–1 天 | Bedrock 没有「引擎截图函数」可钩，所以是自家按键而不是替换游戏截图 |
| ③ 遥测补齐 | 缺输入格式/HDR 标志/jitter 相位/成败计数 | 0.5 天 | 只读，零风险 |
| ④ 换 `nvngx_dlss.dll` | 游戏自带 **310.5.3** | 0.5 小时实验 | TRP 自己随包发运行时。换 310.9.x 可能有新模型，也可能被引擎拒；失败删文件即回滚 |
| ⑤ 78% / 任意比例 | 不做（docs/J §4） | 0.5–1 天实验 | 见 §5：否掉 75% 时只试了 q=5/q=3，**q=1/q=2 的动态范围覆盖 1920×1080**，且因子表里本来就有 0.75 |
| ⑥ mip LOD 偏置 | 从未碰过 | 侦察 0.5–1 天 + 实现 1–2 天 | D3D12 不能照抄 D3D11 重建采样器；得钩 `CreateRootSignature`/`CreateSampler` 改 `MipLodBias`。风险：UI/后处理一起变糊或闪，要先确认世界 pass 与 UI pass 用不同根签名 |

**不做**

| TRP | 原因 |
|---|---|
| XeSS、现场切超分器 | Bedrock 只有 DLSS，没有第二个后端 |
| 输出直写 | 输出纹理与拷贝都在引擎手里，我们没有「省一次拷贝」的位置 |
| 手动 jitter 开关 | 引擎 jitter 已对齐 DLSS，改只会更差 |
| ReShade 前后顺序 / HDR 输出 / 纹理 mip 上限 / 天气覆盖锐化 | 目标进程不归我们（无 ReShade、交换链与 HDR 由 Bedrock 管、VRAM 上限不是 SR、无天气概念） |

## 3. TRP 的 NR

### 3.1 两层

1. **运行时参数**（`DLSSNR.*`，写 NGX 参数对象）：`Style`、`Intensity`、`LocalToneStrength`、
   `LocalStructureStrength`、`SkinStructureStrength`、`UseAutoMask`、`UICorrection`、
   `DepthInverted`、`Reset`、`Enabled`、`Hint.Render.Preset`、`ScalingRatio`、`Width/Height`、各 subrect。
   类型按运行时分：310.x 的 `Enabled/Style/Reset/DepthInverted/UseAutoMask/UICorrection/Hint.Render.Preset`
   是 int，尺寸/子矩形是 unsigned，强度是 float。
2. **TRP 自己的处理**（在运行时之外）：placement、多 pass、reconstruction 数学、隔帧、对象遮罩、对比、计时。

### 3.2 功能清单（0.5.3）

| 层 | 功能 | 说明 |
|---|---|---|
| 运行时 | Style / Intensity / Tone / Structure / SkinStructure | 310.x 强度范围 **0–2**；`SkinStructureStrength` 有 **`-1` 哨兵** |
| 运行时 | UseAutoMask / UICorrection | 自动皮肤遮罩、UI 校正；**TRP 默认都关** |
| TRP | placement：DLSS 前 / 后 | 我们只有「前」 |
| TRP | 1–5 pass，各自独立 | 0.5.2 加到 5 个，pass 3–5 每帧起一个 |
| TRP | **每 pass 输入缩放** `inputScale` | 喂给网络的分辨率；<1 时改用 Residual resolve（TRP 认为缩放输入不能直接采原输出） |
| TRP | **Model rate 隔帧** | 隔帧跑模型，中间帧用 MV+depth 搬运变化，约省一半；重置/改设置/Freeze 会强制跑 |
| TRP | Reconstruction：Auto / Residual / Ratio | Ratio = 取比例/增益再合成，我们的 `Replace=5/6` 已抄这一支 |
| TRP | 曝光 Fixed / Adaptive + EV 偏置 | 自适应测画面平均亮度，让 NR 在室内外看到相近亮度；只改 NR 的视图 |
| TRP | Broad / Detail + grey guard + 黑=阴影 + 稳态网格 | 0.5.1 一批质量修正：宽窄分解、近中性色不染色、黑答案当阴影、喂 NR 未抖动网格 |
| TRP | 对象遮罩（手/装备/其他类人/生物） | 每组独立 Strength/Tone/Structure，逐 pass；需要逐 draw 分类 + 掩码预览 |
| TRP | Peripheral compression / fused preparation | 省成本实验项，默认关 |
| TRP | Compare：Split / Freeze frame | 会话内、不保存 |
| TRP | 计时拆分 | inference / preparation / reconstruction+UI / D3D11↔D3D12 交接 |
| TRP | 天气/时段覆盖、战斗降 pass、NR 热键 | 我们没有对应概念（多 pass 也没有） |

### 3.3 对照我们

我们已有的运行时参数：`Enabled/Style/UseAutoMask/UICorrection/DepthInverted/Reset`、
`Intensity/LocalTone/LocalStructure/SkinStructure`、`Hint.Render.Preset`、`ScalingRatio`、
尺寸与各 subrect、`MVecScale`。合成侧已有 gain（`Replace=5`）与 TRP ratio resolve（`Replace=6`），
以及 `transferStrength / colourStrength / maxRatio / whitePoint` 四个旋钮。

| 缺的 | 现状 | 判定 |
|---|---|---|
| 输入缩放（低分辨率跑，只取 gain） | 固定 1:1 | **最值** |
| 分屏对比 | 无 | **值得，先做** |
| 分阶段计时 | 只有总时长 | 值得 |
| 隔帧（Model rate） | 无 | 值得，第二步 |
| 自适应曝光 + EV 偏置 | 只有手填白点 | 值得 |
| grey guard / 黑=阴影 / Broad-Detail | 无 | 值得（便宜） |
| 稳态网格（未抖动输入） | 无 | 可选实验 |
| 运行时下拉 / 自动挑选 / 驱动检查 | 只有 `nr_runtime.txt` | 便宜 |
| 多 pass / 对象遮罩 / peripheral / fused | 无 | 不做，见 §3.5 |

### 3.4 值得做的项与代价

| 项 | 做法 | 代价 | 依据 |
|---|---|---|---|
| **① 输入缩放** | NR 在半分辨率建 feature（742×417），guide 一起降采样、MV scale 减半；**只取 gain** 再双线性上采样乘回全分辨率线性色 | 2–3 天 | gain 是低频信号，损失主要在细节；探针已证明 NR 接受 640×360 这类尺寸（全 `0x1`） |
| **② 分屏对比** | 我们本来就把 gain 合到自己的纹理再喂 DLSS → 只合成右半边即可，分割线可拖 | 1 天 | 几乎零成本，用来验收 ① 的画面损失 |
| ③ 分阶段计时 | timestamp heap 现成，补 encode / evaluate / composite / copy 四个 stamp | 0.5–1 天 | 回答「3.5 ms 里编码和合成占多少」 |
| ④ 隔帧 | 已有「用上一帧 NR 结果」的 1 帧延迟结构，再加 MV+depth 重投影 | 3–4 天 | 可与 ① 叠加；有鬼影风险 |
| ⑤ 自适应曝光 | 先 0.5 小时试读引擎 NGX 的 `ExposureTexture` / `Exposure.Scale`；没有再自己加亮度归约 pass | 0.5 小时 + 2–3 天 | TRP 的自适应就是「测平均亮度 + EV 偏置」 |
| ⑥ 质量修正 | grey guard、黑=阴影、Broad/Detail 全在已有合成着色器里算 | 1.5 天合计 | 风险低 |
| ⑦ 稳态网格 | encode 时按引擎 NGX 参数里的 jitter offset 偏移 UV，合成时移回 | 1–2 天 | 收益不确定（我们抖动 ≤0.5 px） |
| ⑧ 运行时选择 | 面板列出可用 `nvngx_dlssnr*.dll`，下次启动生效；按文件版本 ≥310 认，不按 hash | 0.5 天 | TRP 的 contract 判定法 |

**排期建议**

- 第一批（3–4 天）：① + ② + ③。做完 NR 应从 3.5 ms 掉到 1 ms 出头，并能用分屏判断糊了多少。
- 第二批（3–4 天）：④ 隔帧。与 ① 叠加后 1440p 的 NR 有机会 <1 ms，让「100% 内部分辨率 + NR」（现在 8.75 ms）变得可用。
- 第三批（2–3 天）：⑤ + ⑥ + ⑧ 一次收完。

### 3.5 不做，以及为什么

| 不做 | 原因 |
|---|---|
| 多 pass（2–5） | 每多一 pass 就是 +3.5 ms；我们预算比 Skyrim 紧 |
| 对象遮罩 | 要在 RenderDragon 里逐 draw 分类（手/装备/生物）+ 深度预通过重放，周级工作量，观感收益不成比例 |
| Peripheral compression / fused preparation | 前者与 ① 目标重叠且更复杂；后者是 TRP 内部准备阶段优化，我们没有对应阶段 |
| Style 3–255 自定义 | TRP 源码注释写明：**310.8 / 310.9.16 上 style ≥2 输出全部等于 Cinematic**，开放到 255 是假选项 |
| 天气/时段覆盖、战斗降 pass | 没有天气/时段概念；1 pass 也没什么可降 |
| NR 放在 DLSS 之后 | 2560×1440 上 NR ≈ 8.75 ms，四倍成本；TRP 自己也建议优先「前」。**2026-10-24 裁定：改为排期项（位置轴），见 [N](N-nr-placement-plan.md)**——成本数字仍然有效，但 RCAS 已证明引擎 `Output` 可读写（[J §9](J-sr-implementation.md#L119)），所以它不再等于「不做」 |

## 4. 一小时实验清单（零/低成本，先做掉）

| 实验 | 怎么做 | 看什么 |
|---|---|---|
| `UseAutoMask=1` | 改一个参数 | 皮肤/手是否少一点塑料感 |
| `SkinStructureStrength=-1` | 先确认 310.9.16 接受这个哨兵 | 与 0/1 的差别 |
| 引擎曝光参数 | 在 evaluate 钩子里读 `ExposureTexture` / `Exposure.Scale` | 有的话，自适应曝光直接白拿 |
| 加载界面/主菜单是否还在跑 NR | 看 evaluate 次数 + 帧时间 | 白付 3.5 ms 还污染历史，就该挂起 |
| 换 `nvngx_dlss.dll` 到 310.9.x | 拷到游戏目录，看日志 | 引擎是否接受、预设行为是否变化 |

## 5. 开放问题（要实机或探针才能定）

1. **78% / 任意比例能不能做**：q=4（Ultra Quality）在本机运行时返回 `0×0`，槽位这条路不通。
   但 docs/J 否掉 75% 时只试了 q=5、q=3——它们的动态范围装不下 1920；**q=1/q=2 的范围覆盖 1920×1080**，
   且因子表里本来就有 `0.75` 一项。候选方案：patch 因子表值 + 借 q=2 + 在尺寸回调里把 `Out` 覆盖到
   1920×1080。这**会推翻一条已定结论**，需要用户拍板再试。
2. **mip LOD 偏置在 RenderDragon 里落在哪**：D3D12 的静态采样器在根签名或描述符堆里，得先侦察
   （钩 `CreateRootSignature` 打日志，看世界 pass 与 UI pass 是否分开）。收益：低分辨率采样的细节；
   风险：UI 一起变。
3. **输入缩放的画面下限**：半分辨率够不够，只能靠 §3.4 的 ② 判断。若糊，可试 66% 或 75% 的输入。
4. **半分辨率下的 guide 契约**：一起降采样（最稳）还是像 TRP 那样用 `ScalingRatio` 让运行时处理，未验证。

## 6. 变更记录

- 2026-10-24：首版。按 TRP `origin/main`（0.5.3）读；SR 结论来自 `OverlayImagePanel.cpp` /
  `DLSSBackend.cpp` / `EngineScreenshot.cpp` / `CHANGELOG.md`；NR 结论来自
  `NeuralRenderingRuntimeContract.h` / `NeuralRenderingTuning.h` / `NeuralRenderingReconstruction.h`。
