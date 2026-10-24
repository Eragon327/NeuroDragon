# 全局路线图

> **来源**：用户 2026-10-24 口述定的四阶段顺序（原话：「首先先做 NR 和 SR，目前这一部分完成了一半，
> 然后把 NR 和 SR 接入 VV 管线，替代掉 TAAU。然后再做 FG 部分。最后尝试 RR」）。
>
> **本文的效力范围**：只回答「先做什么、后做什么、每阶段到什么程度算完」。
> 每阶段**怎么做**以各 spec 为准——NR → [specs/nr.md](specs/nr.md)，SR → [specs/sr.md](specs/sr.md)，
> VV 路径 → [specs/vv.md](specs/vv.md)，RenderDragon 与接入点 → [specs/renderdragon.md](specs/renderdragon.md)，
> TRP 对照与代价 → [specs/trp.md](specs/trp.md)。
> 与这些文档的做法/价格结论冲突时按它们的口径；与本文件的**顺序**冲突时以本文件为准。
>
> **记录说明**：本文只记路线与阶段状态，不描述未提交代码的细节，也不作为「代码已完成」的证据。

## 0. 一句话

```
阶段 1  NR + SR 主体              进行中（用户判定：一半）
阶段 2  NR/SR 接进 VV，替掉 TAAU   M6 已开工；B0 只读探针已实现，待实机采集
阶段 3  FG（帧生成）              未开始（此前判「不排期」，2026-10-24 改为排进来）
阶段 4  RR（光线重建）            最后尝试，可能结构性不可做（判据见 §5）
```

顺序理由（据现有 spec，不是新结论）：

- **阶段 1 先收口**：NR 是唯一功能、SR 是引擎自带的现成管道，两者都已能上屏并有实测数字，
  剩下的都是降本、质量与验收——继续做能看到东西的部分（[specs/trp.md](specs/trp.md) §0）。
- **阶段 2 在 FG/RR 之前**：VV 是 Bedrock 的主渲染路径（`platform_configuration.pc.json` 的 tier
  全走 taau/bilinear），NR/SR 要成为产品必须先接管 taau；而且它顺带把「pre-UI 插入点 + 深度源 +
  bgfx→D3D12 桥」这三件 FG/RR 都要用的地基做完（[specs/renderdragon.md](specs/renderdragon.md) §4）。
- **FG 排在 RR 前**：FG 的未知是「呈现路径与帧节奏」，与阶段 2 的插入点部分重叠；
  RR 的未知是「能不能拿到噪声 radiance」，是全路线里唯一可能**直接答否**的一项，
  放最后可以用一次判定（1–2 天）决定要不要投（[specs/nr.md](specs/nr.md) §12）。

## 1. 四阶段总表

| 阶段 | 内容 | 状态（2026-10-24） | 里程碑 | 当前 spec |
|---|---|---|---|---|
| 1 | NR 神经渲染 + SR 超分（RT 路线，复用引擎 DLSS） | 🟡 一半 | NR 批 1–4；SR 的 S1–S3 | [nr](specs/nr.md) / [sr](specs/sr.md) / [trp](specs/trp.md) |
| 2 | NR + SR 接入 VV 管线，替代 TAAU | 🟡 M6 开工（B0 探针待采集） | M4 侦察 → M5 写透传 → M6 DLSS 替换 → M7 收尾（M8 可选） | [vv](specs/vv.md) |
| 3 | FG 帧生成 | ⬜ 未开始（**排期变更**，见 §6） | 尚无里程碑，须先答 §4 的未知 | [renderdragon](specs/renderdragon.md) / [ui](specs/ui.md) |
| 4 | RR 光线重建（尝试） | ⬜ 未开始，**可能不可做** | 先答 §5 的决定性问题 | [nr](specs/nr.md) §12 |

## 2. 阶段 1：NR + SR（用户判定：一半）

「一半」是用户对**阶段 1 整体**的判定，不是对某一项的测量。已完成/未完成按 spec 待办对出来的映射，
若口径不同以用户为准，并回来改这张表。

| 已完成（能看见） | 未完成 / 未排 |
|---|---|
| NR：建 feature、逐帧 evaluate、增益合成上屏、HDR→LDR 编码、任意尺寸重建、关闭即卸载；输入缩放 / 质量修正 / 运行时模型选择 / 自适应曝光（[nr](specs/nr.md)） | NR：**位置轴**的观察项（After 档分屏、快速转视角鬼影、窗口缩放）；批次 ④ 隔帧、⑦ 稳态网格 **暂缓**；自适应曝光的白点回读探针**搁置** |
| NR 位置轴：Before / After upscaling 两档可切，After 实机 9.78 ms、release 已部署（[nr §7](specs/nr.md)） | SR：75%（TRP 的 78%）实验与 mip LOD 偏置**未排期**；组合预设包**暂不做**（用户 2026-10-24 决定） |
| SR：33/50/58/66/100 档位、模型预设 J/K/L/M、应用即重建、FrameBuilder 因子表 + 质量槽双写；RCAS 锐化；成品帧截图（F2）；读数层（尺寸链 / 质量槽 / 预设 / 回调统计 / Reset）；自检与遥测；DLSS 运行时 310.9.1 已验（[sr](specs/sr.md)） | SR：锐化的 1.0 强度**不适合做默认**（高光过冲）；截图/锐化的验收清单当时只跑了一部分，剩余项见 [sr §10](specs/sr.md) |
| 面板 / i18n / `ll::config` / TEST 构建 / 部署链 | 监控与 NR 的拆分（[ui §8](specs/ui.md)，未实施） |

**注**：NR 的实测开销——Before 档 **3.5–3.8 ms @1485×835**、8.75 ms @100%、After 档 **9.78 ms @2560×1440**；
同帧引擎 DLSS 1.2–1.8 ms；~82 fps ⇒ Before 档约占 29% 帧预算。数字散布在 [nr](specs/nr.md) 与
[trp](specs/trp.md) 两处，两处的口径一致。

## 3. 阶段 2：NR + SR 接入 VV，替代 TAAU

技术内容已排过：M4 侦察 → M5 写透传 → M6 DLSS 替换 → M7 统一收尾，M8 可选
（[specs/vv.md](specs/vv.md) §8）。M4 侦察只做了只读部分（[research/2026-10-24-vv-path.md](research/2026-10-24-vv-path.md)），
五个未知的状态：

| # | 未知 | 状态 | 结论 / 下一步 |
|---|---|---|---|
| 1 | VV 下 `taau` 是否命中、NGX 是否已被引擎 Init | ✅ 已答 | 命中，**每帧 1 次声明 + 1 次 draw**；VV 下没有引擎 NGX 调用 ⇒ DLSS 必须我们自建 feature |
| 2 | bgfx 句柄 → `ID3D12Resource` 的桥 | ⬜ 未完成 | 已证否「帧图依赖对象里直接有 D3D12 资源」，桥必须从 bgfx 侧搭（[renderdragon §5](specs/renderdragon.md)） |
| 3 | 深度从哪来 | 🟡 有候选 | 渲染分辨率下两个深度面（DXGI 格式 41/54），taau 的 4 个输入里确实没有深度；下一步读准格式并与延迟着色对上 |
| 4 | 写 taau 输出目标是否被后续 pass 依赖 | ⬜ 未做 | 渲染目标清单里没有显示分辨率的 HDR 面 ⇒ 怀疑 TAAU 直接写后缓冲，若成立「写输出」比预想简单 |
| 5 | VV 的尺寸语义与 RT 不同 | ✅ 已答 | VV 是 `factor=0.33` × 显示尺寸；`target_resolution` 是**绝对目标**，DRS 换算成 factor |

**stop/go**：[specs/vv.md](specs/vv.md) §8 沿用旧 [I](archive/2026-10/I-sr-final-plan.md) 的设点——M4 侦察一周后仍答不出桥与深度，VV 就只做
「读数 + 提示」，不硬推 M5/M6。这条不变。

**2026-10-24 收尾时的执行顺序**（用户决定，细节见 [specs/vv.md](specs/vv.md) §12）：

```
A. 边界 / 注入验证  在 list 边界往后缓冲写纯色；画面纯色 + UI 仍在 ⇒ 拆提交+插入成立（零纹理依赖）
B. 认纹理           A 通过后，把 dumpTextureBmp 接到显式触发（面板/热键），只在安静窗口跑
C. bgfx 桥          仅当 B 对不上 taau 的输入/输出时才做
```

A 已实现（面板「诊断 → VV 注入」，代码 `probe/VvInject.*`，细节见 [specs/vv.md](specs/vv.md) §12 A），
**暂停界面已验收通过**（k=66/69：世界变洋红、UI 仍在 ⇒ pre-UI 插入成立）；**世界内 HUD 的边界还没测**。
B/C 未动。

**2026-10-25：M6 已拍板**（B0 → B1 → B2 → B3，B4 可选；口径见 [specs/vv.md](specs/vv.md) §13）。
B0 的只读探针已实现（P0.1 会话 / P0.2 域 / P0.3 输入角色 / P0.4 矩阵-jitter-原点），
等一次 TEST 实机采集；B1 起（DLSS feature 与 compute）未开工。

两个必须记住的崩溃前提：交换链后缓冲引用要当场 `Release`（否则 `ResizeBuffers` 失败 → 引擎 `abort`），
以及任何碰引擎纹理的拷贝都不能和「引擎重建资源」并发（否则驱动 AV）。两次现场见
[research §8](research/2026-10-24-vv-path.md)。

## 4. 阶段 3：FG（帧生成）

### 4.1 排期变更

此前文档的结论是**不排期**，2026-10-24 改为第三阶段。「Bedrock 没有 FG」说的是**引擎自己不产出帧生成**，
不等于**不能在呈现层自己接**。面板已经有 Present 侧的设施（`Present`/`Present1`/`ResizeBuffers` 钩子 +
D3D11On12 设备，[ui §2](specs/ui.md)），而 RenderDragon 的 P3/P4/P6 本来就是按「UI 分层 → 帧末 → present」
给 FG 留的位（[renderdragon §4](specs/renderdragon.md)）。

### 4.2 现存事实（都可查，不新增假设）

- 驱动目录里有 `nvngx_dlssg.dll`（与 `_nvngx.dll`、`nvngx.dll` 同级）——**运行时在**，但引擎是否
  加载/创建 feature 未知（见 4.3 第 1 条）；
- 呈现发生在 bgfx 内部（`flip()`），DXGI `Present` 在 bgfx 里（[renderdragon §5](specs/renderdragon.md)）；
- UI 在 SR **之后**由引擎合成（[sr §3](specs/sr.md)），所以 FG 一定需要 hudless/UI 分层，
  否则 UI 会被插帧一起处理。

### 4.3 开工前必须先答的未知

| # | 未知 | 判据 |
|---|---|---|
| 1 | Bedrock 是否已创建 DLSS-G feature（NGX feature 2） | 看引擎是否 `LoadLibrary` 了 `nvngx_dlssg.dll`、参数表里有没有 FG 相关键；没有则要走「自建 feature」——那与 SR 的「一个进程只能一个实例」风险同类，需单独评估（[nr §3](specs/nr.md)） |
| 2 | 谁拥有 swapchain 与帧节奏 | bgfx 持有链；FG 要么钩 bgfx `flip`，要么在 `Present1` 前插入——两者都还没验证过 |
| 3 | depth / MV 能否拿到 | 与阶段 2 的第 3 条同源；RT 路线可从引擎 NGX 参数表拿（[nr §3](specs/nr.md)），VV 路线要先做成阶段 2 的深度源 |
| 4 | UI 分层怎么做 | P3/P5（`drawColorCorrectedUI` / `DefaultUIRendererCompositorStage::preRender`）是候选落点，未验证 |
| 5 | 全屏/MPO/DWM、VRR 与输入延迟的表现 | 实机 A/B：帧生成开/关的帧时间、撕裂、延迟手感 |

### 4.4 明确不碰

TRP 的 `.trpk` 内核包、INT8 量化、MFG 解锁——[decisions/0002](decisions/0002-no-kernel-patching.md)
已定为「逆向 patch `nvngx` 内核，法律与稳定性风险高，不在本项目范围」。这条不因排期变化而改变。
（另：上家包 `E:\V12\MO2\mods\DLSS5+6倍帧生成` 里的 INI 是默认值，所谓「6 倍」不在包里。）

## 5. 阶段 4：RR（光线重建，最后尝试）

技术事实见 [nr §12](specs/nr.md)，这里只记「为什么放最后」和「什么算答否」：

- RR = NGX feature **13**（`0x0D`），runtime `nvngx_dlssd.dll`，参数 `DLSSD.*` + G-buffer；
- 引擎**自带降噪**（`doAllRayTracing` 集中），所以引擎的 `DLSSD.*` 参数位全 0 是「引擎不用 RR」，
  不是「没写完」；
- **插入点不是 `SetPtr("Color")`**：现在的 Color 是降噪之后的 DLSS 输入，喂给 RR 只会双重降噪；
- Mac/Metal 的地址不能套 Windows 1.26.51，必须按字符串 / RTTI 重新定位。

**决定性判据**（1–2 天，在 IDB 里答掉）：能否拿到 `RayTracingResources`（或它背后的噪声 radiance
与 G-buffer）。答不上来 ⇒ **RR 不可做**，NR 就是本平台唯一的神经渲染路线。
**答否也算这一阶段的结论**，写进本文与 [nr §12](specs/nr.md)，不算失败。

注意：RR 只在 RT 模式成立（VV 没有噪声 radiance），所以阶段 2 的 VV 线与阶段 4 **不共享注入点**，
不要为了「统一」把两者合并。

## 6. 口径变更登记

| 旧说法 | 现在 | 处置 |
|---|---|---|
| 「FG 不排期 / Bedrock 没有帧生成」 | FG 排第三阶段 | 以本文为准；旧说法随旧文档归档 |
| 「帧生成 / MFG / XeSS / XeFG …… 不在本项目范围」 | 同上 | 旧 G 已归档、原文保留；**FG 的排期以本文为准**（「不碰 .trpk / INT8」的结论仍有效，见 [decisions/0002](decisions/0002-no-kernel-patching.md)） |
| RR「第二阶段，不在当前范围」 | 现在排第四阶段 | 顺序变了，RR 的技术结论不变 |
| 「路线/优先级 → K」 | 阶段顺序与完成度 → 本文 | K 的内容已并入 [specs/trp.md](specs/trp.md)；它继续管「做什么/代价」 |
| 「NR 放在 DLSS 之后 = 不做」 | 位置轴（Before / After upscaling）排入阶段 1，已实现并实机验收 | 以 [nr §7](specs/nr.md) 为准；旧 G 已归档 |

## 7. 沿用不变的约定

- 不盲猜：先探针（TEST 日志 / 参数往返 / dump / `ngx_probe` 离线复现）再改实现；同一未知项最多用
  一次实机验证回答（[nr §1](specs/nr.md) 的工作约定）。
- 每阶段开工前先写「判据」，做完把结论写回对应 spec；阶段状态只改本文。
- 不 commit：改动留在工作区，由用户自己提交。
- 用户可见变化进 `CHANGELOG.md`；新增 UI 文案双语 + 重跑 `scripts/collect_glyphs.py`。

## 8. 变更记录

- 2026-10-24：首版（旧 `docs/M-roadmap.md`）。据用户口述的四阶段顺序；阶段 2 对齐旧 I 的 M4–M7；
  FG 由「不排期」改为第三阶段并登记被取代的说法；RR 由「不在当前范围」改为第四阶段。
- 2026-10-24：**位置轴**（NR 的 Before / After upscaling 两档）排入阶段 1，随后实现并实机验收，
  release 已部署。
- 2026-10-24：文档改版（[decisions/0003](decisions/0003-docs-layout.md)）。本文由旧 M 迁来并更新：
  阶段 2 的五个未知按 VV 侦察结果标注状态；参考链接改为 `specs/` / `research/`。
