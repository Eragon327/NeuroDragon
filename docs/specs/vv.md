# VV 路径（阶段 2）：把 NR/SR 接进 Vibrant Visuals，替代 TAAU

> **状态**（2026-10-25 16:0x 更新）：阶段 2 **进行中**。M4 侦察与 M5（注入可行）已收口；
> M6 的 B0（只读取证）与 B1a（自建 NGX 会话）已实机通过；B1b（颜色旁路）**切点成立、颜色未定**。
> 下一步以 **§14** 为准；§5.2、§12 A 两处结论已被 §14.3 更正。
> **2026-10-28 更新**：阶段 2 不再走 §14 的 B1b/B1c 路线，转入 **R3**（§34 分析、§35 实施阶段 A–D）。
> 本文由旧 `O`（VV 路径侦察）、旧 `M §3`（阶段 2 的未知）与旧 `I §5`（M4–M7）合并而来；
> 原文归档在 [archive/2026-10](../archive/2026-10/) 与 [research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)。
>
> **验证环境**：`r3` 起用 `E:\LeviLauncher\versions\1.26.51.01-r3`（2560×1440 全屏）；
> §19–§33 的历史实机日志是在 `E:\LeviLauncher\versions\1.26.51.01-agent2` 上取的。
> **注意**：主目录 `E:\LeviLauncher\versions\1.26.51.01` 是另一个 agent 的并发工作区，
> 部署/实验不要碰它。
>
> **2026-10-28：停止论证 R3，进入实施**（用户拍板）。阶段划分与判据以 **§35** 为准：
> A 纯色写 `arg10` → B DLSS 写 `arg10` → C 换引擎 MV、删自算 MV → D 回归后再 suppress。
> §34 是它的上位分析（为什么把插入点上移一层），§34.2/§34.4 的「阶段一 / 阶段二」细化进 §35。
>
> **2026-10-28 TAAU 复核（静态已确认）**：完整拆解见
> [research/2026-10-28-taau-decompiled.md](../research/2026-10-28-taau-decompiled.md)。本文早期
> 「没有 MV 纹理、靠深度 + VP 重建」的表述已被 shader 资源表与指令证伪（下文 §6 的 2026-10-28
> 更正是逐条更正）。
>
> **EXP-0055（同帧直接指针）**：`arg10` 的依赖名是 `Color0`，native 是前置离屏输出；
> 写入它是**显示分辨率、无 DSV、单 draw** 的前置 list（48-list 帧为 25，72/70/107-list 帧为
> 49/47/84），不是带 DSV 的 `list44`。⇒「`arg10` 直接写后缓冲」「`list44` 是 TAAU 输出 draw」
> 均已证伪。
>
> **EXP-0056（下游消费）**：`arg10` 出 TAAU 后以**共享帧图节点引用**进入 `LuminanceHistogram`、
> `Bloom`（两次）、`LocalExposure`；采样帧内两条 copy 路径均无命中（无 GPU 纹理复制）。
> 证据：`docs/agent/evidence/arg10-list-binding-EXP-0055.log`、
> `docs/agent/evidence/arg10-downstream-EXP-0056.log`。

## 0. 一句话

```
VV 下的 TAAU 每帧恰好 1 次声明 + 1 次 draw，跑在 factor 0.33、jitter 递增的常态上；
引擎每帧只发 1 次 ExecuteCommandLists、里面 48 个 command list ——「帧中间插一段 GPU 工作」
在队列层面第一次有了可操作的抓手（边界落点仍未确认）。
```

## 1. 触发条件（用户口述 + 实测钉死）

`options.txt` 三条同时满足，`taau` 才会真正跑（**读的是 per-user 存储**，不是版本根目录那份，
见 §17.1）：

| 键 | 值 | 说明 |
|---|---|---|
| `graphics_mode` | `2` | Vibrant Visuals（延迟渲染）；`3` = 开光追（走 RTX 帧渲染器，是另一条路径） |
| `upscaling_mode` | `0` | `0 = Taau`，`1 = Bilinear`。**Bilinear 根本不进 taau 分支** |
| `upscaling_percentage` | 非 0 且 < 100 | 实测用 `33`；`0` = 原生 100%，上采样无意义，taau 不触发 |

> **2026-10-26 更正**：上表的值在 **per-user** 存储
> `Minecraft Bedrock/Users/<id>/games/com.mojang/minecraftpe/options.txt` 里（引擎启动读、退出写回）；
> 版本根目录的 `options.txt` 是诱饵，改了不生效。`upscaling_percentage` 的值就是渲染比例
> （实测 `66` → taau `factor=0.6600`），不是档位名。

实测：设成上面三行后，**连主菜单**都在跑 taau（菜单背景全景也走 VV 上采样），
所以探针不用进世界也能验证 hook 是否活着。

> 对照：RT 模式下 VV 的全部锚点命中 0，`taau` 一次都不跑；两套渲染路径互不相交
> （[nr §3.1](nr.md)）。

## 2. Windows 侧定位（无 PDB，离线可复现）

锚点 = 代码自己构造的材质 uniform 名，每个名字在 exe 里**只有 1 处 RIP 相对引用**：

| 字符串 | 命中函数 | RVA（1.26.5101.0） | 结论 |
|---|---|---|---|
| `InputFinalColor` / `InputBufferMotionVectors` / `InputTAAHistory` / 材质 `Upscaling` | `dragon::framerenderer::modules::taau`（declarator） | `0xb74f860` | 每帧 1 次，拿到 viewport / `UpscalingData` / 4 个 framebuffer 依赖 |
| `SubPixelJitterAndValidHistory` / `ResolutionRatiosAndFPEpsilon` / `TAAUpscalingParameters` / `TAAUpscaling` | TAAU 的 draw callback（`std::function` 目标） | `0xb752820` | 每帧 1 次，纹理已解析 |

- 工具：`D:\Documents\NeuroDragonWorkSpace\tools\vv_locate.py`（PE 解析 → 找串 → 扫 `.text` 的 RIP
  相对引用 → 用 `.pdata` 反查函数边界）、`vv_disasm_args2.py`（capstone 反汇编 + 追参数）。
- 两个函数的序言都是 `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC`，探针安装前先校验 15 字节，
  不匹配就拒绝挂钩并报错（`[vv] taau prologue mismatch`）。
- 结构对照：Mac `taau` 0x1348 / 回调 0x10b2；Windows 0x1e33 / 0x1767（MSVC + 19 个补丁的体量差，
  语义一致）。

### 2.1 参数基址（踩过的坑）

`taau` 序言是 **8 个 push + `sub rsp, 0xd58` + `lea rbp, [rsp+0x80]`**，所以「第一个栈参数」不是
`[rbp+0xd00]` 而是 **`[rbp+0xd40]`**（漏算 8 个 push 的 0x40）。按 0xd40 起算，参数 5–12 =
viewport / `UpscalingData` / `UpscalingCameraData` / `MeshFilter` / 4×`BindFramebufferColor`。

### 2.2 依赖对象的取用偏移：Windows 是 `+8`，不是 Mac 的 `+0x10`

反汇编 `taau`（`0x14b74fb3d` 起）可以直接读出来：

```
mov rax,[rbp+0xd78]   ; arg12 = BindFramebufferColor[3]
mov rcx,[rbp+0xd70]   ; arg11 = [2]
mov rdx,[rbp+0xd68]   ; arg10 = [1]
mov r8, [rbp+0xd60]   ; arg9  = [0]
...
mov rdx,[rdx+8]       ; 取 [+8] 当材质 uniform 值
mov rax,[rax+8]
mov rcx,[rcx+8]
mov r8, [r8+8]
```

Mac 1.26.32 读的是 `+0x10`。**两者布局差 8 字节，跨版本迁移时必须以本地反汇编为准。**

### 2.3 四个依赖的语义（Windows 26.51，与 Mac 不同）

| 参数 | 依赖 | 材质 uniform | 语义 |
|---|---|---|---|
| arg9 | `[0]` | `InputFinalColor` | 本帧颜色（渲染分辨率） |
| arg10 | `[1]` | `Upscaling` | **上采样目标（显示分辨率）** |
| arg11 | `[2]` | `InputBufferMotionVectors` | 运动矢量 |
| arg12 | `[3]` | `InputTAAHistory` | 上一帧 TAAU 结果 |

Windows declarator 在 `0xB7505C0…0xB75060B` 按资源名 hash 绑定上述四项。
**不要沿用 Mac 的参数顺序**。§22–§27 曾把 arg10 当 InputFinalColor，已由 §28 更正。

## 3. 运行时事实（2026-10-24 21:00 / 21:14 两次运行）

主菜单与进世界后一致的常数：

| 项 | 值 | 含义 |
|---|---|---|
| taau 声明 / draw | **1/帧 各一次** | 插入点是「每帧一次」的稳定事件，不是每对象 |
| `mode` | 0（Taau） | 确实是 TAAU 分支 |
| `factor` | **0.3300** | 渲染/显示比 = 33%（2560×1440 → 约 844×475） |
| `dynScale` | 1.0000 | 动态分辨率当前没缩放 |
| `jitter` | **0 基、每帧 +1、16 项回绕**（EXP-0047 逐帧核过） | 值 = Halton(2,3)−0.5，±0.5 **渲染像素**、屏幕 y 向下 ⇒ 与 DLSS 的 `Jitter.Offset` 同空间，**直传不需要任何换算**；能不能直传取决于世界帧是否按它渲染，见 §37 |
| `clipping` / `mvFilter` | true / false | TAAU 的色域裁剪与 MV 过滤开关 |
| viewport | 2560×1440+0,0 | 显示的 viewport（输出尺寸） |
| `minFactor` / `targetFps` / `pixelScaling` / `smoothing` | 0.25 / 30 / 5 / 0.3 | `UpscalingParameters` 其余字段，布局与 SDK 头一致 |

提交结构（同一帧内）：

| 项 | 值 | 读法 |
|---|---|---|
| `ExecuteCommandLists` 次数 | **2 / 帧** | 一次 `lists=1`（我们自己的 overlay 的 D3D11On12 提交）+ 一次 `lists=48`（引擎整帧） |
| 每帧 command list 总数 | **48–51**（暂停/更衣室界面 **69**） | 引擎把一帧切成 ~48 个 list 再一次性提交；**list 数随场景变化**，固定索引不能跨场景复用 |
| 提交线程 | 单一 tid（34852） | 引擎整帧在同一个线程提交 |
| 一帧的 `Close` 次数 | **49** | 与提交的 48 个 list 相当，多一个是收尾 |
| 49 次 `Close` 的时间跨度 | **约 3.3 ms**，同一线程 | 与 taau 回调（录制相，早 480 ms）不在同一时间窗 |

⇒ **bgfx 不是「边录边提交」**：整帧先在 CPU 上录完，再由渲染线程一次性把 48 个 list 编码/关闭/提交。
因此「在 taau 那一刻拿到正在录制的 command list」这条路不成立；能用的只有下面 §5 的「拆
`ExecuteCommandLists`」。

## 4. 依赖对象里能读出什么

`BindFramebufferColor + 0x8` 指向的对象，前 0x100 字节里有**可打印字符串**（生产者通道名），
实测出现过 `DoDeferredLighting`（与「MV 是延迟着色 G-buffer 附件」一致）与 `PreDeferredLighting`。

⇒ 这个对象携带资源来源信息，不是裸纹理；**它还不足以直接给出 `ID3D12Resource`**。

### 4.1 依赖对象里没有 `ID3D12Resource`（已证否）

探针 v5 用「纹理扫描」验证过：把依赖对象、它 `+0x8` 指向的节点、以及 `+0x10` 那一跳的 qword 逐个当
候选，用「vtable 必须精确等于已知 `ID3D12Resource` 的 vtable」做过滤后调 `GetDesc` ——
**四个依赖、三个层次，一次都没命中**。所以帧图节点里存的是 bgfx 侧的标识，不是 D3D12 资源；
桥必须从 bgfx 那一侧搭。

> 安全备注：早期版本只校验「vtable 落在某个已加载模块」，随机 qword 命中设备/队列对象时会调错虚函数
> 直接崩客户端。现在的过滤是「vtable 精确等于某个真实资源的 vtable」（取自 `CreateRenderTargetView`
> 的第一个参数），命中率为 0 但不会崩。

## 5. 提交侧与资源侧

### 5.1 引擎用的是 Agility SDK 的 `D3D12Core.dll`（关键）

实测（`[vv] engine list vtable=… slot9=D3D12Core.dll+0x232dd0 …`）：

- 引擎提交的 command list，其 vtable 全部落在 **`D3D12Core.dll`**，不是系统 `d3d12.dll`；
- 所以钩 `d3d12.dll` 导出的 `D3D12CreateDevice` 只能抓到 overlay 自己的临时设备，**引擎的设备和它的
  `CreateRenderTargetView` 一个都看不到**；
- 正确入口：拿引擎提交的某个 command list 调 `ID3D12CommandList::GetDevice`（在独立 TU
  `probe/VvCalibrate.cpp` 里做，因为要用 `__uuidof`），再钩那个设备的 `CreateRenderTargetView`。
  **渲染目标清单从此可见**（仍需按一次 F11 触发引擎重建资源）。
- 引擎的 D3D12 设备与渲染目标**在模组加载之前就建好了**，所以正常启动时 `D3D12CreateDevice` /
  `CreateRenderTargetView` 钩子只能看到 overlay 自己的临时设备和它的后缓冲；**按一次 F11
  （切全屏 → 引擎重建资源）之后，44 个渲染目标立刻全部出现在日志里**——这是以后抓引擎资源的通用手法。

实测清单（窗口 2560×1440、factor 0.33）：

| 尺寸 | 格式（DXGI） | 判读 |
|---|---|---|
| 2560×1440 | 26 / 28（R8G8B8A8_TYPELESS / _UNORM_SRGB） | 交换链后缓冲（两块） |
| 844×475 | 10 / 11 / 12（R16G16B16A16_FLOAT / _UNORM / _UINT） | 渲染分辨率的颜色与 G-buffer |
| 844×475 | 26 / 28（RGBA8） | 渲染分辨率的 LDR 面 |
| 844×475 | 41 / 54 | 深度（D32 一类，待读准格式） |
| 422×225 … 13×7 | 26 | bloom 金字塔 mip 链 |

**关键异常**：清单里**没有**「显示分辨率的 HDR 目标」（不存在 2560×1440 的 10/11/12）。也就是说
这一版 VV 的 TAAU **很可能直接写进后缓冲**（8 bit SDR），色调映射发生在 844×475 那一层——
与 Mac 记录的 VV 顺序（taau → 亮度 → bloom → 调色）不一致。这条**待确认**，确认方式见 §7。

### 5.2 不要猜槽位

在引擎的 D3D12Core 列表上钩槽 25，第一次调用收到的寄存器是 `rdx = 堆指针、r8 = 0xFC5D1543EC5F01`
这种「一个指针」的形状，不是 `(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE*)`。转发后
`D3D12Core.dll` 把 `rdx` 当结构体指针解引用，在 `0x6F214130 + 0xCC` 处 AV。

⇒ **这一版运行时不能按公开头文件的槽号去挂钩 `ID3D12GraphicsCommandList` 的方法（除槽 9 `Close`
已验证可用）。** 要拿绑定信息，只能先把槽号**测**出来（§7 第 2 条）。没有验证过的槽不要挂钩；
要认槽，先拿一个已知对象做标定。

> **2026-10-25 16:0x 更正（见 §14.3 ①）**：上面这段的结论是错的，保留是为了记住当时的现场。
> 崩的原因是**把槽 25 当成了 `OMSetRenderTargets`**——25 实际是 `SetPipelineState`，
> `OMSetRenderTargets` 是 46。公开头文件的声明顺序就是运行时 vtable 顺序：已实测对得上的有
> `Close=9`、`DrawInstanced=12`、`DrawIndexedInstanced=13`、`CreateRenderTargetView=20`、
> `CreateCommittedResource=27`、`CreatePlacedResource=29`。**槽号可以查头文件；不能做的是
> 「按猜到的签名批量 hook 再转发」。**

## 6. 对阶段 2 的意义（五个未知的状态）

| # | 未知 | 结论 |
|---|---|---|
| 1 | VV 下 `taau` 是否命中 / NGX 是否已被引擎 Init | ✅ **命中，1/帧**；VV 下没有引擎 NGX 调用（`[sr] ctx` 只是渲染器配置对象）⇒ DLSS 必须我们自己建 feature |
| 2 | bgfx 句柄 → `ID3D12Resource` | ✅ **不需要桥**。输入纹理就是渲染分辨率的渲染目标，`CreateRenderTargetView` 清单里已经有它们的指针，按「渲染分辨率 + 格式」直接取（见下表）；「依赖对象里没有资源」仍然成立，但资源本来就不必从依赖对象里拿 |
| 3 | 深度从哪来 | 🟡 有候选：渲染分辨率下的 `fmt=54`（D32_FLOAT_S8X24）与 `fmt=41`（R32_FLOAT）；taau 的 4 个输入里确实没有深度 |
| 4 | 写输出目标是否被后续 pass 依赖 | ✅ **taau 直接写后缓冲**：扫描里 `in.output` 命中的就是显示分辨率的后缓冲；注入实验确认「倒 4 之后写后缓冲」画面成立 |
| 5 | VV 的尺寸语义 | ✅ `factor=0.33` × 显示尺寸；`target_resolution` 是绝对目标，DRS 换算成 factor（与 [sr §3](sr.md) 的 R_in 口径一致） |

**输入纹理的实际清单（2026-10-25 定稿：hook 创建入口 + 逐张 dump 内容）**

hook `CreateCommittedResource` / `CreatePlacedResource` 之后，渲染分辨率这一组共 **15 张**，
全部 dump 并按内容判定（工具 `NeuroDragonWorkSpace/tools/vv_contact.py`）：

| 编号（241×107 那轮） | DXGI 格式 | 实测内容 | 结论 |
|---|---|---|---|
| 06 / 00 | `R8G8B8A8_UNORM` | 06 = 世界画面（LDR）；00 = 全黑 | 06 是 LDR 输出，00 未用 |
| 12 / 13 / 14 | `R11G11B10_FLOAT` | **HDR 世界画面**（max 20，有高光） | **color 输入候选**（taau 的 `InputFinalColor`） |
| 02 / 03 | `R11G11B10_FLOAT` | 02 偏亮、03 近黑 | 中间面 / 历史候选 |
| 10 | `R16G16B16A16_FLOAT` | 法线（几何朝向色） | SR 不需要 |
| 另一张 `R16G16B16A16_UINT` | 同上 | 法线类（三通道 0..1 全范围，w≈0） | **不是 MV**（静止/移动逐通道对比几乎无差异） |
| 01 / 09 / 10 | `R24G8_TYPELESS` | 深度（1.0=远） | 深度 |
| 05 | `D16_UNORM` | 常数 0.23 | 深度副本 |
| 11 | `R32_FLOAT` | 1.0 | 深度线性化 |
| 04 | `RGBA16_UNORM` | 品红（缺失纹理色） | 未用 |

⇒ **运动矢量没有独立的纹理**。静止批（站着）与移动批（走动+转视角）逐张对比，没有任何一张
出现「静止为零、移动非零」的特征；`taau` 的 `motion` 依赖 `+0x10` 的那个小整数（`0x5242`）
也不是这些纹理之一。**最可能的情况是 taau 从深度 + 相机矩阵重建 MV**（标准 TAA reprojection），
那样 M6 也必须自己做这一步——这是目前唯一未解的输入。

⇒ 颜色是 `R11G11B10_FLOAT`，不是 FP16：DLSS 的 color 输入要 FP16，**这一步转换跑不掉**。

**教训**：格式不能当语义用（同名 FP16 既可能是颜色也可能是法线；`f26` 才是 HDR 颜色），
必须先 dump 内容再认；而且 `DXGI_FORMAT` 的数值记错过两次（`26`=R11G11B10、`44`=R24G8、
`54`=D16），现在按数值写表。

**运动矢量的真相（2026-10-25，Mac 反编译 + Windows 内存 dump）**：taau **没有 MV 纹理**。

- Mac 的 draw callback 反编译显示它上传的是 `PreviousViewProjectionMatrixUniform` /
  `CurrentViewProjectionMatrixUniform`、`CurrentWorldOrigin` / `PreviousWorldOrigin`、
  `SubPixelJitterAndValidHistory`——即 **shader 从深度 + 前后 VP 矩阵重建 reprojection**（标准 TAA）。
- Windows 上这两个矩阵在 **`TAAContext + 0x200`（当前）与 `+0x240`（上一帧）**，实测静止时两者完全相同：
  `[0.5413 0 0 0][0 1.3032 0 0][0 0 -1.0010 -1][0 0 -0.1001 0]` ⇒ **reversed-Z 投影**（近=1、远=0）。

⇒ M6 的 MV 由我们自己算：`prevClip = prevVP * worldPos`，`curClip = curVP * worldPos`，
两者投到像素坐标相减即得 **像素单位的 MV**；`DepthInverted` 置位（reversed-Z）。

**M6 的输入清单（至此齐了）**：

| 输入 | 来源 | 注意事项 |
|---|---|---|
| color | `R11G11B10_FLOAT`（渲染分辨率） | 需转 FP16 |
| depth | `R24G8_TYPELESS` / `R32_FLOAT` | reversed-Z，`DepthInverted=true` |
| MV | **自己重建**（depth + `TAAContext+0x200/+0x240`） | 像素单位 |
| jitter | `UpscalingData.jitterIndex` → 引擎 jitter 表（Halton(2,3)−0.5，±0.5 渲染像素、y 向下） | 单位/坐标与 DLSS 的 `Jitter.Offset` 同空间，**不需要换算**。**2026-10-29 已确认（EXP-0100 几何测量）：颜色层没有按这个偏移光栅化** ⇒ 唯一真实值是 0，产品侧固定用 0；见 §37 |
| 输出 | 后缓冲（倒 4 之后写入） | 已是本 spec 的插入点 |

> **2026-10-28 更正（静态已确认，取代上面「没有 MV 纹理」整段与 M6 表的 MV 行）**：
> taau **有 MV 纹理**，且**不用深度、不用 VP 做重投影**。
> `data/renderer/materials/Upscaling.material.bin` 的 shader（SM5.0 DXBC 与 SM6.3 DXIL 逐段对照一致）
> 绑定 `t0=InputBufferMotionVectors`（读 `.zw`）、`t1=InputFinalColor`（`.rgb`）、`t2=InputTAAHistory`；
> declarator 的四个依赖里 arg11 就是 MV。契约：
>
> ```
> mv          = { z, -w }                     // Y 翻转，渲染分辨率归一化
> mvRenderPix = mv * RenderResolution
> historyPos  = displayCenter - mvRenderPix / ratio
> ```
>
> ⇒ 替换实现的 MV 应**直接绑定引擎 arg11**，按上述契约转成 DLSS 需要的像素单位；
> **不要**再用 depth + `TAAContext+0x200/+0x240` 重建——我们此前的实现正是后者，
> EXP-0039–0042 的公式类问题都源于这个错误前提。VP 由 CPU 上传，但不在 shader 的
> 96 字节常量缓冲里（只有 jitter / params / ratio / display / render 分辨率 6 个 float4）。
> 证据：[research/2026-10-28-taau-decompiled.md](../research/2026-10-28-taau-decompiled.md) §3–§4、§7。
>
> **已定案（2026-10-29，EXP-0101）**：`near/(1-d)` 是对的（标准 Z：近 0、远 1），矛盾出在"告诉
> 运行时"的两处——VV 的 SR 创建标志写了 `11`，把 bit3（`DepthInverted`，1<<3）当成 AutoExposure
> （1<<6）打开；NR 每帧参数也写死 1。而 DLSS-SR 根本不读每帧 `DepthInverted` 参数（runtime 字符串表
> 没有该名），所以 EXP-0041 改键无效。现在 SR 创建标志=3、NR 取自参数对象的声明，判据是 NGX 创建
> 日志 `Depth Inverted 1→0`。证据见 [nr.md §9 P8](nr.md) 与 `docs/agent/evidence/depth-flag-EXP-0101-*`。

**已证伪的路子**（省得再走）：依赖对象 `+0x8` / `+0x10` 是**帧图节点**（带 vtable、带 pass 名字符串），
不是资源也不是 bgfx handle；递归扫节点对象图 3 跳、与已知资源指针比对，命中的全是后缓冲，
没有一条渲染分辨率纹理；Windows 侧从 draw callback 反汇编出的全局 `0x11805230` 指向 `.data` 静态对象，
不是 `bgfx::s_ctx`（Mac 上 `s_ctx` 由 `drawutils` 的 lambda 访问，taau 的 callback 不直接碰它）。

**新增结论（对实现最关键）**：引擎**每帧只调一次 `ExecuteCommandLists`，但一次带 ~48 个
command list**。钩子可以把这个调用**拆成「前缀 + 我们自己的 list + 后缀」**分别在同一个队列上执行，
因此「在 TAAU 之后、调色/UI 之前」插入我们自己的命令**在队列层面是可行的**——前提是能识别
「哪一段 list 边界正好在 TAAU 之后」。

## 7. 下一步（按风险排序）

1. **认纹理 → 已不需要**（2026-10-25）：输入纹理按「渲染分辨率 + 格式」从 `[vv] rtv res=` 清单直接取
   （§6 的表）。唯一还没对上的是 `InputTAAHistory`（候选 `fmt=11` 或另一张 `fmt=10`），
   用一次 A/B 就能确认，不必做 BMP dump。
2. **槽位标定（若需要绑定信息）**：用我们自己的 command list 反推槽号——但 2026-10-24 22:0x 的第一次尝试
   （32 个「记录并转发」detour）在 `nvwgf2umx.dll` 里崩了（AV @0x2b88），说明批量挂钩本身有风险；
   若再来，改成**一次只挂一个槽 + 只调用一次**，并且先在临时设备上做，确认无崩溃再上引擎设备。
3. **边界实验**（用户可见的验收）：在候选 list 边界把目标纹理清成纯色——画面变纯色而 UI 仍在
   ⇒ 边界对了；等价于 M5，但**不需要**先建 DLSS。配合 48 个 list 可以逐帧扫。
   **2026-10-24 已实现**（§12 A 的实现），**2026-10-25 已验收：插入点 = 尾部 k=4**（§12 A 的帧末尾结构）。
4. **深度源**：`fmt=54`（D32_FLOAT_S8X24）在渲染分辨率下只有一张，基本可以认定是主深度；
   `fmt=41`（R32_FLOAT）是候选的线性化/单通道面。M6 先用 `fmt=54`，DLSS 需要线性深度时再补一次比较。

## 8. 里程碑与 stop/go

| 里程碑 | 内容 | 成本 | 验收（用户可见） |
|---|---|---|---|
| **M4** 侦察 | Windows `taau` 只读钩子：viewport / `UpscalingData` / `UpscalingCameraData` / 4×`BindFramebufferColor`；dump 输入三图；bgfx 桥最小闭环 | 3–5 天 | VV 下能看到每帧 taau 的尺寸/factor/jitter，并 dump 出真实纹理 |
| **M5** 写透传 | 在 taau 输出目标写纯色/拷贝 | 3–5 天 | 画面变纯色但 **UI 仍在** ⇒ 「pre-UI 写入」成立 |
| **M6** DLSS 替换 | 自有 feature（core `CreateFeature(1)`）：输入 taau 的 color/MV + 另钩的 depth + jitter；输出写回 taau 目标；先「TAAU 照跑再覆盖」，后「跳过 TAAU」 | 1–2 周 | VV 下 A/B 热键：TAAU ↔ DLSS；两侧 GPU 时间与观感都有数 |
| **M7** 收尾 | 预设贯穿两路径；R_in 策略统一（VV 的 `target_resolution` vs RT 的 L3）；稳定性与回退 | 1 周 | 同一套预设 UI 在两条路径都工作；失败自动回退引擎原路径 |
| M8（可选） | RT 接管 create：不转发引擎 create、返回我们的 handle | 3–5 天 | 改预设字母**立刻**生效 |

依赖：M4 → M5 → M6 是 VV 线；M7 收尾；M8 只在「必须热切」时做。

**stop/go**：M4 侦察一周后仍答不出「bgfx 桥 + 深度源」，VV 就只做「读数 + 提示」，不硬推 M5/M6。
这条沿用旧 I §7，不变。

## 9. 风险

| 风险 | 影响 | 应对 / 退路 |
|---|---|---|
| 没有符号、bgfx 桥/深度源难啃 | M4–M6 可能超预算 | M4 一周后设 stop/go；失败则 VV 只做读数 + 提示 |
| 写 taau 输出目标被后续 pass 依赖 | 画面花/崩溃 | 先「TAAU 照跑再覆盖」（M5），不成立就放弃写入 |
| NGX 在 VV 下自 Init 与引擎冲突 | 可能影响切回 RT | 只在引擎没 Init 时 Init；切模式要求重启 |
| DLSS 在 VV 尺寸下不比 TAAU 快 | 收益只剩画质 | M6 先量时间，再决定是否默认开 |
| 版本漂移（签名/回调） | 每个补丁都要重定位 | 全部签名走 `kSig*` + 启动自检，失败回退引擎原路径 |
| 槽位猜错 | 客户端 AV（已发生：槽 25） | 只钩已验证的槽；要认槽先标定 |
| 持有交换链后缓冲引用 | `ResizeBuffers` 失败 → 引擎 `abort()`（已发生：22:39 / 22:51 / 2026-10-25 03:14 三次 `0xC0000409`） | **拿到的后缓冲引用必须当场 `Release`**；渲染目标清单（`gSeen`）**只存裸指针、不得 AddRef**（03:14 的根因）；只在 `noteSwapChain()` 的调用内部检查、不常驻持有 |
| 碰引擎纹理时引擎正在重建 | 驱动 AV（已发生：22:58 `nvwgf2umx.dll`，栈里有 `dumpTextureBmp ← onPresent ← presentHook`） | 任何对引擎纹理的**状态转换/拷贝**都必须在「确定不会重建」的安静窗口内做；自动触发已关闭，只能显式触发 |

## 10. 证据与工具

- 代码：`src/neurodragon/probe/VvProbe.{h,cpp}`（只读探针）、`probe/VvCalibrate.{h,cpp}`（独立 TU：
  `__uuidof` / `d3d12.h` / BMP 回读都在这里）、`probe/VvInject.{h,cpp}`（§12 A 的边界注入实验，
  同样只 include `<d3d12.h>` / `<dxgi1_4.h>`，对面板只暴露 POD），`ui/Overlay.cpp` 里的回调
  （`onPresent` / `noteSwapChain` / `setGameQueue` / `onExecuteCommandLists` / 拆分提交）。
- 探针**当前行为**（2026-10-24 收尾状态）：只读。hook `taau` 声明 + draw、`Close`（槽 9，
  已验证）、引擎设备的 `CreateRenderTargetView`；日志出 `[vv] rtv res=` 清单、
  `[vv] engine device`、`[vv] engine list vtable=`。**自动 BMP dump 已关闭**（会与重建并发导致驱动 AV），
  `dumpTextureBmp()` 仍保留在 `VvCalibrate.cpp` 里，等显式触发器（面板按钮/热键）。
- 日志标记：`[vv] taau #N viewport=…`（每 300 次）、`[vv] taau #1/#2 …resText=[…]`（前两帧）、
  `[vv] presents=… taau=… seen=… dumps=…`（前 10 帧 + 每 600 帧）、`[vv] submit #N tid= lists= qpc=`（前 24 次）、
  `[vv] rtv res=<ptr> <w>x<h> fmt=<n>`（每个新渲染目标一次）、`[vv] close #N list=… qpc=…`（采样帧前 64 次）。
- 触发引擎重建资源：**按一次 F11（全屏切换）或改窗口大小**；重建后清单才会完整（启动时的创建早于模组加载）。
- 工具（不进 mod 仓库）：`NeuroDragonWorkSpace\tools\vv_locate.py`、`vv_disasm_args2.py`、
  `rtti_names.py`、`vv_ngx_route.py`（均不依赖 IDA；capstone 走 pip 装）。
- 版本约束：所有 RVA 只对 `1.26.5101.0` 成立；换版本先用 `vv_locate.py` 重新定位
  （函数每帧都在跑，串锚点很稳）。

## 11. 参考

- 侦察全文：[research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)（旧 `O`）
- 阶段 2 的原始未知表：[archive/2026-10/M-roadmap.md](../archive/2026-10/M-roadmap.md) §3
- M4–M7 的原始规划：[archive/2026-10/I-sr-final-plan.md](../archive/2026-10/I-sr-final-plan.md) §5
- taau 契约与 bgfx：[renderdragon](renderdragon.md) §3–5
- 路线位置：[roadmap §3](../roadmap.md)

## 12. 下一步执行顺序（2026-10-24 定）

用户决定：**先把「能不能插进去」验证掉，再回头认纹理**。理由：注入架构是阶段 2 的地基，
而且它不需要知道任何纹理身份、不碰引擎资源，风险最低、能亲眼看。

### A. 边界 / 注入验证（先做，零纹理依赖）

目标：证明「拆 `ExecuteCommandLists` + 中间插一段我们自己的命令」在 TAAU 之后成立。

做法：

1. 用引擎设备自建 `ID3D12CommandAllocator` + `ID3D12GraphicsCommandList`（`VvCalibrate.cpp` 里已有同样套路）；
2. 在 `onExecuteCommandLists` 里当 `listCount > 8` 时，把引擎的 48 个 list **按索引 k 拆开**：
   先 `ExecuteCommandLists(前 k 个)`，再提交我们自己的 list，再提交剩下的；
3. 我们的 list 里对**后缓冲**做 `ClearRenderTargetView`（或 `ClearDepthStencilView` 之外的全屏清）；
4. 后缓冲指针从 `noteSwapChain()` 取，**每次取完立刻 `Release`**（§9 第一条崩溃就是这个）；
5. 状态：`PRESENT → RENDER_TARGET → PRESENT`，**必须回到 PRESENT**，否则下一帧黑屏/异常。

验收（用户可见）：

- 画面变成我们清的颜色、**UI 仍在** ⇒ 说明注入点在 UI 之前，"pre-UI 写入"成立；
- 全屏纯色（含 UI 区域）⇒ 注入点在 UI 之后；
- 什么都没有 ⇒ 该边界在后缓冲被消费之后（或被覆盖），换 k。

扫描策略：一帧一个 k（k 从 1 到 48），把 k 写进日志，配合一次截图就能定位边界区间；
确定区间后再二分。

#### A 的实现（2026-10-24，待实机验收）

代码：`probe/VvInject.{h,cpp}`；接线在 `ui/Overlay.cpp` 的 `executeCommandListsHook`。

- **只切引擎整帧**：`listCount > 8`、队列类型必须是 `DIRECT`、且只认第一次注入的那个队列
  （queue 钩子挂在共享 vtable 上，compute/copy 队列也会命中，直连命令列表提交给 compute 队列会失败）。
- **我们自己的 list** 建在引擎设备上（allocator / list / fence / 1 个 RTV 描述符堆，需要时才建）：
  取 `GetCurrentBackBufferIndex()` 那块后缓冲，`PRESENT → RENDER_TARGET → ClearRenderTargetView(洋红) → PRESENT`，
  再 `Close`。同一队列三次提交，顺序就是 GPU 的执行顺序。
- **生命周期**：后缓冲引用在提交后当场 `Release`（§9 第一条崩溃的成因）；allocator 复用前用自建 fence
  确认上一次注入已执行完，没完就跳过该帧（**不等待**，避免把帧时间拖长）。
- **不污染探针读数**：`Close` 钩子会看到我们自己的 list，`isOwnList()` 把它排除，§3 的「每帧 49 次 Close」
  仍然是引擎的数字。

面板「诊断 → VV 注入」：模式 `关 / 固定 / 扫描`、边界 `k`、扫描的每档帧数（默认 20）、
**「从尾部数」开关**、读数（当前边界 / list 数范围 / 每帧 list 数 / 已注入帧 / 跳过 / 完成扫描轮）。
**关掉面板注入仍在继续**，要停就切回「关」（或重启游戏）。

- **k 有两种数法**：默认从头部数（k = 「第 k 个 list 之后」）；打开「从尾部数」后 k=1 表示
  「整帧之后」、k=2「最后一个 list 之前」，依此类推。**引擎每帧的 list 数不是常数**
  （世界 48–51、主菜单更衣室 69、个别帧 74/81/107，2026-10-25 实测），所以从头部数的 k
  会在帧形状变化时漂移；从尾部数才是能跨帧站住的那把尺子，也直接回答下一阶段要的问题：
  **UI 是不是永远在最后几个 list**。

判据（与上面一致，只是读数方式定了）：

| 画面 | 结论 |
|---|---|
| 洋红 + UI 仍在 | 边界在 UI 之前 ⇒ pre-UI 写入成立 |
| 整屏洋红（含 UI） | 边界在 UI 之后 ⇒ 要往前挪 k |
| 无变化 | 后面的 list 覆盖了它 ⇒ k 调大 |

风险：整段只做「清后缓冲 + 状态来回」，不碰任何引擎资源；万一花屏/黑屏，切回「关」下一帧即恢复，
不需要重启（对象一直在，但没有注入就没有提交）。

#### 帧末尾的结构（2026-10-25，从尾部数，主菜单 / 暂停界面）

| 尾部 k | 剩余 list | 画面 | 结论 |
|---|---|---|---|
| 5 | 4 | 完整 | 倒 4 之前都是世界内部，重画后完整 |
| **4** | **3** | **暂停菜单 / 主菜单背景 / 主菜单都在，手臂与粒子消失** | **倒 4 = 世界的最终合成（含手臂 + 粒子）** |
| 3 | 2 | 暂停菜单与主菜单背景消失，只剩主菜单 | 倒 3 = UI 背景 / 暂停层 |
| 2 | 1 | 全空，连退出游戏的菜单都看不见 | 倒 2 = 主菜单 |
| 1 | 0 | 全洋红 | 倒 1 不产生可见输出 |

⇒ **插入点 = 尾部 k=4 之后**（世界最终合成之后、UI 之前）。手臂与粒子在更早的 list 里已经画进世界，
所以这个位置拿到的天然是「含手臂 + 粒子的最终世界画面」——比 RT 路线（手臂 / 粒子与画面分开）少一层处理。

⇒ 倒 4 极可能就是 **TAAU 的 draw**（与 §5.1「清单里没有显示分辨率的 HDR 面 ⇒ TAAU 直接写后缓冲」的怀疑一致）。
B 步要确认的就是：倒 4 这条 list 里画的是不是 TAAU、它写的目标是不是后缓冲。

> **2026-10-25 16:0x 更正**：上面「插入点 = 尾部 k=4」只在**暂停界面 69 个 list** 这一种帧形上成立。
> 世界内 list 数在 69/81 交替时，尾部 4 会落到 UI 之后，HUD 被我们的旁路覆盖（§13.8 04:22 实测）。
> **任何 head/tail 计数都不是跨场景可用的边界**，最终要按内容认 list；在认清之前，k 只是面板上的手选参数。
> 下一步的判定办法见 §14.4 B1c（把后缓冲在切点截图，直接看这个切点看到的世界）。

⇒ 尾部 k=4 在两次测量里都稳定（list 数 74 / 77，范围 73..113 / 75..115 都在变），**从头部数的 k 会跟着漂**。

仍待确认：世界内的 HUD（血条 / 快捷栏 / 准星）在倒 3 / 倒 2 还是在世界合成里——这决定 k=4 是否足够。

#### 实测（2026-10-25 00:2x，主菜单「更衣室」界面，2560×1440）

| 读数 | 值 |
|---|---|
| 每帧 list 数 | **69**（同一实例进世界时是 48–51） |
| k=65 | **完整画面**：清屏被后面 4 个 list 又画了一遍 |
| k=66 | **左半世界变洋红、右侧「更衣室」UI 仍在** |
| k=67 | **整屏洋红**，连 UI 也没了（只剩我们的 overlay 面板） |

⇒ **A 步验收通过**：`ExecuteCommandLists` 拆分 + 中间插入成立；存在一段「世界画完、UI 未画」的边界
（这一场景是 list 66 之后 / 67 之前），**pre-UI 写入成立**，M5 的目标在暂停界面上已经达成。

两条必须记住的：

- **list 数随场景变化**（48–51 ↔ 69 ↔ 74/81/107），所以 k 是「当前帧的第 k 个 list」，不是可跨场景
  复用的常量。**按头部数会在帧形状变化时漂移**（面板上表现成「最大值闪到 107/74/81」），
  因此加了「从尾部数」；M6 最终要的是**按内容认 list**（例如哪一段里的 draw 属于 TAAU），不是记数字。
- 这次测的是**暂停界面的 UI**；世界内的 HUD（血条 / 快捷栏 / 准星）在哪个 list 还没测，
  进世界后要用同样的办法再扫一遍。

### B. 认纹理（A 通过后再做）

把 `dumpTextureBmp()` 接到**显式触发**上（面板按钮或热键），只在安静窗口执行（无 F11、无窗口尺寸变化）。
第一轮先 dump 渲染分辨率那一组（10/11/12/26/28/41/54），确认哪个是 HDR 场景色、哪个是 MV、哪个是深度；
第二轮 dump 显示分辨率那组 8 bit 面，确认 TAAU 的输出。

### C. bgfx 桥（若 B 需要）

如果 B 里 dump 出来的东西对不上 TAAU 的输入/输出（例如 TAAU 用的是 bgfx 侧的持久纹理），
再从 `taau` 的 draw callback 一侧找 `bgfx::TextureHandle` 并搭 `handle → ID3D12Resource` 表。

### 每步的硬约束（都是踩出来的）

- 只钩**已验证**的槽（目前只有 `Close` = 9）；新槽必须先标定；
- 任何碰引擎资源/交换链的动作都要考虑"引擎正在重建"的并发窗口；
- 拿到的 COM 引用一律当场 `Release`；
- 每步先做只读自检（prologue / 槽位 / 设备身份），不匹配就回退并报错，不要静默降级。

## 13. M6：DLSS-SR 替换 TAAU（2026-10-25 拍板）

> **状态**：B0 已收口（2026-10-25 03:44）；B1–B4 未开工。
> 规划按用户 2026-10-25 的「按推荐走」确定：B0 先锁三个未知；第一版 TAAU 照跑 +
> DLSS 覆盖；**HDR 路径用 ACES + gamma 2.2**（B0 实测；Reinhard 差 4 倍多）；A/B 热键 `F8`；
> 「跳过 TAAU」放到 M6-4，可选、后置。

### 13.1 为什么先 B0 再写 feature

vv.md §6 的输入清单里还有三件事没有被运行时证据钉死，而它们各自都能让第一次上屏
「看起来像 DLSS 坏了」，并且无法归因：

| # | 未知 | 为什么必须先答 |
|---|---|---|
| 1 | VV 下 NGX **会话**是否可用 | 「没有引擎 DLSS 调用」不等于「core 没加载/没 Init」。在未 Init 的 core 上调 `GetCapabilityParameters`/`CreateFeature` 没有定义行为 |
| 2 | color/output 的**域** | TAAU 输入是 HDR 还是已经色调映射过的 LDR，决定我们是否需要自己 tonemap；猜错就是颜色/亮度全错 |
| 3 | color/depth 的**指针角色** | 同尺寸同格式有多张（HDR 与 LDR、历史与当前、两份深度）；选错时画面只是「不对」，不会崩 |

外加两项会直接影响 MV 正确性的观测：jitter 偏移、世界原点。`TAAContext+0x200/+0x240`
先前被记成「前后 VP」，但打印出的矩阵**没有平移、没有相机旋转**（对角投影），很可能只是
**投影矩阵**；完整的 view/projection 还可能在 `UpscalingCameraData`（taau 的第 7 个参数）里。
B0 的 P0.4 按「两个对象、整窗采样」重新定位，不再假设偏移。

### 13.2 批次与判据

| 批次 | 内容 | 成本 | 退出条件 |
|---|---|---|---|
| **B0** | 只读探针 P0.1–P0.5：会话、域、输入角色、矩阵/jitter/原点、MV 公式 | 1–1.5 天 | §13.3 的五条判据都有答案；没有答案就按 §8 stop/go 停 |
| **B1** | 抽 NGX 封装；`vv/VvDlssSr`；一个 compute（color→FP16、depth→R32F、MV）；旁路/深度/MV 三个调试视图 | 2–3 天 | 旁路画面、深度视图、MV 视图都正确；HUD 在 |
| **B2** | `CreateFeature(1)` + evaluate + 编码写后缓冲；接 `VvInject` 尾 k=4；F8 A/B；计时；尺寸重建；失败跳过 | 2–3 天 | TAAU ↔ DLSS 可见切换；10 分钟/F11/重进世界/20 次切换不崩 |
| **B3** | 面板读数、失败自动停用、文档、TEST+release 部署 | 1–2 天 | 验收清单通过，release 部署主目录 |
| **B4**（可选） | 跳过 TAAU，量净收益 | 1–2 天 | 只在 B2 稳定且需要净收益数字时做 |

**stop/go 不变**（§8）：会话答不出、或颜色/深度角色在两次采集内仍无法确定，就停在
「VV 只做读数 + 提示」，不硬推 B1。

### 13.3 B0 探针（一次 TEST 构建、一次会话）

| 探针 | 做法 | 判据 |
|---|---|---|
| P0.1 会话 | 日志 `[vv] session`：`_nvngx.dll` / `nvngx.dll` / `nvngx_dlss.dll` / `D3D12Core.dll` 是否加载；引擎 Init 是否被抓到（appId/dataPath）；**只在抓到 Init 后**调 `GetCapabilityParameters`，并记录五个导出是否存在 | `ret==1 && params!=null` → 复用；模块在但未 Init → 单独评估自 Init；模块不在 → stop/go |
| P0.2 域 | 进世界站着不动：`vv_dump_now.txt` 触发渲染分辨率组 dump，同时按 `F2`；离线比较 `upscale(tonemap(HDR候选))`、`upscale(LDR候选)` 与截图中心区域 | 两假设中平均绝对差明显更低者胜；两者都不像 → 先查 TAAU 是否写后缓冲 |
| P0.3 角色 | 同一批 dump 的**文件名带资源指针**，日志 `[vv] semantics: res idx=… ptr=…` 一一对应；离线按内容排序 | 60 帧内 color 指针稳定；深度候选按「非常数 + 近白远黑 + 结构」排序；若交替出现两套则记成集合 |
| P0.4 矩阵/jitter/原点 | `vv_ctx_now.txt` 触发 12 帧连续采样：`TAAContext` 与 `UpscalingCameraData` 各 0x300 字节原始 float；另打 `+0x200/+0x240` 的 16+16 矩阵 | 静止时前后矩阵相同、jitter 每帧变化且绝对值 < 1；走动时原点/平移漂移；若找不到 jitter，退「jitter=0 + 记录风险」 |
| P0.5 MV 公式 | 用 P0.4 的矩阵 + 一帧深度（优先 `R32_FLOAT` 候选）离线跑 reprojection：reversed-Z、像素单位、符号、far（w=0） | 静止 MV≈0；走动径向、转视角切向；否则先改公式，不写 shader |

### 13.4 B0 实现（2026-10-25）

代码（只读，不建 feature、不写任何引擎资源）：

- `probe/VvProbe.cpp`
  - `logNgxSession()`：P0.1；在 `install`、`noteWorldEntered`、以及每 600 帧轮询（未就绪时）。
  - `vv_dump_now.txt` 的 dump 文件名与日志都带资源指针：`vv_b<batch>_res_<idx>_p<PTR>_<w>x<h>_fmt<f>.bin`；
    每批开始记 `qpc`，便于与 `F2` 截图对齐。
  - `vv_ctx_now.txt`：连续 12 帧采样 `TAAContext`（draw callback 第 3 个参数）与
    `UpscalingCameraData`（declarator 第 7 个参数）的 `0x000..0x300` 原始 float；每帧另打
    `[vv] ctxm` 的 `+0x200/+0x240` 两个 4×4；采样行带 `jitter` 与 `qpc`。
  - `taauDetour` 额外保存 `jitterIndex` 与 camera data 指针（只存指针，不读）。
- `nr/D3D12Bridge.{h,cpp}`：新增只读 `nrEngineInitCaptured()`，把引擎 Init 的 appId/dataPath
  交给 P0.1；NR 行为不变。
- 外部工具（不进 mod 仓库）：`NeuroDragonWorkSpace/tools/vv_domain.py`（P0.2/P0.3：域判定 +
  color 候选排序 + 深度候选统计 + 对比图）、`vv_ctx.py`（P0.4：矩阵/jitter/原点候选）。
- 部署：2026-10-25 05:0x 色调诊断构建 `43d5991e…` 已热替换到本工作区（游戏运行中按
  「改名 → 换入」完成；当前进程仍用旧映射，**重启后生效**）。之前用于 03:44 dump 的
  `1b8629a3…`、B1a 的 `5dbfaa12…` / `fac1c25b…` / `92cff8ff…` / `60d19af8…` 依次备份为
  `.b1a-old` … `.b1a-old4`，B1b 的 `fe9d0bf5…` / `263410d8…` 备份为 `.b1b-old` / `.b1b-old2`，
  `.k4test-old` / `.content-old` / `.content-old2` / `.scan-old` / `.tone-old` 是各诊断构建前的
  备份；`.bak-before-m6b0` 保留。主目录 `1.26.51.01` 未动。
- **2026-10-25 03:4x F9 崩溃与修复**：`dumpSemantics` 的回退遍历了**裸指针** `gSeen` 并调用
  `nr::describe()`，其中一个指针已被引擎释放 → AV 在 `D3D12Core.dll`（栈：`describe` ←
  `dumpSemantics` ← `onPresent`）。修复：新增**引用计数**的 `gHeld` 子集（只收 ≤1000×600 且
  非交换链后缓冲的 RTV，环形 128 条），回退只用 `gHeld` 的**存档尺寸/格式**，不再对裸指针取 desc。
  03:28 的 dump 0 命中同时修了：`gSeen` 不再随 resize 清空、容量 4096。
- **2026-10-25 03:14 F11 崩溃与修复**：按 F11 时引擎 `abort()`（ucrtbase，`0xC0000409`，
  `FAST_FAIL_FATAL_APP_EXIT`）。三处根因一起修：
  1. RTV 清单 `gSeen` 对每个新 RTV `AddRef`，其中包含交换链后缓冲 → `ResizeBuffers` 失败；
     现在 `gSeen` **只存裸指针、不持引用**；
  2. `gTextures` 对会话内**所有**纹理 AddRef（数千张，含 1024 图集）→ 显存与重建压力；
     现在只保留重建后的**前 96 张**，并在 `onSwapChainResize()` 里先释放；
  3. 「进世界 3 秒后自动 dump」被关闭：同步 GPU 拷贝不能与 F11 重建并发（§9 的 22:58 驱动 AV）。
- **B0 热键（TEST）**：`F9` = 立刻 dump（站定后按，再按 `F2` 截图）；`F8` / `F10` / `V` 任意一个 =
  2 秒后开始连续 24 帧上下文采样（按下后可以立刻走动/转视角）。`F8` 在部分笔记本上会被 Fn 锁成
  媒体键，所以准备了 `F10` 与 `V`；面板「诊断 → VV B0 (TEST)」也有两个按钮，完全不依赖热键。
- **dump 前先按一次 `F11`**：引擎的渲染图在模组的创建钩子装好之前就可能建好，那次启动的
  渲染分辨率纹理不在清单里（2026-10-25 03:02 实测：0 个 dump 命中）。`F11` 一次让引擎重建资源，
  之后 `F9` 才有货；日志会给出同样的提示。

**一次会话的采集顺序**（TEST 构建，VV，进世界）：

1. 进世界后等 3 秒，**按一次 `F11`**（窗口 resize 不会重建渲染图；2026-10-25 03:23 已证）。
   新构建修了三处引用问题，若这次仍崩就停止，不再重试；
2. 等 3 秒，站住不动，按 `F9`，等日志 `requested by F9` + `captured`（应有非 0 个 target），
   立刻按 `F2`；
3. 仍在原地，按 **`F10`**（或 `F8` / `V` / 面板按钮），站着别动（2 秒后自动采样 24 帧静态）；
4. **再按 `F10`，然后立刻用鼠标快速转动视角约 90°（同时可向前走）**，持续 2 秒——
   这一步必须**转视角**，只前进的话 `+0x200/+0x240` 矩阵不会变，分辨不出它是不是 VP；
5. 退出游戏，保留 `latest.log`、`vv_b*_res_*.bin`、`screenshots/*.png`。

### 13.5 B0 结果

| 项 | 结论 |
|---|---|
| P0.1 会话 | ✅ **VV 下不加载 NGX，必须自建会话**。离线 `ngx_probe --sr` 已证：驱动目录的 `_nvngx.dll` + `Init_ProjectID` + `GetCapabilityParameters` 全部返回 1，`CreateFeature(1)` 接受 360×243 / 844×475 等尺寸（ret=1，handle 非空）。B1 第一步在游戏内重复这条路径 |
| P0.2 域 | ✅ **HDR 路径**。2026-10-25 03:44 的 12 张 844×475 dump + F2 截图：最佳候选 `fmt26 R11G11B10`（idx=100，ptr 在当次会话为 `0x2F0DE60F4D0`）用 **ACES + gamma 2.2** 与截图平均绝对差 **26.3/255**、相关 0.978；SDR 最佳 55.3；Reinhard 113.9。⇒ **M6 用 ACES+gamma，不用 Reinhard** |
| P0.3 color/depth 角色 | 🟡 **color = 渲染分辨率下最新的 `fmt26` R11G11B10**（当次 idx=100；idx=101/102 是历史/中间面，分数明显更差）。**depth = `fmt44` R24G8**（主深度，两个候选）；`fmt41 R32F` 实测几乎恒为 1.0、`fmt54 D16` 0..0.25，都不是主深度。深度不能直接 copy 到 buffer，B1 用 compute 转 R32F 后加深度视图钉死是哪一个 |
| P0.4 矩阵/jitter/原点 | ✅ `ctx+0x200/+0x240` = 当前/上一帧 VP（相机相对、无平移；`prev(n)==cur(n-1)`）；`ctx+0x280/+0x28c` = 当前/上一帧相机位置；`ctx+0x1e4/+0x1e8` = jitter（像素单位） |
| P0.5 MV 公式 | 🟡 数据齐了：`worldRelPrev = inv(curVP)·depth + (posCur − posPrev)`，`prevClip = prevVP·worldRelPrev`；B1 用 MV 调试视图确认矩阵布局/符号 |

**B0 收口结论**：三件会挡住 B1 的事都有答案——会话可自建、颜色是 HDR + ACES、输入指针有可复现的选取规则。
可以开工 B1。证据：`1.26.51.01-agent2` 根目录的 `vv_b0_res_*.bin`、`screenshots/20261025-034431-787-0.png`、
`_vv_domain.png`（工具 `vv_domain.py` 生成）。

其他现场：03:44 的 F11 成功（未崩），F9 回退 dump 出 **12 张 844×475**；03:28 的 dump 0 是因为重建的
RTV 在 resize 钩子之前创建，已由 `gHeld` 修复。

其他现场：03:02 会话是**窗口 1091×736**（factor 0.33 ⇒ 渲染 360×242），不是 2560×1440；B0 的
内容判定不受影响，但 B1 之后的观感/计时验收需要全屏。03:02 的两次 `F9` 都执行了 dump 路径
（`requested by F9`），说明热键轮询本身正常。

### 13.6 B1 起的技术口径（已定，避免实现时再选）

- **NGX 会话（2026-10-25 新增前置）**：VV 下引擎不加载 NGX、不 Init。B1 的第一步改为
  「自加载 core（`C:\Windows\System32\DriverStore\FileRepository\nvami.inf_amd64_*\_nvngx.dll`）
  + `Init_ProjectID`（沿用 NR 的 app id）+ `GetCapabilityParameters` + 用 core 的 `CreateFeature(1)`
  建 DLSS-SR」。离线 `ngx_probe --sr` 已验证：Init 返回 1、`CreateFeature(1)` 对 360×243 →
  1091×736 返回 1 且 handle 非空；模型 `nvngx_dlss.dll` 从 exe 同目录被 core 找到。
  **不要**在未 Init 的 core 上直接建 feature；自建会话只在确认引擎没有 Init 时执行。

- **一个 compute**：读 color 源与 depth 源，一次 dispatch 写 `RGBA16F` / `R32F` / `RG32F` 三张 UAV；
  之后全屏 PS 把 DLSS 输出（或旁路 color）写成后缓冲。compute 不直接写后缓冲（翻转链后缓冲的
  UAV 标志不保证，PS+RTV 是稳的）。
- **NGX 参数对象独立**：优先 `AllocateParameters`，不要与 NR 共用 capability 对象。
- **DLSS 参数**：`Width/Height` = render，`OutWidth/OutHeight` = display；HDR 路径
  `DLSS.Feature.Create.Flags=1`、SDR = 0；**不设 MVLowRes**；`MV.Scale=1`（MV 已是像素单位）；
  `DepthInverted=1`；`Reset=1` 一次性（进世界/重建/重新启用）。
- **输出编码（B0 定案）**：HDR 输出用 **ACES + gamma 2.2**（与 F2 截图平均差 26.3/255，
  相关 0.978；Reinhard 是 113.9，不用）。后缓冲 RTV 是 sRGB 视图时输出线性值、由硬件编码；
  是 UNORM 时 shader 内做 gamma——面板给读数。
- **一个进程一个 SR**：见到引擎 `CreateFeature(1)` 就拒绝并提示重启；VV 下预期没有，必须防。
- **失败即跳过**：任何一步失败当帧不写后缓冲，TAAU 的画面自动保留；连续失败 N 次自动停用。
- **尺寸**：新尺寸稳定 12 帧才重建；两次重建至少隔 20 次求值；每会话上限 64 次；DRS 抖动期跳过。
- **计时口径**：TAAU 仍跑时 A/B 的差值是 **DLSS 增量成本**，不是替换收益；净收益只在 B4 量。

### 13.7 B1a：自建 NGX 会话探针（2026-10-25 已实现，待实机）

**目的**：只验证「VV 里能自建 NGX 会话并建出 DLSS-SR feature 1」，不 evaluate、不碰任何引擎纹理、
不改变画面。离线 `ngx_probe --sr` 已证路径可行；这一步是在**游戏自己的设备**上重复一遍。

实现：

- `src/neurodragon/vv/VvDlssSr.{h,cpp}`（独立 `<d3d12.h>` TU）；
- 面板 `诊断 → VV DLSS B1a (TEST)` 一个按钮 `Probe self-init NGX + CreateFeature(1)`；
  `requestProbe()` 只置原子位，真正的工作在下一个 Present 的 `runPending()` 里做；
- core 路径：`System32\_nvngx.dll` → `System32\nvngx.dll` → DriverStore 的
  `nvami.inf_amd64_*\_nvngx.dll`（本机实际走最后一条）；
- 会话：`NVSDK_NGX_D3D12_Init_Ext(0x0876232C, %TEMP%\NeuroDragon\NGX, engineDevice, 0x15, null)`；
  失败再试 `Init_ProjectID`（不带 FeatureCommonInfo）；
- `GetCapabilityParameters` → 写 `Width/Height/OutWidth/OutHeight`、`PerfQualityValue=3`、
  `CreationNodeMask/VisibilityNodeMask=1`、`DLSS.Feature.Create.Flags=1`（IsHDR、不设 MVLowRes）；
- 自建临时 allocator/list/fence，`CreateFeature(1)` → `Close` → 在游戏队列上执行 → fence 等待 3 s
  → `ReleaseFeature`；全程只读引擎设备，不碰帧。

**验收（重启后点一次按钮）**：
- 日志：`[vvsr] Init_ProjectID(pathlist) ret=0x00000001`、
  `GetCapabilityParameters ret=0x00000001`、`CreateFeature(1) ret=0x00000001`、
  `B1a OK: feature 1 created and released (…)`；
- 面板 `B1a NGX returns` 显示 `init=1 caps=1 create=1`，尺寸正确；
- 画面与点击前逐帧一致（TAAU 照跑），无崩溃。

**2026-10-25 04:04 首次 B1a**：`init=1 caps=1 create=0xBAD0000B`（UnableToInitializeFeature）。
原因：第一次实现用 `Init_Ext(..., nullptr)`，没有给 core 任何模型搜索路径，它找不到 exe 同目录的
`nvngx_dlss.dll`。修复：改用 `Init_ProjectID` + `FeatureCommonInfo.pathList = [游戏 exe 目录]`
（`ngx_probe` 的同一形状）；path list / common info 存成静态对象，因为 core 可能到
`CreateFeature` 时才去加载模型。新构建 `60d19af8…`。

**2026-10-25 04:08 实机通过**：`Init_ProjectID(pathlist) ret=0x00000001`、
`CreateFeature(1) ret=0x00000001`（handle 非空）、`ReleaseFeature ret=1`；
尺寸 **1689×903 → 2560×1369**（当前用户是 66% 上采样）。**B1a ✅**。

**失败处理**：任何一步返回非 1 都只记录并停止，不创建 feature；若 `Init_Ext` 让客户端崩，
说明这条会话路径在游戏进程里不可用，B1 回到「VV 只做读数」并记录证据。

### 13.8 B1b-1：颜色旁路（2026-10-25 已实现，待实机）

**目的**：在 B0 已验证的尾部 k=4 插入点，用我们自己的 compute 读渲染分辨率颜色、按 B0 测得的
**ACES + gamma 2.2** 编码，再拷回后缓冲。画面会变成 TAAU 的模糊替代品，但 UI/HUD 仍在——
这一步确认注入点、颜色输入、色彩域和拷贝路径都成立，还不接 DLSS。

实现：

- `src/neurodragon/vv/VvBypass.{h,cpp}`：一个 compute shader（`Texture2D<float4> Src` → RGBA8 UAV），
  ACES + gamma，采样渲染分辨率颜色、双线性放大到后缓冲尺寸；`CopyTextureRegion` 写到后缓冲。
  PSO/root signature/描述符堆/输出纹理按需创建，输出随窗口尺寸重建。
- `probe/VvInject` 增加 `Action::Bypass`；`record()` 在 Bypass 动作下直接调
  `vv::bypass::record()`，不再清屏；失败则当帧不注入。
- 面板「诊断 → VV DLSS B1b (TEST)」：**「Colour bypass at tail k=4」** 一键把注入设成
  Fixed + 从尾部 k=4 + Bypass；「Stop injection」恢复。
- 颜色输入从 `VvProbe::renderTargets()` 取**最新的渲染分辨率 fmt26**；`gHeld` 的尺寸门已从
  ≤1000×600 放宽到 ≤2048×1200，覆盖当前 1689×903 的渲染尺寸。

**已知假设（第一次实测要验证）**：颜色纹理在 TAAU 之后按 `ALL_SHADER_RESOURCE` 读取（不插屏障）；
后缓冲在 k=4 时按 `PRESENT` 处理（与 M5 清屏实验相同）。若画面黑/花，下一版把颜色前后状态做成
面板可选项（`ALL / COMMON / PIXEL|NON_PIXEL`）再测一次。

**验收**：
- 按按钮后画面变成明显的“模糊但颜色/曝光接近原图”的世界画面，**HUD 仍在**；
- 面板 `B1b sizes` 显示 `1689x903 -> 2560x1369 fmt=26 recorded=1`；
- 不崩；「Stop injection」下一帧恢复原画面。

**2026-10-25 04:15–04:16 首次实机**：开头有效且“很明显”，约 40 秒后失效；日志
`bypass: no render-size fmt26 colour (0 target(s) visible)`。原因：`gHeld` 环形被 UI 图集
（1024×1024 等）填满，把渲染图的颜色挤掉了；`VvInject` 因此每帧跳过（`record failed`），画面回到 TAAU。
修复：
1. `VvBypass` **缓存选中的颜色资源并 AddRef**，直到 `renderGeneration`（resize / 进世界）或
   渲染尺寸变化才重新选——环形再挤也不会丢；
2. `gHeld` 排除**正方形**纹理（图集），环形扩到 256；
3. `renderGeneration` 在 resize / 进世界时递增，缓存不会跨重建拿旧纹理。
新构建 `263410d8…`。

**2026-10-25 04:22 推翻「尾部 k=4 稳定」的旧结论（世界内）**：B1b 稳定后 UI 闪、随后消失，
**手还在**；注入没有跳过（`skipped=0`），但 list 数在 **69/81 交替**。69 时 tail-4 = head-66
（世界合成后、UI 前）；81 时 tail-4 = head-78，晚 12 个 list，HUD 被旁路覆盖。⇒ 世界内 UI 块
大小会变，tail 计数不是内容边界。随后用 head 66 测试：**游戏内正常，但主菜单被擦掉**——
主菜单的 UI 块位置与游戏内不同（用户还提到上采样系数从 33% 改到 66%，帧形可能与旧表不同）。
⇒ 任何 head/tail 计数都不能跨场景成立，必须按内容认 list。

**2026-10-25 04:3x 内容边界（B1b 的正确答案，已实现）**：任何 head/tail 计数都解释不了
「主菜单与游戏内、69/81 两种帧形」——head 66 在游戏内正确、在主菜单却把主菜单擦掉，说明
**场景不同、UI 块位置不同**。改为**按内容认 TAAU list**：

- `drawDetour` 在调用原回调时，扫描执行上下文（第 2 个参数，深一层）、TAAContext、self、fourth
  的 qword；若某个指针的 vtable 等于**引擎命令列表 vtable**（`hookCommandList` 里记录的），
  或该 vtable 的槽 9 等于已知 `Close`，就把这个指针当作「TAAU 正在录制的 list」；
- 全程只读 + `readable` 守卫，不挂钩任何未验证的槽；找不到就不认，不猜；
- `executeCommandLists` 里在该 list 的数组位置 +1 处切分（`VvInject::setUseTaauList(true)`），
  与 list 总数、tail/head 计数无关；
- 日志 `[vv] taau list=... from exec +0x...` 和
  `[vv] inject: taau-list boundary head=.../...`；面板 `B1b taau list: captured/not captured`。

若扫描不到（上下文里没有 list 指针），回退到面板的 head/tail k；这只是后备，不是结论。
构建 `c63a9d3c…`，待实机验证「世界内 + 主菜单都保留 UI」。

**2026-10-25 04:4x 浅层扫描失败（`not captured`）**：执行上下文第一层没有 list 指针。
已改为**按需深层扫描**：从 exec 走最多 3 跳、TAAContext/self/fourth 走 2 跳，
512 节点 / visited 去重 / 每节点 0x100 字节，仍只读；B1b 按钮会请求一次扫描，
面板仍显示 `captured/not captured`，日志给 `deep scan found nothing` 或命中的偏移。
构建 `167796d8…`。如果深层也找不到，下一步是**单槽标定 DrawInstanced（slot 12）**，
用 `gInTaauDraw` 标志直接抓当前 list——那条路有 hook 风险，必须先离线/自建 list 标定。

**2026-10-25 04:49 深层扫描也失败**：日志 `deep scan found nothing (213..298 nodes)`。
⇒ 执行上下文/回调对象图里没有引擎 list 指针，内容边界这条路在当前可读对象里走不通；
B1b 仍回退尾部 k=4。**DrawInstanced 标定成为下一步**，不是继续加深扫描。

> **2026-10-28**：该扫描子树与 `VvInject::setUseTaauList` 已在阶段 A 前的死代码清理中删除
> （§34.5 第一刀）；结论保留为死路，见 `re_kb` 的 `command_list_taau_scan`。

**2026-10-25 04:5x 色调诊断**：回退 tail 4 时主菜单没有消失，但存档内颜色“很奇怪”。这不是
边界问题，而是**颜色候选或色调曲线**不对：渲染分辨率下有多个 fmt26（最终色/历史/中间面），
以及我们的 ACES 曲线是在 33% 那一帧拟合的，66%/另一个存档的曝光可能不同。
已加面板诊断：
- **Next colour candidate**：在 `renderTargets()` 的 fmt26 候选间循环（缓存随之失效），
  面板显示 `choice=x/N` 与指针；日志列出每个候选的 `ptr/WxH`。
- **Cycle tonemap**：`0 = ACES+gamma`、`1 = gamma only`、`2 = ACES only`，
  用来区分“选错颜色”与“曲线不对”。
构建 `43d5991e…`。下一步由这两组 A/B 决定是固定候选选择还是改色调映射。

---

## 14. 接手审查与 B1c 计划（2026-10-25 16:0x）

> 本节是**现行计划**，与 §13 的时间线冲突时以本节为准。§13 保留为证据。
>
> 一句话：**「颜色很奇怪」不要用手摇曲线去解**——把**同一帧的后缓冲**（= TAAU 的输出）截下来当参考，
> 和候选纹理、深度一起 dump，离线一次定死颜色、色调与切点；然后直接做 DLSS。
> B1b 的目标是「验证切点 + 颜色域」，它已经达成（04:15 起画面成立、1920 帧 `skipped=0`）；
> 它没达成也不该由它达成的是「把画面调得像 TAAU」——那是 DLSS 上屏之后用参考图去对的活。

### 14.1 已经可以当依据的（强证据）

| 结论 | 证据 |
|---|---|
| 引擎整帧一次 `ExecuteCommandLists` 带 ~48 个 list；拆成「前缀 + 我们的 list + 后缀」在同队列上执行成立 | M5 洋红实测（§12 A）；B1b 世界内连续注入 1920 帧、`skipped=0`（04:50 日志） |
| `taau` 每帧 1 次声明 + 1 次 draw；factor / jitter / viewport / 四个依赖的语义 | §2–§3 |
| 引擎设备从它自己提交的 list 反取；`F11` 重建后再读 RTV 清单是抓引擎资源的通用手法 | §5.1 |
| VV 下没有 NGX，自建会话 + `CreateFeature(1)` 能建出 DLSS-SR（release 返回 1） | B1a 04:08（1689×903 → 2560×1369） |
| **VP 矩阵不含 jitter**（静止时 `prev(n) == cur(n-1)`，而同帧 jitter 在变） | P0.4。⇒ MV 用未抖动矩阵算、jitter 单独交给 DLSS，正好是 DLSS 的契约 |
| 深度主候选 = `fmt44`（`fmt41` 恒 1.0、`fmt54` 是 D16 副本） | P0.3 |
| 注入列表/边界的机制、`Release` 与并发重建两条崩溃教训 | §9、§12 A |

### 14.2 不能当依据的（弱结论 / 已过期）

| 说法 | 问题 | 处置 |
|---|---|---|
| 「color = 最新的渲染分辨率 `fmt26`」 | 「最新」是 `gHeld` 环的**插入顺序**，不是语义；`VvBypass` 取 `candidates[0]`，与 TAAU 真正读的那张没有直接对应关系 | B1c 用参考图定 |
| 「ACES + gamma 2.2」 | 33% 下的 844×475 **单帧**拟合（MAD 26.3/255），被直接用在 66%、另一个存档；10% 的残差说明还有一项没建模（**曝光**最可疑） | B1c 用参考图拟合 LUT，先看残差 |
| 「插入点 = 尾部 k=4」 | 只对 69 个 list 的暂停界面成立；世界内 69/81 交替时 HUD 被覆盖（04:22） | B1c 换一种问法：直接看每个切点截下来的图 |
| 「不能按公开头文件的槽号挂钩」 | 错的，见 §14.3 ① | 更正 |
| MV 公式 | P0.5 只到「数据齐了」；矩阵/相机位置/远处的 w=0 都还没在一条真实路径上比过 | B2 的 MV 调试视图 + 同帧差分 |

### 14.3 三处更正

**① 槽号可以查公开头文件；25 不是 `OMSetRenderTargets`。**
本机 SDK 的 `d3d12.h`（`D:\Windows Kits\10\Include\10.0.26100.0\um\d3d12.h`）里接口的声明顺序就是运行时
vtable 顺序，本项目**已经实测对得上**的：`Close=9`（每帧 49 次，正在用）、`DrawInstanced=12`、
`DrawIndexedInstanced=13`、`CreateRenderTargetView=20`（RTV 清单正在用）、`CreateCommittedResource=27`、
`CreatePlacedResource=29`。10-24 22:0x 崩在槽 25，是因为**把 25 当成了 `OMSetRenderTargets`**；
25 实际是 `SetPipelineState`（`rdx` = 对象指针、`r8` = 残留值，与 `(UINT, const HANDLE*)` 的形状不符，
与当时的日志一致）。对应表：`OMSetRenderTargets=46`、`ClearRenderTargetView=48`、`ResourceBarrier=26`、
`SetDescriptorHeaps=28`、`Dispatch=14`、`CopyTextureRegion=16`。
**但「槽号对」不等于「批量挂钩安全」**：任何新槽只允许**纯转发**（detour 不改参数、直接调 trampoline），
并且先拿**我们自己的** command list 按头文件调用一次确认命中，再上引擎的 list。这条是给 B1d 用的。

**② 尾部 k=4 只是当时那个界面的答案**（§12 A 的结论已被 04:22 的世界内实测推翻，读 §12 时以更正条为准）。

**③ 颜色纹理的读取没有屏障，这比色调曲线更可疑。**
`vv/VvBypass.cpp` 里给颜色建 SRV 后直接 `Dispatch`，没有 `RENDER_TARGET → NON_PIXEL_SHADER_RESOURCE`
的转换，注释里写的是「假设引擎在 TAAU 之后已经把它留在 ALL_SHADER_RESOURCE」；而
`probe/VvCalibrate.h` 的 `DumpState` 注释恰好记过：**状态猜错，读回来的是全零或旧数据**（VV 纹理
当时就是全零）。所以「颜色很奇怪」在动曲线之前必须先排除这一条——B1c 的候选 dump 会按状态各 dump
一次，用「有数据的那个」反推真实状态。

### 14.4 计划

| 批次 | 内容 | 成本 | 退出条件（用户可见） |
|---|---|---|---|
| **B1c** 切点取证 | 一个构建：注入时把**后缓冲**拷进自有 readback、fence 完成后写 BMP；`Sweep` 对尾部 k=1..8 各截一帧；同帧 dump 候选 + 深度 + taau 参数 | 4–6 h 代码 + **1 次开机**（约 10 分钟）+ 1–2 h 离线 | 8 张切点图（哪张有 HUD / 手臂）+ 一张「参考 vs 最佳候选」对比图 + 面板上的 colour 指针与 LUT 残差 |
| **B1d** 内容边界（**条件触发**） | 只在 B1c 判「尾部 k 跨场景不稳」时做：纯转发挂 46 / 12，按「绑定后缓冲 + 恰好 1 次 draw」认 TAAU 的 list | 1–1.5 天 | 洋红实验：世界洋红、HUD 仍在，且换场景后边界不变 |
| **B2** DLSS 上屏 | 沿用 §13.6 的口径（HDR flag、`DepthInverted`、`MV.Scale=1`、一次 compute、PS/拷贝写后缓冲），加上 B1c 定下的颜色与色调 | 2–3 天 | `F8` A/B 可见；HUD 在；同帧参考 vs DLSS 的差分图无错位；10 分钟 / F11 / 重进世界不崩 |
| **B3** 收尾 | 面板读数、失败自动停用、文档、TEST + release 双构建 | 1–2 天 | 验收清单 + release 部署 |

**B1c 的具体做法**

1. `probe/VvInject` 增加 `Action::Capture`：在我们自己的 list 里
   `后缓冲 → COPY_SOURCE → CopyTextureRegion(自有 readback)`，再把状态还回 `PRESENT`；
   **不改画面**，也不碰任何引擎纹理。提交后由现有 fence 判断完成，随后 `writeBmp` 落盘。
2. 面板「Capture sweep」：`Sweep` + `Capture`，尾部 k=1..8，每档 1 帧。
   **k=1 = 整帧之后**（最终画面，含 HUD），k 越大越早——8 张图连起来就是「这 8 个 list 各画了什么」。
3. 同一会话再扫一组（转视角），用来分开「当前色」与「历史色」：历史色在转动时会滞后。
4. 同帧顺手跑现有 dump（`fmt26/28/44` + taau 参数），文件名带指针与 qpc。
5. 离线 `NeuroDragonWorkSpace/tools/vv_cut.py`（不进 mod 仓库）：
   ① 按 k 排序并标出**第一张出现 HUD** 的；② 每个候选与参考做块相关（先降采样 + 归一化，与色调无关）；
   ③ 对最佳候选拟合单调 LUT，输出残差与 `_vv_match.png`（参考 / 最佳候选 / 差异三联）。
6. 判据：块相关 ≥ 0.99 且 LUT 残差 ≤ 2/255 ⇒ 颜色与色调都定；残差 > 8/255 ⇒ 色调里有随场景变化的项
   （查曝光），此时 B2 先固定近似曲线，把「读曝光参数」另立一条。
7. 对象图深扫与 `taauList` 分支（两条路都已证伪）留到 B1c 之后清理：本轮不动探针，免得在采集构建里
   引入新变量。其中 `methods[9] == gOriginalClose` 这个兜底比较本身是死的——MinHook 不改 vtable，
   只改函数序言。边界回到「面板上的 k」，直到 B1d 给出内容边界。

**B1c 实现（2026-10-25 16:2x，TEST 构建 `c8cb017e…`，已热替换到本工作区；旧 `43d5991e…` 备份 `.b1c-old`）**

- `vv/VvCapture.{h,cpp}`：`record()` 在我们的 list 里把后缓冲拷进自有 READBACK，4 个槽的环，
  fence 没过就整帧跳过（不覆盖上一张）；`onSubmitted()` 在拆提交之后于同一队列 `Signal` 捕获 fence；
  `presentHook` 里的 `poll()` 映射已完成的槽并写 BMP
  `vv_cut_<seq>_k<split>_of<count>_tail<k>_<w>x<h>.bmp`（游戏根目录）。
- `probe/VvInject`：`Action::Capture`；`setSweepDepth(8)` 让一轮只走尾部 k=1..8，跑完自动关注入
  （一轮 = 每档一张图）；每档 hold=1，避免同一个 k 连拍多张。
- `probe/VvCalibrate`：导出 `writeBmpRgba8()`，复用原来的 24 位 BMP 写法。
- 面板「诊断 → VV B1c cut capture (TEST)」：`Capture sweep: tail k=1..8 (one pass)` / `Stop capture`，
  读数 `captured/skipped/pending`、`tail/split/of`、最后文件名。
- 离线 `NeuroDragonWorkSpace/tools/vv_cut.py`：切点联表 `_vv_cut_strip.png`、相邻切点平均差
  （哪一档多出了一个 UI 块）、候选与参考的结构相关排名、单调 LUT 拟合与残差、`_vv_match.png`、
  `_vv_curve.png`（散点：紧贴曲线 = 固定色调；散成一团 = 含曝光或选错纹理）。
- **一次会话的采集顺序**：进世界 → 按一次 `F11` → 站定 → 面板按 capture（约 8 帧，画面不变）
  → **立刻**按 `F9` 或 B0 的「Dump now」（候选 dump；相机必须还没动）→ 退出 → `python vv_cut.py --dir <版本目录>`。

**B1c 的备用分支**（只在第 6 条判「残差大且不是选错候选」时走）：从引擎的 PSO 里把 TAAU 的 shader
字节码 dump 出来，把 tonemap / jitter / reprojection 直接读出来，而不是拟合。只需三个**纯转发**钩子：
device 的 `CreateGraphicsPipelineState=10`（存 bytecode）、list 的 `SetPipelineState=25`（记当前 PSO）、
`DrawInstanced=12`（记这一 draw 用的是哪个 PSO）；随后离线反汇编。代价约 1 天 + 一个 DXBC/DXIL 工具，
但这条能一次性把色调和 MV 两个未知都答掉。

**不做的事**（都是这半天的教训）：不再扫回调的对象图找 list；不在游戏里手摇色调曲线；
不做批量槽标定；不在 B1b 上继续加功能；不为 33% 与 66% 各拟合一套。

### 14.5 B1c 结果（2026-10-25 16:37 采集，2560×1440 全屏、66% 上采样、70 个 list）

**切点（看画面，不猜 k）**：8 张尾部切点里，只有 **tail 5 → tail 4** 这一档整个画面变了
（中心区平均差 **31.19/255**），其余相邻档差 ≤ 0.21。读法：

| 尾部 k | 画面 | 结论 |
|---|---|---|
| 8 … 5 | 世界 + **上一帧的侧栏面板与 HUD** | 这一帧的**世界合成还没发生**：引擎在这几档完全没动后缓冲（画面上是上一帧留下的内容，包括我们自己的 ImGui 面板） |
| **4** | **世界，无 HUD、无手** | **list 67 = 世界最终合成**（一张全屏 pass 覆盖整个后缓冲）⇒ 插入点 |
| 3 | 世界 + 手 + 快捷栏 + 准星 | list 68 = 手 / HUD 层 |
| 2 / 1 | 再加坐标与小头像 | list 69 / 70 = 其余 UI |

⇒ 在这一帧里，**世界合成之后只有 3 个 list**，`tail 4` 正好落在合成之后、UI 之前；
手与 HUD 是**显示分辨率**、在合成之后直接画进后缓冲的（所以我们的输出只要覆盖世界，不碰 UI）。

**颜色**：候选里 `fmt26` 1689×950（本次 `res_100`，ptr `0x1F7AACC6010`）秩相关 **0.9875** 最高，
101/102 是历史/中间面（0.9824 / 0.9644）。**色调不是 ACES**：用 B1c 的帧对重新拟合，

| 模型 | 残差（中心 2/3，0..255） |
|---|---|
| 逐通道 64 节点单调曲线（拟合） | **2.31** |
| 单条幂函数（逐通道增益） | 6.34 |
| ACES + gamma 2.2（B0 用的） | 21.40 |
| 纯 gamma 2.2 | 28.87 |
| sRGB | 30.07 |

**跨会话复验**：用 03:44 那次（33%）的 dump + F2 截图独立拟合一遍，两条曲线在中段一致
（x=0.15: 0.52 vs 0.55；x=0.30: 0.72 vs 0.74；x=0.45: 0.77 vs 0.74）⇒ 这是**显示编码**，
不是随场景变化的 tonemap；颜色缓冲里已经带了色调映射，TAAU 只做「线性 → 8 bit 显示」这一段。

**落地**：

- `vv/VvTone.h`：把测得的编码存成 3×33 表（二次网格，暗部密），以两个 root-constant 块
  （64 + 35 floats）交给 shader；`vv/VvBypass.cpp` 用它替换 ACES（面板「Cycle encode」可切
  table / gamma / ACES+g 对照）。
- 离线工具：`vv_cut.py`（切点联表 / 候选排名 / 曲线拟合）、`vv_tonefit.py`（模型对比）、
  `vv_curvecheck.py`（两次会话的曲线是否一致）、`vv_gen_tone.py`（生成 `VvTone.h` 的表）。
- TEST 构建 `6fe07a45…` 已热替换到本工作区（上一版 `c8cb017e…` 被覆盖，同一源码可重建；
  `43d5991e…` 仍在 `.b1c-old`）。

**仍未答的两件事**：

1. ~~`tail 4` 在别的帧形是否成立~~ → **已改：不再数尾巴，改用内容边界（§14.6）**。
2. 编码曲线只在**白天**两个场景验过。夜间/洞穴/强曝光差异下要再抓一帧复验（同一条工具链）。

### 14.6 内容边界（B1d，2026-10-25）：按「谁写了后缓冲」认，不数尾巴

**问题**：`tail 4` 只在 70 个 list 的那一帧对。世界内的帧形会在 69/81 之间变，暂停界面又是另一种；
2026-10-25 04:22 的旁路就是把 HUD 冲掉了。**计数天生会漂**——它描述的是「离结尾多远」，
而我们要的是「世界合成之后」。

**不变量**（B1c 的画面已经证明）：一帧里只有**一个** list 写整个后缓冲（世界合成），它之前的 list
一次都没碰过后缓冲，之后才是手 / HUD / UI。所以锚点是「这一帧**第一个绑定后缓冲并 draw** 的 list」，
与帧有多少个 list 无关。

**实现**（纯转发：不改参数、不碰资源状态）：

- `OMSetRenderTargets` = 槽 **46**、`DrawInstanced` = **12**、`DrawIndexedInstanced` = **13**
  （公开头文件的声明顺序；本项目已实测对上的还有 `Close=9`、`CreateRenderTargetView=20`、
  `CreateCommittedResource=27`、`CreatePlacedResource=29`）。
- `noteSwapChain` 每次 Present 把「后缓冲资源 ↔ 它们的 RTV 句柄」对上（日志 `back-buffer view handles=`），
  所以 `OMSetRenderTargets` 的热路径只比较几个整数，不做纹理查找。
- 记录每帧「哪些 list 绑定过后缓冲」「哪些随后 draw 过」；`ExecuteCommandLists` 里按数组顺序取
  **第一个** bound+drew 的 list，得到 1 基 split，`inject::prepare()` 直接用它当切点。
- 面板：`B1b boundary` 显示 `content: back-buffer writer`，`B1d writer` 显示 `split= list=`；
  日志 `[vv] content boundary split=…`（前 6 帧 + 失败时每 600 帧）。

**自检（用户可见）**：面板「Capture at content boundary (1 frame)」截出来的图**必须不含 HUD 与手**。
在世界、暂停菜单、背包界面各按一次，图仍然干净 ⇒ 锚点成立。这条同时验证三个槽没有错位
（首个 `OMSetRenderTargets` 调用的 count/single/handle 会打在日志里）。

**风险**：三个槽都是纯转发，猜错只会「看不见 / 看错」，不会像 10-24 的槽 25 那样把参数
当结构体传进去崩掉；失败时 `split=0`，`inject` 自动退回面板上的 k。

**第一次实机（2026-10-25 16:56–16:59）：钩子对了，句柄没找到。**
`[vv] om rt #1 list=… count=1 single=1 handle0=… increment=32` ⇒ 槽 46 的形状完全正确；
但 `bbHandles=0`、`back-buffer view handles=0`。原因：交换链后缓冲的 RTV 是**模组加载之前**创建的，
`gRtvs`（CreateRenderTargetView 的句柄表）里没有它们；而重扫只在「交换链指针变化」时触发，
16:59:11 引擎重新创建了两张 `2560x1440 fmt=28` 视图，却没有触发重扫。
修复：**RTV 清单增长也触发重扫**（`rtv views=` 进日志）。这条同时也说明锚点能不能用，取决于
「引擎会不会在钩子装好之后重建后缓冲视图」——B1d 的第二个不确定点。

**顺手修掉的坑**：`Action::Capture` + `Mode::Fixed` 原本会**每帧**写一张 11 MB 的 BMP
（16:58 那一分钟写了 1941 张、21 GB）。现在 Fixed+Capture 是**一次性**（截完自动关），
并且 `capture::record()` 有 64 张的硬上限。已删掉那批 `tail0` 文件，保留 16:37 的 8 张。

**兜底锚点（已埋好，只记录不使用）**：`RSSetViewports` = 槽 **21**（纯转发）。世界 pass 用渲染分辨率
视口，合成与 UI 用显示分辨率视口，所以「这一帧第一个用显示分辨率视口并 draw 的 list」是后缓冲写者的
替代判据。前 3 帧会打一行 `[vv] frame viewports #N: 0:1689x950 … 67:2560x1440 …`——
如果句柄路线仍然拿不到，就按这一行来定判据，而不是继续猜。

**第二次实机（17:03–17:04）：重扫生效，句柄仍然对不上。**
`back-buffer view handles=8` 出现了（说明「RTV 清单增长也重扫」有效），但 `content boundary split=0`
依旧，`marks` 大多 0、偶尔 1 —— 合成绑定的那个 RTV 句柄不在表里：后缓冲的视图是**模组加载前**建的；
16:59 重建的几张又按「取最新 8 个」被后来的视图挤掉。已改为**按创建顺序取最早 32 个**，
句柄日志只在数量变化时打（否则一次会话 3778 行）。

同一轮还拿到了第一份**整帧清单**（视口 / 是否绑深度 / draw 形状）：
48 个 list 的菜单帧里有三个显示分辨率视口（list 2 / 25 / 44）。这正是兜底判据要用的数据，
所以诊断升级成「帧形一变就打一行」——世界帧（70 个 list）比菜单帧更有用。

**兜底判据的数据来源**：`[vv] frame #N lists=M: 0:v… 1:v…`，每个 list 一段
`v<宽>x<高>[z] d<draw 次数>[ s<顶点>x<实例>][ x<索引数>]`，例如
`67:v2560x1440 d1 s3x1` = 该 list 用显示分辨率视口、绑了深度、画了一次三角形。
拿到世界帧的这一行，就能定出「合成」的签名（预期：显示视口 + 无深度 + 一次全屏 draw），
且这个判据完全不依赖句柄的创建时机。

**第三次实机（17:09）：19 种帧形的整帧清单，锚点改成「签名」。**

| 位置（相对帧尾） | 视口 | 绑深度 | draw | 判读 |
|---|---|---|---|---|
| 帧首 idx 2 | 显示分辨率 | 有 | 0 | 只设视口 / 绑 RT，不画 |
| tail 23 | 显示分辨率 | 无 | 1 次（6 索引） | 一次全屏 blit |
| **tail 3 / 4 / 5（随帧形变）** | **显示分辨率** | **有** | **恰好 1 次（6 索引）** | **世界合成** |
| tail 3 | 显示分辨率 | 有 | 40–73 次 | HUD |

48、49、50、51、55、58、59、62、65、69、70、73、74、75、81、82、102、107、108 这 19 种帧形里，
**合成 = 显示分辨率视口 + 绑深度 + 恰好一次 draw** 的签名全部唯一命中（HUD 在个别帧里也只有一次
draw，但排在合成之后，取**第一个**即合成）。而 tail 偏移在 **3 / 4 / 5** 之间变
（58 / 62 / 65 个 list 的帧分别是 tail 5 / 3 / 3）——**这正好证明「数尾巴」会错**。
16:37 用切点图量出来的 `split=67/70` 与签名算出的 67 完全一致。

⇒ 运行时的切点顺序：**签名**（`via=signature`）→ 后缓冲句柄（`via=handle`，句柄表仍可能被
overlay 的视图挤满）→ 面板上的 k。

**第四次实机（17:13）：签名自检通过。** 在世界里按「Capture at content boundary」，
注入点 `split=67/70`（`via=signature`），截出来的图是**世界 + 手、没有 HUD / 准星 / 快捷栏 / 坐标**
（`_check_boundary.png` 由那张 BMP 转出）。手在合成里（渲染分辨率内），所以它本来就该出现在这层——
DLSS 会像 TAAU 一样把它一起放大。

顺带修掉一个「早一格」的边角：48 个 list 的帧里出现过一次 `split=45`（比合成早一格，注入的帧会被
下一次合成覆盖回来，属于**无害但无效**的失败）。改法：**优先选「紧接着多 draw 的 HUD 那一格」的
候选**（合成后面就是 HUD），没有这样的候选再退回第一个 —— 日志会写 `via=signature+hud`。
TEST 构建 `01aac5bf…` 已热替换到本工作区。

## 15. B2：DLSS-SR 上屏（进行中，2026-10-25 17:2x）

### 15.1 管线

引擎后缓冲在内容切点之后，我们的 list 依次做：

```
引擎 colour (fmt26, 渲染分辨率) ─┐
引擎 depth  (R24G8, 渲染分辨率) ─┴→ prep compute → RGBA16F / R32F / RG32F（MV 先置 0）
                                                        │
                                    （下一步）NGX DLSS-SR：Color/Depth/MV → Output（显示分辨率 RGBA16F）
                                                        │
                              final compute：VvTone 编码 → RGBA8 → CopyTextureRegion 到后缓冲
```

### 15.2 现在能看的（B2a，构建 `8b6baaf8…`）

面板「诊断 → VV B2 SR (TEST)」三个按钮，都用内容边界：

| 按钮 | 看什么 |
|---|---|
| `SR colour bypass` | prep 出来的颜色 + 编码，应该和游戏画面几乎一样（只是无 SR 的糊） |
| `SR depth view` | 深度图：近白远黑（reversed-Z），应有地形/树的结构 |
| `SR motion view` | MV（现在恒 0 → 均匀紫）；接上重建后用来判断「静止为 0、前进径向、转视角切向」 |

读数：`B2 pipeline`（是否就绪、渲染/显示尺寸）、`B2 inputs`（当帧选中的颜色/深度指针与格式）。

为什么先做这几件：颜色与深度的格式/状态、编码、写回后缓冲——这四件正是 DLSS 的输入输出契约。
它们能上屏之后，接 NGX 就只剩「参数表 + 求值」这一件未知。

### 15.3 下一步

1. **MV**：用 `TAAContext+0x200/+0x240`（未抖动 VP）、`+0x280/+0x28c`（相机位置）与深度重建；
   用 `SR motion view` 验「静止为 0」。`DepthInverted=1`。
2. **NGX**：B1a 已证的自建会话 + `CreateFeature(1)`；参数按 §13.6 写，`Reset` 每帧（先不做时间累积），
   观察 `evaluateRet`。
3. 再接 jitter 与时间累积、F8 A/B、计时。

### 15.4 实机（2026-10-25 18:2x）：颜色链路验收通过

- `SR colour bypass` 上屏 = 正常颜色、整屏覆盖；`Dump SR inputs` 的三张图对得上：
  引擎颜色均值 0.10/0.13/0.18、prep 后的 `colorhalf` 一致、`final` 一致。
- 踩掉四个坑，全写进代码注释：
  1. **根签名寄存器重叠**：prep/final 两张描述符表都想用 `t0/u0`，D3D12 直接拒签
     （`E_INVALIDARG`，序列化器原文："overlaps with another shader register range"）。改成
     一张 SRV 表（6）+ 一张 UAV 表（4）。
  2. **根签名 64 DWORD 上限**：99 个编码表常量当 root constants 必挂。编码最终改成**无参数的
     逐通道幂函数**（R 0.332 / G 0.352 / B 0.405，残差 6.3/255），LUT 留作参考。
  3. **CBV 里的表运行时读不全**：红屏（只有 R 行有效）。离线同一张表套同一张图是正常的，
     所以放弃 CBV 路线。
  4. **final 尺寸用了 TAAU viewport（2560×1369）而不是后缓冲（2560×1440）** → 底部 71 行黑边。
     改成以后缓冲自身尺寸为准（DLSS 的输出尺寸也必须如此）。
- 深度视图：reversed-Z 的数值挤在 0.97–1.0，显示改成 `(1-d)*20` 才有对比。

**B2b（DLSS 接线，构建 `4714da92…`）**：`vv/DlssSr` 增加常驻 feature（`ensureSrFeature` /
`evaluateSr` / `releaseSr`）；`VvSr` 在 `View::Dlss` 下：prep → NGX evaluate（`Reset=1`、`MV=0`、
`DepthInverted=1`、`MVScale=1`）→ 把输出当 t2 → 编码 → 写后缓冲。面板 `SR + DLSS` 按钮，
读数 `B2 DLSS: feature= create= eval=`（1 = 成功）。

### 14.7 待用户拍板

| 选项 | 内容 | 代价 |
|---|---|---|
| **A（推荐）** | B1c → B2 → B3；B1d 按需插入 | 3 次开机，颜色/切点一次定死，B2 的第一次上屏就有可对比的参考 |
| **B（最快看到 DLSS）** | 跳过 B1c，颜色沿用「最新 fmt26 + ACES」直接做 B2 | 省 1 天；第一次 DLSS 上屏的颜色仍然可能不对，回头再修 |
| **C（先交付旁路）** | B1c → 修好 B1b 并交付一版「VV 里能看的 SR 旁路」→ 再 B2 | 多 1–2 天；每一步都能上屏 |



## 16. 实机：DLSS 常驻（2026-10-25 23:13-23:16，构建 `1337c195…`）

**已确认**

- `EvaluateFeature` 从 f=1 到 f=18600 全部 `ret=0x1`（3 分 14 秒，2560×1440，渲染 1689×950）；
  注入 0 跳过，HUD / 手 / 准星 / 坐标都在（`docs/agent/evidence/dlss-on-screen-3min.log` 与同会话截图）。
- 上一轮「约 27 s 后掉回 TAAU」的机制：深度清单只有 8 槽，引擎每 ~6 s 新建一张 `128×128` 深度，
  第 8 张（23:05:30.235）正好把 `1689×903` 主深度挤出环，紧随其后的 re-pick（.239）拿到 0 张。
  修法：环扩到 64 槽 + 清单拒收 < 显示尺寸/4 的纹理 + 淘汰大尺寸条目时 warn（EXP-0004）。

**未解决（用户观察，均有原始证据，尚未定位到根因）**

1. **回主菜单后背景被覆盖**：菜单里 SR 仍在跑（`inject fixed k=46/48`、`eval=1`），且用的仍是世界里
   那份 colour/depth（指针从 f=2 到 f=18600 没变）。截图显示菜单背景就是世界最后一帧（含第一人称的手），
   即注入把进世界时选中的 colour 一直用到菜单。⇒ 颜色输入应当跟着引擎的 taau 声明走（§6 的 `in.color`
   在深扫里能读到 `res=`），而不是只在 generation 变化时选一次。
2. **底部约 6–11 行黑边**：我们的写回是整资源拷贝（`sr buffers 1689x950 -> 2560x1440`，`CopyTextureRegion`
   无 region），同日游戏自带 F2 出图 2560×1440 **无黑边**；用户两张图为 2559×1439 / 2558×1439。
   ⇒ 待确认：黑边是屏幕上肉眼可见（那就要在拷贝后加探针），还是只在截图里（截图/窗口边缘）。
3. **切场景时约 0.78 s 冻结**：世界内稳定 99–100 fps（每 ~21 s 有一段 3 秒的 77–82 fps，非单帧卡死）；
   唯一的长帧 `779.6 ms at present #20003`（23:16:17.850）与 taau 深扫的 11 行同步 dump
   （23:16:17.154→17.848，每行 ~70–120 ms）完全重合。⇒ 冻结来自深扫，不来自 SR 管线。

## 17. 运行时切换与相机（2026-10-26）

> 这轮要回答两件事：S5 怎么让 TAAU 不执行；A2 在 TAAU 不跑时拿什么当 MV 相机来源。
> 两条都绕不开「引擎设置到底存哪」与「相机对象能给什么」。

### 17.1 设置存在哪

| 路径 | 结论 |
|---|---|
| `<version>\options.txt`（版本根目录） | **诱饵**：改了不生效。EXP-0016 把 `upscaling_mode` 改成 1，引擎仍 `mode=0` |
| `Minecraft Bedrock\Users\<id>\games\com.mojang\minecraftpe\options.txt` | **引擎真正读的那份**：启动读、退出写回 |

- 证据：EXP-0017 改 per-user `upscaling_mode=1` → `[vv] taau #` 0 次；EXP-0020 运行时刻意改完，
  退出时引擎把 `1` 写回 per-user 文件。
- `upscaling_percentage` 的值就是渲染比例（per-user 里 `66` → taau `factor=0.6600`）。
  早期文档写「33%」用的是根目录诱饵的值，别再当依据。
- 旧备份 `options.txt.bak-*-20261023` 表明这条路径 10-23 就被人用过，只是没写进文档。

### 17.2 OptionID 在 26.20+ 不可用，而且编号移位

- `mc/options/option_types/OptionID.h` 在 26.51.7 里是空枚举（3 行）；26.10.14 的包里有完整枚举
  （LiteLDev/mcapi-requests#255：客户端 options 头被裁剪）。
- **编号也变了**：26.10 的 `DeferredUpscalingMode = 579`；26.51 运行时实测
  `579 = upscaling_percentage`、`580 = upscaling_mode`、`581 = gfx_upscaling`、
  `582 = target_resolution`、`703 = graphics_mode`。照抄旧枚举会写到错的选项上。
- 可行做法：`IOptionRegistry::isValidOption(id)`（有边界检查）扫 0..1023 → `get(id)` →
  `Option::save(vector<pair<string,string>>&)` 拿 save tag（就是 options.txt 的键名），
  匹配后 `Option::load("1")`。EXP-0020 实测 `valid=470`、`id=580 upscaling_mode`。
- **2026-10-29 起运行时写死编号**（`probe/RuntimeOptions.h` 的 `Known`）：579/580/581/582/703
  就是上面那五条实测值，写入前用 `save()` 复核目标的 save tag，对不上就拒绝并记
  `[vv] options: write failed …`，所以改版后编号移位只会变成有日志的 no-op，不会写错选项。
  0..1023 的扫描退回「一次性取证」（EXP-0020），不再进运行时路径。排空点也从「Present 投递 +
  join 一次」改成 `ClientLevelTickEvent`（客户端线程 20 Hz）——join 只触发一次，而面板按钮要在
  世界里随时可用。
- **LeviLamina 暂不升级（26.51.7 → 26.51.9，2026-10-29 决定）**：完整 `OptionID` 只在 26.51.9 的
  头里，且与上面五条运行时实测逐条一致；但本检出的 xmake 包与实例 loader 都是 26.51.7，只换头是
  ABI 赌博，换 loader 又要同时动三个实例与部署账本——为五个整数不值得。为此把依赖改成
  `add_requires("levilamina 26.51.7")`：不锁版本时重新配置可能解析到 26.51.9，编译一个 ABI、
  跑另一个。
  将来要升就**头 + 三个实例 loader 一起升**，升完重跑 `flow.py verify` 与一次启动自检
  （26.51.8 改了 reflection / function_traits，26.51.9 还 bump 了 bedrock-runtime-data；
  本 mod 直接读引擎结构体偏移，这类改动要当风险项看待）。

### 17.3 运行时切换（不碰 options.txt）

- 入口：`ClientInstance::getOptions()` → `IOptionRegistry` → `Option::load("1")`。
  EXP-0020：load 之后再无一条 `[vv] taau #`（含随后进世界），退出时引擎写回 `mode=1`。
- **必须在客户端线程**。`ll::thread::ClientThreadExecutor` 在启动早期投递的任务不保证及时执行
  （EXP-0018/19：hand-off 有日志，任务没跑或很晚才跑）；它本身也是在 `ClientLevelTickEvent` 上
  排空的，所以现在直接挂同一个 tick（20 Hz）——排队到写盘最多一个 tick，不再有 2 s 重投窗口。
  join 事件仍会顺手排空一次（标题界面没有 tick，早到的请求留到第一次 level tick）。
- 测试开关：`<mod>\config\force_bilinear.flag` 存在时，TEST 构建在 load 时排队，由 join / level tick
  排空。日志序列：`marker … present=1` → `bilinear switch requested at load time` →
  `[vv] options: write ok 580 upscaling_mode 0 -> 1`。
- **`upscaling_percentage` 也是实时生效的（EXP-0081，2026-10-29 实测）**：

  | 时刻 | 事件 |
  |---|---|
  | 09:23:28.812 | `write ok 579 upscaling_percentage 66 -> 50` |
  | 09:23:28.835（+23 ms） | 渲染图出现新的渲染尺寸目标 `1280x720`（fmt=26 colour ×2 + 深度） |
  | 09:23:28.862（+50 ms） | `[vv] taau #4500 … factor=0.5000`，viewport 仍是 2560x1440 |

  100/66/50 来回写，每次 factor 都在下一帧跟随；反过来 **写 100 时 `taau` 声明整条消失**
  （渲染尺寸 = 显示尺寸，没有上采样可做），写回 50 后立刻恢复。⇒ 面板的「内部分辨率」在 VV 下
  可以做成真开关（写 579），但必须同时处理：① 按新渲染尺寸重建我们自己的 DLSS feature；
  ② 玩家把比例设成 100% 时后端不存在，SR 分区要禁用。
  证据：`docs/agent/evidence/option-percentage-EXP-0081.log`。注意 R3 管线那次**没有 arm**
  （日志里没有 `sr arg10`），所以这条只证明选项本身，不证明注入在新尺寸上重建成功。

### 17.4 相机对象能拿到什么、拿不到什么

`ClientInstance::getCamera()` → `mce::Camera`（`mc/deps/renderer/Camera.h`）：

| 字段 | 实测（EXP-0021/0022） |
|---|---|
| `mRight/mUp/mForward` | 与同帧 taau context 的列向量**归一化后完全一致** ⇒ 是渲染用的相机 |
| `mInverseViewMatrix` | 只有旋转（camera-relative 渲染，没有平移） |
| `mPosition` | 恒 0（世界位置不在这里） |
| `viewMatrixStack` | 恒等（VV 不用 view 栈） |
| `projectionMatrixStack` | 大多为空；偶发上次 pass 的投影（Sx=0.76575 / Sy=1.36133 与 taau 列长一致，v14=-0.05） |
| `mAspectRatio/mFov/mZNear/mZFar` | 恒 0（0x320 字节 raw dump 里 basis 之后的标量段全 0） |

- **结论**：bilinear 档下相机方向可以自建 MV；**位置必须另找**
  （`getLocalPlayer()->getEyePos()` 是候选，第三人称/视角平滑会有偏差）；
  投影不能每帧可靠读到，读到后要缓存 Sx/Sy。
- **近平面不一致**：相机投影 v14 = -0.05，taau context = -0.025（2×）。
  MV 的 `depth = near/(1-d)` 用 0.025；若深度其实来自 0.05 的投影，平移项会差 2×。**未定论**。

### 17.5 快照时机（P0-2，已实机验证；2026-10-26 反编译更正）

- TAAU draw callback 拿到的 context 里，`+0x240` 是**上一帧**的 VP；提交后再读会变成
  `prev == cur`，所以 `drawDetour` 必须在转发前快照 `+0x200/+0x240/+0x1e4/+0x280`。
- EXP-0021 实机：走动时 `[vvsr] mv snapshot: previous VP differs from current (frame 298, delta=…)`；
  站定 `prev_same=1`。
- **更正**：Mac 全符号反编译和 Windows 26.51 全函数 store 扫描均表明 callback 只读 TAAContext；
  `prev=cur` 的推进在更上游，不是这个 draw 自己写的。因此不转发 callback 不会冻结相机历史，
  也不需要另写一份 previous VP。

### 17.6 bilinear 档的 render size fallback（编译通过、待实机）

- `taauDetour` 是唯一填 `renderWidth/Height` 的地方；Bilinear 不跑它 ⇒ SR 管线拿到 0，直接 return。
- 现在 display 由 swapchain 后缓冲 desc 填（`noteSwapChain`，走 `nr::describe`，不依赖 taau）；
  render size 从 RTV 清单取「最新的、小于 display、比例匹配的 fmt26」，latch 一次，resize 时丢弃重取。
- 日志：`[vvsr] sr: TAAU is not running; render size from the inventory = WxH`。

### 17.7 未结：SR+DLSS 的 2 px 竖缝（EXP-0023）

- 现象：**屏幕固定、贯穿画面、约 2 px、极淡（~2/255）**；只在运动时明显；SR 关没有、SR+DLSS 有；
  窗口模式与游戏 F2 截图里都有（⇒ 在后缓冲内容里）。
- 已排除：EXP-0023 的六组移动帧 dump 里，引擎颜色（mid colourhalf）、DLSS 输出（dlssout）、
  写回 final 在 65%（render x≈1099 / display x≈1666）**都没有**静态缝；MV 非零、无 NaN、无列跳变；
  深度正常（天空≈1.0、远处地形 0.9968+）。
- 现场手段已备：`vv_sr_spatial.txt`（纯空间 A/B：零 MV/jitter、每帧 Reset）、
  `vv_dump_sr.txt`（移动中 dump，含 dlssout/mv/depth）。下一步用 F2 那一帧对齐同帧 dump 再比。

### 17.8 dump 的一个坑

我们自己的纹理在写回后处于 `NON_PIXEL_SHADER_RESOURCE`。用 `DumpState::Common` 去拷会读到
stale 字节（MV 全 0、depth 全 1.0），看起来像"MV 坏了"。`dumpTextureRawAs` 必须传对应状态；
`DumpState` 里已补 `NonPixelShaderResource = 0x40`。

### 17.9 不写 percentage option 的直接控制点（2026-10-29，静态确认）

Mac 全符号库与 Windows 26.51 已逐字段对应：VV `frameImpl` 最早调用的
`createDeferredRenderingCameras`（canon: `bgfxbridge_createdeferredrenderingcameras`）收到
`shared_ptr<UpscalingData>`，函数入口读取 `mUpscalingFactor`（对象 `+0x4`），并计算：

```text
dynamic = !mEnableDynamicResolution (+0x8)
    ? 1.0
    : clamp(useOverride (+0x9) ? override (+0xC) : mDynamicResolutionScale (+0x38),
            mMinUpscalingFactor (+0x10), 1.0)
effective = mUpscalingFactor * dynamic
```

`effective` 随后交给 `camerautils::resizeCameraViewport`（canon: `camerautils_resizecameraviewport`），
直接缩放相机视口与渲染矩形。Windows 的 `frameImpl` 还在该调用之后读取同一对象的 `+0x4`，
决定上采样分支是否启用；后面调用 `taau` 时，传入的也是 renderer 持有的这同一个
`UpscalingData` 指针。⇒ 在 `createDeferredRenderingCameras` 入口、原函数之前改写共享对象的
`+0x4`，即可同时控制场景渲染尺寸、TAAU 分支和 TAAU ratio，不需要写 Option 579。
Windows 下 shared_ptr 是函数第 3 个参数（`r8`）；先解引用得到对象，再写对象 `+0x4`。

这是比 `taauDetour` 更早且语义完整的 hook 点；只在 `taauDetour` 改 factor 仍然太晚，
因为相机和帧图尺寸已经确定。资源按新尺寸重建、切场景与 `1.0` 往返尚需实机验证，
静态结论不冒充运行时通过。

**实现（2026-10-29，EXP-0089）**：`probe/VvUpscaling.cpp` 用 canon 签名 hook
`mce::framebuilder::bgfxbridge::createDeferredRenderingCameras`（win-26.51 rva
`0x8c7c2f0`，32 字节唯一签名），在 `origin` 之前从 `shared_ptr`（r8）解出对象：
写 `+0x4 = factorForPercent(sr.vvPercent)`，并把 `+0x8`（`mEnableDynamicResolution`）
置 0，避免动态分辨率把目标比例再乘一次。`vvPercent = 0`（跟随引擎）不覆盖。
面板 Apply 不再写 Option 579，玩家的 `upscaling_percentage` 保持原值；写入前校验
对象可读、`mode` 0..3、原 factor 0..4，避免 ABI 位置变化时写坏内存。
EXP-0089 实测：factor=1.0 时引擎直接停掉 TAAU/arg10，现有 arg10 路径无法在 100% 工作；
保活方案见下。

**100% 保活（EXP-0090 通过）**：`vvPercent=100` 时 hook 先写 1.0 建相机，
`origin` 后恢复 `before`（<1）或 0.6667 作为 branch，让 frameImpl 仍声明 TAAU/arg10；
`nativeRender()` 为真时 VvProbe 把 display size 当 render size，TAAU draw 仍由现有 suppress
跳过。这样 DLSS 以 1:1 写 arg10，NR 在 display 分辨率上跑。

EXP-0090 实机：`[vv] native render: camera 1.000, branch 0.5000`；
`[vvsr] sr buffers 2560x1440 -> 2560x1440`；`sr arg10` 连续 `eval=1`；退出无新 crash。
代价：NR 在 display 分辨率约 9.8 ms（1440p），帧率明显下降，这是 100% 档的固有成本。
**未单独验证**：100% → 66% 的运行时回切（这次最后是直接禁用 VV DLSS，日志里没有回切段）。

**菜单 100%（EXP-0093）**：菜单背景不走 R3 arg10 链；在菜单里应用 native 保活会把相机建在 1.0、菜单采样器仍按 <1 取，背景放大到左上角并延续进世界。修法：native 100 只在 `worldLoaded()` 时应用；菜单保持引擎自己的 factor。

**EXP-0093 更正**：只跳过菜单不够。join 事件会让 `worldLoaded()` 在进世界的加载帧就为真，而 NGX 会话/R3 管线还没 live（`session: no NGX core`、无 `sr arg10`）；TAAU 仍按 branch 0.5 采样 display 颜色，画面左上角放大且 DLSS 不启动。EXP-0094 起 native 100 只在首次 `sr arg10` 成功之后启用，首帧保持引擎 factor。

**EXP-0094 / 0095**：逐帧撤销 native 会形成反馈环：100% 帧仍持旧 1280×720 arg11，注入失败后退回 50%，成功后再进 100%；43 次尺寸交替对应用户看到的黑白闪动，最后在 MV 的 `CreateShaderResourceView` 中触发 NVIDIA 驱动空读。EXP-0095 把 live 改为首次成功后锁存到 disable/release，等待原生 MV，不再在 record 开始或单次注入失败时切回。实机通过：原生 MV 在 36 ms 后接管，`sr arg10` 连续到 #600，无尺寸振荡或新崩溃。证据：`docs/agent/evidence/vv-native-live-oscillation-EXP-0094.log`、`docs/agent/evidence/vv-native-live-latch-EXP-0095.log`。

## 18. S5：callback 级替换（EXP-0025 失败，见 §19）

### 18.1 反编译依据

- Windows 26.51：declarator RVA `0xB74F860`，draw callback RVA `0xB752820`。
  对 callback 全函数的 store 扫描显示它只读 TAAContext，写本 pass 的 bgfx view、材质 uniform
  和 draw 命令；不写相机历史、不换历史纹理、不做 barrier、不提交 command list。
- Mac 全符号：declarator `0x10B772140`、functor `0x10B774160`、
  `ConcretePass::execute 0x10B773F80`。declarator 把 `UpscalingCameraData/UpscalingData`
  拷进 TAAContext，functor 只把这些值灌进材质并录 draw。
- 因此保留 declarator、在 draw detour 快照后不转发，是候选的最小替换点；不需要切 Bilinear，
  也不需要自管 previous VP。Mac 的 `WaitOnUpscalingOutput` 提示下游知道该输出依赖，
  Windows 只写后缓冲是否足够仍是本轮实机要回答的未知，不能由静态分析提前宣称通过。

### 18.2 安全门与边界

实现位于 `probe/VvProbe.cpp` 与 `probe/VvInject.cpp`：

1. 首次点击 `SR + DLSS` 仍走原 TAAU；只有本次 DLSS 录制成功、`eval=1` 且有内容边界，
   才 arm 下一帧替换。
2. arm 后 `drawDetour` 仍快照相机，但不调用 `gOriginalDraw`；分别记录
   `declared/callbacks/forwarded/suppressed`，避免把“callback 还执行”误写成“原 draw 还执行”。
3. 原合成 draw 消失后，旧的 `draws==1` 签名不再可用。替换态改认显示分辨率、绑 DSV、
   多 draw 的 HUD list，并切在它**之前**（`via=pre-hud`）。不使用 first-back-buffer-writer
   或固定 head/tail，因为它们可能落在 HUD 后。
4. `prepare()` 以失败守卫保持默认回退；录制失败、eval 失败、mode off 或 pre-HUD 边界丢失
   都会 disarm，让下一帧恢复原 TAAU。替换帧没有可靠边界时，当前帧也不注入，避免覆盖 UI。
   **EXP-0025 实测**：这条门没有按设计生效 —— 小提交的提前返回就 disarm、进菜单后靠陈旧 ready
   继续 suppress，`via=pre-hud` 全程 0 次。两处缺陷与修法见 §19.2。

本轮只验证 callback 级替换，不顺带修改 MV、2 px 缝、颜色选择或 Bilinear fallback。
日志断言为 `scripts/agentflow/smoke/taau-callback.json`。

### 18.3 旧证据归因更正

- EXP-0013 没发生 suppression；EXP-0014 只证伪“callback 后第一次 draw”的匹配规则；
  二者没有证伪 callback 级 skip。
- EXP-0015 的 drop 与 UI 损坏相符，但 13:34 崩溃早于首次 drop；13:36 栈为
  `taauDetour → logDependencies → scanForKnownTargets → nr::describe → D3D12Core`，
  不是 drop/提交路径。不能把两次崩溃都记成 drop 导致。
- EXP-0023 没有完成空间 A/B，dump 也未与 F2 同帧对齐；2 px 缝仍不能归因为纯时间路径。

## 19. EXP-0026 计划：skip 后 picked colour 是否 stale（2026-10-26 夜）

> EXP-0025 已失败关闭（`U-taau-callback-skip`）；EXP-0026 打开：
> `U-taau-skip-colour-freshness`，attempt 1，tier L2，源码树 `31257d1ff8fe866c`。
> **本轮唯一未知**：跳过原 TAAU callback 之后，`pickInputs` 选中的那张 fmt26 颜色是否还在被写。
> 唯一判据是两批全候选 dump 的字节哈希；症状（冻结 / 闪烁）只当线索，不当证据。

### 19.1 EXP-0025 的冻结与菜单异常

时间线（`docs/agent/evidence/taau-callback-EXP-0025.log`，sha256 `a37459db…`；
源 `logs/latest.log` 21:33:40–21:34:46，构建 DLL sha 前缀 `ccff2f3093212cd0`）：

| 时刻 | 日志 | 含义 |
|---|---|---|
| 21:33:38.023 | `sr pick: render=1689x950 colourTargets=36 fmt26=15 depthTargets=9` | 颜色候选 15 张，选一张 |
| 21:33:40.117 | `[eval] f=1 … c=0x21ce74ce560 d=0x21ce74ceef0 ret=0x1` | 首帧 NGX 成功；随即 arm，首个 `suppressed=1`（.123） |
| 21:33:40.118 | `inject: back-buffer boundary split=66/69` | 切点走的仍是老的 `draws==1` 签名 |
| 21:34:12.334 | `[eval] f=3000 … ret=0x1`（此后无 eval） | 录制停了 |
| 21:34:44.126 | `declared=11428 callbacks=11428 forwarded=3028 suppressed=8400` | suppress 一路涨，forwarded 不动 |

- **已确认（原始日志）**：原 callback 被连续 suppress（`suppressed` 1→8400），同期 NGX 返回 1；
  `forwarded` 停在 3027，21:34:18 只 +1 到 3028。替换在**提交**里发生了，但世界输出不是活的替换。
- **已确认（原始日志）**：`via=pre-hud` 全程 **0 次**，替换态切点一次都没启用；
  实际用的是老 back-buffer-boundary（17 次）。本轮说的"替换"从未按设计切点跑过。
- **已确认（用户原话，摘要）**：进存档按两次 F11 → 开菜单点 `SR + DLSS` → 画面冻结，转头时**左上角区域有波动**；
  退出存档后主菜单背景在「开启 SR+DLSS 那一帧」与「退出存档那一帧」之间极快交替闪烁
  （不是叠加；单张截图抓不到两帧，视觉暂留像叠加）。
- **菜单异常的机制（部分证实）**：`eval` 在 21:34:12 停，suppression 却涨到 21:34:44 ——
  arm 用的是跨世界的陈旧 ready（见 19.2 缺陷 2）。闪烁本身与"逐帧在 suppressed/forwarded 之间交替"
  一致（`forwarded` 停住后偶发 +1），但**没有逐帧证据**，标为待验证。
- **未证实**：冻结的原因。「输入 stale」（本轮假设）与「输出/切点不对」（备用解释）都还没被区分；
  不能从"看起来冻住"推出"颜色 stale"。同理，`d` 指针在同轮 `eval` 里也没变，但**指针稳定只说明
  pick 没换面，不说明那张面的内容在更新** —— 这正是要用字节哈希的原因。

### 19.2 两个已证实的代码缺陷（本轮已改，只经构建、未部署）

**缺陷 1：小提交把安全门 disarm 了（位置错）**

- `VvInject::prepare()` 的替换失败守卫原先建在 `listCount<8` 与 queue 过滤**之前**：overlay 自己每帧
  几条单 list 提交一进来就 disarm，随后引擎帧再 arm。
- 证据：`arm1=8653 / arm0=8653`；`via=pre-hud count=0`；老 `back-buffer-boundary` 17 次。
- 修法：守卫移到 queue 过滤**之后**（`listCount>=8`、`DIRECT` 队列类型、队列身份三项都过）——
  只有引擎帧的提前返回才 disarm；`setMode(Off)` 同时立刻 disarm，Stop SR 当帧恢复原 callback。
- 顺带：替换态的切点由 probe 自己算（显示分辨率、绑 DSV、`draws>=5` 的 HUD list **之前**，`via=pre-hud`）；
  拿不到就 `SkipReason::NoBoundary`，本帧不注入 —— 宁可不动画面，也不覆盖 UI。

**缺陷 2：没有世界时的陈旧 ready（判据不足）**

- `VvSr::record()` 在 `!inWorld()` 时**不录制但返回 true**（原意：避免"回到世界后 SR 动作死掉"）。
  于是 `sr.dlssReady` 跨菜单存活，`prepare()` 只看 ready 就 arm，菜单里继续 suppress。
- 证据：`eval` 停在 21:34:12（f=3000），suppression 继续到 21:34:44 的 8400。
- 修法：arm 条件改成 `Action::Sr && view==Dlss && dlssReady && dlssEvaluateRet==1 && content && worldLoaded()`。
  `record()` 的 no-op 成功语义**保持不动** —— 改的是消费方，不是生产方。
  断言见 `scripts/agentflow/smoke/taau-callback.json`。

**安全门的新语义（审查修正，四条）**

1. pre-hud 命中改成**独立计数**的日志：
   `[vv] inject: replacement boundary split=k/N via=pre-hud hit=n missed=m`，自有计数器，前 3 次 + 每 120 次。
   原先用 `suppressed<=1` 判断"首次"是错的 —— `suppressed` 是整场累计的 draw 计数，那条线最多只打一次，
   之后再有合法的 pre-hud 边界也看不见。
2. `TaauDrawStats` 增加 `missed`：**armed 了、原 draw 也 suppress 了，但这一帧什么都没注入**
   （没有 pre-hud 边界、没有后缓冲、录制失败）。这种帧在下一帧恢复 TAAU，画面不至于冻住；
   `missed` 持续增长的含义是"suppression 跑在替换前面"，而不是"替换在生效"。周期行里也带上了：
   `[vv] taau replace declared=… callbacks=… forwarded=… suppressed=… missed=…`。
3. `missed` 只在**引擎帧**上累加：守卫先看 `replacing`（在 queue 过滤之后取的），小提交/外来队列
   根本走不到这条路径，所以它们不会把 `missed` 灌大。
4. `missed` 单独一行 warn（前 3 次 + 每 120 次）：
   `[vv] taau replacement missed=N suppressed=M (armed frame injected nothing; TAAU restored next frame)`。

两处修法只经 `xmake -y` 通过、尚未部署。**构建通过 ≠ 门修好了**，本轮实机才能回答。

### 19.3 Mac 静态资源语义：哪些资源跟 TAAU 是否执行无关

来自 Mac 全符号（静态结论，尚无 Windows 侧逐字段对齐）：

| 资源 | Mac 类型 | 谁写 |
|---|---|---|
| `InputFinalColor`、`InputBufferMotionVectors` | ExternalImage（包裹引擎的 `ResolvedTextureResource`） | TAAU **上游**，与 TAAU 跑不跑无关 |
| `InputTAAHistory`、upscaling 输出 | TransientImage（池资源） | TAAU / 池自己 |

- `pickInputs` 并**没有绑定** `InputFinalColor`：它选的是"最新创建、渲染尺寸的 fmt26 RTV"。
  EXP-0025 那帧 `colourTargets=36 / fmt26=15`，`eval` 采样 f=1..3000 的 `c=0x21ce74ce560` 一直没变。
- 两种解释都与"选了 fmt26"相容：
  (a) 选中的那张属于 TAAU 的 transient/历史侧，skip 后没人再写 → 冻结；
  (b) 选中的是上游某张，但写回或切点让画面没变成活的替换。
- 静态语义只说明「`pickInputs` 的选面规则与 `InputFinalColor` 不是同一个对象」，
  **不能**据此宣称颜色 stale —— 那正是本轮要测的。

### 19.4 本轮两批全候选 dump：文件名与判定表

两批 = 同一次运行里两次点 `Dump SR inputs`；批号是进程内计数器 `dumpNo`（`run`），从 1 起，写进每个文件名：

| 文件 | 内容 |
|---|---|
| `sr_cand_<run>_colour_<i>_p<PTR>_<W>x<H>_fmt26.bin` | 第 `run` 批第 `i` 个 fmt26 颜色候选（`tolerance=64`，与 pick 同源，最多 16 个） |
| `sr_cand_<run>_depth_<i>_p<PTR>_<W>x<H>_fmt<fmt>.bin` | 第 `run` 批第 `i` 个深度候选（`tolerance=64`，最多 16 个，格式集与 pick 一致） |
| `sr_colour_<run>_p<PTR>_<W>x<H>_fmt<fmt>.bin` | 该批 `pickInputs` 实际选中的颜色 |
| `sr_depth_<run>_p<PTR>_<W>x<H>_fmt<fmt>.bin` | 该批实际选中的深度 |
| `sr_mid_colourhalf_<run>.bin`、`sr_final_rgba8_<run>.bin` | 引擎侧半分辨率颜色、写回后的最终画面 |
| `sr_mv_rg32_<W>x<H>_<run>.bin`、`sr_depth_r32_<W>x<H>_<run>.bin`、`sr_dlssout_<W>x<H>_<run>.bin` | MV、`R32` 深度、DLSS 输出 |

- 批号在**每个**名字里：上一版 picked 那一对没带号，第二批会覆盖第一批（本轮已修）。
  批号是进程内计数，**重启游戏会回到 1**，所以两批必须来自同一次运行。
- **配对键是 `p<PTR>` + 尺寸，不是索引 `i`**（`i` 只是当批的枚举顺序，下一帧可能重排）。
- 候选批用的是 **pick 自己的 `tolerance=64`**（上一版用 2：几张只差几个像素的候选会被排除，
  连 picked 本身都可能不在批里）。同源之后 picked 一定在批内，日志同时给出下标 `idx=…`；
  `idx=-1` 只可能是撞上 16 个上限、或 record 与 dump 之间清单变了 —— 这时读作"这一批不能比"，
  不是"颜色 stale"。
- 候选名里带各自的真实 `<W>x<H>`：resize 在途时会出现几张不是 latch 尺寸的候选，按指针+尺寸配对即可。
- 注意旧产物：游戏根里 10-26 17:xx–18:xx 那批 picked 用的是旧命名 `sr_colour_<PTR>_fmt26_<W>x<H>.bin`
  （无批号），跟本轮 `sr_colour_<run>_p<PTR>_…` 并存；辅助名（`sr_final_rgba8_<n>.bin` 等）新旧相同，
  只能靠时间戳区分。比哈希只认本轮两种新名。
- 目录是游戏根（`utils::applicationDirectory()`）。`bin` 不进 git，账本只记文件名 + sha256：

```bash
cd "E:/LeviLauncher/versions/1.26.51.01-agent2"
sha256sum sr_cand_*_colour_* sr_cand_*_depth_* sr_colour_* sr_depth_*
```

先读这几行日志，确认这一批不是空采集（`N` = 批号，`F` = 这批读到的那次录制的 in-world SR 帧号）：

```
[vvsr] sr dump #N frame=F start
[vvsr] sr dump #N inputs colour=… depth=… mid=… final=… mv=… depth32=… dlss=…
[vvsr] sr dump #N cand summary colour ok=x/C depth ok=y/D pickedColour=0x… idx=… pickedDepth=0x… idx=…
```

**比哈希之前先比 `frame=`：两批的 `F` 必须不同。** `F` 相同说明第二次 dump 读的还是同一次录制
（没进世界 / record 没推进 / 同一个 present），此时颜色哈希相等是构造出来的，什么都证明不了。
这一步不过就不用往下比。

判定表（同一指针跨两批比；`hash1 == hash2` 表示两批字节相同 = 这张面在两批之间没有新内容）：

| 观察 | 判定 | 下一步 |
|---|---|---|
| 两批 `frame=` 相同 / `idx=-1` / `cand summary ok=0/C` | **采集无效**：同一帧重复 dump、picked 不在批内（16 上限或清单变了）、或整批拷贝失败 | 不判定；重采两批，先确认 `frame` 递增、`idx>=0`、`ok` 非零 |
| picked colour 同哈希 **且** picked depth 不同哈希 | **假设成立**：skip 后选中颜色冻结、深度仍新鲜 ⇒ 选面语义错（选了 TAAU 独占写的面） | 改颜色来源（绑上游 `InputFinalColor`，或只在 TAAU 写面的下一帧 skip），再单独验证 |
| picked colour 不同哈希 | **假设证伪**：颜色是新鲜的，冻结另有原因 | 转查输出路径：写回是否被后缓冲覆盖、pre-hud 切点是否真在 DLSS 之后、`dlssout` 是否在变 |
| picked colour 同哈希 **且** picked depth 同哈希 | **证据不足**，不是通过 | 先证转视角确实发生（`[vvsr] mv snapshot … previous VP differs`、`sr_mv` 非零）再重采；不猜下一条 |
| 所有 fmt26 候选两批全同哈希 | 上游整帧没写任何 fmt26（不只是选中那张） | 比第二行更强：选面必然错，优先修来源 |
| 只有 picked 那张同哈希、其他候选有变化 | 选面错误（选了不再更新的那张），不是上游停写 | 修候选规则（新→旧、外部 vs 池），复核挑选日志 |
| 两批的指针集合不同（出现新 `p<PTR>`） | 分配在变，按指针配对失效 | 本表结论降级为参考；缩短两批间隔或改按 (尺寸, 格式, 序号) 对齐后再判 |

`reset`、`jitter`、`MV` 都不在本轮判据里，只能从 `sr_mv` / `sr_dlssout` 取旁证。

### 19.5 本轮不宣称

- **颜色 stale 尚未证实**。19.4 的表才是判据；"冻结""闪烁"只是症状，不能当结论。
- **pre-hud 切点机制成立，但输出未证**。EXP-0025 的 `via=pre-hud` 是 0；EXP-0026 已连续命中
  （`hit=1..3360`，`missed=0`），说明切点本身可用，但这不等于世界输出是活的替换。
- **不宣称画质通过**。F2（无整体偏色）与 §17.7 的 2 px 缝都不在本轮范围；用户说"看不出区别"也不能当验收。
- **不宣称 callback 级替换不可行**。EXP-0025 只证明当前实现失败：门是错的、切点没启用、输入新鲜度未知。
- **不宣称已解决 §16 第 1 条**（回主菜单背景被覆盖）。本轮只是把"菜单里继续 suppress"这个门缺陷修掉，
  颜色是否跟着引擎声明走仍未回答。
- 落到"证据不足"行时按 AGENTS.md §2 停手补证据，不猜下一版；下一轮实验必须带新的原始产物。

## 20. EXP-0027：运动窗口里的近 stale 判定（2026-10-26 夜）

> EXP-0026 已失败关闭（`U-taau-skip-colour-freshness` 的第 1 次尝试）；EXP-0027 打开：
> attempt 2，tier L2，源码树 `fcc3a7ba62fb2320`。相对 EXP-0026 只加了 F7 双采与逐批运动日志，
> 选面/替换逻辑未再改。

### 20.1 EXP-0026 为什么算失败、又留下了什么

三批采集都有效（`docs/agent/evidence/colour-freshness-EXP-0026.log`，sha256 `d2b41106…`，
DLL 前缀 `c9145929d6053820`）：`frame=774/2098/2722` 递增，每批 `colour ok=15/15 depth ok=9/9`，
picked `idx=0` 且三次同指针（`colour 0x29717e82680` / `depth 0x29716e43380`），
采集窗口内 `pre-hud hit=1..3360 missed=0`。

| 对比 | picked colour | picked depth |
|---|---|---|
| 1↔2 | 6052 / 6566276 B（0.09%） | 18.52% |
| 1↔3 | 6077 B（0.09%） | 20.19% |
| **2↔3** | **25 B（0.0004%）** | **19.80%** |

- **按预注册的字面判据 FAIL**：三批 picked colour 哈希互不相同，所以"完全停止更新 / 哈希相等"被证伪。
- **但等值判据本身太脆**：2↔3 只有 25 字节差异，深度却变了 19.8%，其他活跃颜色候选变 9–96%
  （候选 5..14、深度 3..8 静止）。近冻结是真信号，不能改写成 pass，也不能因为"哈希不等"就宣布颜色新鲜。
- **run3 缺逐批 VP 证据**：菜单会吞掉移动输入（用户原话：无法在菜单打开时移动），其中一次还是完全静止，
  所以三批里没有一批能独立证明"画面确实在动"。
- 结论：§19.4 的等值判定表自此被相对阈值取代 —— 稀疏写入下 25 B 就能让哈希不等。

### 20.2 本轮操作与逐批日志

F7（TEST 热键，present 线程轮询，边沿触发；一对还在跑时拒绝并 `F7 ignored`；不占用 F8/F10/V 的采样）：

- 按 F7 后 **+2 s 第一批**，**再 +2 s 第二批**；两批文件名仍带批号 `run`，与 §19.4 同规则。
- 菜单保持关闭，整个 ~4 s 窗口内持续晃头。用户只负责按一下 F7、晃头、报"好了/异常"。
- 每批一行运动状态（无节流，每批都有）：
  `[vvsr] sr dump #N frame=F mv valid=V prev_same=P delta=(dx,dy,dz) jitter=(jx,jy)`。
  `prev_same=1` = 两帧 VP 相同（没动或快照没取到），这一批不能当运动证据。
- 采集有效性仍按 §19.4：`frame=` 两批必须不同、`idx>=0`、`ok` 非零；`pair 1/2`、`pair 2/2` 两行给出 `run=` 与 `ok=`。

### 20.3 预注册判据（相对阈值，先写死再跑）

变化率口径与 EXP-0026 相同：逐字节不同数 / 文件字节数。

| 条件 | 判定 |
|---|---|
| 两批都 `mv valid=1` **且** `prev_same=0`（`delta` 非零），`frame` 不同 | **前提成立**，否则本表其余行不适用 |
| picked colour 变化 **≤ 0.01%**，且至少一个其他颜色候选 **≥ 1%** | **支持近 stale / 选面语义错**：选中的面几乎不随画面更新 |
| picked colour 变化 **≥ 1%** | **证伪**：近冻结假设不成立，颜色是新鲜的，冻结另有原因 |
| 缺 VP 运动（`prev_same=1` / `valid=0` 且 delta 为零）、dump 无效、`frame` 相同 | **无效**：不记 pass 也不记 fail，不猜下一条 |

- 0.01% 与 1% 是**跑之前**定的相对阈值，不是事后按观测挑的；落在 0.01%–1% 之间按"未定论"处理，
  只能说"比 EXP-0026 更接近冻结/更远离冻结"，不许改阈值重算。
- 仍**不宣称颜色 stale 已证实**：本表只区分"近 stale"与"新鲜"，不等同于证明资源归属；
  §19.5 的边界（pre-hud 未实机验证、不宣称画质、不宣称替换不可行）继续有效。
- 证据落点：游戏根 `sr_cand_<run>_*` 新两批 + `docs/agent/evidence/colour-freshness-motion-EXP-0027.log`。

## 21. EXP-0028：InputFinalColor 里有没有原生资源（2026-10-26 深夜）

> EXP-0027 已失败关闭（`U-taau-skip-colour-freshness` 第 2 次尝试）；EXP-0028 打开：
> `U-taau-color-dep-native`，attempt 1，tier L2，源码树 `24799b2f7f0cfe69`。
> 本轮是**只读身份实验**：不改选面、不改 DLSS、不改 TAAU suppress、不碰任何 GPU 资源。

### 21.1 EXP-0027 与离线内容审查的结论

- EXP-0027 两批 VP 均有效、`prev_same=0`、`delta` 非零，picked colour 变化 **2.5639% ≥ 1%**，
  按预注册判据**近冻结假设证伪**（其他候选 2.85–92.37%，深度 29.07%）。
- 离线解析同一轮的全候选内容（`_local/scratch/colour_sem/`）：picked `idx0` 是**完整的渲染分辨率世界图**，
  `idx1` 是同一张图换增益；`idx4` 是 HDR 天空/远景层，地形被裁掉。**不支持盲换 idx4** ——
  B1c 已表明颜色缓冲里已经做过 tone mapping，"HDR 值更大"不是"更完整"。
- 同一轮 run1/run2 的 picked 图实测**零像素位移**，所以 2.56% 的字节变化不能当作"语义新鲜"的证据。
- 旧 DFS 依赖扫描不是"不存在原生资源"的证明：全局预算会被模块指针吃掉；运行时对象只能靠 vtable
  区分 ExternalImage（颜色/MV）与 TransientImage（历史/输出）。语义来源本来就在 taau arg10，
  缺的只是**依赖 → 原生资源的身份**。

### 21.2 本轮扫描（有界、扁平、只读）

`decl` 侧只在 replacement 已 arm 时扫；`draw` 侧读的是上一个 armed declarator 存下的 dep，
在 suppress/forward 判定**之前**执行 —— 原 callback 跑不跑都要读这一份。两侧都只听前几次调用，
之后对象图就是同一张。

- **两个 phase，各自 3 次调用预算**：`decl`（declarator，声明阶段）与 `draw`（draw callback，引擎已解析的阶段）。
  分开计数，避免 declarator 把预算花光、draw 一次都跑不到。
- **四个固定区，全部扁平 qword**：`dep` 0x40、`node`(`dep+8`) 0x200、`dep16`(`dep+16`) 0x200、
  `node16`(`node+16`) 0x200；每个区整块 `readable()` 通过才读，不递归、不做 DFS、没有预算概念。
- 每个 qword **按值**与当帧 `renderTargets()`/`depthTargets()`（tolerance 64，与 pick 同源）以及
  `gSeen`（`createRtvDetour` 同一渲染线程追加的原始清单）比较。
- 未命中的值**永不**被解引用、`describe()` 或交给虚调用 —— 指针过期只花一次比较。

日志（`phase` 取值 `decl`/`draw`）：

```
[vv] in.color nativeHIT phase=… cand=colour|depth|seen idx=… ptr=… at=<region>+<off> fmt=…
[vv] in.color nativeSCAN|nativeNONE phase=… dep=… node=… dep16=… node16=… scanned=… candidates=… hits=… call=i/3 seq=…
```

每个区的大小也打进那行（`none` = 不存在或不可读），这样"四个区确实都走过了"能从日志读出来，
而不是靠"没有命中"去推断。

### 21.3 判据与边界

| 观察 | 判定 | 下一步 |
|---|---|---|
| 任一 phase 出现 `nativeHIT` | **身份成立**：InputFinalColor 依赖里带着原生 RTV/DSV/`gSeen` 指针（日志给出候选 index 与依赖偏移） | 做**语义绑定**：把该候选当作颜色来源，而不是"最新 fmt26"；绑定后仍需单独证明它在 skip 后是新鲜的 |
| `draw` phase 四区**完整扫描**后 `nativeNONE` | 这一跳不存在 | 转向 `mce::Texture` / bgfx 解析，**不再手摇候选、不再调阈值** |
| 只有 `decl` 是 `nativeNONE` | **不算否定** | 该指针可能只在执行期才填；以 `draw` 的结果为准 |

- "四区完整扫描"指那行里四个尺寸都不是 `none` 且 `scanned` 非零；缺区或不可读时只能记"未覆盖"。
- 本轮只解决**身份**，不解决**新鲜度**：命中不等于颜色在 skip 后仍被写。
  §20.3 的 0.01%/1% 阈值与 §19.4 的哈希表都不在本轮复用。
- §19.5 的其余边界继续有效：不宣称画质，也不宣称 callback 级替换可行或不可行。
  pre-hud 切点在 EXP-0026 实机已连续命中（`hit=1..3360 missed=0`），但那是切点生效，不是"世界输出是活的替换"。
- 证据落点：`docs/agent/evidence/color-dep-native-EXP-0028.log` 里的 `nativeHIT` / `nativeNONE` 原始行。

## 22. EXP-0029：bgfx handle → native 映射（2026-10-27 凌晨）

> EXP-0028 失败关闭（`U-taau-color-dep-native`）；EXP-0029 打开：`U-bgfx-handle-native`，
> attempt 1，tier L2，源码树 `96d88e3a3f4da255`。
> 本轮仍是观察实验：不改选面、不改 DLSS、不改 suppress，只记录 handle→native 映射与它和 RTV 清单的交集。

### 22.1 为什么直接指针扫描必然 none

- EXP-0028 的 `nativeNONE` 不是"扫得不够"：`draw` 第 2 次已是预注册的完整样本 ——
  四个区、200 qword、150 个候选，0 命中（另两次 `scanned=8` / `136` 是部分区不可读，不削弱这个完整负样本）。
- Mac 全符号给出了真实身份：`InputFinalColor` → `ResolvedTextureResource`，其 **+0 是一个 uint16 的 bgfx
  `TextureHandle.idx`**（`0xFFFF` = 无效）。所以依赖里根本不会出现 `ID3D12Resource*`，按值比指针必然全 none。
- Windows 1.26.51 静态定位（ImageBase `0x140000000`）：
  `createTexture`/install RVA **`0xDF9D680`**（5 参：`ctx, uint16 handle, unknown, void* native, void* texRef`，
  第 5 参必须原样转发）；`destroyTexture` RVA **`0xDF9EE00`**（`ctx, uint16 handle`）；
  renderer `m_textures` 在 `+0xBDE170`，entry 0xB8，**entry+0 即 native**。
  `handle<=0xFFF` 时可直接读表；`getInternal` 自身会写保护字节，所以本实验不走它。

### 22.2 运行时安装（指纹校验后才 patch）

- 用项目的 `LL_STATIC_HOOK`（AGENTS §7 的宏，**不手写 MinHook**）；`hook()` 只在指纹通过后调用。
- 指纹：install 41 字节、destroy 20 字节的前缀逐字节比对。不匹配 ⇒ 打 error 并保持**未安装**，
  probe 其余部分照常工作（换了构建不会半挂）。
- 表是 `handle → native` 的定长数组（`0xFFFF` 无效），install 写入、destroy 清 `nullptr`；
  `atomic` 每槽一个，install/destroy 可在不同引擎线程调用。
- 同时统计 install 的**上下文个数**（最多记 4 个）：>1 说明句柄可能来自另一张表，命中就不能算干净。
- F11 的作用：mod 加载后引擎会重建渲染资源，只有按过 F11，install hook 才看得到这批新资源。

### 22.3 采样与日志

三个相位，各自 3 次预算：`decl`（armed declarator）、`draw`（armed draw callback）、
**Stop 后 forwarded**（`armed=0 forwarded=1`，预算在 armed→disarmed 时补），所以能回答
"Stop 前就能解析，还是 suppress 改变了依赖带的东西"。

```
[vv] in.color handle=0xHHHH valid=… mapped=… phase=… armed=… forwarded=… decl=… res0=… res8=… native=… dep=… res=… candidates=… call=i/3 seq=… sets=… destroys=… nonNull=… contexts=… hooked=…
[vv] in.color bgfxHIT cand=colour idx=… idxRtv=… idx26=… ptr=… w=… h=… fmt=… at=handle handle=0xHHHH phase=… ambiguous=…
[vv] in.color bgfxNONE reason=invalidHandle|notMapped|notInRtv handle=0xHHHH res=… native=… at=handle phase=… contexts=… sets=… destroys=… nonNull=…
```

（上面三条各自是一整行，这里按字段换行只为可读。）

- HIT 的判据是**映射出来的 native 指针命中当帧 `renderTargets()`（tolerance 64）里的某一项**，
  并且必须同时满足 `ambiguous=0` 且带出 `idx26`（在 fmt26 候选里的序号，与 `sr_cand` dump 同序）、
  `w/h/fmt`。
- **`valid=1` / `mapped=1` 只是诊断，不是证据**：读得出、映射得到，仍可能指向的不是 SR 实际读的那张颜色。
- `bgfxNONE` 的 `reason` 直接决定下一步：`invalidHandle` → 布局/偏移复核；`notMapped` → 安装覆盖率
  （F11、安装时机、上下文数）；`notInRtv` → 清单口径复核（是否只认 RTV、tolerance、尺寸）。

### 22.4 判据与边界

| 观察 | 判定 | 下一步 |
|---|---|---|
| `bgfxHIT` 且 `ambiguous=0`、`idx26`/`w/h/fmt` 齐全 | **handle 桥成立**：`InputFinalColor` 的 u16 handle 能解析到当帧某张颜色候选 | 用该候选做语义绑定；绑定后仍需单独证明 skip 后新鲜 |
| `bgfxHIT` 但 `ambiguous=1` | 句柄有多张来源表，命中不唯一 | 先定位上下文，不据此绑定 |
| `valid=1 mapped=1` 但 `notInRtv` | 映射本身通，指向的不在当帧清单 | 复核清单口径与时机，不换候选 |
| 三个相位都 `notMapped` / `invalidHandle` | 这一跳在当前构建上不成立 | 按 `reason` 转布局复核或退出该路线，不手摇候选 |

- 本轮只回答**身份**，不回答**新鲜度**，也不解决 EXP-0025 以来的画面冻结；
  §20.3 的相对阈值、§19.4 的哈希表都不复用，§19.5 的边界继续有效。
- 证据落点：`docs/agent/evidence/bgfx-handle-native-EXP-0029.log` 里的 `bgfxHIT` / `bgfxNONE` 原始行。

## 23. EXP-0030：直接读取引擎纹理表（2026-10-27）

### 23.1 EXP-0029 事后结论

EXP-0029 在账本里先以“未跑”取消，随后用户按同一 runbook 完成了实跑；取消记录不改写，
本轮证据以 `bgfx-handle-native-EXP-0029.log` 与 `…-postmortem.log` 为准。

- 三个相位都没有 `bgfxHIT`；`sets=0 / nonNull=0 / contexts=0`，而 destroy 超过 2000 次。
  离线反汇编确认这不是 hook 安装失败：`0xDF9D680` 只把调用方已有的 native 指针装进
  `entry+0`，并设置“外部持有”位，是 adopt/wrap 路径，不是常规纹理创建。真正的创建函数是
  `0xDFA4EA0`。因此旧影子表必然为空，不能据此否定 handle 布局。
- 首次点击 `SR + DLSS` 的崩溃与句柄实验无关。`minidump_2026-10-27_10-06-01.dmp` 给出
  `0xC0000005`，异常位于 `_nvngx.dll+0x67979`，异常参数含 `0x1968022DD8`；原始日志同一时刻
  把 `FeatureCommonInfo*` 打印成了 `api=0x68022dd8`。`RenderProbe.cpp` 的
  `Init_ProjectID` detour 参数顺序已改为与 `VvDlssSr.cpp` 一致的 `device, apiVersion, info`。

### 23.2 直接读表的静态布局

不再维护 install/destroy 影子映射，也不新增 hook。以下布局均来自 26.51 exe 的离线反汇编：

| 项 | 值 |
|---|---|
| renderer 单例发布指令 | RVA `0xDFA4214`，指纹 `48 89 3D 45 1E 06 04` |
| renderer 单例全局 | RVA `0x12006060`，运行时 `ctx = *(base + rva)` |
| 内联 entries | `ctx+0xBDE170`，`0x1000` 个，每个 `0xB8` 字节 |
| 分页目录 / 已用字节 | `ctx+0xC96170` / `ctx+0xC96178` |
| 分页块 | 每块 `0xB8000` 字节 entries，下一块指针在 `+0xB8000` |
| native | `entry+0`；handle `<0x1000` 走内联，否则走分页 |

所有读取都经 `readable()/peek()` 守卫；不解引用未检查指针、不做虚调用、不写引擎内存。
`0xDFC2830` 的 reserve 反汇编确认容量按完整 handle 的 `handle*0xB8` 口径比较。

### 23.3 本轮判据

唯一未知仍是 `U-bgfx-handle-native`：依赖里的 u16 是否能通过引擎原表命中当帧某个
`fmt26` 颜色候选。选面、DLSS 求值和 suppress 策略均不改。

- 通过：`bgfxHIT` 给出 `idx26/w/h/fmt`，且 native 等于当帧颜色 RTV。
- 失败按原始日志分流：`noContext`（单例/指纹）、`outOfRange`（不是本表下标或容量口径不符）、
  `noDirectory/noBlock`（分页链）、`notMapped`（entry 为空）、`notInRtv`（有 native 但不在颜色清单）、
  `invalidHandle`（依赖提取链）。不再靠新增 hook 或手摇候选续试。
- 主日志行另带 `ctx/cap/dir/block/entry`，失败可以定位到具体一跳；证据计划为
  `docs/agent/evidence/bgfx-handle-direct-EXP-0030.log`。

### 23.4 EXP-0030 结果

2026-10-27 13:59 实跑：用户原话“好了，没有崩溃，DLSSSR有效果”。原始日志确认
`Init_ProjectID` / `CreateFeature` 返回 1，连续 1800 帧 `eval=1`，`suppressed=1800 missed=0`；
这验证了崩溃修复和本次运行稳定性，不作为画质或最终替换验收。

句柄桥未通过：decl 的 `res0=0x00007ff6157294b0` 对应 exe RVA `0xE9394B0`，是 vtable；
`0x94B0` 只是它的低 16 位。三次 decl 均为 `outOfRange`，没有 `bgfxHIT`。
证据：`docs/agent/evidence/bgfx-handle-direct-EXP-0030.log`。

## 24. EXP-0031：ResolvedTextureResource 的真实一跳（2026-10-27）

### 24.1 Windows 对象布局

两条独立离线分析（vtable 构造路径与 taau 消费路径）得到同一布局：

```
dep+0x08       -> wrapper（0x20 字节）
wrapper+0x00   -> 主 vtable，RVA 0xE9394B0（colour/motion）
wrapper+0x08   -> 第二基类 vtable，RVA 0xE775460
wrapper+0x10   -> holder（shared_ptr raw payload，不是纹理资源本身）
wrapper+0x18   -> shared_ptr 控制块
holder+0x18    -> ResolvedTextureResource*（0x98 字节）
resource+0x00  -> uint16 bgfx TextureHandle.idx（0xFFFF 无效）
```

- 包装对象 ctor `0x533F230` 在 `0x533F2A7` 写 holder、在 `0x533F2AB` 写控制块；
  deleting dtor `0x533F3D0` 的对象大小为 0x20。
- 资源创建路径 `0x5342C80` 清零 holder 的前两个 qword，并在 `0x5342DA2` 把真正的
  `resource+0` 初始化为 `0xFFFF`，与 Mac 全符号的 `TextureHandle.idx` 布局一致。
- draw/forwarded 中旧依赖已失效，甚至读到函数机器码；只在 armed declarator 采样。

### 24.2 EXP-0031 结果

三次 decl 都读到 holder 的零字段，误得到 `handle=0`；它解析到同一个非当帧 RTV 的
native，全部为 `notInRtv`，没有 `bgfxHIT`。`holder = ctrl+0x10`、`holder+0x18` 为资源指针
这一关系与 `0x5342C80` 的构造序列完全吻合。DLSS 连续 600 帧 `eval=1`，
`suppressed=600 missed=0`。证据：`docs/agent/evidence/resolved-color-handle-EXP-0031.log`。

## 25. EXP-0032：holder 后的资源与解析时机（2026-10-27）

`0xB715060` 给出了读写两侧的闭环：`0xB715074` 取 `holder+0x18`，
`0xB7150AA` 读取资源 `+0` 并只拒绝 `0xFFFF`，`0xB715173` 把新分配的 handle 写回资源 `+0`。
同族写入点还有 `0xB71455C / 0xB71460E / 0xB714965 / 0xB714A15 / 0xB772737 / 0xB788B89`。
因此 0 是合法 handle，`0xFFFF` 才是未解析哨兵。

本轮链为 `dep+8 → wrapper+0x10(holder) → holder+0x18(resource) → resource+0(handle)`，
并在原 declarator 前后各采样三次：

- `decl-pre` / `decl-post` 任一出现 `bgfxHIT`，则身份桥成立；前后差异用于判定解析时机。
- `invalidHolder / invalidResource / invalidHandle` 或原表 reason 指示失败一跳；
  不再增加偏移候选，不改容量、选面、DLSS 或 suppress。
- 证据计划：`docs/agent/evidence/resolved-color-handle-EXP-0032.log`。

### 25.1 EXP-0032 结果与首次崩溃

第二次完成 runbook 时，`decl-pre/post` 一致读到交替的 `0x017B/0x017C`，解析出的 native
分别为 `0x29a13fb45e0 / 0x29c18efbd60`。两者都在完整 RTV 清单中，且都是
**2560×1440 fmt26**，但不在 render-size 候选集合内，所以预注册的 `bgfxHIT` 窄判据未满足。
身份链已经由原始日志连通；接下来必须先验证内容与有效区域，不能把显示尺寸资源直接喂给 DLSS。
证据：`docs/agent/evidence/resolved-color-handle-EXP-0032.log`。

15:02 第一次运行的 dump 是另一处旧诊断缺陷：`logDependencies → scanForKnownTargets →
nr::describe → D3D12Core`，对失效裸指针取描述时 AV。与 NGX 无关。已删除这两个不再需要的
递归依赖扫描函数及调用；不能再用 `readable()` 为 COM 虚调用背书。
证据：`docs/agent/evidence/crash-EXP-0032-first.log`。

## 26. EXP-0033：InputFinalColor 的内容与有效区域（2026-10-27）

唯一未知 `U-input-final-color-content`：显示尺寸的语义 native 是当前世界图的哪一种存储，
是 render-size 有效视口还是完整显示尺寸图像，与 picked colour / final 如何对齐。

- `VvProbe` 保存最近 declarator 解析出的 `InputFinalColor` native；`VvSr::dumpInputs()` 同批
  额外写 `sr_semantic_<run>_p<PTR>.bin`，不改变 DLSS 选面、求值或 suppress。
- runbook 在 SR 开启后点一次 `Dump SR inputs`，与现有 `sr_colour_*`、`sr_final_*`、
  全候选 dump 对齐。日志给出 `semantic colour=<ptr> ok=<0|1>`。
- 通过要求 semantic 拷贝成功，离线内容对齐能确认它是当前世界图并明确有效区域/缩放关系；
  全黑、拷贝失败或与当前世界不对应都不可绑定。
- 本轮不验证 skip 后新鲜度，不宣称画质或最终替换通过。

### 26.1 EXP-0033 结果

用户多点了一次 dump，两批均有效：`frame=756 / 1038`，semantic、picked、final 全部 `ok=1`，
无需重测。semantic 是完整世界图；resize 后与 picked 的 log-luma 相关系数 **0.997990**、
logRMSE **0.006847**。两批 semantic 的 SHA-256 都是 `9076d54c…`，picked 都是 `d6fc8cd8…`，
且 `prev_same=1`，所以只确认内容角色，不证明运动新鲜度。
证据：`docs/agent/evidence/input-final-color-content-EXP-0033.log` 与同批游戏根 bin。

## 27. EXP-0034：语义颜色的 skip 新鲜度（2026-10-27）

这是 `U-taau-skip-colour-freshness` 第 3 次尝试。与 EXP-0026/27 不同，颜色不再靠
“最新 render-size fmt26”挑选：每个 armed declarator 都通过 §25 的链更新 native，
`VvSr::pickInputs()` 优先绑定它；prep 仍采样成 render-size `colorHalf`，深度、MV、边界和
suppress 逻辑不变。旧 RTV 选面只作无语义资源时的回退。

F7 双采的判据：两批 frame 不同且 VP 运动有效，semantic 字节变化率 **≥1%**，
同时 `eval=1`、`missed=0`。semantic ≤0.01% 而 final/其他输入变化则为 stale 或绑定失败。
证据为两批 `sr_semantic_* / sr_colour_* / sr_final_*` 与
`docs/agent/evidence/input-final-color-freshness-EXP-0034.log`。

### 27.1 EXP-0034 结果与参数序更正

最后一次 F7 双采 `prev_same=0`、`frame=698/894`，semantic 与 colorHalf 跨批均 **0%**，
final 变化 **27.0213%**，新鲜度失败；`eval=1`、`suppressed=1200 missed=0` 只说明求值和门在跑。
放大是 prep 的 `Color.Load(id.xy)` 把 2560×1440 源直接裁成左上 1689×950。
该 semantic 偏好已撤回。

进一步静态复核 declarator 的四个材质绑定项，确认 Windows 的顺序是
`arg9 InputFinalColor / arg10 Upscaling / arg11 MotionVectors / arg12 TAAHistory`，
不是 Mac 的顺序。因此 §22–§27 的 arg10 实际为显示尺寸上采样目标，冻结与 `notInRtv`
都与这个角色一致。EXP-0034 同批旧 `colour[0]` 变化 **10.774%**，是新鲜回退。

## 28. EXP-0035：arg9 FinalColor 绑定与新鲜度（2026-10-27）

唯一未知 `U-arg9-final-color-freshness`：arg9 是否为 render-size 上游颜色，并在 skip 后持续更新。

- 已验证的 wrapper → holder → resource 链不变，起点改为 dep0；每帧读取 `resource+8/+0xC/+0x10`
  的宽、高、格式。仅当 `w/h == 当前 render size && fmt==26` 才优先绑定，否则回退旧 pick。
- 因尺寸门只接受 render-size，prep 的 `Color.Load` 不会再裁显示尺寸资源；不顺便改 shader。
- F7 双采要求 VP 运动有效、semantic 变化率 ≥1%、`eval=1`、`missed=0`，并无放大/冻结异常。
  尺寸不匹配或 semantic ≤0.01% 都失败，不回到 arg10。
- 证据计划：`docs/agent/evidence/arg9-final-color-freshness-EXP-0035.log` 与同批 bin。

### 28.1 EXP-0035 结果与 ExternalImage 结构

放大消失，但三次 decl 都是 `holder=0 / invalidHolder`，semantic `ok=0`，实际仍走旧 pick。
arg9 的 0x28 字节声明对象只是 `{u16 w, u16 h, u32 fmt, …}` 附件描述：构造点
`0x5266BD0/D7/BDE` 写入 1689/950/26，内部没有资源指针，不能套 arg10 的 shared holder 链。
旧 colour0 跨批仅变化 0.005726%，colorHalf 0.002793%，final 17.0097%，冻结依旧。
证据：`docs/agent/evidence/arg9-final-color-freshness-EXP-0035.log`。

后续复核确认 `0xB71D820` 是 **ClusterLights**，不是 TAAU。它的
`record+0x10 → ExternalImage+0x18 → resource+0(handle)` 结构不能移植到 TAAU；
其记录向量属于 `*(exec_context+8)+0x50`，不属于 TAAContext。

## 29. EXP-0036：从 live TAA context 取 FinalColor（2026-10-27）

本轮把上述 ClusterLights 记录格式错误地用于 `third+0x50/+0x58`，三次均
`contextNONE=noRecords`，semantic 为 0，仍走旧 pick。TAAContext 实际只有参数块与一个
可能为空的纹理槽；四个材质覆盖写在 declarator 的 override 向量，不在 third。
证据：`docs/agent/evidence/taau-context-final-color-EXP-0036.log`。

## 30. EXP-0037：直接观察 bgfx setTexture（2026-10-27）

唯一未知 `U-taau-settexture-final-color`：TAAU 最终纹理绑定中是否存在 render-size、
skip 后仍新鲜的 FinalColor。

- 在 `0xB732500` 用 `LL_STATIC_HOOK` 观察并原样转发。反汇编确认参数为
  `(encoder, dx=handle, r8d=stage, r9=flags)`；只在原 TAAU draw 的线程窗口内检查。
- 对每个 handle 查引擎 `m_textures`，native 再与当前 render-size RTV 清单求交集；
  命中 fmt26 才作为 semantic 输入，日志记录 handle/stage/native/match。
- F7 判据不变：VP 运动有效、semantic ≥1%、`eval=1`、`missed=0`，无冻结/放大。
- 证据计划：`docs/agent/evidence/taau-settexture-final-color-EXP-0037.log` 与同批 bin。

### 30.1 EXP-0037 结果与真实材质路径

`0xB732500` 实际编码命令 0x13（update 类），TAAU 原 draw 中一次都未调用，semantic 为 0。
TAAU 每帧只调用一次材质编码器 `0xDFF5480`；其 tag-3 消费函数 `0xDFF64E0` 在
`0xDFF6510` 直接读取第四参 `resource+0` 的 handle。六参 ABI 已确认，返回 bool。
证据：`docs/agent/evidence/taau-settexture-final-color-EXP-0037.log`。

## 31. EXP-0038：材质 tag-3 的 FinalColor（2026-10-27）

唯一未知 `U-taau-material-final-color`：这个真实消费点给出的 render-size native 是否在
skip 后仍新鲜。观察点改为 `0xDFF64E0`，六个参数全部原样转发，只在原 TAAU draw 窗口内
读取 `resource+0`，再按 §30 的 RTV 交集和尺寸门绑定。F7 判据不变。
证据计划：`docs/agent/evidence/taau-material-final-color-EXP-0038.log` 与同批 bin。

### 31.1 EXP-0038 结果

材质语义绑定成功：arg9 `1689×950 fmt26`，F7 两批 `prev_same=0`，semantic 变化
**95.5703%**、colorHalf 69.5155%、final 74.3464%；冻结消失且无放大。停止时有一次
`missed=1`，所以严格预注册判据记 fail，但输入 stale 已被排除。
证据：`docs/agent/evidence/taau-material-final-color-EXP-0038.log`。

## 32. EXP-0039/0040：左上竖缝与天空重投影（2026-10-27）

EXP-0039 的纯空间 A/B（零 MV/jitter、每帧 Reset）使左上波动消失，证明异常在时间重投影，
不在 FinalColor、DLSS 输出资源或 Present 边界。原始 bin 的竖缝位于 **x=1689**，恰为输入宽度。
证据：`docs/agent/evidence/top-left-spatial-EXP-0039.log`。

随后发现明确的 shader 错误：reversed-Z 天空 `d=1`，旧公式 `CurNear/(1-d)` 把天空当成
**近平面**而非无穷远，产生错误的大幅 MV。EXP-0040 仅对 `d>=0.999999` 的天空用
`1e6` 远场深度并忽略 `CamDelta`，恢复时间累积，其他输入和绑定不变。
判据：左上竖缝/波动消失且 semantic 仍新鲜；否则继续分解近地 MV 矩阵/符号。
证据计划：`docs/agent/evidence/sky-reprojection-EXP-0040.log` 与同批 bin。

### 32.1 EXP-0040 结果与 EXP-0041

天空远场修复后波动仍在，因此它不是充分根因。复核发现 `evaluateSr` 实际写入
`DepthInverted=0`，与 reversed-Z 输入直接矛盾。EXP-0041 仅改为 **1**。
证据：`docs/agent/evidence/sky-reprojection-EXP-0040.log`。

深度方向主要影响遮挡和历史接受，不必然造成全屏异常；x=1689 的精确位置还可能涉及
MV 索引或 DLSS 内部分块，所以本实验是隔离验证，不预先宣称它解释全部症状。
若波动仍在，继续查 MV 坐标映射，不回改 FinalColor。
证据计划：`docs/agent/evidence/depth-inverted-EXP-0041.log` 与同批 bin。

### 32.2 EXP-0041 结果与 EXP-0042

`DepthInverted=1` 后波动仍在，深度方向不是充分根因。随后定位到更直接的分辨率契约：
MV 是 1689×950，输出是 2560×1440，但 feature flags 未设 `MVLowRes`，DLSS 因而可能按
显示分辨率读取 MV，并在 **x=1689** 产生边界。EXP-0042 将
`DLSS.Feature.Create.Flags` 从 IsHDR(1) 改为 **IsHDR|MVLowRes(3)**，其余不变。
证据计划：`docs/agent/evidence/mv-lowres-EXP-0042.log` 与同批 bin。

### 32.3 EXP-0042 结果与阶段 5

用户观察竖缝消失；最新 Sobel 在 x=1689 无异常竖线，DLSS 输出该列梯度仅 0.003288。
`MVLowRes` 修复成立，时间累积可保留。S5 已由 `flow.py claim --stage 5` 接受：
TAAU callback 被持续 suppress，semantic DLSS 实际输出且不再冻结。
证据：`docs/agent/evidence/mv-lowres-EXP-0042.log`。

## 33. EXP-0043：替换回归（2026-10-27）

唯一未知 `U-replacement-regression`：当前构建在暂停菜单、背包、F11 重建、切场景和
反复开关下是否稳定。代码不变，检查 `eval/suppress/missed/long frame`、崩溃和 UI 保留。
证据计划：`docs/agent/evidence/replacement-regression-EXP-0043.log`。

### 33.1 EXP-0043 结果与 EXP-0044

回归发现切存档、Stop→Start、F11 后逐渐变暗/变亮；背包、暂停菜单和更衣室无异常。
代码只在 `worldLoaded` 改变时 Reset，未在 view 进入 DLSS 或 `renderGeneration` 改变时 Reset，
与症状一一对应。EXP-0044 在这两类事件上 `resetFrames=2`，清除不可信历史。
证据：`docs/agent/evidence/replacement-regression-EXP-0043.log`。
验证计划：`docs/agent/evidence/history-reset-EXP-0044.log`。

### 33.2 EXP-0044 结果与 EXP-0045

view/renderGeneration 重置后亮暗往返仍未改变，历史 Reset 不是充分根因。
EXP-0045 不再改渲染行为：暗态与 F11 亮态各采一批 `sr_semantic / sr_mid / sr_dlssout / sr_final`，
用平均 log 亮度和增益确定变化发生在输入、NGX 还是最终编码级。
证据计划：`docs/agent/evidence/exposure-stage-EXP-0045.log` 与两批 bin。

### 33.3 EXP-0045 结果与 EXP-0046

三批数据证明曝光变化在输入本身：semantic/mid 均值为正常 0.131628、极暗 0.002354、
极亮 20.734270；DLSS 输出等比变化，final 仅编码。历史 Reset 和输出级均不是根因。
EXP-0046 启用 **IsHDR|MVLowRes|AutoExposure = 11**，验证 HDR 曝光归一化，不调色调曲线。
证据计划：`docs/agent/evidence/auto-exposure-EXP-0046.log` 与亮暗两批 bin。

### 33.4 EXP-0046 结果与 EXP-0047

AutoExposure 未归一化：输入暗 0.001832、亮 17.489810，DLSS 输出等比；原版 VV 的 F2
均值 0.629083。EXP-0047 不改渲染，暗态与 F11 亮态分别用 F10 采集 TAAContext 与
UpscalingCameraData 的 0x300 float 窗口，定位引擎实际曝光/tonemap 字段；找不到再取 shader。
证据计划：`docs/agent/evidence/exposure-context-EXP-0047.log`。

### 33.5 EXP-0047 结果与 EXP-0048

TAAContext 的 0x50 候选随亮暗变化，但取值（暗 -5.834 / 亮 0）是采样/透明度而不是曝光；
cam 侧同偏移恒为 0，稳定期 SR 本身也偏暗，说明颜色目标里没有可用的曝光参数。
EXP-0048 不再找字段：在 final encode 前自算 1×1 白点（64×64 点采样的 log2 亮度几何平均 ÷ 参考值），
DLSS 视图统一除以它，**其它视图保持 Mode 4 Bypass**（不读白点纹理）。
参考值取正常 SR 批次同一纹理的 64×64 采样几何平均 **0.0671**
（`_local/scratch/semantic_exp38/exposure_stats.py` 复核：正常 0.131583 / sub_geo 0.067111，
极暗 0.002353 / 0.001212，极亮 20.727217 / 9.271280，括号内为 RGB 均值 / 采样几何平均）。

判据：`sr_exposure_*.bin` 的 [0,0] 在暗态应 < 1、亮态应 > 1；`sr_final` 均值回到正常区间
（≈0.43–0.49，8-bit 0–1）。若正常帧整体偏亮/偏暗，用同一批 `sr_exposure` 与 `sr_final`
的比例校准参考值，而不是再改 NGX 标志或猜曝光字段。
证据计划：`docs/agent/evidence/adaptive-whitepoint-EXP-0048.log` 与暗/亮两批 bin。

### 33.6 EXP-0048 结果与 EXP-0050

重进暗态的白点（0.025118 / 0.012431）与输入几何均值吻合，final 均值为 0.386 / 0.397；
但亮态 DLSS 输出 sub_geo=1.830909 时 White 仍为 0.345814（目标 27.286274），final=0.997894，
画面饱和。EXP-0050 只让白点复用 DLSS 的 Reset 事件，在重置帧直接采用当前 target，稳态仍作
3% 对数平滑；dump 增加 view，以区分曝光 pass 未执行与 target 错误。参考常数暂保持 0.0671，
保证 A/B 只改重置行为。

用户同时报告仅上下方向拖影反向。静态公式确认 MV 的 Y 少了 NDC 向上到屏幕向下的符号换算，
但该修复属于另一未知，不混入 EXP-0050。

离线复核发现 NDRW `RawHeader` 实为 32 字节，旧脚本按 28 字节读取；因此旧参考批的 sub_geo
应由 0.067111 修正为 0.074404。重新定标是独立未知，本轮不改常数。
证据：`docs/agent/evidence/adaptive-whitepoint-EXP-0048.log`；postmortem：
`docs/agent/evidence/adaptive-whitepoint-EXP-0048-postmortem.log`。
验证计划：`docs/agent/evidence/adaptive-whitepoint-reset-EXP-0050.log` 与重进/F11 后 F7 批。

### 33.7 EXP-0050 结果与 EXP-0051

多次 F11 后仍崩坏。世界内 run3 的 DLSS 输出 sub_geo=0.335135，对应 White 目标 4.993964，
实际仅 0.096003，final=0.989284；final 反解 White 与直接 dump 一致，两个候选颜色源的
几何均值也一致，因此排除单纯平滑不足、final 绑错和 dump 陈旧。EXP-0051 不改渲染行为，
只在 4×4 exposure 纹理空余 texel 写入 target/previous/Mode/写入计数/W/H/实际纹理尺寸/
1e-4 地板样本数，并在 dump 行记录 dlssReady，以分离采样错配和写入/绑定故障。
证据：`docs/agent/evidence/adaptive-whitepoint-reset-EXP-0050.log`；postmortem：
`docs/agent/evidence/adaptive-whitepoint-reset-EXP-0050-postmortem.log`。
验证计划：`docs/agent/evidence/exposure-target-EXP-0051.log` 与 F11 后 F7 批。

### 33.8 EXP-0051 结果与 EXP-0053

异常 run3/4 的 shader meanLog 与同批 DLSS 输出离线结果一致，W/H 与实际纹理尺寸一致、
floor=0、dlssReady=1，写入计数与 frame 均增加 196，排除采样错配和 pass 未执行。target
线性值为 4.545352 / 4.560649，但 White 仅 0.121069 / 0.121139；主 previous 在 196 帧内
几乎不收敛，而 run1/2 是健康平滑对照。EXP-0053 不改渲染，在 texel(3,2) 镜像同一平滑链，
并用第 4 行记录 4 帧 target 环，以区分主状态 texel 的历史保持失败和逐帧目标跳变。
证据：`docs/agent/evidence/exposure-target-EXP-0051.log`；postmortem：
`docs/agent/evidence/exposure-target-EXP-0051-postmortem.log`。
验证计划：`docs/agent/evidence/exposure-history-EXP-0053.log` 与 F11 后 F7 批。

### 33.9 EXP-0053 结果与 EXP-0054

正常批 target 环稳定，异常批则在 +2.72 与约 -8.0 间逐帧交替，主/镜像 White 始终一致；
异常批 floor 超过 3000、mid 全黑。根因转向输入身份：resize 清空 semantic，而 semantic 只能在
原 TAAU draw 的材质绑定中重捕，替换模式持续 suppress 该 draw，最终回退到黑/交替 RTV。
EXP-0054 在 resize 后设置最多 4 帧的有界转发预算，材质匹配增加当前 render-size 尺寸门，
捕获成功立即恢复 suppress，预算耗尽则报警。保留旧 native、declarator 直接解析和盲换冻结候选
均不采用。验证 semantic 非零且尺寸正确、target 环连续、final 不饱和。
证据：`docs/agent/evidence/exposure-history-EXP-0053.log`；postmortem：
`docs/agent/evidence/exposure-history-EXP-0053-postmortem.log`。
验证计划：`docs/agent/evidence/f11-semantic-recapture-EXP-0054.log` 与 F11 后 F7 批。

## 34. R3：把插入点上移一层（2026-10-28 拍板）

> **状态**：已拍板，**未实现**；判据（§34.3）未过之前不改渲染行为。
> 实现分支：`r3`（worktree `r3/`，测试实例 `E:\LeviLauncher\versions\1.26.51.01-r3`，
> 2026-10-28 从 `agent2` 尖端切出）。
> **2026-10-28 细化**：阶段划分改为 **§35 的 A–D**（下一步 = 阶段 A：纯色写 `arg10`）；
> 下面 §34.2 / §34.4 的「阶段一 / 阶段二」仍然成立，但顺序与判据以 §35 为准。
> 决策记录：[decisions/0005](../decisions/0005-vv-r3-write-arg10.md)。
> 结构性复盘：[research/2026-10-28-vv-structural-review.md](../research/2026-10-28-vv-structural-review.md)。

### 34.1 为什么

§19–§33 的 30 个实验可以归到三笔债：身份靠形状猜、suppress 断掉角色来源、**在后缓冲上覆盖
等于把引擎的显示变换一起替换**。第三笔的证据：

- 引擎有显示分辨率的 HDR 中间面（`2560×1440 fmt26`，仅 RT，双缓冲）；declarator 的 `arg10`
  链解析出的两个 native 与日志里的两张逐位相同，且逐帧交替（EXP-0032、EXP-0035）；
- 48-list 世界帧里 `25:v2560x1440 d1`（无 DSV）不写后缓冲，`44:v2560x1440 z d1` 才是写者：
  B1c 在 tail-8…5 的切点图是「上一帧内容（含我们自己的面板）」，说明 25 之后后缓冲未被写过；
- EXP-0045 三批 dump 是**同一指针**跨 8800×（0.1316 / 0.00235 / 20.73），而引擎自己的 F2 输出
  在同一状态下仍正常（EXP-0046，0.629083）⇒ 下游存在一级归一化。
- 2026-10-28 反编译（`Upscaling.material.bin` + declarator / draw callback）：TAAU 是**纯
  temporal resolve**，输出 `H + w*(C−H)`，无曝光 / tonemap / encode；MV 是上游输入（`.zw`）。
  ⇒ `arg10` 与 `InputFinalColor` 同域，V2/V4 **静态已确认**。

⇒ 现状（suppress TAAU + 在 44 之后覆盖后缓冲）用一次拟合的幂函数 + 自算白点去复刻这一级；
被复刻的对象是自适应的，固定近似不可能收敛。EXP-0048–0053 的白点线是这笔债的利息。

### 34.2 阶段一的管线（TAAU 照跑）

```
TAAU list 25  ──写──>  Color0 / arg10 (2560x1440 R11G11B10, 双缓冲)
我们的 list（切在 Color0 写入 list 之后立刻）
    prep: 读 arg9 颜色 + depth → RGBA16F / R32F；MV 直接读引擎 arg11（.zw，Y 翻转、渲染归一化）
    NGX DLSS-SR: Color/Depth/MV(渲染像素) → dlssOut (display, FP16)
    PS + RTV: dlssOut → Color0（不做 tonemap/encode）
下游后处理（bloom / LocalExposure / 编码等）→ 合成 list 44（写后缓冲）→ HUD list 45
```

- 输出目标 `Color0` 没有 UAV 位（`flags=0x1`）⇒ 必须 **PS+RTV**，不能 compute + copy；
- 切点 = **`Color0` 写入 list 之后**（EXP-0055：48-list 帧是 list 25；44 写的是另一张 RTV，
  不是 `Color0` 的读写者）；判据是「该 list **任一次** `OMSetRenderTargets` 的 RTV 反查 == arg10
  native」，不再是帧形签名。**2026-10-28 EXP-0057 证伪了「首次绑定」版本**：writer 的第一次绑定
  是 128x128 或未解析，从不是 arg10（`hits=[]`、`split=0/69`），见
  `canon/refuted.jsonl` 的 `taau_arg10_writer_first_binding`；
- 第一阶段**不 suppress**：材质绑定每帧都在，输入身份不掉线；代价是 TAAU 的 GPU 时间仍在，
  F1 不成立；
- 阶段一同时**删掉 prep 里的 depth+VP 重建**，MV 改绑引擎 arg11（契约见 §6 的 2026-10-28 更正）。

### 34.3 判据（一次诊断构建 + 一次会话）

| # | 前提 | 判据 | 不成立 |
|---|---|---|---|
| V1 | `arg10`/`Color0` 被下游消费 | ✅ **已证（2026-10-28）**：写入者 = 显示分辨率、无 DSV、单 draw 的前置 list（EXP-0055；48-list 帧为 25，72/70/107-list 帧为 49/47/84）；消费者 = `LuminanceHistogram`、`Bloom`×2、`LocalExposure`，以**共享帧图节点引用**消费，采样帧内无 GPU 复制（EXP-0056：运行期材质引用 + frameImpl 静态数据流）。剩下 `WaitOnUpscalingOutput` 的登记语义与 RQ-0004..0007 的静态复核，属尾巴、不阻塞阶段 A | 若下游完全不用，回查后处理链 |
| V2 | ✅ **静态已确认**（2026-10-28 反编译）：TAAU 输出 = `H + w*(C−H)`，与 `InputFinalColor` 同域、不做归一化 | 无需再测；运行时只需确认我们写的 `Color0` 就是下游读的那张（并入 V5） | — |
| V3 | 能安全写 `Color0` | ✅ **阶段 A 通过（EXP-0058）**：Clear+RTV 连续写 960 帧，无 D3D12/设备错误；写 RT、留 RT | 状态不可控 ⇒ `CopyResource` 兜底 |
| V4 | ✅ **静态已确认**：TAAU 是纯 temporal resolve，无曝光/tonemap/encode、无深度、无 VP 使用 | — | 无需再测 |
| V5 | 写的是本帧下游要读的那张 | ✅ **阶段 A 通过（EXP-0058）**：双缓冲交替 960 帧无闪烁/滞后/冻结；writer 判据 = 该 list **任一次** `OMSetRenderTargets` 绑定 arg10 | 写错缓冲 ⇒ 滞后一帧（可见） |

### 34.4 阶段二（suppress）

只有在找到**与 suppress 正交的角色通道**（材质 hook 在 declarator 阶段是否也触发；或状态机 +
有界转发并强制 Reset 两个时间滤波器）之后，才 suppress TAAU。否则保留阶段一（F1 不成立，
但 F2/F3 可达）。

阶段二多一个未知：**suppress 后那条 list 是否仍把 `Color0` 绑成 RTV**（EXP-0055 的锚点是
在未 suppress 状态下测的）。若绑定也消失，锚点失效，需要退回「按上一帧的 writer 下标 + 帧形」
或另找与 draw 无关的角色通道。

### 34.5 删除清单（两刀，AGENTS §4）

- **第一刀（R3 前，零行为变化）**：零引用死代码——`scanForTextures`、`printableRuns`、
  `dumpCandidates`、`findTarget`，以及面板已标 `(dead)` 的 taau-list 扫描子树；
  **2026-10-28 已执行**：连带删掉辅助链 `looksLikeD3D12Object`、只服务该扫描的 `Close` 钩子
  （槽 9，`void(this)`）与 `VvInject::setUseTaauList` 死分支/面板开关。扫描这条死路连同它的
  槽 9 锚点记入 `re_kb canon refute`（key `command_list_taau_scan`），代码里不再保留；
- **第二刀（F2 之后）**：`VvBypass` + `Action::Bypass`、`VvTone` + 曝光 compute + final 的
  exposure 分支 + 白点状态、`Action::Clear`。

### 34.6 不宣称与止损

- 不宣称 R3 已成立：V1 已由 EXP-0055（写入者）+ EXP-0056（消费者）补齐，V2/V4 由 2026-10-28 反编译
  静态确认，**V3/V5 仍待阶段 A 的实写验证**；
- 不宣称「曝光是幻影」：已被同指针 8800× 证伪；R3 的作用是**复用**那一级，不是否定它；
- 不宣称深度方向已定：`near/(1-d)`（标准 Z）与 `DepthInverted=1` 互相矛盾，需一次判定；
- 止损（命中即回「读 shader」路线，不再加补丁）：把 `Color0` 写成纯色后**下游画面完全无变化**
  （说明它没有被任何下游消费）／`Color0` 状态连续两次调试层报错。

## 35. R3 实施阶段 A–D（2026-10-28 拍板「停止论证，进入实施」）

> **状态**：**阶段 A 通过（EXP-0058）**、**阶段 B 通过（EXP-0061，2026-10-28）**；
> 下一步阶段 C，C–D 未开工。分支 `r3`，实例 `E:\LeviLauncher\versions\1.26.51.01-r3`。
> 上位分析与判据表见 §34 与 [ADR 0005](../decisions/0005-vv-r3-write-arg10.md)；
> 本文只回答「按什么顺序做、每一步怎么算过、哪一步失败就停」。
> **不再继续反编译论证 R3**：V2/V4 已静态确认（TAAU 是纯 temporal resolve，无 tonemap/encode，
> MV 是上游 `.zw` 输入）；剩下的 V1/V3/V5 只能靠**运行时不加修饰地写一次 `arg10`** 来判。

### 35.1 阶段 A：纯色写入验证（只回答 V3/V5）

目标：证明运行时**真的控制了下游所读的那张 `arg10`**，并且写它不出状态错。

`arg10` 的下游已经被 EXP-0056 证到「共享帧图节点引用 + 三个后处理消费者」，所以阶段 A 不再论证
「有没有人读」，只测「写进去能不能被读到」——这是它能当止损点的原因。

做法（最小实验，别的都不动）：

- TAAU 照跑，**不 suppress**；不接 DLSS；不改输入、MV、曝光或其它管线；
- 每帧 declarator 后解析当前 `arg10`（EXP-0055 的探针已在做：holder → resource → bgfx handle
  → native，逐帧在两个 native 之间交替）；
- 按 EXP-0055 的 RTV 指针找到它的**写入 list**：判据是「该 list **任一次** `OMSetRenderTargets`
  的 RTV 反查 == 当帧 `arg10` native」，**不是**下标、也**不是**帧形签名（下标随帧形在 25 / 49 / 47 / 84
  之间漂移）。EXP-0057 证伪了「首次绑定」版本（`hits=[]`、`split=0/69`），见
  `canon/refuted.jsonl` 的 `taau_arg10_writer_first_binding`；
- 在该 list **之后**插入我们自己的 list（`probe/VvInject` 已有「前缀 + 自己的 list + 后缀」的
  同队列提交机制，阶段 A 只加一个切点和一个 action）；
- 用 **PS + RTV** 向当前 `arg10` 写纯色，**保持 RT 状态**（写 RT、留 RT，让下一个消费者自己做
  barrier；`Color0` 没有 UAV 位，写不了 compute）。

判据（三条都要）：

| # | 判据 | 不成立意味着 |
|---|---|---|
| A1 | 最终画面**明确受纯色影响**，HUD 保留 | 运行时尚未真正控制下游所读的 `arg10` ⇒ **停止实施**，不再追加补丁 |
| A2 | 双缓冲切换时颜色**不闪、不滞后一帧** | 写错缓冲（native 逐帧交替，EXP-0055）⇒ 目标与切点必须**当帧现取**，禁止缓存 |
| A3 | 无 D3D12 状态错误（调试层） | 状态不可控 ⇒ 退 `CopyResource` 兜底（ADR 0005 的 V3 分支） |

HUD 与准星在后缓冲上，只动 `arg10` 理应不受影响；若 HUD 一起变色，说明写到了后缓冲，按 A1
失败处理。**手不在这个外推里**：EXP-0058 里手随 `arg10` 一起变品红而 HUD 未变，说明手参与了
`arg10` 的消费链。**A1 不成立即停**——这是实现前最后一个低成本止损点。

**实现（2026-10-28）**：

- `VvProbe` 每帧在 declarator 后取 `arg10` native；`onExecuteCommandLists` 用「该 list **任一次**
  `OMSetRenderTargets` 的 RTV 反查 == 当帧 native」找 writer（要求无 DSV），写入
  `arg10WriterSplit`（1-based）。`setArg10Solid(true)` 会让每个 list 重新记录本次 arm 的全部
  RTV handle（EXP-0057 证伪了只看首次绑定）。
- `VvInject` 新增 `Action::SolidArg10`：切在 writer 之后，用 RTV 把 `arg10` 清成品红；
  **不发 barrier**（写 RT、留 RT，A3 就是测这一点），不 suppress TAAU；找不到 writer 就整帧跳过。
- 面板 `VV R3 stage A (TEST)` → `Write solid arg10` / `Disarm arg10 solid`；
  日志 `[vv] inject: arg10 boundary split=.../...`，未命中为 `arg10 writer not found`。
- 与上文的差别：阶段 A 用 `ClearRenderTargetView`（仍走 RTV），不建 PS 管线；PS 留给阶段 B。

**结果（EXP-0058，2026-10-28）**：A1/A2/A3 全过。

- 用户原话：「画面和手变为品红，hud不影响，然后逐渐变白（秒级），变成全白后，恢复成原来 VV 的样子，
  画面无闪烁，恢复后无冻结。」
- `inject: arg10 boundary split=47/69` 连续 960 帧（injected 1→960，skipped=0）；无 D3D12/设备错误。
- **手在 `arg10` 链上**：纯色把画面和手一起染色，HUD 不受影响（所以不是写了后缓冲）。阶段 B 用
  真实 DLSS 输出时要单独复验手部；目标语义里的「手不变」不能靠「HUD 没变」外推。
- **曝光会追着纯色走**：品红在秒级内被 `LuminanceHistogram` / `LocalExposure` 推到全白；停止注入后
  恢复原样。这是 V1 的可见证据，也说明阶段 B 的 DLSS 输出必须与 TAAU 同域（否则曝光会漂）。
- **Writer 定位的边界**：面板打开后帧形变 74/107，`writer not found`，注入停止；回世界 69 帧后没有
  自恢复（疑似 `kSigSlots=128` 签名表满，新 list 没有槽位）。阶段 B 必须让 writer 在 UI 帧后自恢复，
  或 disarm 再 arm。证据：`docs/agent/evidence/arg10-solid-EXP-0058.log`。

### 35.2 阶段 B：DLSS 写 `arg10`（只改输出）

纯色通过后，把纯色换成真实 DLSS 输出：

```
现有已工作的颜色 / 深度 / MV
              ↓
            DLSS
              ↓
       PS + RTV 写 arg10
              ↓
  引擎曝光 / Bloom / 显示变换
```

- 这一轮仍**不 suppress**，并尽量保留当前已工作的输入路径，避免同时引入新变量；
- 应当移除出新路径的：白点 compute、`vvEncode`、`finalRgba8`、后缓冲
  `PRESENT → COPY_DEST → PRESENT` 往返。

验收重点**不是** `eval=1`，而是：

- DLSS 与 TAAU 经过**同一条显示链**后无整体偏色、极亮或极暗（同帧参考比较只剩细节差异）；
- F11 后输出尺寸与双缓冲仍正确；
- HUD、手、准星保留。

**实现（2026-10-28，待 EXP-0059）**：

- `vv::sr::recordArg10`：走现有 prep + DLSS evaluate，然后用一个全屏三角形 PS（`vs_5_0` / `ps_5_0`）
  把 `dlssOut` 写进 `arg10`；**不跑**白点 compute / `vvEncode` / `finalRgba8` / 后缓冲
  `PRESENT → COPY_DEST → PRESENT`，也不发 `arg10` 的 barrier（沿用阶段 A 的 RT 状态）。
- `VvInject` 新增 `Action::DlssArg10`；切点仍是 `arg10` writer 之后，面板
  `VV R3 stage B (TEST)` → `DLSS -> arg10 (stage B)` / `Stop arg10`。
- `kSigSlots` 128→512：EXP-0058 里面板打开后 writer 不再命中，疑似签名表满；满表会打
  `list signature table full`。
- 构建门禁同步支持 `*VsShader` / `*PsShader`（`checks.py`），VS/PS 分别用 `vs_5_0` / `ps_5_0` 编译。
- 日志：`[vvsr] sr arg10: <w>x<h> fmt=26 eval=<ret>`；DLSS 评估失败时整帧跳过，不回退到任何旧路径。

**结果（EXP-0061，2026-10-28）**：黑闪未复现，world 帧连续注入。

- 用户原话：「这次轮到黑闪无法复现了，抖动来了一次，依然没有找到办法复现。」
- 18:51:55 起 `split=48/70`，injected 1→1080 时 skipped=1；随后 45/70、44/69、46/71 继续注入到
  3240；`sr arg10 #600..#2400` 与 injected 同步，说明 record 前早退生效。
- 黑闪的两个已修原因：`record()` 在丢弃帧上 evaluate（历史污染）与 list 级 `!dsv` 误杀 writer
  （EXP-0059/0060 复盘）。
- **抖动仍是唯一未解**：一次、无触发、不可复现；疑与自算 MV/时域有关，阶段 C 换引擎 MV 后重点看。
  → **2026-10-29 定位（EXP-0098/0099）**：不是 MV，是喂给 DLSS 的亚像素 jitter。用户 A/B
  「=0 就稳了；锐化只是让抖动更明显，关掉锐化也抖」⇒ 直传的引擎值与当帧输入对不上。
  「世界帧没按 `TAAContext+0x1e4` 渲染」仍是**推断**；这个值本身与 DLSS 同空间（渲染像素、y 向下，
  不需要任何换算，见 §37 / `research/2026-10-29-taau-jitter-dlss-contract.md`）。产品侧改成 jitter 恒为 0，
  引擎值只留作 TEST 开关；机制同 EXP-0073 的菜单抖动。
- 观察项：108-list UI 帧出现过一次 `split=13/108` 注入，本轮未见黑闪；若黑闪回来，先查这类 UI 帧。

### 35.3 阶段 C：切换引擎 MV，删除自算 MV

输出路径稳定后，**单独**改输入，不与阶段 B 混合：

```hlsl
dlssMv = float2(engineMv.z, -engineMv.w) * renderResolution;
```

- 按**材质角色**区分 `t0` MV / `t1` FinalColor / `t2` History，不能再用「最后一个 fmt26 匹配」；
- 保留 `MVLowRes`；
- 验证运动方向、静止零 MV、转头无异常拖影；
- 然后删掉 VP / 相机原点重建、近平面处理与自算 MV 的 Y 符号补丁。

深度先沿用当前可用路径，避免这一轮再次扩大范围；**深度语义**（代码 `near/(1-d)` 与
`DepthInverted=1` 的矛盾）单列未知，不在这四个阶段里顺手改。

**第一轮（EXP-0062，两跑，引擎 MV 未进入）**：

- 两次实机（19:26 dll `3110a443`、19:39 dll `b56aa5cb`）`arg11` 全部
  `handle=0xFFFF reason=invalidChain`；`sr arg10 #N: ... mv=vp`，dump `mvsrc=vp`
  ⇒ 这一轮画面仍是 depth + VP 重建，**不能作为阶段 C 的结果**。
- `phase=decl` 读到 `wrapper` 是堆指针，但 `*(wrapper+0x10)` 固定是 `0x3be70000`；
  `phase=draw` 时它是 `0xa02f6ee841000000` 一类非指针 ⇒ **ExternalImage（颜色/MV）的
  wrapper 类没有 arg10（transient）那条 `+0x10 holder → +0x18 resource` 链**。
  这与 §34 的结构复盘一致：颜色/MV 只出现在材质绑定里。
- 同一次 TAAU draw 的三次材质绑定已能按角色分开（19:39:43 原始日志）：
  `0x0413 → 0x1a098fcd860`（fmt26 + render-size + RTV，与 `sr semantic colour` 同一指针）
  = `FinalColor`；`0x04F1 → 0x1a098fcc540`（render-size RTV、fmt≠26）= `MV`；
  `0x05F4 / 0x0243`（不在 render-size 清单、逐帧交替）= `History`。
- ⇒ 下一步是把 MV 捕获挪到**材质 hook**（`gInTaauDraw` 内、render-size RTV 且不是
  FinalColor 的那次绑定），不再走 declarator 依赖链；这一步不需要反编译。
  反编译只在「suppress 后材质绑定也消失、需要与 suppress 正交的角色通道」时才必要（阶段 D）。
- 证据：`docs/agent/evidence/engine-mv-EXP-0062.log`（两轮原始行）与游戏目录
  `sr_mv_rg32_*.bin`（run 1–6，均为 vp 路径）。

**第二轮（材质 hook 捕获有效；早期“不是 MV”判读已更正，2026-10-28 复核）**：

- 材质 hook 捕获确实接上了（`arg11 mv material ... fmt=10`、`sr arg10 #N ... mv=engine`）。
  早期报告把 `zw` 写成“精确为 0”，并据 `xy` 的法线图判死整张 fmt10；这个判读是错的。
- 对 `sr_mvraw_1689x950_{1,2}.bin` 与 `sr_mvengine_{1,2}.bin` 逐通道复核：`xy` 在 ±1、是法线；
  `z/w` 非零覆盖率约 52–55%，量级约 0.005–0.01，是渲染分辨率归一化 MV。
  这张图是 **`xy=法线、zw=MV` 的复合 G-buffer**，TAAU 只读 `.zw`，所以法线外观并不排斥 MV。
- 运动方向也成立：四份 dump 的 `DLSS(-z,+w) * RenderResolution` 都是 100% 指向画面中心的
  径向场；`TAAU(z,-w)` 则全部反向，正好符合各自 `current→previous` / 历史重投影约定。
- `record+0x2d` 提供的槽位是 0（该绑定的 slot），对应材质 slot 0；同一次 draw 的另外两次
  绑定是 slot 1（fmt26 颜色）与 slot 2（显示尺寸、逐帧交替的历史）。
- 对 0xDFF64E0 的离线反汇编：`record` 只被读 `[r8]`、`+0x18`、`+0x1c`、`+0x1d`、`+0x2c`、
  `+0x2d`（纹理描述指针与用法/槽位字节），**没有 uniform 名字 hash**；但这不妨碍由材质 slot 0
  与 `.zw` 数值共同确认角色。
- ⇒ 阶段 C 应继续使用材质捕获的引擎 MV，公式 `dlssMv = float2(-z, +w) * RenderResolution`；
  不应回退到自算 MV，也不再需要仅为角色解析去改材质服务/override。
- 复核证据：`docs/agent/evidence/engine-mv-EXP-0062-review.log`；早期 `taau_material_bind_is_mv`
  否定条目由 canon 的新更正条目取代。

**仍未解 / 需要收口（2026-10-28 20:3x，第二次复核）**：

- 「903 帧形没有 MV」的假设**已被否掉**：同一 session 第一次录制（窗口 1689×903）的
  `sr_mv_rg32_1689x903_1/2` 是 p99 **15.4 / 14.0 px**、**100% 指向中心**；`mvraw` 的 `zw`
  非零率 54%。所以 903 的复合 G-buffer 与 950 一样带 MV。
- 真正为零的是**第二次 F11（切真全屏）之后的两批**：`sr_mv_rg32_1689x903_3/4` p99 0.01 px、
  `mvraw` 全零，而同帧引擎原图 `sr_mvengine_3/4`（1689×950）仍是 100% 向内的正常场。
  ⇒ 尺寸切换后引擎 MV 是 950、我们的 render 尺寸还是 903，`prepMode` 落 0；
  新 `mvRaw` 还没被写过，于是喂了零场。这是** resize 窗口期的状态问题**，不是 MV 源的问题。
- 用户在这次会话的**第一次录制**（MV 正确的那段）报告了画面抖动 ⇒ **抖动与 MV 源无关**，
  在自算 MV（EXP-0060/0061）和引擎 MV 下都能复现，按独立未知项继续查（present/历史/reset 方向），
  不阻塞阶段 C 的输入切换。
  → 该未知项已在 2026-10-29 关闭：原因是 jitter，不是 MV（见 §35.2 的同一条注）。
- 收口要求：resize 后到引擎 MV 尺寸与 render 尺寸一致之前，**不注入**（跳过该帧并记日志），
  不允许用自算 MV 或零场静默顶上；然后再做「F11 两种形态 × 静止/走动」的回归。

**回归第一轮（2026-10-28 20:39–20:40，dll `0dbb95d7`）**：

- 窗口态（1689×903）四批 dump 里前两批 `sr_mv_rg32` 为 p99 **9.25 / 9.11 px**、**100% 指向中心**，
  与 950 的 11.8 px 同量级 ⇒ **引擎 MV 输入本身已通过**（两种帧形都成立）。
- 但 **20:40:08 起注入停了**（`inject skipped: no pre-HUD boundary`，连续 1800+ 次），
  早于 20:40:15 的 F11；之后 `arg10 probe ... hits=[]`，即没有任何 list 的 OMRT 绑定当帧 arg10
  native ⇒ **writer 锚点在帧形变化后失效**。后两批 dump 是停注入后的陈旧缓冲，不代表画面。
- 用户报告第一次录制期间仍有**抖动**，而那时 MV 是正确的（9.25 px/100% 向内）⇒ 抖动与 MV 源
  无关，继续单列；它同时也是「注入是否连续」的观察点：停注入期间画面回 TAAU，不应把那段算进抖动判据。
- ⇒ 下一步不是动 MV，而是**把 writer 锚点做成帧形无关**（否则 C 的屏幕回归与 D 的 F11 回归都过不了）。

**收口（2026-10-28 20:5x）**：

- **EXP-0062 通过**：20:48 的构建里注入跨「开关背包/菜单（帧形 71→108/109）」和 F11 切全屏继续，
  `sr arg10` 到 #3600+，全程仅 2 次单帧 skip；走路 dump 为 **9.0 / 10.0 / 5.5 px、100% 向内**。
  证据：`docs/agent/evidence/engine-mv-EXP-0062-pass.log`。
- writer 锚点修了三处：RTV 句柄表改成**环回绕**（旧实现超过 4096 次就不再记录，这是 20:40:08
  之后永久停注入的根因）、每 list 句柄容量 8→16、`OMSetRenderTargets` 时**就地打标**
  （`arg10Bound` + 声明序号，提交阶段直接认标，不再事后重算）。
- **自算 MV 已删**（EXP-0063 已通过，证据 `docs/agent/evidence/mv-cleanup-EXP-0063.log`）：
  prep shader 去掉 MV 常量缓冲与重建分支，只读 arg11；`mvCb`、VP/相机原点/近平面解码一并移除，
  `decodeMvFrame` 只留 jitter 与相机 delta；两条渲染路径在 arg11 缺失时都跳过而不是回退。
  删除后回归：`sr arg10` 连续、`mv=engine`，走路 dump **11.92 / 8.12 px、100% 向内**，无 D3D12 错误。
  附带影响：`sr_mvraw` 与「非 fmt26 候选靶」两类诊断 dump 随这次删除一起没了（`dumpInputs` 恢复为
  删除前的版本）；若还要查引擎 MV 本身，需要重新加回。
- **抖动仍未解**：在 MV 正确的段里复现，确认与 MV 源无关，单列。

### 35.4 阶段 D：回归后再 suppress

先在**不 suppress**下过 F11、切世界、Stop/Start，确认没有亮暗漂移和错代输出。之后才做第二阶段：

- 输入角色通道**不得依赖被 suppress 的 TAAU draw**；
- 输入无效时**恢复 TAAU**（不注入 DLSS）；
- **禁止**回退到启发式 RTV；
- 不把 EXP-0054 的「定期转发」当最终机制。

成功后再删 `VvTone`、`VvBypass`、白点与自算 MV 等旧实现，避免两套路径长期并行（AGENTS §4）。

**回归第一轮（EXP-0064 fail → EXP-0065 pass，2026-10-28）**：

- EXP-0064（dll `6cbf7cae`）：F11 双向与帧形 49/71→52/74 期间注入连续（injected 1→1560），
  但切出世界触发渲染图重建时，`recordInternal` 因 arg11 暂缺返回 false，`inject::record()` 在
  记录中途返回**没有 Close 命令列表**；allocator 从下一帧起 `Reset` 永久失败，只剩
  `recording the injected work failed`，重进世界与 Stop→Start 均不恢复（21:07:46 之后没有任何
  `[vvsr]` 行，失败发生在进入 `recordInternal` 之前；无设备错误）。证据
  `docs/agent/evidence/r3-d-regression-EXP-0064.log`。
- 修复：`VvInject::record()` 在 Reset 前先 Close 上一帧可能遗留的列表；所有子 record 的失败/成功
  返回统一走 `finish()`（必 Close，Close 失败带 hr 记日志）。
- EXP-0065（dll `fee85e7e`）：两次切出/重进世界（渲染图重建）与 1689x903↔1689x950 两种尺寸切换
  期间 injected 1→7920，仅 3 次单帧 no-boundary、零次 RecordFailed；`sr arg10` 持续推进、
  `eval=1`、`mv=engine`，无 D3D12 错误。**未覆盖**：Stop→Start 本次没有执行（无 `armed=0` 行），
  反向 F11 只随第二次进世界发生；用户 21:17 之后的最小化动作在判据窗口外。证据
  `docs/agent/evidence/r3-d-regression-EXP-0065.log`。
- ⇒ 不 suppress 的回归只差 Stop→Start 一项，并入下一次实机的回归清单；之后才进入 suppress。

**Suppress 诊断（EXP-0066，2026-10-28）**：

- 整回调 suppress（8 帧）：没有任何 list 再把 `arg10` 绑成 RTV（每帧 `writer=0`），现有 writer
  切点会消失；MV 也不再更新。⇒ **不能**整回调 suppress。
- declarator 四依赖一次 walk：`arg9 colour`/`arg11 mv` 都是 `noHolder`（dep2 的 `+0x10` 是
  `0x7df0000`，不是指针），`arg10`/`arg12 history` 能解析。⇒ MV 没有 declarator 侧通道。
- TAAU 的 draw 本身可精确识别：它是 arg10 writer list 的**第一条** `DrawIndexedInstanced(6,1)`，
  draw 时刻 `exact=1`（该 list 已绑定当帧 arg10），每帧一条。⇒ 只跳过这一条 draw。
  证据 `docs/agent/evidence/r3-suppress-probe-EXP-0066.log`。

**Draw 级 suppress（EXP-0067，dll `b5d3072a`）**：

- 实现：`VvProbe` 的 draw 级开关（默认关）+ 上一帧注入成功门；命中条件 = 该 list 已绑定当帧
  `arg10`（bind-time exact）+ 该 list 第一条 draw + 6 索引/1 实例 + arg11 非空。命中时只跳过
  这一条 draw，回调、材质绑定（MV 通道）与 arg10 的 RTV 绑定全部保留。
- 结果：约 30 s 内 `skipped TAAU draw` #1→#2400+（每帧一条），`inject fixed` 从 840 继续到 3480、
  `skipped` 保持 1，`sr arg10` #600→#3600 全程 `eval=1 mv=engine`，UI 帧形 87/109 也连续；
  Restore 后跳停；Stop→Start 重新 arm 后从 #1 恢复（补上 EXP-0065 的缺口）。无 D3D12 错误/崩溃。
- **未覆盖**：Restore 后二次 arm；输入无效时恢复 TAAU 的路径（需在 draw suppress 开着时
  Stop arg10 / 遇面板帧触发）。证据 `docs/agent/evidence/draw-suppress-EXP-0067.log`。

**出世界继续 DLSS（2026-10-28 用户拍板，EXP-0070/0073/0074）**：

- 早期 VV 路线出世界不停注入，后来因为「一次 record 失败会把 SR 路径弄死」加了 `inWorld()`
  空转门；EXP-0065 的 Close 修复消除了那个隐患，门已撤掉：`recordInternal` 在菜单里照常
  prep + DLSS + 写 `arg10`，菜单背景由同一条 R3 管线处理（EXP-0070：菜单里 injected 840→6960、
  `sr arg10` eval=1 mv=engine，用户确认背景在动）。
- **标题界面仍不能注入**：未进过世界时 render graph 里没有 render-size 的 colour/depth
  （`colourTargets=0 depthTargets=0`），`pickInputs` 失败。这不是 world 事件门，是新进程的
  标题界面没有可喂的输入；进过一次世界后菜单即可用。
- **菜单抖动已定位并收口**：抖动源是把世界 TAAU 的 `SubPixelJitter` 喂给了**未按该 jitter 渲染**
  的菜单全景——DLSS 每帧按错误的亚像素偏移重建，表现就是上下抖。EXP-0073 起 `!worldLoaded()`
  时 jitter=0；EXP-0074 再取消「菜单每帧 Reset」，允许累积历史，旋转背景无抖动、无拖影
  （用户确认）。A/B 时注意：`Restore TAAU draw` 只停跳 draw，stage B 仍会覆盖 `arg10`，要真
  回 TAAU 必须 `Stop arg10`。
- 证据：`docs/agent/evidence/r3-menu-dlss-EXP-0070.log`、`r3-menu-judder-EXP-0073.log`、
  `r3-menu-accum-EXP-0074.log`。

**F1–F3 与 F11 硬化（EXP-0076，dll `37625c36`，2026-10-28）**：

- **F1**：`d-suppress: skipped TAAU draw` #1→#1200+（每帧一条），与 `inject fixed` / `sr arg10`
  同步；draw 级跳过在 `DrawIndexedInstanced(6,1)` 处直接不转发。证据
  `docs/agent/evidence/f1-tauu-draw-absent.log`。
- **F2**：同帧对比（注入前把该帧 TAAU 的 `arg10` 复制到 `taauRef`，与 `dlssOut` 同批 dump）：
  run 3 通道均值比 1.0036/0.9977/1.0003、luma +0.5%、tile 相关 1.000；run 1 最大通道差 1.5%、
  luma 差 0.02%。**无整体偏色**，差异只在细节。证据 `f2-same-frame-compare.log` + 游戏目录
  `sr_taauref_*.bin` / `sr_dlssout_*.bin`（sha256 见该文件）。
- **F3**：多轮实机 HUD / 手 / 准星 / UI 面板保留；Stop/Start、Restore/Suppress、背包、暂停、
  F11、切世界均不崩（EXP-0067/0068/0070/0076）。证据 `f3-ui-checklist-EXP-0076.log`。
- **F11 硬化**：曾连按 F11 出现冻结/截断/强曝光（124 次注入跳过、arg10 停在旧内容）；已加
  generation 门——每次渲染图重建后必须先在新 generation 成功注入一帧才跳 TAAU。复测连按 5 次：
  120 次注入失败期间画面保持 TAAU，恢复后 skip #3600，无冻结/截断/强曝光、无崩溃。证据
  `r3-f11-hardening-EXP-0076.log`、`s6-regression-EXP-0076.log`。

### 35.5 现在这一刀（immediate next）

**只做阶段 A 的纯色 `arg10` 实写**：A1–A3 任一不成立就停。通过后就以 §35.2 为下一步，
不再回到反编译论证 R3。

## 36. NR 接入 VV（EXP-0083 / EXP-0085，2026-10-29）

> **状态**：EXP-0083 首次实机**全黑**（原因见 §36.4）；EXP-0084（detour 透传）取消；
> 现行设计是**删掉 VV 侧 NR 编排，完全由 RT 探针的 NR detour 驱动**。EXP-0085 实机确认
> 不再全黑；退出崩溃与窗口 reset 已修（§36.6），移动闪动定位到 MV 单位并修复（§36.7）。
> **EXP-0088 实机通过**：移动 30–60 秒无闪动、正常退出无崩溃。NR 在 VV 的输入级接入
> 完成（DLL `f0955df3`）。**已知限制**：F11/改分辨率后的 NR 合成纹理尺寸未验证。
> 设计目标：把 NGX feature `0x12` 作为 R3 DLSS-SR 的**输入级**跑起来，不新增输出路径。

### 36.1 管线

```text
引擎 color (R11G11B10, render res) ──\
引擎 depth (R24G8,      render res) ───> prep -> RGBA16F / R32F / RG16F
引擎 arg11 (MV,        render res) ──/                 |
                                                       v
                       RenderProbe::ngxEvaluateDetour（RT 的 NR 代码）
                       nrCreate -> nrEnsureRenderSize -> nrRecord
                       encode -> evaluate -> ratio composite
                                                       |
                                              gDecodedColor (RGBA16F, linear HDR)
                                                       v
                                   original NGX DLSS-SR evaluate（同一参数对象）
                                                       v
                                        PS + RTV -> arg10（原有 R3 输出）
```

- NR 的 `Color/Depth/MVec` 就是 VV `evaluateSr()` 写进参数对象的三张 prep 纹理
  （RGBA16F/R32F/RG16F），**不碰引擎资源**；detour 在调用 original DLSS 之前把
  `Color` 指到 NR 合成结果；
- **VV 侧没有 NR 代码**：不建 feature、不调 `nrRecord`、不读 `nrFedResource`；
  `nrCreate/nrEnsureRenderSize/nrRecord` 全部由 RT 探针现成的 detour 执行；
- detour 用 `gNgxCreateOriginal` / `gNgxEvalOriginal` 建/调 NR，所以 NR 的 evaluate
  不会再次进入 detour（无递归）；
- `nr.enabled` 门控，默认关闭；`nr.placement` 在 VV 下也生效（0 = 放大前默认，
  1 = 在 DLSS 之后处理 `dlssOut`），`nr.inputPercent` 同样生效。

### 36.2 会话与创建

- VV 不加载引擎 NGX；`VvDlssSr` 自己 `Init_ProjectID`（app id 与 NR 相同）并持有 core；
- 第一次 `nr.enabled` 的 DLSS evaluate 上，detour 看到参数对象里的
  Color/Depth/MotionVectors/Output/Width/Height 齐全，于是用**同一个会话**创建
  feature `0x12`；core 从 exe 同目录加载 `nvngx_dlssnr.dll`；
- 之后每帧 `nrEnsureRenderSize` 跟随 render size，尺寸变化走重建。

### 36.3 状态与回退

- prep 后 color/depth/MV 统一留在 `ALL_SHADER_RESOURCE`：对 DLSS 合法，也是 NR 桥
  encode/composite 的前置（桥自己会还原）；这样切换 `nr.enabled` 时不会出现
  「VV 设的状态」与「detour 的判断」不一致的窄窗口；
- NR 创建失败、`nrEnsureRenderSize` 拒绝、evaluate≠1 或本帧未合成时，detour 不会改
  `Color`，DLSS 读到的仍是 `colorHalf`——**整帧退回无 NR**，不拿上一帧结果顶替；
- 合成纹理是「只增不减」的：窗口缩小后它会大于当前 render size，而我们的 DLSS evaluate
  不设 Color subrect。**因此 VV 下的 NR 目前只保证固定窗口尺寸**；F11/改分辨率后的
  行为不在本轮判据里，需另做 Color subrect 或按当前尺寸重建；
- 关闭 `nr.enabled` 只停止调用，feature 常驻到进程结束（与 RT 路径的「应用不卸载」一致）。

### 36.4 为什么删 VV 侧 NR（EXP-0083）

第一版让 VV 自己调 `nrRecord`。RT 探针在 `_nvngx.dll` 上装了 `EvaluateFeature` detour，
VV 的 DLSS evaluate 也会经过它，于是同一帧出现两次 `nrRecord`：

- 桥的 encode/composite 描述符槽固定（6..10 / 12..16），后一次写入覆盖前一次的 encode 输入；
- 第二次以参数对象里的 `Color`（上一次的 `gDecodedColor`）为 base，合成结果又回灌输入，
  形成自反馈；实测 `out_after_w1.bmp max=5.96e-08`，画面几帧内全黑。
  证据 `docs/agent/evidence/nr-vv-black-EXP-0083.log`、`nr-vv-detour-EXP-0083.log`。

修复没有选「detour 对 VV handle 透传」（EXP-0084 已取消），而是**删掉 VV 侧编排**：
detour 本来就是为「在引擎 DLSS 的 EvaluateFeature 前插入 NR」写的，VV 的 DLSS evaluate
正好提供同样形状的参数对象。这样每帧只有一次 `nrRecord`，代码也只有一份 NR 实现。

### 36.5 判据（EXP-0085）

| # | 判据 | 不成立 |
|---|---|---|
| N1 | `[nr]` detour 出现 create / record；`[eval]` 只显示 VV SR handle（c/d/mv/o 非空）；不再有 `[vvsr] nr` 行 | 按 `[nr]` 返回码判断模型/驱动/会话 |
| N2 | 画面不黑、`sr arg10` 连续 `eval=1`、HUD/手/准星保留 | 查 NR 输入域与合成纹理；同帧 dump |
| N3 | 关掉 `nr.enabled` 后回到无 NR 输入，不崩 | 查 detour 的 falling-edge Color 归还 |

代价：NR 前档 ~3.5 ms @1485×835（RT 实测），叠加 DLSS ~1.5 ms。首次启用有模型加载的一次性卡顿。

### 36.6 EXP-0085 的两个新问题（已修，待 EXP-0086/0087）

**退出崩溃**（trace `nr-vv-exit-crash-EXP-0085.log`）：栈是
`evaluateSr → ngxEvaluateDetour:1146 → original DLSS`，崩在 `nvwgf2umx.dll`。日志里
`[nr] 把引擎 Color 交还 color=0x17609432720` 的指针就是 `colorHalf`（`[desc] fmt=10 1280x720`），
而 `vv::sr::release()` 已经先把它释放了；`nrShutdown` 在释放之后又把该悬空指针写回 NGX 参数对象，
下一帧 DLSS evaluate 读它。修法：

- `VvSr` 增加 `gRecordMutex` / `gReleased`：`recordInternal` 全程持锁，`release()` 先置
  `gReleased` 再等锁，保证录帧结束后才释放纹理/PSO；
- `NeuroDragon::disable()` 先 `vv::sr::setEnabled(false)`（停注入），再 release；
- `nrShutdown()` 退出时**不再**回写 `Color`（`nrUnload()` 仍保留回写，它跑在渲染线程）。

**周期性闪动**（EXP-0085 用户「明显的闪动」）：`[nr] 窗口 #0/#1/#2` 每 600 次求值
（~12 s）开一个诊断窗口，窗口起点 `nrReset = true` 给 NR 发 `Reset=1`，时域历史被清空。
诊断窗口只该管计时/dump。修法：窗口起点不再 reset；换世界/进出存档时改发一次
`nrRequestReset()`（NR + engine DLSS 双侧），显式重置热键不变。换世界的监听器现在**无论
启动时 NR 是否开启都注册**，因为 VV 是在面板里运行时开启 NR 的；否则 detour 收不到换关卡的
信号，NR 历史会跨世界残留。EXP-0087 实机确认窗口 reset 已消失，但**移动时仍闪** ⇒ 另有原因。

### 36.7 移动才闪：MV 单位（EXP-0087 fail → EXP-0088）

VV 的 `mvRg32` 已经是**渲染像素**（prep 里 `{-z,+w} * RenderResolution`），而 NR detour
沿用了 RT 的归一化契约：把参数对象的 `Width/Height`（1280/720）当 MV scale 传给
`nrRecord`。更糟的是 `nrRecord` 会把**共享参数对象**的 unprefixed `MV.Scale.X/Y` 写成
`guideWidth/Height`，随后 original DLSS evaluate 也读这个值 ⇒ NR 和 DLSS 都把像素向量再乘
1280。静止时 MV≈0 看不出来，移动时历史重投影飞天，表现就是闪/拖影。

修法：

- detour 从参数对象读 `MV.Scale.X/Y`（float）作为 `nrRecord` 的 `motionVectorScale`：
  VV 是 1.0，RT 是 Width/Height；读不到才退回 width/height；
- `nrRecord` 的 unprefixed `MV.Scale.X/Y` 改成写**同一个有效 scale**（四舍五入，最小 1），
  不再写 guideWidth/Height；变化时打一行 `[nr] MV scale x,y (guide WxH)`；
- `evaluateSr` 在 evaluate 后回读一次 `MV.Scale.X`（`[vvsr] sr readback`），证明 detour
  没有再把 1.0 覆盖成 1280。

方向没有改：VV 的 `{-z,+w}` 与 RT 的 MV 都是 current→previous；差异在单位/缩放，不在符号。

**EXP-0088 结果**：`[nr] MV scale 1.000,1.000 (guide 1280x720)`；用户「已完成，无闪动」；
12:56:31 正常退出，无新 crash dump。证据 `docs/agent/evidence/nr-vv-mv-scale-EXP-0088.log`。

## 37. TAAU jitter → DLSS 的契约与判别实验（2026-10-29，已判别：帧没有按它渲染）

全文与证据链在 `research/2026-10-29-taau-jitter-dlss-contract.md`，地址在 canon `taau_jitter_table`。
这里只放结论与待办。

**已确认（静态 + 原始日志）**：

- 值 = `float2(VanDerCorput(2,k+1), VanDerCorput(3,k+1)) - 0.5`，`k = mCurrentJitterIdx`（**0 基、每帧 +1、16 项回绕**）；
  与 RTXPT 的 Halton 抖动是同一序列，只差相位数量（引擎 16，RTXPT 取 `max(32, 8×面积比)`）。
- 单位 = **渲染分辨率像素**、屏幕 **y 向下**：TAAU PS 是 `srcPos = displayCenter*ratio - jitter - 0.5`，jitter 从
  渲染像素坐标里直接减；那个 `-0.5` 是像素中心约定，不属于 jitter。
- DLSS 侧要的是同一个量：RTXPT 把 `View::GetPixelOffset()` 原样交 `slConstants.jitterOffset`，只对光线相机翻 Y；
  同一 View 出来的 MV 也是像素单位。**MV 需要 `(-z,+w)`（翻 Y + 时间方向取反），jitter 两项都不需要。**
- 因此「直传前要 ×ratio / ×0.5 / ±0.5」已进 `canon/refuted.jsonl`：直传在数值上就是对的。

**已判别（EXP-0100，四对 F7 双采 + 离线亚像素测量）**：世界帧**没有**按这个偏移光栅化。

- 方法：站定不动，每对 dump 相隔 ≈131 帧；两张 `sr_colour_*`（喂给 DLSS 的 render-size 颜色，1706x960 fmt26）
  用相位相关 + 1/50 px 上采样 DFT 求位移（工具 `_local/tools/jitter_shift.py`，用真实 dump 造已知位移标定到 ±0.01 px）。
- 结果：`|Δj|` = 0.758 / 0.486 / 0.161 / 0.462，对应实测位移 = 0.063 / 0.216 / 0.331 / **0.028** px。
  最干净的一对（相关峰最高、五个区域全是 0.02）在 `|Δj| = 0.462` 时只动了 0.02 px；`|Δj|` 最大的一对只动了 0.06–0.13。
  位移与 `|Δj|` 无关，较大残差随取景区域变化（贴图动画）。
- ⇒ 唯一真实的 `Jitter.Offset` 是 **0**（产品默认）；TAAU 自己用这个值只是它重采样核的相位，不要求世界帧真的抖过。
- 仍未解释（记账，不当证据）：用户 A/B 里 `-j(n)` 与 `j(n-1)` 看着也稳、只有 `j(n)` 抖；按测量它们同样注入
  ~0.37 px 假相位，几何上应与 `j(n)` 同量级，疑为 DLSS 历史权重差异或一次观感漏判。

**判别实验**（每个改几行 + 面板切换，不需要新工具；`j(n)` 为当帧 ctx 值）：

| 喂给 DLSS | 帧没按它渲染 | 帧有、符号正确 | 帧有、符号反 |
|---|---|---|---|
| `j(n)`（现状） | 抖 | 稳（不该抖） | 抖 |
| `-j(n)` | 幅度几乎不变 | 幅度翻倍 | 稳 |
| `j(n-1)` | 抖 | 抖 | 抖；**稳** ⇒ 相位差一帧 |

- `-j(n)` 幅度翻倍 ⇒ 值与帧对得上，别再改符号或缩放，去查相位 / depth / MV 一致性；
- `-j(n)` 幅度不变 ⇒ 这一帧确实没按它渲染 ⇒ 只能喂 0；想要亚像素采样增益就得在渲染侧自己施加 jitter（另一个工程）；
- `j(n-1)` 稳 ⇒ 引擎 ctx 的 jitter 领先渲染一帧，属相位 bug，修 source。

**顺序**：产品侧维持 jitter = 0（`VvSr` 默认；四档诊断只剩 `0 / j(n) / -j(n) / j(n-1)` 供复查）。
若要拿回 66% 下的亚像素采样，得在 `createDeferredRenderingCameras`（直写 factor 的同一个钩子）里**自己抖投影**，
再把同一个值交给 DLSS —— 这是新工程，不在本轮。depth 语义（`near/(1-d)` 与 `DepthInverted=1`）仍待收口：
它影响的是重投影精度，与 jitter 是否喂值相互独立。
