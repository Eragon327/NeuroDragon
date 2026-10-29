# NR：神经渲染（NGX feature `0x12`）

> **状态**：现行设计 + 执行记录。RT 路线已跑通并上屏，位置轴两档都已实机验收。
> 本文合并了旧 `D`（NR 事实）、`L`（剩余功能与探针）、`N`（位置轴）、`G`（TRP 客制化对照）；
> 原文归档在 [archive/2026-10](../archive/2026-10/)，探针的原始日志也留在那里。
>
> **本文回答**：NR 怎么接进游戏、参数怎么填、数据怎么流、现在做到哪、还剩什么。
> **不回答**：SR（[sr](sr.md)）、VV 下怎么替换 TAAU（[vv](vv.md)）、TRP 功能盘点（[trp](trp.md)）。

## 0. 现状与一句话

```
超分白送、NR 已跑通：引擎自己加载 NGX 并每帧调 DLSS（RT 下）；
NR 是我们自己的 feature 0x12，用 core 的 CreateFeature 建、在引擎的 command list 上逐帧求值。
```

| 项 | 值（2026-10-24） |
|---|---|
| 接入方式 | NGX feature **`0x12`**，runtime `nvngx_dlssnr.dll`（310.9.16，266 MB，不进安装包） |
| 创建方式 | **core**（`_nvngx.dll`）的 `CreateFeature`；模型 DLL 自己的 `Init_Ext` 恒失败 |
| 硬性前提 | 驱动 **≥ 617.42**；模型文件**必须叫 `nvngx_dlssnr.dll`** 且位于 **exe 同目录** |
| 上屏 | 前档：改引擎参数对象的 `Color`，由引擎 DLSS 放大；后档：直接写回引擎 `Output` |
| 默认 | `nr.enabled=false`（面板启用）、`replace=6`（TRP Ratio）、`placement=0`（放大前） |
| 成本 | 前档 **3.5–3.8 ms @1485×835**（~29% 帧预算）、100% 时 8.75 ms；后档 **9.78 ms @2560×1440** |
| 同帧引擎 DLSS | 1.2–1.8 ms（1485×835 → 2560×1440） |

## 1. 工作约定（本主题的执行规则）

1. **不盲猜**。运行时行为（运行时接不接受某个值、某条路径有没有生效）不许"按经验推断"，
   一律先加探针：TEST 日志 / 参数往返读回 / BMP dump / `ngx_probe` 离线复现。
   探针结论写进本文（§9）或对应 spec，再改实现。
2. **探针优先于反复试错**：一个未知项最多用一次实机验证回答，不允许"改一版、进游戏看一眼、再改一版"。
   每个未知项在动手前先写清「判定判据」。
3. **每批一次交付**：`--test=y` + `--test=n` 双构建 → 部署到
   `E:\LeviLauncher\versions\1.26.51.01\mods\NeuroDragon` → 给用户 3–5 条验收清单 → 按实机结果修正。
   用户没确认上一批之前，不开始下一批（除非用户明确要求）。
4. **不 commit**：改动留在工作区，由用户自己提交。
5. **文档同步**：新结论进对应 spec，用户可见变化进 `CHANGELOG.md`。
6. **新增 UI 文案**：`src/lang/{zh_CN,en_US}.json` 双语 + 重跑 `python scripts/collect_glyphs.py`。
7. **每个新开关都要能被验证**：面板「诊断」页有「本帧写入」回读，新参数必须在回读里出现，
   否则等于假开关。

## 2. 运行时契约

### 2.1 可以直接用 TRP 的（`D:\Documents\theosrp`，0.4.5 工作树 / `origin/main` = 0.5.3）

| 项 | 值 |
|---|---|
| 入口来源 | `nvngx_dlssnr.dll` 自己导出的 `NVSDK_NGX_D3D12_*`（TRP 走这条）；**本项目走 core（`_nvngx.dll`）的导出**，见 2.3 |
| feature / api version | `0x12` / `0x15` |
| app id（先借用跑通） | GUID `f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11`，CmsId `0x0876232C`，engine version `0.1.0-neural-rendering-source` |
| 每帧 Set（资源） | `DLSSNR.Color` / `DLSSNR.MVec` / `DLSSNR.Depth` / `DLSSNR.Output`；可选 `ControlMask` / `Backbuffer` / `UI` / `UIAlpha` |
| 每帧 SetF | `DLSSNR.MVecScaleX/Y`（像素单位） |
| 不需要的 | **jitter / exposure**：NR 参数表里没有这两项（引擎写 `Jitter.Offset.*`、`DLSS.Pre.Exposure` 是给 DLSS 的） |
| 内核包 | `.trpk` 只服务 RTX 20/30/40；**50 系用原始 kernel，不需要** |
| 打包体积 | 只带 `nvngx_dlssnr_310.9.16.dll`（266 MB）；legacy 310.8（165 MB）与 int8（310 MB）不必带 |

### 2.2 必须自己提供的

- **device**：从 `Color` 的 `ID3D12Resource::GetDevice` 取（引擎 NGX 参数对象直接给 `ID3D12Resource*`，
  所以 [renderdragon §5.1](renderdragon.md) 的 bgfx 桥在 RT 路线上**不需要**）；
- **command list**：用游戏自己的（`EvaluateFeature` thunk 第一参数）；
- **同规格输出纹理**：格式沿用引擎 Color（linear HDR），NR 要吃未色调映射的辐射度；
- **历史 reset 时机**：维度切换 / 传送 / 加载 / 开菜单（NR 有内部历史，错了就是拖影）；
- **runtime 落盘位置**：放 exe 同目录（core 的唯一搜索位置之一），不要指望进程搜索路径。

### 2.3 三条硬性前提（每条都有反例）

1. **驱动 ≥ 617.42**：核心 `_nvngx.dll`（32.0.16.1742）的 feature 名字表里才有 `dlssnr`；
   610.88 的核心（32.0.16.1088）没有，`CreateFeature(0x12)` 返回 `0xBAD0000C`（OutOfDate）。
2. **模型必须叫 `nvngx_dlssnr.dll`，放 exe 同目录**：用版本名（`nvngx_dlssnr_310.9.16.dll`）
   返回 `0xBAD0000B`（找不到/初始化不了该库），改名后立刻 `0x00000001`。
3. **必须用 core 的 `CreateFeature`**（core 自己加载模型），不要用 runtime 自己的 `Init_Ext` /
   `CreateFeature`——后者在干净进程里也恒为 `0xBAD00002`（PlatformError），与 Minecraft 无关。

证据链（同一台机器，只改一个变量）：

| 条件 | core `CreateFeature(0x12)` |
|---|---|
| 610.88，无模型 | `0xBAD0000C` OutOfDate |
| 617.42，应用目录无模型 | `0xBAD0000B` UnableToInitializeFeature |
| 617.42，应用目录有 `nvngx_dlssnr.dll` | **`0x00000001`，handle 非空** |
| 617.42，只放版本名 DLL | `0xBAD0000B`（名字是硬要求） |

另外两条实测事实：

- 模型 DLL **不实现 `Init`**（`NVSDK_NGX_D3D12_Init` 的 RVA 处是 `mov eax, 0xBAD00001; ret`），
  模型与 core 之间是运行期动态加载，不是静态依赖；
- core 只从 **exe 同目录** 或 **Init 时 `PathListInfo` 给的目录** 加载模型；`SetDllDirectoryW`、
  当前目录、`AddDllDirectory`、NGX 的 `dataPath` 全部无效（A–G 组探针，别再试）。
  TRP 能把模型放自己模组目录，是因为它在自己的 `Init_with_ProjectID` 里设了 `PathListInfo`。

### 2.4 模型随包落地

包内带 `mods\NeuroDragon\nvngx_dlssnr.dll`（或 `nr.modelFile` 选中的文件），`load()` 时按
**硬链接 → 复制 → 明示日志** 落到 `Minecraft.Windows.exe` 同级：

1. 目标已存在 → 记一行大小后跳过，**绝不覆盖**（用户自己放的版本优先）；
2. `CreateHardLinkW`（同卷零拷贝，普通权限即可）；
3. 失败 → `CopyFileW`；
4. 再失败 → 打印"游戏目录不可写：请把 `<源>` 复制到 `<目标>`"，NR 报不可用，**不崩**。

未走的两条路（留档）：① 游戏内二次 `Init_ProjectID` 设 `PathListInfo`——pathlist 有效，但引擎已
Init 过，二次 Init 有弄坏引擎 DLSS 会话的风险；② IAT hook core 的 `LoadLibraryW/ExW` 改写模型路径
——可行但需要 patch 驱动目录里 DLL 的 IAT，暂不做。若遇到游戏目录只读的安装形态，优先做 ②。

## 3. 引擎侧的 NGX 契约（RT 路线）

### 3.1 引擎每帧写入的参数（实测）

```
SetUI  PerfQualityValue=1  RTXValue=0  DLSS.Feature.Create.Flags=3  Reset=0
SetPtr Color  Output(双缓冲交替)  Depth  MotionVectors
SetF   Jitter.Offset.X / Y            ← 引擎已给亚像素抖动，NR 不用自己算
SetF   Sharpness=0.35  DLSS.Pre.Exposure=1  DLSS.Exposure.Scale=1
SetF   MV.Scale.X=Width  MV.Scale.Y=Height   ← 单位是像素，不是归一化
SetPtr TransparencyMask / ExposureTexture / DLSS.Input.Bias.Current.Color.Mask  (0)
SetPtr GBuffer.{Albedo,Roughness,Metallic,Specular,Subsurface,Normals,ShadingModelId,
               MaterialId,Attrib.8..15}  (0)
SetPtr MotionVectors3D / IsParticleMask / AnimatedTextureMask / DepthHighRes /
       Position.ViewSpace / RayTracingHitDistance / MotionVectorsReflection  (0)
```

命名**无前缀**（不是 `DLSS.*`）。`Width/Height` 是渲染分辨率（1485/835），`OutWidth/OutHeight` 是
放大目标（2560/1440），子矩形键是 `DLSS.Render.Subrect.Dimensions.*` 与
`DLSS.Input.{Color,Depth,MV,Translucency}.Subrect.Base.X/Y`。`DLSS.Feature.Create.Flags=3`
（`IsHDR | MVLowRes`）**只在创建时生效**；不设就没有子矩形感知的输入路径。

### 3.2 六条规则（每条都有运行证据）

1. **不要写引擎的 `Color`。** 它参与引擎的时序链，写进去一帧近黑内容后整条链永久死掉
   （之后每帧读回都是 0）。
2. **不要 transition 引擎拥有的纹理**（`Color/Depth/MV/Output`）。NGX 自己管理这些资源的状态；
   我们把它 `ALL → UAV → ALL` 来回切会打乱引擎 feature 的内部跟踪。
3. **一个进程里只能有一个 DLSS SR 实例。** 我们 `CreateFeature(1)` 自建第二个 SR 之后，引擎那个
   feature 就再也产不出正确结果（`EvaluateFeature` 仍返回 1，但输出不再更新，只有重启能恢复）。
4. 求值必须用钩子的 **original** 指针。`GetProcAddress` 拿到的是我们自己的 detour，会递归到栈溢出。
5. 我们的 SR 里必须按上面的契约设 `MV.Scale.*`（否则历史不重投影 → 内容固定在屏幕上）与
   `DLSS.Feature.Create.Flags=3`（否则子矩形输入路径没开 → 竖条纹垃圾）。
6. **`Reset` 无前缀**：`DLSS.Reset` 不被读取；跨存档/跨关卡的时序残留要写 `Reset=1`（一次性）。
   引擎每帧重写 `Color`（P4 已验），所以合成不会形成反馈环；关闭/卸载时仍会把 `Color` 指针还回去。

### 3.3 参数对象的槽位（实测，和 SDK 头文件不一致）

| 操作 | 槽位 | 语义 |
|---|---|---|
| Set | **1** | 指针/资源 |
| Set | **3** | unsigned |
| Set | **6** | float（不是 `ID3D12Resource*`！SDK 头是 1=float、6=资源，**1 与 6 互换**） |
| Get | **8 / 9** | 资源指针（两者都返回同一个，`10..15` 全 `0xBAD00010`） |
| Get | **11** | unsigned（`Width`） |

读资源顺序：**8 → 9 → 14 兜底**，并打印返回值。**禁止盲探测参数对象**（依次写 slot 0..7
可能命中 `Reset`/非预期重载，把参数表清脏 → `nvngx_dlss` 读 `0x10000004A` 崩溃）；
只读探测安全，任何写操作必须先确认槽位语义。

## 4. 我们写入 NR 的参数

创建参数（全部经实测槽位：1=指针/资源、3=uint、6=float；读回 11=uint）：

| 时机 | 键 | 值 |
|---|---|---|
| 创建 | `DLSSNR.Width/Height` | 当前 feature 尺寸（前档 = 渲染尺寸；后档 = 输出尺寸） |
| 创建 | `DLSSNR.ScalingRatio` | 1.0 |
| 创建 | `DLSSNR.Hint.Render.Preset` | `nr.networkPreset`（0 = 默认网络 / 1 = 上一版） |
| 创建 | `PerfQualityValue` / `CreationNodeMask` / `VisibilityNodeMask` | 2 / 1 / 1 |
| 每帧 | `DLSSNR.Color` / `Depth` / `MVec` / `Output` / `Backbuffer` | 引擎资源；`Backbuffer` = 我们的 Output |
| 每帧 | `DLSSNR.{Color,Output,Backbuffer}Subrect` | 0,0 / feature 尺寸 |
| 每帧 | `DLSSNR.{Depth,MVec}Subrect` | 0,0 / guide 尺寸（渲染分辨率，整张 guide 纹理） |
| 每帧 | `DLSSNR.UI / UIAlpha / ControlMask` 及其子矩形 | null / 0 |
| 每帧 | `DLSSNR.MVecScaleX/Y` | guide 尺寸（像素单位）；`nr.mvecScale` 可覆盖 |
| 每帧 | `DLSSNR.Enabled/Style/UseAutoMask/UICorrection/DepthInverted/Reset` | 1 / style / 0 / 0 / depthInverted / 窗口首帧 1 |
| 每帧 | `DLSSNR.Intensity/LocalTone/LocalStructure/SkinStructure` | 配置值（TRP 默认 1.0） |

类型随运行时分：310.x 的 `Enabled/Style/Reset/DepthInverted/UseAutoMask/UICorrection/Hint.Render.Preset`
是 **int**，尺寸/子矩形是 **unsigned**，强度是 **float**。

固定/未启用项：`SkinStructureStrength`（`-1` 哨兵在 MC 内容上无可辨差异，正式版面板不提供，TEST 保留）、
`UseAutoMask` / `UICorrection`（没有 `ControlMask`/`UI`/`UIAlpha` 输入纹理，写了也是假开关）、
Style 3–7（310.x 上 ≥2 全等于 Cinematic）。

## 5. 数据流与实现

### 5.1 前档（Before upscaling，默认）

```
引擎 Color (linear HDR, 峰值 ~69)
  └─ 编码 pass：Reinhard(x/(1+x)) + gamma 2.2 → 私有 encoded 纹理
       └─ NR evaluate（core feature，cmdlist = 引擎的）
            └─ 合成 pass：gain (replace=5) 或 TRP Ratio (replace=6，默认)
                 └─ 把引擎参数对象的 Color 指向我方纹理 → 引擎 DLSS 放大 → 调色 → UI
```

- **编码是必须的**：HDR 直接喂 LDR 特征会"发蓝、发灰"，主体来自**域不匹配**；编码后
  `|NR − 输入|` 平均绝对差从 4.11% 降到 1.48%。
- **Ratio 合成**（默认）：取比例/增益再合成，四个旋钮 `whitePoint / transferStrength /
  colourStrength / maxRatio`；`broad / detail` 是宽域/局部变化的分档（1.0/1.0 = 不改）。
- **增益合成**（`replace=5`）：`gainBoost` 可放大增益，只用于判断。
- 模式收敛到两个：`5 = 增益`、`6 = TRP Ratio（默认）`；**任何不是 5 的值都按 6 处理**，
  旧的「只求值 0」已删除且不做配置迁移。

### 5.2 后档（After upscaling）见 §7

### 5.3 描述符堆：8 槽，两段互不复用

早期"幽灵地形/手臂"的根因不是合成方式，而是**编码纹理被冻结**：编码 pass 与合成 pass 共用同一个
4 槽描述符堆、各写一次；命令列表先记录后执行，GPU 执行时才读描述符内存，于是编码 dispatch 实际用了
合成 pass 那一套描述符，`gEncodedColor` 只在第一帧被写过。

修复：堆扩到 **8 槽，0–3 给编码 pass、4–7 给合成/解码 pass**，各自绑定自己那一段的 GPU 句柄。
判定手法可复用：把中间纹理按窗口 dump 出来逐字节比较（修复前相隔 11 秒的两个窗口完全相同，
修复后差 41.6/255）。

### 5.4 尺寸与生命周期

- **尺寸变化即重建**（窗口、全屏、超分档都会改渲染分辨率）：`nrEnsureRenderSize` 每个入世帧比一次，
  尺寸没变时两次比较即返回。
- **先建后拆**：新 feature 建成功后才把旧的移进退役队列；反过来每次改分辨率都要重载 264 MB 模型。
- **纹理只增不减**：引擎 Color 是「显示分辨率大小、内容在左上渲染子矩形」，输出/编码/解码都照它的
  desc 分配；只有新尺寸更大才重分配，变小只改子矩形。
- **退役对象等 GPU 空闲再释放**：overlay 的 `ExecuteCommandLists` 钩子拿到游戏直接队列
  （`ui::overlayGameQueue()`），Signal 一个 fence、最多等 3 s；队列未知或超时就留着，下次重建再试
  （泄漏一次比「GPU 还在用就释放」安全）。
- 每会话最多 **64 次**重建，两次重建至少隔 **20 次求值**：动态分辨率每帧变时宁可不跟随也不每帧重建。
- **关闭 NR 会真正卸载**（`nr.enabled=false` + 保存为默认 → `nrUnload()`）：feature、输出、编码/解码
  纹理一起进退役队列，fence 后 Release；编码 PSO/根签名/描述符堆保留。**「应用」不卸载**（只停求值），
  这样调参时来回勾选不必每次重载 264 MB；重新启用走正常创建路径（~0.3 s 模型重载）。

## 6. 配置与面板

`<模组目录>/config/config.json`，`ll::config`，聚合 struct + `version`（当前 3），
文件缺失/缺键/版本不符 → 自动补默认并回写；写坏 → 记 error 后重写默认，不崩。
**只在启动时导入**，运行期不读文件（面板的「应用」写内存，「保存为默认」才落盘）。

| 键 | 默认 | 说明 |
|---|---|---|
| `nr.enabled` | `false` | 总开关（面板「神经渲染 → 启用神经渲染」） |
| `nr.placement` | `0` | 0 = 放大前，1 = 放大后；切档重建 feature |
| `nr.replace` | `6` | 5 = 增益合成，6 = TRP Ratio；其余值按 6 |
| `nr.gainBoost` | `1.0` | 增益合成专用 |
| `nr.style` / `intensity` / `localToneStrength` / `localStructureStrength` | 0 / 1 / 1 / 1 | 网络风格与强度 |
| `nr.skinStructure` | `1.0` | `-1..2`；MC 内容上无可辨差异，正式版面板不提供，TEST 保留 |
| `nr.inputPercent` | `100` | NR 工作尺寸 = 渲染尺寸 × 50/66/100，guide 仍全分辨率 |
| `nr.networkPreset` | `0` | 0 = 默认网络，1 = 上一版（创建时生效，需重启） |
| `nr.modelFile` | 空 | 模组目录里的模型文件名；下次启动落到 exe 目录 |
| `nr.compare` / `compareSplit` | 0 / 0.5 | 分屏对比（诊断，会话内有效） |
| `nr.adaptiveExposure` / `exposureBias` | false / 0 | 自适应白点 + EV 偏置 |
| `nr.broad` / `detail` | 1 / 1 | Ratio 合成的宽域/局部分档 |
| `nr.whitePoint` / `transferStrength` / `colourStrength` / `maxRatio` | 1 / 1 / 1 / 2 | Ratio 合成旋钮 |
| `nr.mvecScale` / `nrEncode` | `-1` / `-1` | <0 = 自动（MV 用 guide 尺寸 / 按 Replace 选编码） |
| `nr.depthInverted` | `-1` | -1 = 跟随引擎 |
| `nr.sweep` | 0 | TEST 探针：1 = Style 轮换，2 = Enabled 冻结测试 |
| `nr.resetEveryFrame` / `srResetEveryFrame` | false | 诊断用，每帧清历史 |
| `nr.paramLog` | false | TEST 构建才有效：记录引擎 NGX 参数写入 |

面板热键（`ui.*Keys`，`Config.h` 的默认值）：`End` 面板、`[` NR 开/关、`]` 重置 NR+DLSS 历史、
`F2` 截图。热键是可重绑的**组合**，空数组 = 玩家主动解绑。

## 7. 位置轴：Before / After upscaling

### 7.1 两档语义

| | 前 NR（Before，默认） | 后 NR（After） |
|---|---|---|
| 输入 | 引擎 `Color`（渲染分辨率，linear HDR） | 引擎 `Output`（输出分辨率，引擎 DLSS 刚写完） |
| NR 工作尺寸 | 渲染分辨率（可 ×50/66/100） | **输出分辨率**（默认 100%） |
| guide | `Depth`/`MVec` 渲染尺寸，`MVecScale` = 渲染尺寸 | 同左（guide 比 color 小） |
| 结果去向 | 合成后改引擎参数对象的 `Color` 指针 | 合成后 `CopyTextureRegion` **写回引擎 `Output`** |
| 之后谁接手 | 引擎 DLSS 放大 → 调色 → UI 合成 | 引擎调色 → UI 合成（不再放大） |
| 成本（1440p） | 3.5 ms @1485×835；66% ≈1.5、50% ≈0.9（预期） | **9.78 ms @100%**；66% ≈4.6、50% ≈3.0（估算，P3 未开工） |

- 「拷回」的等价物：前档没有真的拷回放大器输入，而是换指针（Route A），对放大器是同一件事，
  且不碰引擎资源。
- 「接管最终图像」的边界：Bedrock 的调色与 UI 合成在 SR **之后**，所以我们接管的是**调色前的
  最终 HDR 图**，调色/UI/Present 仍归引擎——这正好符合「UI 不被 NR 处理」，也是阶段 3 FG 要的
  HUD-less 源。

### 7.2 为什么后来敢重开 After 档

旧结论是「不做」（旧 [G §4](../archive/2026-10/G-nr-customization-plan.md)：要接管 `EvaluateFeature` 输出、UI 合成与 present；旧 [K §3.5](../archive/2026-10/K-trp-feature-gap.md)：成本 4 倍）。
**判据变了**：RCAS 锐化已经在引擎 `Output` 上做过 `ALL → COPY_SOURCE → COPY_DEST → ALL` 往返，
两次实机画面正常、窗口缩放不崩 ⇒ After 只需要「读 `Output` + 写回 `Output`」，**不需要接管 UI 合成与
present**。于是它从「结构性不可做」降级为「一个待探针回答的契约未知」：guide（渲染分辨率）比
color（输出分辨率）小，310.x 运行时接不接受。

### 7.3 P0 探针结论（离线，`ngx_probe --after`）

走 core 的 `CreateFeature(0x12)`（和模组同一条路径），`R16G16B16A16_FLOAT`，两帧求值：

| 变体 | color/output | guides | MVecScale | create/eval | 输出 mean | 跨帧 meanAbsDiff |
|---|---|---|---|---|---|---|
| before（对照） | 1485×835 | 1485×835 | 1485×835 | 1 / 1,1 | 0.802 → 0.825 | 0.130 |
| after-guide | 2560×1440 | 1485×835 | 1485×835 | 1 / 1,1 | 0.768 → 0.793 | 0.169 |
| after-output | 2560×1440 | 1485×835 | 2560×1440 | 1 / 1,1 | 0.768 → 0.787 | 0.166 |

⇒ **`color > guide` 的 after 契约成立**，不需要 O1（自升采样 guide）或 O2（降采样）；
两档 `MVecScale` 的 frame1 输出不同 ⇒ 运行时确实消费它，默认取 **guide 尺寸**（TRP 310.x 契约同款）。
补测：模组真实参数组合（非前缀 `Width/Height` = guide、`DLSSNR.Width/Height` = feature）与首轮探针
逐位相同 ⇒ 运行时不吃那两个非前缀键。

### 7.4 实现要点

- `nr.placement`：0/1 互斥，同一帧只跑一个；后档 `DLSSNR.Color` = 引擎 `Output`（display），
  `Depth/MVec` = 渲染尺寸 guide；`Width/Height` = 输出尺寸 ×100%；Color/Output/Backbuffer subrect =
  输出尺寸，Depth/MVec subrect = guide；`MVecScale` = guide。
- 后档求值在引擎 `EvaluateFeature` 返回 1 之后、RCAS 之前（顺序固定 `NR → RCAS`）。
- 写回：`ALL → COPY_DEST → ALL`，用 **`CopyTextureRegion` + `D3D12_BOX`**（私有纹理只增不减，
  窗口缩小时 `CopyResource` 的等尺寸约束不成立）。
- 切位置：重建 feature（先建后拆）+ NR/引擎双侧 `Reset` + 合成渐入；0→1 前先把引擎 `Color` 指针还回去。
- 失败：after 创建/重建被拒 → `afterFallback`，回落 before（core→runtime 完整梯子）并保持旧 feature；
  `Output` 缺一帧跳过该帧；`Output` 非 RGBA16F → 回落并写 `nr.warn.afterFallback`。回落是本会话的。
- 面板：位置两档 + 成本提示；诊断页回读位置 / NR 处理尺寸 / guide 尺寸。

### 7.5 验收状态（2026-10-24 21:45–21:47，主目录 TEST 构建）

- 重建成功：`特征 1485x835 → 2560x1440（placement after，域 2560x1440，guide 1485x835，100%，第 1 次重建）`。
- 窗口 #1（240 帧）：NR median **9.782 ms**（evaluate 9.229 / encode 0.115 / composite 0.427），
  引擎 DLSS 1.841 ms；`evaluate` 无失败、无回落、无崩溃。
- dump：`color_ref` vs `nr2` mean 6.98/255（NR 确实改了画面）；`fed` 与 `base`（写回之后抓）逐字节相同
  ⇒ 写回成功。
- **未显式跑**：分屏对比、快速转视角鬼影、窗口缩小/内部分辨率切换——release 里继续观察；
  出现鬼影/花屏按 §11 处置。
- release 已部署主目录（DLL SHA-256 `fd90f578…`）；TEST 备份 `.bak-test-placement`，
  改动前备份 `.bak-before-placement`。

### 7.6 VV 路径（2026-10-29，EXP-0083 → EXP-0085）

VV 不加载引擎 NGX，但 VV 的 DLSS-SR 自己有一个 `Init_ProjectID` 会话。NR **完全由 RT 探针的
`ngxEvaluateDetour` 驱动**：它在 VV 的 DLSS evaluate 上看到参数对象里的
Color/Depth/MotionVectors/Output/Width/Height，于是用同一个会话 `nrCreate`，之后每帧
`nrEnsureRenderSize` + `nrRecord`，再调用 original DLSS。创建/求值用的是
`gNgxCreateOriginal`/`gNgxEvalOriginal`，所以 NR 自己的 evaluate 不会再次进入 detour。

- VV 侧**没有 NR 编排**：只把 prep 的 color/depth/MV 留在 `ALL_SHADER_RESOURCE`，其余交给
  detour；NR 的输入是 VV 的 RGBA16F/R32F/RG16F 私有纹理，输出 `gDecodedColor` 再交给 DLSS，
  `arg10` 输出路径不变；
- `nr.enabled` 门控，`nr.placement`（before/after）与 `nr.inputPercent` 在 VV 下同样生效；
- 失败/未合成时 detour 不改 `Color`，DLSS 读回 `colorHalf`（整帧无 NR）；
- **已知限制**：合成纹理「只增不减」，窗口缩小后大于 render size，而 VV 的 DLSS 不设 Color
  subrect；固定尺寸之外的 resize 行为未验证。

第一版曾在 VV 里自己调 `nrRecord`，与 detour 同一帧跑两次：固定描述符槽被后一次覆盖，
第二次又以 `gDecodedColor` 为 base 自反馈，画面数帧内全黑（EXP-0083）。修复是删掉 VV 侧
编排，只留 detour 这一份 NR 代码。详见 [vv.md §36](vv.md)。

EXP-0085 又暴露两条 NR 侧问题（均已修）：

- **退出崩溃**：`nrShutdown()` 曾在 `vv::sr::release()` 之后把已释放的 `colorHalf` 写回
  参数对象，下一帧 DLSS evaluate 在驱动里崩。退出路径不再回写 `Color`；`nrUnload()`（渲染
  线程）仍保留回写。
- **周期性闪动**：detour 曾在每个 600 次求值的诊断窗口起点发 `DLSSNR.Reset=1`，NR 历史
  每 ~12 s 被清空。窗口起点不再 reset；换世界时用 `nrRequestReset()` 做双侧重置。
- **移动才闪（MV 单位）**：`nrRecord` 默认把 `motionVectorScale = width/height` 当归一化
  MV；VV 的 `mvRg32` 已是像素（`MV.Scale=1`），而且 unprefixed `MV.Scale` 会被写成
  `guideWidth/Height`，连带 original DLSS 也按 1280× 重投影。现在 detour 从参数对象读
  `MV.Scale.X/Y` 传给 `nrRecord`，unprefixed 写同一个有效值；方向仍为 current→previous。

**EXP-0088 通过**（2026-10-29）：`[nr] MV scale 1.000,1.000 (guide 1280x720)`，用户确认
移动无闪动、退出不崩。NR 在 VV 的接入到此可用；`nr.enabled` 仍是 opt-in。

## 8. 已完成 / 未完成

### 8.1 已完成

| 批次 | 内容 | 状态 |
|---|---|---|
| 批 1 | `Replace=6` 接通、参数回读、SkinStructure（→ 正式版隐藏）、网络预设、NR 热键、分屏对比、分阶段计时 | ✅ |
| 批 2 | 输入缩放 50/66/100（fail-safe 回 100）、HueGuard、broad/detail、失败求值不参与合成 | ✅ |
| 批 3 | 运行时模型选择、自适应曝光（1×1 白点 + 归约 pass）、求值失败计数 | ✅ |
| 批 4 | 位置轴 Before/After（P0 探针、P1 实现、P2 顺序与回读、P4 收尾） | ✅ 实机主路径通过，release 已部署 |

### 8.2 未完成 / 暂缓

| 项 | 原因 | 重新评估的触发条件 |
|---|---|---|
| ④ 隔帧（Model rate，3–4 天） | 需 MV+depth 重投影 + change 合成两个新 pass，鬼影风险中高；输入缩放落地后 NR 已 ~1 ms | 输入缩放后 NR 仍 >2 ms，或要把「100% 内部分辨率 + NR」变常驻 |
| ⑦ 稳态网格（1–2 天） | NR 参数表没有 jitter 输入，只能在自己降采样里反抖；且要在发布版装 `SetF` 钩子抓 `Jitter.Offset` | 输入缩放的 A/B 显示 jitter 真的让画面糊 |
| After 档 P3（66/50 降本档） | 等拍板 | 后档 9.78 ms 的卡顿不可接受时 |
| After 档观察项 | 分屏 / 快转鬼影 / 窗口缩放未显式跑 | 出现鬼影或花屏时按 §11 处置 |
| P7 自适应曝光回读探针 | 肉眼无差异；结构上 Ratio 合成让白点对最终亮度基本抵消 | 要做「可测量」的验收时，先加 1×1 白点纹理回读 |
| Style 3–7 / 原始 HDR 直喂 / 多 pass / 对象遮罩 | 见 §10 | 不做 |

## 9. 探针记录（结论）

| 编号 | 问题 | 结论 |
|---|---|---|
| P1 | 310.9.16 是否接受 `SkinStructureStrength=-1` | 三档都返回 1，但 MC 内容上看不出区别；TRP 把它和自动皮肤遮罩配对，我们没有皮肤分类 ⇒ 正式版不提供该滑条，TEST 保留 |
| P2 | 网络预设 0/1 是否真换网络 | 是。默认网络 evaluate 中位数 3.18/3.47 ms，上一版 5.85/5.84 ms（**慢 80%**）；默认仍 0 |
| P3 | 分屏对比两半是否公平 | 左半与引擎原图**逐字节相同**（max=0），右半 mean 2.6/255、36% 像素变化；w0 是加载帧（全黑）不能当结论 |
| P4 | 引擎是否每帧重写 `Color` | 是。整局没有出现"Color 仍是我方纹理"的 warn；加载帧参数对象给的是引擎自己的资源 ⇒ 合成不形成反馈环 |
| P5 | `Replace=6` 与 5 的画面差异 | 用户拍板默认 **6（TRP Ratio）**；删除「只求值」模式，不做配置迁移 |
| P6 | 小 feature + 全分辨率 guide | 实机四条验收通过：创建日志/诊断回读/三行计时/画面正常 |
| P7 | 自适应曝光是否让室内外一致 | 用户反馈肉眼无差异；结构性原因见 §8.2，**搁置**，等白点回读探针 |

## 10. 不做（结构性，逐条给理由）

| 选项 | 为什么不做 |
|---|---|
| 多 pass（2–5） | 每多一 pass 就是 +3.5 ms；我们预算比 Skyrim 紧 |
| 对象遮罩（手/装备/人形/生物） | 要在 RenderDragon 里逐 draw 分类 + 深度预通过重放，周级工作量；NR 在 MC 的增量只有 1.5–2.5%，投入产出不成立 |
| `UseAutoMask` / `UICorrection` | 需要 `ControlMask` / `UI` / `UIAlpha` 输入纹理，我们显式传空；写了也是假开关 |
| Peripheral compression / Combined preparation | 是 TRP 两条自家 prepare pass 的优化；我们只有一条编码 pass，没有可合并的对象 |
| 天气/时段预设、战斗降 pass、返回延迟 | MC 没有对应概念；1 pass 也没什么可降 |
| Style 3–255 | 310.8 / 310.9.16 上 style ≥2 输出全部等于 Cinematic（TRP 源码注释），开放到 255 是假选项 |
| `.trpk` / INT8 / MFG 解锁 | 逆向 patch `nvngx` 内核，法律与稳定性风险高（[decisions/0002](../decisions/0002-no-kernel-patching.md)） |
| HDR 输出 / 输入是线性 HDR 的显示链 | 输出链（交换链 + UI 合成）不归我们；我们只有"线性 HDR → 显示域编码"这一层 |

## 11. 风险与处置

| 风险 | 表现 | 处置 |
|---|---|---|
| After 写 `Output` 与引擎后续 pass 打架 | 花屏 / 冻结 | RCAS 已证明可行；失败自动关 After 并回落 Before，不留死状态 |
| 切位置时历史错位 | 拖影 / 重影 | 切位置强制双侧 `Reset=1`，切换后首帧不合成 |
| NR 历史建立在「已放大」的图像上 | 快速转视角出现鬼影 | 验收单列一条；严重则加「移动时降低合成强度」作为收尾项 |
| 100% 成本 ≈9.6 ms | 帧率明显下降 | After 档的固有代价；P3 给 66/50 降本档，面板直接显示成本 |
| 描述符堆共用 / 纹理冻结 | 画面里出现不存在的地形/手 | 编码/合成各用一段堆槽（§5.3）；用 dump 逐字节比较判断中间纹理是否在更新 |
| DLSS 实例冲突 | 引擎 SR 输出冻结 | 不建第二个 SR feature（[sr](sr.md) §2） |

## 12. RR 地基（阶段 4，不做但留判据）

RR = NGX feature **13**（`0x0D`），runtime `nvngx_dlssd.dll`，参数 `DLSSD.*` + `DiffuseAlbedo` /
`SpecularAlbedo` / `Normals` / `Roughness` / `DiffuseHitDistance`。Skyrim/TRP 结构上不可能有 RR
（光栅化，没有 noisy radiance），所以「TRP 没有、无参考」是必然的。

Mac 1.26.32 全符号 IDB 实测：RenderDragon 的 RT 降噪集中在一个函数里

```
dragon::framerenderer::drawcommands::doAllRayTracing(
    ComputeCommandContext&, unsigned short, RayTracingFeatureConfiguration const&,
    RayTracingResources const&, ServerResourcePointer<ResolvedMaterialResource>,
    span<RtLightInfo>, span<uint>, unsigned, unsigned)
```

- Mac base `0x10b72e700`，大小 `0x1746`；引用 `TemporalDenoising`、`FinalizeDenoising`、
  `AdaptiveDenoiserCalculateGradientsInline`、`AdaptiveDenoiserGenerateReferenceInline`；
- `RayTracingResources` 持有 `Diffuse2Denoised` / `DiffuseChroma2Denoised` / `Specular2Denoised` /
  `AdaptiveDenoiserGradients{0,1}` / `AdaptiveDenoiserReference` / `AdaptiveDenoiserPlaneIdentifier`。

三条结论：

1. **引擎自带降噪** ⇒ 引擎的 `DLSSD.*` 参数位全 0 是「引擎不用 RR」，不是「没写完」；
2. **RR 的插入点不是 `SetPtr("Color")`**：现在的 Color 是降噪之后的 DLSS 输入，喂给 RR 只会双重降噪；
   真 RR 要 `doAllRayTracing` 内部的噪声 radiance + G-buffer ⇒ 与 NR 是两条独立路线，**不复用注入点**；
3. 这些地址来自 Mac/Metal 版，Windows 1.26.51 必须按字符串 / RTTI 重新定位。

**决定性判据**（1–2 天可在 IDB 里答掉）：能否拿到 `RayTracingResources`（或它背后的噪声 radiance /
G-buffer）。答不上来 ⇒ RR 不可做，NR 就是本平台唯一的神经渲染路线。

## 13. 参考

- 旧 `D`（NR 事实权威，807 行实测记录）：[archive/2026-10/D-rt-dlss-nr-plan.md](../archive/2026-10/D-rt-dlss-nr-plan.md)
- 旧 `L`（批次、验收、探针全文）：[archive/2026-10/L-nr-remaining-plan.md](../archive/2026-10/L-nr-remaining-plan.md)
- 旧 `N`（位置轴路线与验收）：[archive/2026-10/N-nr-placement-plan.md](../archive/2026-10/N-nr-placement-plan.md)
- 旧 `G`（TRP 客制化逐项推理）：[archive/2026-10/G-nr-customization-plan.md](../archive/2026-10/G-nr-customization-plan.md)
- Windows 定位与 NGX 参数读取全过程：[research/2026-10-22-windows-probe.md](../research/2026-10-22-windows-probe.md)
- 引擎侧 SR 参数契约与 SR 控制：[sr](sr.md)；TRP 对照：[trp](trp.md)
- 工具（不进 mod 仓库）：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp`（含 `--after`、`--sizes`、`--sr`）
- 模型来源：`E:\V12\MO2\mods\DLSS5+6倍帧生成\SKSE\Plugins\TheosRenderPipeline\NVIDIA\nvngx_dlssnr_310.9.16.dll`
  （复制到 exe 目录并**改名**为 `nvngx_dlssnr.dll`）
