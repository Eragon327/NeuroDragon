# SR：超分（引擎 DLSS feature 1）

> **状态**：现行设计 + 实现记录。内部分辨率、模型预设、应用即重建、RCAS 锐化、成品帧截图都已落地。
> 本文合并了旧 `H`（可行性/预设四层）、`I`（控制层与 VV 迁移规划中仍属 SR 的部分）、`J`（实现与证据）；
> 原文归档在 [archive/2026-10](../archive/2026-10/)。
>
> **本文回答**：SR 能控到什么程度、怎么控、哪些已经做了、哪些不做。
> VV 下「用 DLSS 替换 TAAU」是另一条路线，见 [vv](vv.md)。

## 0. 一句话

```
不写自己的超分器，而是在引擎自己的 DLSS 调用序列里改值：
两个尺寸来源（DLSS 质量槽 + FrameBuilder 因子表）同时改，才真正改了"内部分辨率"。
```

**三种「能」的划分**（判断框架，仍然成立）：

1. **现在就能**：在引擎的参数对象上写值（预设字母、Reset、尺寸提示）；
2. **要一次重建才生效**：创建期参数（预设字母、feature 尺寸）；
3. **结构性做不到**：进程内第二个 DLSS SR 实例；让我们自己的 SR 成为最终画面。

## 1. 最终行为（面板「超分」页）

| 项 | 取值 | 生效方式 |
|---|---|---|
| 内部分辨率 | 跟随引擎 / 33 / 50 / 58 / 66 / 100 | 应用即生效，**不需要**拖窗口/F11 |
| DLSS 模型（预设） | 默认 / E / F / J / K / L / M | 应用时若预设变化，强制一次 feature 重建 |
| 锐化（RCAS） | 开/关 + 0–1 强度（默认 0.672） | 每帧：RT 下在引擎 DLSS 之后；VV 下在写 arg10 之前（§7.1） |
| 引擎上采样读数 | 尺寸链、创建时的质量槽、创建时的预设 | 只读；重建在飞时显示「重建中…」 |
| 自检与遥测 | 四项自检 + 求值次数/失败数/输入格式/HDR/jitter | 只读 |

**不提供 75%**：DLSS 没有原生质量槽，借槽 + 覆盖 Out 会被槽位动态范围夹住（§4）。
**不提供 `Sharpness` 参数**：运行时已废弃该参数（§6）。

## 2. 结构性边界

| 说法 | 实际是什么 | 结论 |
|---|---|---|
| 自建 DLSS SR（`CreateFeature(1)` 再 evaluate） | 一个进程只能有一个 DLSS SR，第二个会把引擎的顶掉（引擎 evaluate 仍返回 1 但输出不再更新） | **不行**；详见 [nr §3.2](nr.md) 规则 3 |
| 我们自己的 SR 当最终图像 | UI 在 SR **之后**由引擎合成，Present 拿到的是「已放大 + 已合成」的画面 | **不行**（UI 纹理我们没有） |
| 我们的整屏后处理（锐化/增强） | Present 钩子是我们自己的（覆盖层就在上面画） | **能**（RCAS 就是这么做的） |
| 改引擎 SR 的行为（预设/比例/Reset） | 在引擎参数对象上写值 | **能** |
| 用 NR 承担放大 | NR 契约是 1:1（`DLSSNR.ScalingRatio=1.0`） | **不做**，且仍受所有权限制 |

## 3. 先把「分辨率」拆成三个数

| 代号 | 含义 | 谁决定 | 能不能控 |
|---|---|---|---|
| **R_in** | 内部渲染分辨率（DLSS 输入） | RT：窗口 × DLSS 质量槽（实测 58% = Balanced）；VV：tier 的 `target_resolution`（绝对目标）× 窗口 | **能，这是「原生分辨率」的正解** |
| **R_sr** | SR 的放大目标（`OutWidth/OutHeight`） | RT：就是引擎 `Output` 纹理；VV：taau 的输出目标 | RT 不能单独控；VV 可加一层 blit |
| **R_out** | 呈现分辨率（后缓冲/交换链像素） | 窗口客户区 / 显示模式 | 能，但代价大（UI 与鼠标坐标一起缩放），默认不控 |

控制手段分级（历史规划，标注现状）：

| 级 | 手段 | 状态 |
|---|---|---|
| L1 | 游戏设置（`upscaling_percentage` / `target_resolution` / `upscaling_mode`） | RT 下游戏内没有可改的超分档位；读数仍读 |
| L2 | 创建期参数覆盖（`DLSS.Hint.Render.Preset.*`、`PerfQualityValue`） | ✅ 已实现（模型预设） |
| L3 | 钩 `DLSSOptimalSettingsCallback` → 写回任意 `Width/Height` | ✅ 已实现（内部分辨率） |
| L4 | 引擎 render-size / framegraph（`UpscalingParameters` / DRS） | ❌ 未做（1–2 周，跨版本风险） |
| L5 | 交换链覆盖（`ResizeBuffers` 强制尺寸） | ❌ 实验级，默认不做 |
| L6 | 自有 SR（VV；或 RT 接管 create） | 阶段 2 的 M6（[vv](vv.md)） |

**结论**：「原生输出分辨率」要控的是 **R_in**，不是 R_out。R_out 只有「少出像素/让系统拉伸」时才有意义，
代价（UI 糊、坐标错）几乎一定不划算。

## 4. 两个尺寸来源（缺一不可）

| 侧 | 谁决定 | 我们改哪里 |
|---|---|---|
| DLSS 输入 | 引擎 optimal-settings 回调（q 槽） | ctx 的 q（`+0xe806dc`），shim 每帧复制到渲染器 |
| 场景渲染矩形 | FrameBuilder 因子表 | `getUpscalingFactor()` 的 `table[fb+0x57c]` |

`FrameBuilder::getUpscalingFactor()` = **虚表槽 83**（我们自己的调用编译成 `call [rax+0x298]`；
按头文件顺序推的 62 是错的）。RT 下按 `[fb+0x57c]` 查表：

```
[0.5, 0.58, 0.667, 0.3334, 0.75]   // RVA 0xE9776E0（1.26.51.01）
index > 4 或非 RT → 常量 1.0
```

patch 表项（`.rdata` 用 `VirtualProtect` 包一次）→ 场景按该比例渲染。实测 100%：`[desc] Depth`
从 `1485x836` 变为 `2560x1440`，画面正常。「跟随引擎」时恢复原表值。

**两个写入者，ctx 才是权威**：

- `srDriveFrameContext`（每帧 shim，`ctx+0xe806dc`）是真正到达帧构建器的路径；
- `srDriveEngineRequest`（evaluate，渲染器 `+0x3c56428`）每帧会被 shim 用 ctx 覆盖。

⇒ 任何「强制重建」的 nudge **必须写 ctx**。2026-10-24 实测：先写渲染器 → 66↔75 无效；改 ctx 后生效。

### 4.1 槽位与动态范围（为什么只有 33/50/58/66/100）

| 槽 | 比例 | 动态范围（2560×1440 下） | 备注 |
|---|---|---|---|
| 3 | 33.3% | 853×480 .. 853×480 | 固定 |
| 0 | 50% | ~1280×720 .. 2560×1440 | |
| 1 | 58% | 1280×720 .. 2560×1440 | Balanced |
| 2 | 66.7% | 1280×720 .. 2560×1440 | Quality |
| 5 | 100% | 2534×1426 .. 2560×1440 | DLAA |

75% 没有原生槽：借 q=5 会被夹到 2534（变 100%），借 q=3 会被夹到 853；只有 q=1/2 的范围能装 1920，
但 66 已占 q=2、q=1 是 58。行为不稳定，2026-10-23 用户决定不提供。

**向上曾经卡在 58%**：2026-10-23 只改 optimal 回调（尺寸提示）时，>58% 会让 DLSS 去读引擎没画过的
区域（下半黑）；现在**因子表与质量槽同时改**，场景真的按新比例渲染，100% 实测正常。
[旧 I §11](../archive/2026-10/I-sr-final-plan.md) 的记录只对「只改回调」那一版成立。

## 5. 模型预设与重建

- `srApplyCreateOverrides` 在 CreateFeature 之前写**六个** `DLSS.Hint.Render.Preset.*`（保留引擎加的
  `0x80000000` 位）；**0 也写**，否则参数对象复用会让「驱动默认」保留上一次的 K/L/M。
- 预设值：0 = 驱动默认、5 = E、6 = F、10 = J、11 = K、12 = L、13 = M。引擎风格的值是
  `Quality/DLAA/Balanced/UltraQuality = 0x8000000B`、`UltraPerformance = 0x8000000C`、
  `Performance = 0x8000000D`；低位 11/12/13 = K/L/M（DLSS 4.x transformer 预设）。
- 预设只在 CreateFeature 读，所以需要一次 feature 重建：
  - `srRequestFeatureRebuild()` 记下当前渲染尺寸；
  - phase 1 用另一个质量槽（写 ctx），phase 2 回到映射槽；
  - 重建期间 sizing 回调把 `OutWidth/OutHeight` 与动态范围钉在记录的尺寸，否则临时槽的动态范围会夹它
    （q=3→853，q=5→2534）；
  - 「跟随引擎」也支持：基准 q 用 ctx 里的值。
- 只读区会短暂看到临时槽（已加 ~90 帧读数保持，仍偶发）；当前设计改为重建在飞时显示「重建中…」。
- **没有引擎 DLSS feature 时**（主菜单、没开光追）应用预设不会留下卡住的重建请求：没有东西可重建，
  请求在帧上下文里直接丢弃，下一次 CreateFeature 仍会读到我们的覆盖。

## 6. 不会变成功能的东西

- **`Sharpness` 参数**：`nvsdk_ngx_defs.h` 里 `DoSharpening` 已标 `SR_DEPRECATED_SHARPENING`
  （"Sharpness is not supported"）。实测写入 0/0.5/1.0 后回调读数仍是 0.35、画面无变化。
  要锐化只能做后处理 pass（RCAS，见 §7）。
- **75% / 78%**：见 §4.1。若一定要做，候选方案是 patch 因子表值 + 借 q=2 + 在尺寸回调里把 Out 覆盖到
  1920×1080；这会推翻一条已定结论，需要用户拍板再试。
- **config 热读**：运行期间 config 只写不读（`config::reload()` 已删除）；文件改动需重启。

## 7. RCAS 锐化

实现：`src/neurodragon/sr/SrSharpening.{h,cpp}`（独立编译单元，只 include `<d3d12.h>`），
由 probe 的 evaluate detour 在**引擎 `EvaluateFeature` 返回 1 之后**调用。

- **落点**：DLSS 之后、引擎调色与 UI 合成之前 ⇒ HUD 与文字不会被锐化；与 TRP 的
  `DLSSBackend::Evaluate` 同一语义位置。
- **为什么不能直抄 TRP**：引擎 `Output` 是**线性 HDR**（峰值实测 ~69），RCAS 的 clamp 求解只对 [0,1]
  成立：`hitMax=(1−maxRGB)/(4·minRGB−4)`，当「有通道 >1、有通道 <1」时为正 → `lobe` 被夹到 0 →
  这些像素完全不锐化（直抄会得到「暗部锐、亮部不锐」的边界伪影）。实现先在 Reinhard 编码域
  （`x/(1+x)`，与 NR 编码同一条曲线）锐化，再解回线性。
- **资源与状态**：引擎 `Output` 只做 `ALL → COPY_SOURCE → COPY_DEST → ALL` 往返，不进入 UAV：
  `Output → 私有 A`，RCAS 读 A 写私有 B，`B → Output`。私有纹理随 `Output` 的尺寸/格式重建，
  但要求新尺寸连续 **12 帧**稳定（拖动窗口期间跳过锐化），被替换的纹理 120 帧后释放。
- **强度语义**：不是照 TRP 只缩 lobe（那样实测 mean |Δ| 只有 0.38/255），而是**放大 RCAS 增量 0–4 倍**
  （编码/感知域内）；默认 0.672 ≈ 2.69 倍。
- **实测（2026-10-24 17:39，整帧开关 + 分强度 dump）**：

  | 强度 | before max / mean / >1 | after max / mean / >1 |
  |---|---|---|
  | 0.672（2.69 倍） | 48.4 / 0.216 / 4.00% | **199.0** / 0.312 / 4.07% |
  | 1.000（4.00 倍） | 40.0 / 0.213 / 3.94% | **199.0** / 0.530 / 4.09% |
  | 0.673 | 41.9 / 0.204 / 3.83% | **199.0** / 0.295 / 3.89% |

  `after max` 恒为 199.0 = `DecodeReinhard` 的 `y ≤ 0.995` 上限 —— 高光边缘被推到 Reinhard 域顶端、
  解回线性就是原值的数倍。钳制保护画面不炸，但**高光边缘确实有过冲**，1.0 时最明显。
- **结论**：0.672 可留作默认；**1.0 不适合做默认**。可选打磨（不排期）：「解回线性后夹到 3×3 邻域
  最大值」的无过冲变体。
- 分屏对比（`sr.compare`）**已删除**：分屏只保留「神经渲染」页判断 NR 的那一个（两个都开会出现
  两条缝、四段画面）；锐化的 A/B 用整帧开关。
- 每累计 120 帧自动写一对 dump（强度变化后重新计时）：`dump/sharp_before_<强度>.bmp` /
  `sharp_after_<强度>.bmp`，日志给两者 max。

### 7.1 VV 路径（2026-10-29，EXP-0096）

VV 不走引擎的 `Output`，所以这里**复用同一套 RCAS**，只换入口与资源：

```
vv DLSS 输出 (dlssOut, RGBA16F, 显示尺寸) → RCAS → sharpOut → 写 arg10 的 PS 采它
```

- **同一份实现**：`sr::recordSharpeningInto(cmd, src, dst, w, h, frame)` 与引擎路径共用
  `kRcasHlsl`、PSO、根签名、强度映射和统计（`sharpStats()` 两边共用一组计数）。它只做
  dispatch，**不碰资源状态**——状态归 `VvSr`（`sharpOut: COMMON → UNORDERED_ACCESS → PIXEL`），
  因为只有它知道那张纹理的来历。引擎路径那套「copy 进私有纹理、再 copy 回去」在这里不需要：
  两边都是我们自己的纹理。
- **必须的一处修正**：VV 的 `NVSDK_NGX_D3D12_EvaluateFeature` 也经过 RT 探针的 detour（NR 就是
  靠这条路径接进去的），而引擎路径的锐化假定 `Output` 在 `ALL_SHADER_RESOURCE`——VV 的输出那一刻
  是 `UNORDERED_ACCESS`。所以 VV 在自己的 evaluate 外面套 `sr::OwnEvaluateScope`，detour 见到它
  就跳过引擎锐化（`RenderProbe.cpp` 的 `if (result == 1 && out && !sr::insideOwnEvaluate())`）。
  没有这层，打开 `sr.sharpening` 会在 VV 下产生状态不匹配的 barrier。
- **落点语义与 RT 不同的一处**：arg10 里没有 HUD / 准星，但**第一人称手在**（EXP-0058 的品红实验），
  所以 VV 下锐化会连手一起锐化。HUD 与文字仍然不受影响。
- **离线数值（离线逐行重放 `kRcasHlsl`，输入是本管线自己 dump 的 1440p DLSS 输出，强度 0.672）**：

  | 输入 | 编码域 mean | p99 | 边缘区 / 平坦区 | 整体均值比 |
  |---|---|---|---|---|
  | `sr_dlssout_2560x1440_5/3/1` | 0.82 / 0.82 / 0.66 (÷255) | 6.7–8.6 | 3.0–4.1 vs 0.39–0.46 | 1.0021–1.0024 |
  | `sr_dlssout_2560x1369_4` | 0.37 (÷255) | 3.5 | 1.5 vs 0.24 | 1.0019 |

  即：变化集中在边缘（平坦区只有一个零头），整体亮度几乎不动（+0.2%）。
  证据 `docs/agent/evidence/vv-sharpen-offline-EXP-0096.txt`。
- **运行时（EXP-0096，dll `d846ec24`）**：`sharp=off` 三帧 → 打开后 `[sr] RCAS 就绪/已启用` →
  `sr arg10` 到 #1200 都是 `sharp=on` 且 `eval=1`，100% 原生保活态同样如此；正常退出、无 crash。
  证据 `docs/agent/evidence/vv-sharpen-EXP-0096.log`。
- **仍未取到**：同帧 GPU 对（`sr_sharp_*` / `sr_dlssout_*`）。日志里没有 `[vvsr] sr dump`，
  说明 F7 没被按下；取法是面板开着锐化时按一次 F7，或往游戏根目录放 `vv_dump_sr.txt` 标记。
  `dumpInputs()` 已经会顺带写 `sr_sharp_*`（`DumpState::ShaderResource`）。

## 8. 成品帧截图

TRP 0.5.3 的做法是钩住游戏自己的截图保存、再用成品帧覆盖文件；Bedrock 没有这样的函数可钩，
等价物是「自己按一个键、在 Present 里抓当前后缓冲」。实现：`src/neurodragon/ui/Screenshot.{h,cpp}`
（D3D11 + WIC，独立编译单元），由 `Overlay` 在 **drawFrame 之前**调用。

- **时机**：Present 钩子里、画我们的面板之前 ⇒ 文件是「屏幕所见」：DLSS/NR 之后的成品帧、
  含 HUD、**不含面板**、输出分辨率。
- **键**：`ui.screenshotKeys`（默认 `VK_F2`，Bedrock 不用这个键）；面板开/关都生效，按键被吞掉；
  「高级」页可直接改键（与面板键/NR 键同一个捕获机制）。
- **落盘**：`<version>/screenshots/YYYYMMDD-HHMMSS-mmm-N.png`（**游戏根目录**，与 `mods/`、`logs/` 同级）。
- **实现细节**：`CreateWrappedResource` 之后 `AcquireWrappedResources` → `CopyResource` 到 staging →
  `Flush`（11on12 的命令在 Flush 时提交，Map 才能等到数据）→ Map → 转 BGRA8 → WIC PNG。
  11on12 的列 pitch 从 `Map` 拿，不自己算。格式支持 B8G8R8A8 / R8G8B8A8 / R10G10B10A2 /
  R16G16B16A16_FLOAT（后者按 Reinhard+gamma 编码，与 dump 同曲线）。
- **反馈**：按下后**聊天栏**出现「截图已保存：<绝对路径>」/「截图失败（见日志）」——
  走 `ui::notice::chat`（`GameNotice.cpp`）：投给客户端线程，调
  `ClientInstance::getGuiData()->displayClientMessage`，纯客户端消息、不发包、不过滤。
  路径**必须**是绝对路径（`getModDir()` 给的是相对的）。
- **代价**：整条链路最初在 Present 线程同步跑 395 ms/张（可见卡顿）；PNG 编码已移到单独线程
  （`CreateThread`，卸载时 join），Present 线程只剩读回 + 转 BGRA8。
- **实测（04:50）**：两张 PNG 都是 2560×1440、格式 28（`R8G8B8A8_UNORM`）、5.7–5.9 MB、
  含 HUD、不含面板、颜色正确。

## 9. 读数、自检与遥测

- 「超分」页：内部分辨率（含「尺寸（实测）」「引擎质量槽（创建时）」回读）、DLSS 模型（含
  「引擎预设（创建时）」）、锐化、引擎上采样配置（实测）、尺寸回调统计、自检与遥测、操作。
- **四项自检**（对应 K §2.2 ③ 与 I §7 的风险登记）：尺寸回调包装 / 帧上下文钩子 / 因子表定位 /
  创建覆盖（CreateFeature）——每项显示「正常」或「失败」。因子表要进过一次世界才定位；任何一项
  「失败」都说明游戏更新把签名挪了，SR 会静默失效。
- **遥测**：SR 求值次数、求值失败次数、输入格式（引擎 `Color` 的 DXGI 格式）、HDR 输入（创建 flags
  bit0）、抖动（引擎当帧写入的 `Jitter.Offset.X/Y`，单位像素）。
- **「引擎超分（只读）」整块已删除**：它把「我们要求的值」和「引擎实际做的值」混在一起，四项尺寸读数
  又和新「尺寸（实测）」重复，且强制重建期会显示临时槽。留下的只有能验证写入的三项：尺寸链、
  创建时的质量槽、创建时的预设。

## 10. 未完成 / 未排

| 项 | 状态 | 说明 |
|---|---|---|
| 组合预设包（I 的 M2 剩余） | **暂不做** | 用户 2026-10-24 决定「后面再说」 |
| 75%（TRP 的 78%） | 未排期 | 见 §4.1；要推翻一条已定结论，需用户拍板 |
| mip LOD 偏置 | 从未碰 | D3D12 不能照抄 D3D11 重建采样器；得钩 `CreateRootSignature`/`CreateSampler` 改 `MipLodBias`。风险：UI/后处理一起变糊或闪 |
| 锐化 1.0 / 无过冲变体 | 可选打磨 | 见 §7 |
| 换 `nvngx_dlss.dll` | 已换入 310.9.1（离线验证与 310.5.3 同构；备份 `nvngx_dlss.dll.bak-310.5.3`） | 可随时还原备份回滚；发布包要不要随包带运行时另议 |
| 监控与 NR 拆分 | 未实施 | 见 [ui §8](ui.md) |

**待实机复核的验收清单**（2026-10-24 部署时只跑了一部分，下次进游戏补）：

1. 锐化 0.672 / 1.0 的观感、整帧开关下可否辨认、1.0 是否出现白边；
   日志看 `[sr] RCAS 锐化已启用：强度 …（增量 … 倍）` 与 `[sr] 锐化前后 dump（强度 …）`。
2. 截图：按 F2 生成 `<version>/screenshots/*.png`、内容含 HUD 不含面板、面板开着按 F2 也不含面板、
   连按两次两个文件、拖窗口后仍是新尺寸；`[shot] 已排队` / `[shot] 已写入` 两行耗时（编码移到线程后的新数）。
3. 锐化每累计 120 帧自动写 dump；拖动/缩放窗口约 10 秒无花屏、无冻结，停下后自动恢复。

## 11. CreateFeature 的调用路径（留档）

```
exe+dfc7102   ← sizing 0xdfc6c00 + 0x502
exe+dfb606a   ← 0xdfb5ff0 + 0x7a
exe+dfc545b   ← shim 0xdfc53c0 + 0x9b
exe+b725025 … 游戏侧
```

sizing 入口先比较 active/requested mode、factor、feature handle、display size；只有状态变化或 handle
为空才会走到 +0x502 的 CreateFeature（`0x14e412d30`）。因此直接调用 sizing 而不改状态不会重建——
nudge（换槽）是必要的。

## 12. 参考

- 旧 `J`（实现、证据与坑，239 行）：[archive/2026-10/J-sr-implementation.md](../archive/2026-10/J-sr-implementation.md)
- 旧 `I`（控制层 L1–L6、三层分辨率、M0 结果）：[archive/2026-10/I-sr-final-plan.md](../archive/2026-10/I-sr-final-plan.md)
- 旧 `H`（三种「能」与预设四层）：[archive/2026-10/H-sr-plan.md](../archive/2026-10/H-sr-plan.md)
- 引擎 NGX 参数契约与「不要写引擎资源」：[nr §3](nr.md)
- VV 下接管 TAAU：[vv](vv.md)；TRP 的 SR 功能对照：[trp §2](trp.md)
- NGX SDK：`nvsdk_ngx_defs.h` 的 `DLSSOptimalSettingsCallback`、`DoSharpening`、`DLSS.Hint.Render.Preset.*`
- 工具：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp`（`--sr`）、`compare_bmp.py`、`check_shader.py`
