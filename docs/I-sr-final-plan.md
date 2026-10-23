# I. SR 最终规划：控制层 → 任意分辨率 → VV 接管 TAAU

> **2026-10-24 状态：部分落地。** M1/M3 与「比例 + 模型预设 + 应用即重建」已实现，机制/证据/坑见
> [`docs/J`](J-sr-implementation.md)；**L3–L6 与 VV 迁移仍未开始**，这部分本文仍是规划权威；
> 优先级与代价见 [`docs/K`](K-trp-feature-gap.md)。

> **规划 + 落地记录。** 前情：`docs/H-sr-plan.md`（可行性 + 预设四层）、`docs/A-renderdragon-passmap.md`
> （VV/taau 白盒取证）、`docs/D-rt-dlss-nr-plan.md` §6.16/§6.19（引擎 SR 契约）。
> 本文回答三件事：① 先交付的 ④ 预设包怎么做；② "更全的控制"里**分辨率**这一维到底能控到什么程度；
> ③ 为"VV 用 DLSS 替换 TAAU"铺一条可复用、可回退的路。
>
> **实现状态（2026-10-24）**：M1/M3 已完成，M2 部分完成（比例 + 模型预设 + 应用即重建）；
> 任意比例、FrameBuilder 因子表、模型预设与重建的最终机制、证据和坑见 **`docs/J-sr-implementation.md`**。

## 0. 目标与最终形态

用户定调：先做 ① 读数 + ② 只读，最终做 ④ 预设包；但要求**更全的控制，包括"原生输出什么分辨率"**；
后面有迁移 VV、用 DLSS 替换 TAAU 的规划。

**最终形态一句话**：同一个 SR 控制核心，两种所有权——
**RT 上"改引擎的 SR"，VV 上"我们自己的 SR"**；预设、UI、计时、诊断、NR 全部共用。

## 1. 先把"分辨率"拆成三个数（不拆清就会一直对不上）

| 代号 | 含义 | 现在谁决定 | 能不能控 | 控制手段 |
|---|---|---|---|---|
| **R_in** | 内部渲染分辨率（DLSS 的输入） | RT：窗口 × DLSS 档（实测 58% = Balanced）；VV：platform tier 的 `target_resolution`（720p/1080p/1440p/2160p，**绝对目标**）× 窗口 | **能，且这是"原生分辨率"的正解** | L1 游戏设置 / L2 创建参数 / L3 optimal 回调 / L4 引擎 render-size / L6 自有 SR |
| **R_sr** | SR 的放大目标（DLSS `OutWidth/OutHeight`） | RT：就是引擎 `Output` 纹理（= R_out 尺寸）；VV：taau 的输出目标 | RT 不能单独控；VV 可加一层 blit | O2（仅自有 SR 时） |
| **R_out** | 呈现分辨率（后缓冲/交换链像素） | 窗口客户区 / 显示模式 | **能，但代价大**：UI 与鼠标坐标一起被缩放 | O1 交换链覆盖（实验级） |

**两条关键事实（新，写方案前先钉死）**：

1. `data/renderer/platform_config/pc/platform_configuration.pc.json`：5 个 tier × {Performance, Quality}，
   `target_resolution` = `720p / 1080p / 1440p / 2160p`、`upscaling_mode` = `taau / bilinear`
   ⇒ **VV 路线的内部渲染分辨率本来就是绝对目标**（再由 DRS 换成 factor），这正是"原生分辨率"的抓手；
   而 RT 路线实测是"窗口 × 58%（Balanced）"，两条路线算尺寸的机制不同。
2. 引擎 exe 里有 `DLSSOptimalSettingsCallback`（NGX 参数，`nvsdk_ngx_defs.h:721`）；NGX 的
   `NGX_DLSS_GET_OPTIMAL_SETTINGS` 是内联 helper，靠从参数表里取出这个回调再调用。
   ⇒ **如果引擎就是用它算 1485×835，那么钩这个回调就能任意改 R_in**（L3，最高性价比的一条）。

## 2. 控制手段分级 L1–L6（成本 / 收益 / 风险）

| 级 | 手段 | 能控到 | 成本 | 风险 / 代价 | 依赖 |
|---|---|---|---|---|---|
| **L1** | 游戏设置：`upscaling_percentage`（0/33/50/66/75/100）、`target_resolution`、`upscaling_mode`、`graphics_mode` | R_in 的"档位级"取值 | 0.5–1 天（面板读数 + 提示；直写要探路 Options 门面） | 低 | 无（已经能读 options.txt） |
| **L2** | 创建期参数覆盖：`DLSS.Hint.Render.Preset.*`、`PerfQualityValue` | 画质/网络预设；**不直接改尺寸** | 1–2 天 | 需把 create 包装钩子提到发布版；改字母要等重建 | `kSigCreateWrapper`（已有） |
| **L3** | 钩 `DLSSOptimalSettingsCallback` → 写回任意 `Width/Height` | **任意 R_in（像素级）** | 0.5–1 天验证 + 1–2 天落地 | 引擎若不用这个回调就不成立；尺寸必须与引擎 render target 分配同源（否则只改参数不改画布 = 花屏） | 回调地址待定位 |
| **L4** | 引擎 render-size/framegraph：`UpscalingParameters.mUpscalingFactor` / `mDynamicResolutionScale` / taau 声明的资源尺寸 | 任意 R_in，且是"真渲染"（无降采样损失） | 1–2 周 | 深，可能破坏帧图；跨版本漂移 | VV 侦察（M4） |
| **L5** | 交换链覆盖（`ResizeBuffers` 强制尺寸） | R_out | 0.5–1 天实验 | **UI 与鼠标一起缩放**、DWM 拉伸发虚；只适合"降输出" | overlay 已挂 13/39 |
| **L6** | 自有 SR（VV；或 RT 接管 create）：自建 DLSS feature、自己决定 In/Out、可插第二层 blit | R_in / R_sr / 全部 | 2–4 周 | 最大；但这就是 VV 迁移本身 | 自有 feature + bgfx 桥 |

> 结论：**"原生输出分辨率"要控的是 R_in**（L1→L2→L3→L4/L6 递进），**不是 R_out**。
> R_out 只有在"我想少出像素/让系统拉伸"时才有意义，而它的代价（UI 糊、坐标错）几乎一定不划算。

## 3. R_out（呈现分辨率）的三条路，先把话说死

| 方案 | 做法 | UI 影响 | 评价 |
|---|---|---|---|
| **O1 交换链覆盖** | 拦 `ResizeBuffers(13/39)`，把后缓冲钳到选定尺寸，让 DWM 拉伸 | UI/文字**一起被缩放**；窗口→后缓冲坐标映射要自己补 | 实验级，默认不做；只在"降输出换性能"时考虑 |
| **O2 自有 SR + 固定尺寸目标 + blit** | 只有"我们拥有 SR"时才成立（VV，或 RT 接管 create）：DLSS 写到固定尺寸目标，再 blit 到引擎的 SR 输出目标 | UI 不受影响（UI 在 SR 之后画），但 SR 图被二次缩放 | 可行，代价是一次额外全屏 blit + 发虚；VV 阶段再评估 |
| **O3 不控** | R_out = 窗口/显示模式 | 无 | **默认**：面板只读显示，并解释"4K 屏开 720p 窗口，输出就是 720p" |

## 4. 最终架构（两条路径共用一个核心）

```
Config / Presets ──► SrController（路径无关：预设、R_in/R_out 策略、Reset、锐化、自动分档）
                          │
        ┌─────────────────┼──────────────────┐
  RtEngineAdapter      RtOwnedAdapter        VvOwnedAdapter
  （现状：读写引擎参数）  （可选：接管 create）    （未来：钩 taau / 自有 pass）
        │                  │                  │
        └──────────┬───────┴──────────────────┘
              Telemetry / Dump(TEST) ─ Overlay（SR 页 / A-B 热键 / 后处理锐化）
                          │
                  NR 核心（复用；按路径切色彩域）
```

| 模块 | 职责 | 复用/新增 |
|---|---|---|
| `sr/SrController` | 预设解析、策略（比例/像素/自动分档）、Reset 计时 | 新（薄） |
| `sr/PathRT` | 现状参数读写 + L2 创建覆盖 + L3 回调 | 改（RenderProbe + D3D12Bridge 抽出） |
| `sr/PathVV` | taau 钩子、自有 feature、输入输出接管 | 新（最大） |
| `sr/BgfxBridge` | bgfx `TextureHandle` → `ID3D12Resource*`（VV 的地基，docs/A §7.3） | 新 |
| `ui/Menu` SR 页 | 读数/预设/像素滑条/A-B 状态 | 扩 |
| 诊断（TEST） | 三张图 dump（引擎输入 / SR 输出 / 呈现）、`[ngxset]` | 扩 |

## 5. 路线图（每阶段都有"能亲眼看见"的验收）

| 里程碑 | 内容 | 成本 | 验收（用户可见） |
|---|---|---|---|
| **M0** 离线参数表 | 扩 `ngx_probe`：feature 1 任意尺寸、J/K/L/M、带/不带 `0x80000000`、flags 组合、`Sharpness` 对输出统计的影响 | 1 天 | ✅ **已完成（2026-10-23，见 §10）**：参数表 + 引擎侧回调定位 + 换指针自测全通过 |
| **M1** SR 读数层 | 面板"超分"页（渲染/输出尺寸、比例、档位、预设字母、Sharpness、flags、尺寸回调统计）+「立即清除 SR 历史」按钮 | 1–2 天 | ✅ **已完成（2026-10-24，见 docs/J）**：读数、回调统计、Reset 已接线；只读区待删（换预设时会闪临时槽） |
| **M2** 预设包 v1 | 比例（L1）+ 字母（L2）+ 锐化 A/B + Reset 策略 + NR 绑定 + 窗口面积自动分档 | 3–5 天 | 🟡 **部分完成（2026-10-24）**：比例 + 模型预设（J/K/L/M）+ 应用即重建已接线；锐化不做（运行时废弃）、组合预设包未做 |
| **M3** 任意 R_in（L3） | 回调里改写 `OutWidth/OutHeight`（配置 `sr.scalePercent`，面板下拉：跟随引擎/33/50/58/66/75/100） | 2–4 天 | ✅ **已完成（2026-10-24，见 docs/J）**：档位直接生效，不需要拖窗口/F11；DLSS 槽 + FrameBuilder 因子表同时改；75% 因动态范围夹取不提供 |
| **M4** VV 侦察 | Windows `taau` 只读钩子：viewport / `UpscalingData`（mode/factor/jitterIdx/dynRes）/ `UpscalingCameraData` / 4×`BindFramebufferColor`；dump 输入三图；bgfx 桥最小闭环 | 3–5 天 | VV 模式下能看到每帧 taau 的尺寸/factor/jitter，并 dump 出真实纹理 |
| **M5** VV 写透传 | 在 taau 输出目标写纯色/拷贝 | 3–5 天 | 画面变纯色但 **UI 仍在** ⇒ 证明"pre-UI 写入"成立 |
| **M6** VV DLSS 替换 | 自有 feature（core `CreateFeature(1)`）：输入 taau 的 color/MV + 另钩的 depth + jitter；输出写回 taau 目标；先"TAAU 照跑再覆盖"，后"跳过 TAAU" | 1–2 周 | VV 下 A/B 热键：TAAU ↔ DLSS；两侧 GPU 时间与观感都有数 |
| **M7** 统一收尾 | 预设贯穿两路径；R_in 策略统一（VV 的 `target_resolution` vs RT 的 L3）；稳定性与回退 | 1 周 | 同一套预设 UI 在两条路径都工作；失败自动回退引擎原路径 |
| **M8**（可选）RT 接管 create | 不转发引擎 create、返回我们的 handle：预设热切、统一所有权 | 3–5 天 | 改字母**立刻**生效，不用等重建 |

依赖关系：M0→M1→M2 可先交付；M3 独立（只依赖 M0/M1）；M4→M5→M6 是 VV 线；M8 只在"必须热切"时做。

## 6. 先做这几个实验（决定后面所有成本）

| 实验 | 做法 | 耗时 | 决定什么 |
|---|---|---|---|
| **K1** RT 下 `upscaling_percentage` 生不生效 | 游戏内把超分百分比改 33/50/66/75/100，读 `[dlss] w= h=` | 30 分钟 | L1 是否是 RT 的比例入口 |
| **K2** `target_resolution` 语义 | 改 `options.txt` 的 `target_resolution`（0/1/2/3）→ 看 R_in 是否变、是否脱离窗口 | 30 分钟 | 是否存在"绝对内部分辨率"选项（VV 尤其） |
| **K3** optimal 回调 | 钩 `DLSSOptimalSettingsCallback`：打印调用/入参/出参；改成固定尺寸看引擎是否按它分配 | 0.5–1 天 | **L3 成不成立** —— 离线已证引擎会读并调用它（§10.3），只剩"调用时机/是否真改渲染目标"需进游戏确认 |
| **K4** VV 插入点是否活着 | `upscaling_mode=0`（Taau）+ `graphics_mode=2`（VV）→ taau 命中；VV 下 NGX 是否已 Init | 0.5 天 | M4 是否值得开工 |
| **K5** taau 输出目标 | 钩子打印输出目标的尺寸/格式、后续 UI 是否在其后 | 0.5 天 | M5 的"写哪里" |
| **K6** bgfx 桥 | 钩 bgfx D3D12 纹理创建，拿到 1 张 `ID3D12Resource*` | 1 天 | M6 的地基 |
| **K9** 窗口化边界 | 拖动/缩放窗口时引擎重建 SR 的频率、最小化/遮挡时参数 | 0.5 天 | 消抖与重建上限的设计 |
| **K8**（实验级）交换链覆盖 | 拦 `ResizeBuffers` 钳尺寸，看 DWM 拉伸与 UI | 0.5 天 | R_out 要不要保留"实验开关" |

## 7. 风险登记（按"会不会推翻计划"排序）

| 风险 | 影响 | 应对 / 退路 |
|---|---|---|
| 引擎不用 optimal 回调（K3 失败） | 任意 R_in 只能走 L4/L6，成本 ×5 | 退到 L1（6 档）+ VV 的 `target_resolution` |
| VV 没有符号、bgfx 桥/深度源难啃 | M4–M6 可能超预算 | M4 侦察 1 周后设 stop/go；失败则 VV 只做"读数 + 提示" |
| 写 taau 输出目标被后续 pass 依赖 | 画面花/崩溃 | 先"TAAU 照跑再覆盖"（M5），不成立就放弃写入 |
| NGX 在 VV 下自 Init 与引擎冲突 | 可能影响切回 RT | 只在引擎没 Init 时 Init；切模式要求重启 |
| 强制 R_out | UI 糊、鼠标错位 | 默认不做（O3），只留实验开关 |
| DLSS 在 VV 尺寸下不比 TAAU 快 | 收益只剩画质 | M6 先量时间，再决定是否默认开 |
| 版本漂移（签名/回调） | 每个补丁都要重定位 | 全部签名走 `kSig*` + 启动自检，失败回退引擎原路径 |

## 8. 需要你拍板的点

1. **"原生输出分辨率"你指的是哪个**：(a) 内部渲染像素（R_in，DLSS 输入）—— 我建议按这个做；
   (b) 呈现/后缓冲像素（R_out）—— 只能 O1/O2，代价是 UI 糊或二次缩放。两个都做也行，但 (b) 默认关。
2. **任意 R_in 的路线**：先赌 L3（回调，1–2 天见分晓），失败再谈 L4/L6？还是直接等 VV 的 `target_resolution`？
3. **VV 迁移的时间**：接受 2–4 周（M4–M6）吗？还是先只做 M4 侦察、看完数据再决定？
4. **M8（RT 接管 create）**要不要排：它换来"预设热切"，但要动 create 所有权。
5. 预设的默认语义：以"内部像素"（1280×720 / 1440p）为主，还是"比例 + 窗口自动分档"为主？

## 9. 估算汇总

| 交付 | 内容 | 天数 | 风险 |
|---|---|---|---|
| P1 | M0 + M1 + M2（预设包 v1） | 5–8 | 低 |
| P2 | M3（任意 R_in，若 K3 成立） | 2–4 | 中 |
| P3 | M4–M6（VV DLSS 替换） | 15–25 | 高 |
| P4 | M7 统一 + M8 可选 | 6–10 | 中 |
| — | K1–K9 实验 | 4–6 | 低 |

## 10. M0 结果（2026-10-23 实测，只读）

工具：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.exe` 新增 `--sr` 模式（本机 D3D12 设备 + 驱动里的
NGX core，不碰游戏、不挂钩、不写引擎）。用法：

```
ngx_probe.exe --sr --pathlist="E:\LeviLauncher\versions\1.26.51.01" --out=2560x1440,1920x1080,3840x2160
```

### 10.1 DLSS SR 的 optimal settings 回调（core 参数表 slot 8）

- `SuperSampling.Available=1`、`NeedsUpdatedDriver=0`。
- `DLSSOptimalSettingsCallback` 存在，**宿主模块 = 游戏目录的 `nvngx_dlss.dll`**。
- 直接调用它（等同 `NGX_DLSS_GET_OPTIMAL_SETTINGS` 的内联逻辑）得到的渲染尺寸：

| display | q=0 MaxPerf | q=1 Balanced | q=2 MaxQuality | q=3 UltraPerf | q=4 UltraQuality | q=5 DLAA |
|---|---|---|---|---|---|---|
| 2560×1440 | 1280×720 (50%) | **1485×835 (58%)** | 1707×960 (66.7%) | 853×480 (33.3%) | 0×0（不支持） | 2560×1440 |
| 1920×1080 | 960×540 | 1114×626 | 1280×720 | 640×360 | 0×0 | 1920×1080 |
| 3840×2160 | 1920×1080 | 2227×1253 | 2560×1440 | 1280×720 | 0×0 | 3840×2160 |

- 动态范围（同一回调输出）：`DLSS.Get.Dynamic.Min/Max.Render.*` = 50% .. 100%（q=3 时 min=max=33%）。
- **`1485×835` = 该回调在 2560×1440 / Balanced 下的输出**，与引擎实测完全一致 ⇒ 引擎的渲染尺寸就是它算的。

### 10.2 `CreateFeature(1)` 的接受度（core 创建）

| 项 | 结果 |
|---|---|
| 尺寸 | 845×475 / 1280×720 / **1485×835** / 1690×950 / 1920×1080 / 2560×1440 **全部 ret=1** |
| 预设提示 | `0 / 5(E) / 6(F) / 10(J) / 11(K) / 12(L) / 13(M)` 与引擎风格的 `0x8000000B/0C/0D` **全部 ret=1** |
| 结论 | 任意比例（含非标准 58%）与任意预设字母都能建 feature；**尺寸不是障碍，障碍只在"谁决定尺寸"** |

### 10.3 引擎侧只读定位（不用开游戏）

对 `Minecraft.Windows.exe` 做 PE 字符串交叉引用（脚本 `tools/find_optcb.py`）：

- 字符串 `DLSSOptimalSettingsCallback` @ RVA `0xec43368`，**全文件只有 1 处引用**：
  `base+0xdfc6da1`（`lea rdx, [rip+…]`），位于函数 **`base+0xdfc6c00`（size 0x534）**。
- 该函数的流程（逐条对上字符串）：

```
[rsi+0x3c56408] = NGX 参数对象
GetVoidPointer(params, "DLSSOptimalSettingsCallback", &cb)   ; base+0xe413860
if (!cb) -> 走 "Upscaling Mode: Fallback"（渲染尺寸 = 输出尺寸）
SetUI(params, "Width",  displayW)        ; base+0xe413680
SetUI(params, "Height", displayH)
SetUI(params, "PerfQualityValue", q)     ; base+0xe4136e0
SetUI(params, "RTXValue", 0)
call cb（经 CFG 跳板 base+0xe585660 = jmp rax）
if ((ret & 0xfff00000) == 0xbad00000) -> Fallback
读回：OutWidth / OutHeight / DLSS.Get.Dynamic.Max|Min.Render.Width|Height / Sharpness
存进配置对象：+0x3c56434 = 渲染宽、+0x3c56438 = 渲染高、+0x3c5641c = Sharpness、+0x3c56418 = 档位
```

⇒ **引擎自己的"上采样设置"函数就是这么算渲染尺寸的**：回调给多少，它就存多少，存进配置对象的渲染尺寸字段
（`+0x3c56434/38`，后续按它建渲染目标——这一步仍待进游戏确认）。这正是 L3 的落点，而且**不需要 hook 引擎代码**：
把参数对象里那个函数指针换成我们的（`SetPtr` 槽 1 即可，和写 `Color` 同一套机制），引擎下次调它就会拿到我们的尺寸。

### 10.4 换指针自测（离线已跑通）

`ngx_probe --sr` 里做了同样的动作：读原回调 → 用自己的 wrapper 替换 `DLSSOptimalSettingsCallback` →
再按引擎的顺序调一次：

```
原回调 1485×835  →  替换后 1280×720（wrapper 调原函数后改 OutWidth/OutHeight），ret=0x1
```

⇒ 机制成立；剩下的只有"引擎何时调它"（启动 / 改分辨率 / 改画质 / 每帧）与"改了之后渲染目标是否真的变小"，
两者都必须在游戏里用**只读日志**确认（下一步 K3-in-game）。

### 10.5 首次实机（2026-10-23 19:33，窗口化 2560×1369）

面板「超分」页读到的值（RT 开启、进存档后）：

| 项 | 值 |
|---|---|
| 渲染尺寸 / 输出尺寸 | 1485 × 794 / 2560 × 1369 |
| 比例 | 58%（= Balanced） |
| 网络预设 | K（0x8000000B） |
| 创建标志 | 0x00000003（IsHDR \| MVLowRes） |
| 回调次数 | **2**（进入世界 / 改窗口尺寸各一次） |
| 回调入参 | 2560 × 1369 |
| 回调返回 | 1485 × 794，Sharpness 0.35 |
| 动态分辨率范围 | 1280 × 685 .. 2560 × 1369（50%..100%） |

实测行为：

- **改分辨率（拖窗口/切尺寸）会让回调次数 +1** ⇒ 引擎重新算尺寸时确实走这个回调，L3 的"读取"侧闭环；
- 游戏内**没有**可改的超分档位（L1 在 RT 路线不可用）⇒ 渲染比例只能由我们这个回调覆盖来给；
- 「立即清除 SR 历史」按下只闪一下（写了一帧 `Reset=1`），**不触发回调**（预期：回调只负责尺寸，历史是另一条键），
  观感上没有可辨的提升 ⇒ 它的定位只是"切视角/传送后擦一次历史"，不是画质开关。

结论：**R_in 的可控性吃在"什么时候触发一次重建"上**。当前可用触发 = 改窗口尺寸。
2026-10-23 晚曾加过「应用并重建」按钮（窗口 +1 px 再恢复，借引擎 resize 触发重建），实机表现为**一次明显的
卡顿**（两次 swapchain 重建 + NR 跟随），用户要求不要用这个方法，已整段移除。现在的触发方式就是用户自己在用的：
**F11 切一次全屏**（每次切换会产生两次回调）或拖一下窗口边缘。
另：同一实机里把比例改成 100% / 50% 都生效了（渲染目标按回调返回的尺寸重建），但 50% 没有换来更高帧率
（NR 8.75→2.63 ms，帧时间反而 35.3→36.7 ms）⇒ 这个场景是**分辨率无关的瓶颈**（CPU/固定开销为主），
需要同地点三档对比才能定性。

### 10.6 对计划的影响

| 结论 | 影响 |
|---|---|
| 引擎确实读并调用 `DLSSOptimalSettingsCallback` | **L3 从"待验证"升为"高置信"**：任意 R_in 的实现路径 = 换指针 + wrapper（无需 MinHook、无需 hook 引擎函数） |
| 回调还提供 `Sharpness` | 引擎把回调返回的 Sharpness 存起来（`+0x3c5641c`）⇒ 我们**顺手也能控制引擎使用的锐化值**（是否真生效仍取决于 DLSS 的 deprecated flag，需 A/B） |
| 任意尺寸 / 任意预设都能建 feature 1 | M3 只需解决"尺寸来源"，不用再担心 feature 创建失败 |
| 引擎的 fallback 分支存在 | 我们的 wrapper 若抛错/返回异常，引擎会退回"输出尺寸=渲染尺寸"（不会崩）⇒ 风险可控 |

## 11. 覆盖尺寸的上限：只能往下、且未必是真降分辨率（2026-10-23 实机）

用户在「超分」页把比例设到 66/75/100 时，**画面下半部分变黑**；设 33/50 时没有黑，但"看起来正常"
并不代表真的降了分辨率。原因（几何上对得上）：

- 引擎实际渲染的矩形仍由它自己的上采样因子（58%）决定，我们改的是**交给 DLSS 的尺寸**
  （`Width`/`Height`、子矩形、`MV.Scale`）；
- 比例 > 58% 时 DLSS 去读引擎没画过的区域 ⇒ 下半（以及可能右侧）黑；
- 比例 < 58% 时**经用户实机确认"没有被裁掉"**（58% 与 33% 内容一致、FOV 不变）⇒ 引擎向下确实按我们
  返回的尺寸重建了渲染矩形，这是**真实的渲染分辨率下降**，不是裁切。

⇒ 结论：**"内部分辨率"向下（33/50/58）是真的，可用；向上被引擎自己的 58% 上限卡住**（它的渲染矩形/因子
上限就在 58%，我们只改了交给 DLSS 的尺寸）。要突破 58% 得改引擎自己的渲染矩形来源
（`UpscalingData`/`mUpscalingFactor`/DRS，L4，1–2 周、中风险），但收益只是"更高的内部分辨率、更高的开销"，
不建议为它立项；把 58% 当最高档即可。

因此面板保留「跟随引擎 / 33 / 50 / 58」，>58% 移除；触发方式 = F11 切一次全屏或拖窗口边缘。

## 12. 参考

- `docs/H-sr-plan.md`（SR 四层、窗口化、S1–S4）；`docs/A-renderdragon-passmap.md` §3（taau 契约）、
  §7.1（taau 不采样深度）、§7.3（bgfx 句柄→D3D12 资源）、§7.4（`upscaling_mode` / `target_resolution`）。
- `data/renderer/platform_config/pc/platform_configuration.pc.json`（tier → `target_resolution` / `upscaling_mode`）。
- NGX：`nvsdk_ngx_defs.h:721`（`DLSSOptimalSettingsCallback`）、`:70-88`（J/K/L/M）、`:281-294`（flags）；
  TRP `src/DLSSBackend.cpp:285-295`、`src/DLSSFeatureParameters.h:30-45`。
- 本项目代码：`probe/RenderProbe.cpp:408/548/581`、`nr/D3D12Bridge.cpp:1657-1664/2194-2260`、`ui/Menu.cpp`。
