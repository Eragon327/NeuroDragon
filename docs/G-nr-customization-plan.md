# G. NR 客制化选项规划（对齐 TRP 的 NR 面板）

> **2026-10-24 状态：已被 [`docs/K`](K-trp-feature-gap.md) 取代**（K 按 TRP 0.5.3 读，覆盖 SR + NR）。
> 本文保留的价值是逐项「为什么能做 / 为什么做不到」的推理，以及和 TRP 0.4.5 面板的对应关系。

> **仅规划，未改任何代码。**
> 判断依据全部对着两边的源码：TRP 在 `D:\Documents\theosrp`（0.4.5），NR 在 `src/neurodragon/nr/D3D12Bridge.cpp`。
> 「能做」= 我们有现成管道，只差配置字段/UI/接线；「做不到」= 结构性缺输入或必须接管引擎独占的资源。

## 0. 一句话结论

TRP 的 NR 面板约 **28 个选项**，按实现机制可以拆成六类，我们的处境完全不同：

| 类别 | TRP 有多少 | 我们的处境 | 结论 |
|---|---|---|---|
| ① NR 网络参数（NGX 参数写值） | 7 | 4 个已暴露，`SkinStructure` 写死，`AutoMask/UICorrection` 缺输入纹理 | **补 1 个；另 2 个做不到** |
| ② 合成/色调（TRP 自家 shader） | 6 | shader 已写、旋钮已存在，但 `Replace=6` 没接线 | **接 2 行线就全活** |
| ③ 输入重采样（子矩形 + 降分辨率） | 3 | 只做过 1:1 | **可做 1 个（1–2 天）；另 2 个不值得** |
| ④ 需要额外输入纹理 | 2 | 没有 ControlMask / UI / UIAlpha | **做不到** |
| ⑤ 需要游戏世界状态 / 物体分类 | 5 | MC 没有对应概念 | **做不到** |
| ⑥ 必须独占最终图像 | 2 | 引擎拥有 SR、UI 合成与交换链 | **做不到** |

**所以真正的问题不是"选项不够"，而是三件事**：`Replace=6` 没接线（一整组旋钮是死的）、几个参数写死（Skin / 掩码 / 网络预设）、面板上没有"我到底改了什么"的回读。前两件都是半天内的事。

## 1. TRP 的 NR 选项全表（含实现机制）

| # | 选项 | TRP 里的实现 | 需要什么 |
|---|---|---|---|
| 1 | Style 0–7 | `DLSSNR.Style` | 参数写值 |
| 2 | Intensity 0–2 | `DLSSNR.Intensity` | 参数写值 |
| 3 | Local tone 0–2 | `DLSSNR.LocalToneStrength` | 参数写值 |
| 4 | Local structure 0–2 | `DLSSNR.LocalStructureStrength` | 参数写值 |
| 5 | Skin structure −1–2 | `DLSSNR.SkinStructureStrength` | 参数写值 |
| 6 | Automatic skin mask | `DLSSNR.UseAutoMask` + `ControlMask` 输入 | 参数 + **物体分类纹理** |
| 7 | UI correction | `DLSSNR.UICorrection` + `UI/UIAlpha` 输入 | 参数 + **UI 纹理** |
| 8 | NR input resolution 25–100% | `DLSSNR.Width/Height` + 子矩形 + 自家降采样 | 子矩形 + 重采样 |
| 9 | Network preset（Default / 上一版） | 创建参数 `DLSSNR.Hint.Render.Preset` | 参数写值（创建时） |
| 10 | Placement（放大前 / 放大后） | 两个 stage 尺寸各跑一遍 | **最终图像所有权** |
| 11 | Sharpening on/off + 强度 | TRP 自己后处理 RCAS | 后处理 pass |
| 12–14 | 单趟战斗/拔武器 + 返回延迟 | 两趟 NR 的降载策略 | **两趟 NR + 游戏状态** |
| 15 | Peripheral compression（中 80%/边 90%） | 改 UV 映射 + 自家 prepare | 重采样管线 |
| 16 | Combined preparation | 合并自家两条 prepare pass | 重采样管线 |
| 17–22 | Method（Auto/Residual/Ratio）、效果强度、色度强度、最大亮度比、HDR 白点、输入是线性 HDR | TRP 自家 resolve shader | shader 常量 |
| 23 | 对象遮罩（手臂/装备/人形/生物 × 0–1） | 捕获不透明光照绘制 + 分类 + 合成 | **几何重放 + 分类** |
| 24 | Runtime DLL 选择 | 自己的 `Init` 带 `PathListInfo` | 自己的 NGX 会话 |
| 25 | 天气/时段预设 + 平滑过渡 + 可分享文件 | `[Appearance]` + 世界状态 | **Skyrim 天气系统** |
| 26 | NR 开关热键 `[` / `]` | 键钩子 | 键钩子 |
| 27 | NR on/off | — | — |
| 28 | GPU 计时 / frame trace | 计时器 | — |

## 2. 我们这边的现状（对照）

**已有、且是同一套 NGX 契约**（所以 TRP 的调参能直接抄）：

- 参数对象写值：`paramSetPtr/F32/I32/U32`（vtable 槽 1/3/4/6/11），创建参数在 `D3D12Bridge.cpp:1659-1664`，每帧参数在 `2147-2259`。
- 已暴露：`enabled`、`replace(0/5/6)`、`gainBoost`、`style 0–2`、`intensity`、`localTone`、`localStructure`、`depthInverted`、`mvecScale`、`nrEncode`、`whitePoint`、`transferStrength`、`colourStrength`、`maxRatio`（`Menu.cpp:441-517`，`Config.h`）。
- 编码 shader：4 个模式（0 Reinhard+γ2.2 / 1 / 2 增益 / 3 TRP shoulder+sRGB），`D3D12Bridge.cpp:713-833`。
- 合成 shader：mode 2 = 增益合成，mode 4 = TRP Ratio（OkLab 迁移 + 亮度重基准），`D3D12Bridge.cpp:2035-2051`。
- 任意分辨率：先建后拆 + 子矩形，`D3D12Bridge.cpp:1790-1869`。
- 进世界重置：`nrNotifyLevelLoaded()`（`D3D12Bridge.cpp:2111`，监听在 `probe/RenderProbe.cpp:367-377`）。
- 面板 / `ll::config` / i18n（`src/lang/{zh_CN,en_US}.json` + `scripts/collect_glyphs.py`）。

**写死或没接线的地方（这就是"选项少"的全部原因）**：

| 位置 | 现状 | 后果 |
|---|---|---|
| `D3D12Bridge.cpp:2428, 2450` | 只有 `replace == 5` 才调 `compositeGain` | **`Replace=6` 选了也不上屏**，`whitePoint/transfer/colour/maxRatio` 四个旋钮全是死的（面板 hint6 已承认） |
| `D3D12Bridge.cpp:2259` | `SkinStructureStrength` 硬编码 `1.0` | TRP 的 `−1 = 跟随 local structure` 用不了 |
| `D3D12Bridge.cpp:2240-2241` | `UseAutoMask=0`、`UICorrection=0` | 对应 TRP 第 6、7 项 |
| `D3D12Bridge.cpp:2179-2192` | `UI/UIAlpha/ControlMask` 传空、子矩形清零 | 没有这些输入，上面两个参数写了也无意义 |
| `D3D12Bridge.cpp:1660-1661` | `preset=0`、`ScalingRatio=1.0` 固定 | 第 9 项（Network preset）不能选 |
| `Menu.cpp:467-468` | Style 只有 0/1/2 | 310.x 契约允许 0–7，但 §6.15 实测 0–2 无差别 |
| `D3D12Bridge.cpp:2147-2148` | `useEncoded` 恒真（5/6 模式） | 不能 A/B「原始线性 HDR 直接喂 NR」 |

## 3. 能做的（三档）

### T1 — 接线与暴露（预计半天，无新管线，风险低）

| 项 | 改哪里 | 用户能看到什么 |
|---|---|---|
| **T1.1 接通 `Replace=6`** | 两处 `== 5` 改成 `5 \|\| 6`（`2428`、`2450`）；hint6 文案同步 | 「TRP Ratio」这个模式从"等同只求值"变成真的另一种上屏观感；四个 Ratio 旋钮开始起作用 |
| **T1.2 Skin structure** | `Config.h` 加 `skinStructureStrength=1.0f`；`2259` 用配置；面板加滑条 −1..2 | 皮肤/人脸区域的 NR 强度，−1 = 跟随 local structure |
| **T1.3 Style 扩到 0–7** | `Menu.cpp:467-468` 改两个数组；tooltip 注明"0–2 实测无差别，3–7 未验证" | 8 档可选；**先 A/B，别当卖点** |
| **T1.4 Network preset** | `Config.h` 加 `networkPreset`，`1660` 用配置；标注"需重建 feature（保存为默认后重进世界或改分辨率）" | Default / 上一版网络；是否真切换待验证 |
| **T1.5 输入域开关** | 把 `useEncoded` 拆成「原始 / Reinhard / TRP」三档（配置已有 `nrEncode`，加一个"不编码"取值） | 能亲手对比"编码 vs 原始 HDR 直接喂"的亮暗与噪点 |
| **T1.6 热键** | 复用 `InputGuard` 的两个键钩子（面板关闭时按键照常下传）：NR 开/关、**立即重置 NR 历史**（写一次 `DLSSNR.Reset=1`）；键码进 `config.ui` | 游戏中不用开面板就能 A/B 和擦历史 |
| **T1.7 参数回读** | 诊断页显示本帧实际写入的 `Style/Intensity/Local*/Skin/Reset/输入域/合成模式` | 调参不再是"盲按"，也能一眼看出哪档没生效 |

> 成本提醒：每个新选项 = 1 个配置字段 + 中英各 1 条文案 + 重跑 `scripts/collect_glyphs.py`（`Glyphs.h` 会变）+ 一次 `--test=y` 构建自检。

### T2 — 新处理选项（预计 1–3 天，要实机 A/B）

| 项 | 做法 | 代价 | 风险 |
|---|---|---|---|
| **T2.1 NR 输入分辨率 50–100%** | feature 尺寸 = 渲染尺寸 × scale，`Color/Output` 子矩形同步缩小；编码 pass 降采样写进去；合成 pass 上采样读出来。TRP 走的正是这条路（`NeuralRenderingReconstruction.h:56-75`） | 1–2 天 | 中：子矩形/视口错一个数就花屏；省的时间是平方关系（70% ≈ 一半 GPU 时间） |
| **T2.2 编码白点 / 伽马可调** | 把 `kEncodeHlsl` 里写死的 γ `2.2`、shoulder 的 `0.75`/系数提成常量（`spare` 字段现成），并让 `whitePoint` 在 Reinhard 档也生效 | 半天 | 低；主要影响 mode 3/4（TRP 编码 + Ratio） |
| **T2.3 自动重置策略** | 进世界已有；再加「切维度 / 传送 / 视角切换」触发一次性 `DLSSNR.Reset=1`。LL 的 event 目录里**没有维度事件**，可行做法是低频轮询 `Dimension::getDimensionId()`（`mc/world/level/dimension/Dimension.h:438`）+ 玩家坐标跳变（需确认取坐标的入口） | 1 天 | 低-中：判错=多擦一次历史（闪一下） |
| **T2.4 场景档案（维度/昼夜）** | 3–4 组参数（主世界白天/夜晚、下界、末地）自动套用；数据源可用 `Dimension::getDimensionId()` + `Level::getTime()`（`mc/world/level/Level.h:1867`，按 24000 取模即昼夜） | 1–2 天 | 中：会和"保存为默认"的语义打架（档案改了要不要落盘） |
| **T2.5 性能保护** | 帧时间中位数超阈值时自动把 NR 降到更低分辨率档（配合 T2.1） | 半天 | 中：来回切档会闪 |
| **T2.6 引擎 DLSS 锐化覆盖**（可选项，属于超分不属于 NR） | TRP 的锐化是自家的 RCAS pass（`DLSSBackend.cpp:464`）；我们最便宜的等价物是每帧往引擎参数对象写 `Sharpness`（引擎只在创建时写 0.35，见 §6.16 参数表） | 半天 | 低：写错时机就会和引擎打架，需先确认写在引擎 create 之后 |

### T3 — 评估后再定

| 项 | 说明 |
|---|---|
| 两趟 NR | 技术上可行（探针实测 3 个 feature 可同时存活），但每趟 3.5 ms ≈ 再吃 29% 帧预算；只适合"截图/暂停"场景 |
| 运行时选择（310.8 / int8） | core 只按固定名 `nvngx_dlssnr.dll` 从 exe 目录加载（§6.17 实测 A–G 组），面板最多做到"换文件 + 提示重启" |
| NR 跑在输出分辨率 | 要接管引擎 DLSS 的输出与 UI 合成/present，等于另一条路线；成本 4 倍，收益未知 |

## 4. 做不到 / 不该做的（结构性，逐条给理由）

| 选项 | 为什么不行 |
|---|---|
| **Placement：放大后跑 NR** | 引擎拥有 SR 与最终图像；放大后跑 NR 就得接管 `EvaluateFeature` 的输出、UI 合成与 present。MC 里 NR 只能住在「Color → 引擎 DLSS」之间 |
| **对象遮罩（手臂/装备/人形/生物）** | TRP 要钩不透明光照绘制、重放几何、按物体分类、再做一次分组合成（`NeuralObjectHooks.cpp` / `NeuralObjectMask.h:43`）。MC 的 draw command 流是另一套，工程量级不属于"选项"；而 NR 在 MC 的增量只有 1.5–2.5%（§6.15），投入产出不成立 |
| **Automatic skin mask / UI correction** | 需要 `ControlMask` / `UI` / `UIAlpha` 输入纹理；我们显式传空并清零子矩形（`2179-2192`）。写了参数也只是假开关 |
| **天气/时段预设 + 可分享文件** | MC 没有 Skyrim 的天气系统与社区预设生态；只能退化成 T2.4 的维度/昼夜，做不到 TRP 那种"社区共享 .wt 文件" |
| **Peripheral compression / Combined preparation** | 是 TRP 两条自家 prepare pass 的优化；我们只有一条编码 pass，没有可合并的对象。改 UV 映射换 19% 像素还会引入边缘伪影 |
| **单趟战斗 / 拔武器 / 返回延迟** | 前提是两趟 NR，且需要 SKSE 那边的战斗状态；MC 没有对应概念 |
| **HDR 输出 / 输入是线性 HDR 的显示链** | 输出链（交换链 + UI 合成）不归我们，且 NR 结果最终要塞回引擎的 SDR 链。TRP 的 HDR10 在这里没有落点（我们只有"线性 HDR → 显示域编码"这一层） |
| **帧生成 / MFG / XeSS / XeFG** | 都是不同 NGX feature 或别家 SDK；MC 的 SR 也被引擎独占，塞不进去 |
| **Runtime 内核 patch（`.trpk`、INT8、MFG 解锁）** | TRP 自己逆向并 patch `nvngx` 内核；法律与稳定性风险都高，不在本项目范围 |

## 5. 建议顺序

1. **T1.1**（2 行）→ 立刻进游戏对比 mode 5 与 mode 6 的观感。这一步决定后面所有 Ratio 旋钮值不值得做。
2. **T1.2 / T1.5 / T1.6 / T1.7**（半天）→ 面板变成"能调、能看见生效、能一键 A/B"。
3. **T1.3 / T1.4** → 带保留意见地暴露（实测无差别的就写清楚）。
4. **T2.1（降分辨率跑 NR）** → 唯一有明确性能收益的一项；先单独量 70% 档的 GPU 时间与边缘质量。
5. T2.2 / T2.3 → 观感与稳定性收尾。
6. T2.4 / T2.5 / T3 → 等前面有了结论再说。

## 6. 需要你拍板的点

1. **先做 T1（半天，把死旋钮接活、补齐缺的参数），还是直奔 T2.1（降分辨率跑 NR，能省 GPU 但要冒花屏风险）？** 我建议先 T1。
2. **默认值用 mode 5（增益合成）还是 mode 6（TRP Ratio）？** 这需要你进游戏看，我看不到画面。
3. **Style 3–7 要不要暴露？** 实测 0–2 无差别，暴露了可能是假旋钮；我倾向于"先验证再暴露"。
4. **场景档案（维度/昼夜）要不要做？** 它最像 TRP 的"自动化"，也是唯一要读世界状态、会和"保存为默认"语义打架的一项。

## 7. 需要先量、再决定做不做的未知

| 未知 | 怎么量 | 决定什么 |
|---|---|---|
| mode 6（Ratio）与 mode 5（增益）哪个好看 | 同一地点，各拍一局，看高光/暗部/色彩 | 默认值 + 文案 + 是否保留 gainBoost |
| Style 3–7 是否有可见差别 | 把 `sweep=1` 的取模从 `3` 改成 `8`（`D3D12Bridge.cpp:2229`），逐窗 dump | 是否暴露成 UI |
| Network preset 0/1 是否真切换网络 | 创建时换 preset，比帧时间与观感 | T1.4 是否值得做 |
| 输入域（编码 / 原始 HDR） | 面板切三档，同帧比 | T1.5 的默认值 |
| 降分辨率 NR 的边缘质量 | 70% / 85% 各测一局 | T2.1 是否值得做 |
| `UICorrection` 在 MC 是否有 UI 输入 | 看 Color 里是否含 UI（dump 一帧 UI 开/关） | 确认"做不到"这条结论 |

## 8. 参考

- TRP：`src/OverlayNeuralPanel.cpp`（面板）、`src/FrameGen/NeuralRenderingRuntimeContract.h`（参数契约）、`NeuralRenderingReconstruction.h`（输入缩放/合成口径）、`NeuralRenderingPassSettings.h`（第二趟）、`package/SKSE/Plugins/TheosRenderPipeline.ini`。
- 本项目：`src/neurodragon/nr/D3D12Bridge.cpp`、`src/neurodragon/Config.h`、`src/neurodragon/ui/Menu.cpp`、`docs/D-rt-dlss-nr-plan.md` §6.14–6.20。
