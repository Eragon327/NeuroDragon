# VV 路径（阶段 2）：把 NR/SR 接进 Vibrant Visuals，替代 TAAU

> **状态**（2026-10-25 16:0x 更新）：阶段 2 **进行中**。M4 侦察与 M5（注入可行）已收口；
> M6 的 B0（只读取证）与 B1a（自建 NGX 会话）已实机通过；B1b（颜色旁路）**切点成立、颜色未定**。
> 下一步以 **§14** 为准；§5.2、§12 A 两处结论已被 §14.3 更正。
> 本文由旧 `O`（VV 路径侦察）、旧 `M §3`（阶段 2 的未知）与旧 `I §5`（M4–M7）合并而来；
> 原文归档在 [archive/2026-10](../archive/2026-10/) 与 [research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)。
>
> **验证环境**：`E:\LeviLauncher\versions\1.26.51.01-agent2`（本工作区，2560×1440 全屏）。
> **注意**：主目录 `E:\LeviLauncher\versions\1.26.51.01` 是另一个 agent 的并发工作区，
> 部署/实验不要碰它。

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

### 2.3 四个依赖的语义（按源码顺序，与 Mac 一致）

| 参数 | 依赖 | 材质 uniform | 语义 |
|---|---|---|---|
| arg9 | `[0]` | `InputTAAHistory` | 上一帧 TAAU 结果（渲染分辨率） |
| arg10 | `[1]` | `InputFinalColor` | 本帧颜色（渲染分辨率） |
| arg11 | `[2]` | （无名字） | **输出目标（显示分辨率）** |
| arg12 | `[3]` | `InputBufferMotionVectors` | 运动矢量 |

⇒ 探针日志里的标签（`history/color/output/motion`）按此修正；早期版本把它们错标成
`color/history/motion/output`。

## 3. 运行时事实（2026-10-24 21:00 / 21:14 两次运行）

主菜单与进世界后一致的常数：

| 项 | 值 | 含义 |
|---|---|---|
| taau 声明 / draw | **1/帧 各一次** | 插入点是「每帧一次」的稳定事件，不是每对象 |
| `mode` | 0（Taau） | 确实是 TAAU 分支 |
| `factor` | **0.3300** | 渲染/显示比 = 33%（2560×1440 → 约 844×475） |
| `dynScale` | 1.0000 | 动态分辨率当前没缩放 |
| `jitter` | 每帧递增回绕（2,6,10,14,…） | 亚像素抖动在跑 ⇒ DLSS 需要的 jitter 可直接取 |
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
| jitter | `UpscalingData.jitterIndex` → 引擎 jitter 表 | 需确认是否已进投影矩阵 |
| 输出 | 后缓冲（倒 4 之后写入） | 已是本 spec 的插入点 |

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

### 17.3 运行时切换（不碰 options.txt）

- 入口：`ClientInstance::getOptions()` → `IOptionRegistry` → `Option::load("1")`。
  EXP-0020：load 之后再无一条 `[vv] taau #`（含随后进世界），退出时引擎写回 `mode=1`。
- **必须在客户端线程**。`ll::thread::ClientThreadExecutor` 在启动早期投递的任务不保证及时执行
  （EXP-0018/19：hand-off 有日志，任务没跑或很晚才跑）；可靠做法是每 2 s 重投，
  或直接在 `ClientLevelJoinEvent` 里调用（那一刻就是客户端线程）。
- 测试开关：`<mod>\config\force_bilinear.flag` 存在时，TEST 构建在 load 时排队、Present 投递、
  join 事件直接调用。日志序列：`marker … present=1` → `requested` → `handing…` →
  `task running` → `scan done` → `load … -> 1` → `reread = 1`。

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

### 17.5 快照时机（P0-2，已实机验证）

- TAAU draw callback 拿到的 context 里，`+0x240` 还是**上一帧**的 VP；引擎在自己的 draw 里会把
  prev 覆盖成 cur。所以在提交后（ExecuteCommandLists）再读，永远是 `prev == cur`。
- 修法：`drawDetour` 转发前快照 `+0x200/+0x240/+0x1e4/+0x280`，`taaMatrices()` 只读快照。
- EXP-0021 实机：走动时 `[vvsr] mv snapshot: previous VP differs from current (frame 298, delta=…)`；
  站定 `prev_same=1`。

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
