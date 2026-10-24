# N. NR 位置轴：Before upscaling / After upscaling

> **来源**：用户 2026-10-24 口述。原文要点：
> 「只做 `[SourceDLSSG] NRBeforeUpscaling` 的两档（面板 Placement 里的 Before upscaling 和
> After upscaling）」；「前 NR：NR 在 DLSS/CS 放大之前、按渲染分辨率处理世界色，结果拷回给放大器当输入。
> 最终图像仍归放大器所有，UI 在放大之后叠加」；「后 NR：NR 在放大之后、按输出分辨率处理放大后的世界色，
> 结果直接就是最终画面；原生 UI 在 NR 之后叠加，帧生成的 HUD-less 实时帧也取自 NR 输出，等于 NR 接管了
> 最终图像」；「这两个，其他不用」。
>
> **效力**：本文管「位置轴」这一条的路线、判据与验收。实现落地后：NR 契约与实测 → 写回
> [D](D-rt-dlss-nr-plan.md)；功能对照状态 → [K](K-trp-feature-gap.md)；阶段完成度 → [M](M-roadmap.md)；
> 执行约定沿用 [L §0](L-nr-remaining-plan.md#L8-L22)。
>
> **状态（2026-10-24）**：路线已拍板；**位置轴相关代码零改动，P0 未开工**（同工作树里另有 agent 在改
> `ui/` 的截图提示，与本条无关）。按用户裁定，上一版规划里的其它项
> （RCAS/截图的实机验收、ToneBands 保真、白点回读、换 `nvngx_dlss.dll`、78%、mip LOD 偏置）**全部不排**；
> 其中只有「After 档与 RCAS 的先后顺序」会在 P2 处理。

## 0. 一句话

把「NR 站在放大器的哪一侧」做成可切换的两档：**Before upscaling**（现状，NR 处理渲染分辨率的
`Color`，结果进放大器）与 **After upscaling**（新增，NR 处理输出分辨率的 `Output`，结果写回
`Output`、成为调色与 UI 之前的最终 HDR 图）。两档互斥，同一帧只跑一个。

## 1. 两档的语义（用户定义 → 我们的落点）

| | 前 NR（Before upscaling，现状） | 后 NR（After upscaling，新增） |
|---|---|---|
| 输入 | 引擎 `Color` 1485×835 线性 HDR（峰值实测 ~69） | 引擎 `Output` 2560×1440 线性 HDR（引擎 DLSS 刚写完） |
| NR 工作尺寸 | 渲染分辨率（可 ×50/66/100，见 L 批 2.1） | **输出分辨率**（默认 100%） |
| guide | `Depth`/`MVec` 1485×835，`DLSSNR.MVecScaleX/Y` = 渲染尺寸 | 同左（**未知：guide 比 color 小，运行时接不接受**，见 §3 P0） |
| 结果去向 | 合成后把引擎参数对象的 `Color` 指针指向我方纹理 | 合成后 `COPY_DEST` **写回引擎 `Output`** |
| 之后谁接手 | 引擎 DLSS 放大 → 调色 → UI 合成 | 引擎调色 → UI 合成（**不再放大**，NR 输出即最终 HDR 图） |
| UI 被 NR 处理吗 | 不被（UI 在放大之后合成） | 不被（同理，两档一样） |
| 成本（1440p） | 3.5 ms @1485×835；66% ≈1.5、50% ≈0.9（L 批 2.1 的**预期**值） | ≈9.6 ms @100%（网络 8.75 实测 + 编码/合成/写回 ≈0.8）；66% ≈4.6、50% ≈3.0（估算） |

两点需要说清：

- **「拷回」的等价物**：前 NR 现在没有真的拷回放大器输入，而是把引擎参数对象的 `Color` 换成我方纹理
  （[D §6.16](D-rt-dlss-nr-plan.md#L518) 的 Route A）。对放大器是同一件事，且不碰引擎资源——已验证，不改。
- **「接管最终图像」的边界**：Bedrock 的调色与 UI 合成在 SR **之后**（[J §9](J-sr-implementation.md#L105)），
  所以我们接管的是**调色前的最终 HDR 图**，调色/UI/Present 仍归引擎。这正好符合 TRP 的意图（UI 不被 NR 处理），
  也是阶段 3 帧生成要的 HUD-less 源（[M §4.2](M-roadmap.md#L88)）。

## 2. 为什么这次敢重开 After 档

旧结论是「不做」，两条都在本仓库里：

- [G §4](G-nr-customization-plan.md#L110)：「Placement：放大后跑 NR —— 引擎拥有 SR 与最终图像；放大后跑 NR
  就得接管 `EvaluateFeature` 的输出、UI 合成与 present」⇒ 判结构性不可做；
- [K §3.5](K-trp-feature-gap.md#L171)：「2560×1440 上 NR ≈ 8.75 ms，四倍成本；TRP 自己也建议优先『前』」。

**判据变了（2026-10-24）**：[J §9](J-sr-implementation.md#L119) 的 RCAS 已经在引擎 `Output` 上做过
`ALL → COPY_SOURCE → COPY_DEST → ALL` 的往返，两次实机画面正常、窗口缩放不崩。⇒ After 档只需要
「读 `Output` + 写回 `Output`」，**不需要接管 UI 合成与 present**。

于是 After 档从「结构性不可做」降级为「一个待探针回答的契约未知」：**guide（Depth/MV，渲染分辨率）
比 color（输出分辨率）小，310.x 运行时接不接受**。成本仍按 ≈9.6 ms 记（这条老结论不变，只是不再等于「不做」）。

## 3. 路线（P0–P4）

| 步 | 内容 | 判据（先写判据再动手） | 代价 | 状态 |
|---|---|---|---|---|
| **P0 探针**（离线，不进游戏） | 扩 `ngx_probe` 的 `--after` 组（设计见下） | ① create=1；② 两帧 eval 都=1；③ 输出非退化（非全黑/非 NaN/非常量）；④ 两帧不同（时序在跑）；⑤ 两档 `MVecScale` 的 dump 差异记下来 | 0.5–1 天 | **未开工** |
| **P1 After 最小闭环**（TEST） | `nr.placement` + 面板「位置」两档；After 路径：读 `Output` → 编码 → NR → Ratio 合成 → 写回 `Output`；切位置重建 feature + 双侧 Reset + 失败回落 Before | 分屏：左=`Output` 原图、右=NR 合成（输出分辨率上可辨）；切回 Before 画面与开关前一致；不花屏/不冻结/不崩 | 1–1.5 天 | 未开工 |
| **P2 顺序与读数** | After 写回之后再跑 RCAS（若开）；诊断页回读位置 / work×guide×输出尺寸 / 编码·求值·合成·写回四段计时 | 顺序固定「NR → RCAS」；回读值 = 实际写入；关 RCAS 时画面不受影响 | 0.5 天 | 未开工 |
| **P3 降本档**（可选） | After 也支持 work = 输出 × 50/66，合成仍在输出分辨率 | 66% 成本 ≈4.6 ms 且画面可接受 | 0.5 天 | 未开工（拍板点见 §7） |
| **P4 收尾** | `--test=y` + `--test=n` 双构建 → 部署 `E:\LeviLauncher\versions\1.26.51.01-agent2\mods\NeuroDragon`；文档回写 D/K/M + CHANGELOG | 发布构建同样能切两档 | 0.5 天 | 未开工 |

### P0 探针设计（离线，不进游戏）

- 工具：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp`（已有自建设备 + core Init +
  `CreateFeature(0x12)` 尺寸扫描 + ratio kernel；改工具不进 mod 仓库，见 L §0 与工作区分层约定）。
- 要加 `--after` 组：建 3 张纹理（color / output = 2560×1440 的线性内容，depth / MV = 1485×835），
  按 `D3D12Bridge.cpp` 的**同名参数**置：
  `DLSSNR.Width/Height` = 输出尺寸；`DLSSNR.ColorSubrect*` / `OutputSubrect*` / `BackbufferSubrect*`
  = 输出尺寸；`DLSSNR.DepthSubrect*` / `MVecSubrect*` = 渲染尺寸；`DLSSNR.MVecScaleX/Y` 跑**两档**
  （渲染尺寸 / 输出尺寸）；连跑 2 帧（第 2 帧色内容轻微变化），dump 两次 output 并打印 create/eval 返回码。
- 判据：见上表 P0 行；两档 `MVecScale` 的差异记进本文件，作为实机 A/B 的先验。
- **判否时的退路**（P0 出结论当天就要选，不要悬着）：
  - **O1** 自升采样 guide：depth 双线性、MV 拷贝并按输出尺寸重标定 scale（1–2 天，保住「按输出分辨率处理」的定义）；
  - **O2** 降采样后按渲染尺寸跑、只在输出分辨率合成（1 天，便宜但语义偏离用户定义）。

**为什么先做探针**：这是 After 档唯一的未知（guide 尺寸契约），用离线探针答掉，就不需要
「改一版、进游戏看一眼、再改一版」（L §0 约定 1–2）。

## 4. 改动落点（已核对代码）

| 文件 | 要改什么 |
|---|---|
| `src/neurodragon/Config.h` | `nr.placement`：0 = 放大前（默认）/ 1 = 放大后 |
| `src/neurodragon/nr/D3D12Bridge.{h,cpp}` | 参数集**已经**按 feature / guide 分离（`DLSSNR.*Subrect` 用 feature 尺寸、`Depth`/`MVec Subrect` 用 guide 尺寸，`D3D12Bridge.cpp:2640-2689`），所以 After 档主要是三件：① 输入资源从引擎 `Color` 换成 `Output`；② 纹理按 **`Output` 的 desc** 分配（现在统一按 `Color` 的 desc，`:1049-1077`、`:1862`、`:2183`）；③ 合成落点从「换引擎参数对象的 `Color` 指针」（`:2532`）换成「`COPY_DEST` 写回 `Output`」（照 `sr/SrSharpening.cpp` 的 source/target 模式） |
| `src/neurodragon/probe/RenderProbe.cpp` | After 路径放在 `gNgxEvalOriginal` 返回 1 之后、RCAS 调用点（`:1132`）之前，顺序固定「NR → RCAS」；切位置时置一次双侧 Reset |
| `src/neurodragon/ui/Menu.cpp` + `src/lang/{zh_CN,en_US}.json` + `scripts/collect_glyphs.py` | 面板「位置」两档 + 诊断页回读（位置 / work / guide / 输出尺寸） |
| `D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp` | 只 P0 用；不进 mod 仓库 |

## 5. 风险与处置

| 风险 | 表现 | 处置 |
|---|---|---|
| 运行时不接受 color > guide | create 返回非 `0x1`，或 eval 后输出全黑/花屏 | **P0 先答**；按 §3 的 O1 / O2 选退路 |
| 写 `Output` 与引擎后续 pass 打架 | 花屏 / 冻结 | RCAS 已证明可行；失败自动关 After 并回落 Before，不留死状态 |
| 切位置时历史错位 | 拖影 / 重影 | 切位置强制双侧 `Reset=1`，切换后首帧不合成 |
| NR 历史建立在「已放大」的图像上 | 快速转视角出现鬼影 | 验收单列一条；严重则加「移动时降低合成强度」作为收尾项 |
| 100% 成本 ≈9.6 ms（≈78% 帧预算） | 帧率明显下降 | After 档的固有代价；P3 给 66/50 降本档，面板直接显示成本 |

## 6. 验收（一次进游戏，5 条）

1. 面板切「放大后」→ 日志出现 `[nr] placement=after work=2560x1440 guides=1485x835 create=0x1`，
   诊断页位置显示「放大后」。
2. 开「分屏对比」→ 左半是放大后的原图、右半是 NR 合成，分界线可拖。
3. GPU 读数：After 档 NR 总量 ≈9–10 ms（100%），明显高于 Before 的 3.5 ms——**这是预期，不是 bug**。
4. 快速转视角 / 移动：无拖影、无重影、无残影。
5. 面板切回「放大前」→ 画面与开关前一致；期间改窗口大小 / 内部分辨率 → 不花屏、不崩。

## 7. 拍板状态

| 事项 | 状态 |
|---|---|
| 只做位置轴，其它项不排 | ✅ 已拍板（2026-10-24） |
| P0 先做 | ⏳ 已指示，**未开工**（当次改口为「先更新文档」） |
| After 档默认工作尺寸 = 100%（严格按用户定义） | 拟定，P0 出结论时确认 |
| guide 被拒时走 O1（自升采样 guide） | 拟定，P0 判否当天确认 |
| After 档下 RCAS 默认关 | 拟定 |
| 位置切换面板 Apply 即时生效（走现成先建后拆重建） | 拟定 |

## 8. 不做（沿用现有文档，不因本文改变）

多 pass / 对象遮罩 / peripheral / fused（[K §3.5](K-trp-feature-gap.md#L162)）、Style 3–7、天气时段、
UI mask 与 `ControlMask`（[G §4](G-nr-customization-plan.md#L110)）、HDR 输出、`.trpk`/INT8/MFG 解锁
（[M §4.4](M-roadmap.md#L106)）。

## 9. 变更记录

- 2026-10-24：首版。路线据用户口述的位置轴两档；旧结论（G §4 / K §3.5「不做」）的判据变化登记在 §2；
  P0 的探针设计与判据先写在这里再开工；按用户裁定，其余 NR/SR 待办全部不排。
