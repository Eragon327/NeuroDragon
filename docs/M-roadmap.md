# M. 阶段路线：NR/SR → VV 接管 TAAU → FG → RR

> **来源**：用户 2026-10-24 口述定的四阶段顺序（原话：「首先先做 NR 和 SR，目前这一部分完成了一半，
> 然后把 NR 和 SR 接入 VV 管线，替代掉 TAAU。然后再做 FG 部分。最后尝试 RR」）。
>
> **本文的效力范围**：只回答「先做什么、后做什么、每阶段到什么程度算完」。
> 每阶段**怎么做**仍以各权威文档为准——NR 执行 → [L](L-nr-remaining-plan.md)，功能对照与代价 →
> [K](K-trp-feature-gap.md)，SR 机制 → [J](J-sr-implementation.md)，NR 事实 → [D](D-rt-dlss-nr-plan.md)，
> VV 里程碑 → [I §5](I-sr-final-plan.md#L87-L101)。
> 与这些文档的做法/价格结论冲突时，按它们的「状态」列；与本文件的**顺序**冲突时，以本文件为准。
>
> **记录说明**：写这份记录时，代码由另一个 agent 在改。本文只记路线与阶段状态，
> 不描述未提交代码的细节，也不作为「代码已完成」的证据。

## 0. 一句话

```
阶段 1  NR + SR 主体          进行中（用户判定：一半）
阶段 2  NR/SR 接进 VV，替掉 TAAU   未开始（= docs/I 的 M4–M7）
阶段 3  FG（帧生成）          未开始（此前文档判「不排期」，本次改为排进来）
阶段 4  RR（光线重建）        最后尝试，可能结构性不可做（判据见 §5）
```

顺序理由（据现有文档，不是新结论）：

- **阶段 1 先收口**：NR 是唯一功能、SR 是引擎自带的现成管道，两者都已能上屏并有实测数字，
  剩下的都是降本、质量与验收——继续做能看到东西的部分（[K §0](K-trp-feature-gap.md#L15-L22)）。
- **阶段 2 在 FG/RR 之前**：VV 是 Bedrock 的主渲染路径（`platform_configuration.pc.json` 的 tier
  全走 taau/bilinear），NR/SR 要成为产品必须先接管 taau；而且它顺带把「pre-UI 插入点 + 深度源 +
  bgfx→D3D12 桥」这三件 FG/RR 都要用的地基做完（[A §5](A-renderdragon-passmap.md#L148-L162)、
  [I §5](I-sr-final-plan.md#L87-L101)）。
- **FG 排在 RR 前**：FG 的未知是「呈现路径与帧节奏」，与阶段 2 的插入点部分重叠；
  RR 的未知是「能不能拿到噪声 radiance」，是全路线里唯一可能**直接答否**的一项，
  放最后可以用一次判定（1–2 天）决定要不要投（[D §7](D-rt-dlss-nr-plan.md#L716-L748)）。

## 1. 四阶段总表

| 阶段 | 内容 | 状态（2026-10-24） | 里程碑 | 当前权威文档 |
|---|---|---|---|---|
| 1 | NR 神经渲染 + SR 超分（RT 路线，复用引擎 DLSS） | 🟡 一半 | L 批 1–3；I 的 M0–M3 | L / K / J / D |
| 2 | NR + SR 接入 VV 管线，替代 TAAU | ⬜ 未开始 | I 的 M4–M7（M8 可选） | I / A |
| 3 | FG 帧生成 | ⬜ 未开始（**排期变更**，见 §6） | 尚无里程碑，须先答 §4 的未知 | A（P3/P4/P6）/ F（Present 设施） |
| 4 | RR 光线重建（尝试） | ⬜ 未开始，**可能不可做** | 先答 D §7 的决定性问题 | D §7 |

## 2. 阶段 1：NR + SR（用户判定：一半）

「一半」按已完成/未完成对照（口径来自 [L §2](L-nr-remaining-plan.md#L47-L84)、
[K §3.4](K-trp-feature-gap.md#L143-L161)、[J §1](J-sr-implementation.md#L6-L19)）：

| 已完成（能看见） | 未完成 |
|---|---|
| NR：建 feature、逐帧 evaluate、增益合成上屏（`replace=5`）、HDR→LDR 编码、任意尺寸重建、关闭即卸载 | L 批 1 的实机验收与探针（P1 皮肤哨兵、P2 网络预设、P3 分屏公平性） |
| NR：实测 3.5 ms @1485×835、8.75 ms @100%（占 ~29% 帧预算） | L 批 2：①输入缩放（降本主力）、⑥质量修正（HueGuard / ToneBands 等） |
| SR：33/50/58/66/100 档位、模型预设 J/K/L/M、应用即重建、FrameBuilder 因子表 + 质量槽双写 | L 批 3：⑧运行时选择、⑤自适应曝光、遥测补齐 |
| SR：读数层（渲染/输出尺寸、回调统计、Reset） | SR：组合预设包（I 的 M2 剩余）；78% 与锐化**判定不做**，不是未完成 |
| 面板 / i18n / `ll::config` / TEST 构建 / 部署链 | 面板「超分」页只读区的删除（J §8 待办）；监控与 NR 的拆分（F §9，未实施） |

**注**：「一半」是用户对**阶段 1 整体**的判定，不是对某一项的测量。上面两列是按文档里的
待办清单对出来的映射，若口径不同，以用户为准，并回来改这张表。

## 3. 阶段 2：NR + SR 接入 VV，替代 TAAU

这阶段的全部技术内容已经在 [I §5](I-sr-final-plan.md#L87-L101) 排过（M4 侦察 → M5 写透传 →
M6 DLSS 替换 → M7 统一收尾，M8 可选），本文不重复，只记**开工前必须先答的未知**
（规则沿用 [L §0](L-nr-remaining-plan.md#L8-L22)：先探针、先写判据，不「改一版看一眼」）：

| # | 未知 | 为什么是前置 | 探针/判据 |
|---|---|---|---|
| 1 | VV 下 `taau` 是否命中、NGX 是否已被引擎 Init | 不命中就没有插入点；已 Init 则不能二次 Init | I 的 K4：`upscaling_mode=0` + `graphics_mode=2`，看 taau 命中次数 |
| 2 | bgfx 句柄 → `ID3D12Resource` 的桥 | taau 输入只有 bgfx 句柄（[A §7.3](A-renderdragon-passmap.md#L237-L257)），拿不到资源就什么都做不了 | I 的 K6：钩 bgfx D3D12 纹理创建，拿到 1 张资源 |
| 3 | 深度从哪来 | taau **不绑深度**（[A §7.1](A-renderdragon-passmap.md#L201-L222)），NR 要深度 | 钩延迟着色通道或自建深度拷贝 |
| 4 | 写 taau 输出目标是否被后续 pass 依赖 | 写错就是花屏/崩溃 | I 的 M5：先「TAAU 照跑再覆盖」，不成立就放弃写入 |
| 5 | VV 的尺寸语义与 RT 不同 | VV 的 `target_resolution` 是**绝对目标**（720p/1080p/1440p/2160p × DRS），RT 是「窗口 × 58%」 | I 的 K2；两套 R_in 策略最后统一（M7） |

**stop/go**：I §7 已经把「M4 侦察 1 周后设点」写死——侦察不出来，VV 就只做「读数 + 提示」，
不硬推 M5/M6。这条不变。

## 4. 阶段 3：FG（帧生成）

### 4.1 排期变更（重要）

此前文档的结论是**不排期**，本次改为第三阶段。被取代的原文与处置见 §6，这里说明为什么口径变了：
「Bedrock 没有 FG」说的是**引擎自己不产出帧生成**，不等于**不能在呈现层自己接**。面板已经有
Present 侧的设施（`Present`/`Present1`/`ResizeBuffers` 钩子 + D3D11On12 设备，
[F §1](F-ui-plan.md#L11-L31)），而 A 的 P3/P4/P6 本来就是按「UI 分层 → 帧末 → present」给 FG 留的位
（[A §5](A-renderdragon-passmap.md#L148-L162)）。

### 4.2 现存事实（都可查，不新增假设）

- 驱动目录里有 `nvngx_dlssg.dll`（与 `_nvngx.dll`、`nvngx.dll` 同级）——**运行时在**，但引擎是否
  加载/创建 feature 未知（见 4.3 第 1 条）；
- 呈现发生在 bgfx 内部（`flip()`），DXGI `Present` 在 bgfx 里（[A §7.5](A-renderdragon-passmap.md#L306-L322)）；
- UI 在 SR **之后**由引擎合成（[H §0](H-sr-plan.md#L29-L30)），所以 FG 一定需要 hudless/UI 分层，
  否则 UI 会被插帧一起处理。

### 4.3 开工前必须先答的未知

| # | 未知 | 判据 |
|---|---|---|
| 1 | Bedrock 是否已创建 DLSS-G feature（NGX feature 2） | 看引擎是否 `LoadLibrary` 了 `nvngx_dlssg.dll`、参数表里有没有 FG 相关键；没有则要走「自建 feature」——那与 SR 的「一个进程只能一个实例」风险同类（[D §6.16 规则 3](D-rt-dlss-nr-plan.md#L551-L553)），需单独评估 |
| 2 | 谁拥有 swapchain 与帧节奏 | bgfx 持有链（A §7.5）；FG 要么钩 bgfx `flip`，要么在 `Present1` 前插入——两者都还没验证过 |
| 3 | depth / MV 能否拿到 | 与阶段 2 的第 3 条同源；RT 路线可从引擎 NGX 参数表拿（[D §2](D-rt-dlss-nr-plan.md#L39-L55)），VV 路线要先做成阶段 2 的深度源 |
| 4 | UI 分层怎么做 | A 的 P3/P5（`drawColorCorrectedUI` / `DefaultUIRendererCompositorStage::preRender`）是候选落点，未验证 |
| 5 | 全屏/MPO/DWM、VRR 与输入延迟的表现 | 实机 A/B：帧生成开/关的帧时间、撕裂、延迟手感 |

### 4.4 明确不碰

TRP 的 `.trpk` 内核包、INT8 量化、MFG 解锁——[G §4](G-nr-customization-plan.md#L110-L123) 已判为
「逆向 patch `nvngx` 内核，法律与稳定性风险高，不在本项目范围」。这条不因排期变化而改变。
（另：上家包 `E:\V12\MO2\mods\DLSS5+6倍帧生成` 里的 INI 是默认值，所谓「6 倍」不在包里，
见 [D §8](D-rt-dlss-nr-plan.md#L756-L761)。）

## 5. 阶段 4：RR（光线重建，最后尝试）

技术事实全部在 [D §7](D-rt-dlss-nr-plan.md#L716-L748)，这里只记「为什么放最后」和「什么算答否」：

- RR = NGX feature **13**（`0x0D`），runtime `nvngx_dlssd.dll`，参数 `DLSSD.*` + G-buffer；
- 引擎**自带降噪**（`doAllRayTracing` 集中），所以引擎的 `DLSSD.*` 参数位全 0 是「引擎不用 RR」，
  不是「没写完」；
- **插入点不是 `SetPtr("Color")`**：现在的 Color 是降噪之后的 DLSS 输入，喂给 RR 只会双重降噪；
- Mac/Metal 的地址不能套 Windows 1.26.51，必须按字符串 / RTTI 重新定位。

**决定性判据**（1–2 天，在 IDB 里答掉）：能否拿到 `RayTracingResources`（或它背后的噪声 radiance
与 G-buffer）。答不上来 ⇒ **RR 不可做**，NR 就是本平台唯一的神经渲染路线。
**答否也算这一阶段的结论**，写进本文与 D §7，不算失败。

注意：RR 只在 RT 模式成立（VV 没有噪声 radiance），所以阶段 2 的 VV 线与阶段 4 **不共享注入点**，
不要为了「统一」把两者合并。

## 6. 与既有文档的冲突登记

| 既有说法 | 出处 | 现在 | 处置 |
|---|---|---|---|
| F 阶段「不排期」/「Bedrock 没有帧生成」 | [README:36](../README.md#L36-L37) | FG 排第三阶段 | 以本文为准；README 该行已改为「第三阶段」并指向本文 |
| 「帧生成 / MFG / XeSS / XeFG …… 不在本项目范围」 | [G §4](G-nr-customization-plan.md#L110-L123) | 同上 | G 是历史文档，原文保留；**FG 的排期以本文为准**（G 里「不碰 .trpk / INT8」的结论仍有效） |
| RR「第二阶段，不在当前范围」 | [D §7 标题](D-rt-dlss-nr-plan.md#L716) | 现在排第四阶段 | 顺序变了，D §7 的技术结论不变；改动只落在顺序上 |
| 「路线/优先级 → K」 | [docs/README 更新约定](README.md) | 阶段顺序与完成度 → M | M 新增为「阶段路线权威」；K 继续管「做什么/代价」，L 继续管 NR 执行 |

## 7. 沿用不变的约定

- [L §0](L-nr-remaining-plan.md#L8-L22)：不盲猜、探针优先于反复试错、每批一次交付、不 commit、
  文档同步、新增 UI 文案双语 + 重跑 `collect_glyphs.py`、每个开关都要能被回读验证。
- 每阶段开工前先写「判据」，做完把结论写回对应权威文档（D / J / K / L），阶段状态只改本文。

## 8. 变更记录

- 2026-10-24：首版。据用户口述的四阶段顺序；阶段 2 对齐 I 的 M4–M7；FG 由「不排期」改为第三阶段
  并登记被取代的说法；RR 由「不在当前范围」改为第四阶段（顺序变，判据不变）。
