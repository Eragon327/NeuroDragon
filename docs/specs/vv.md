# VV 路径（阶段 2）：把 NR/SR 接进 Vibrant Visuals，替代 TAAU

> **状态**：阶段 2 未开工；M4 侦察（只读）已有结论，五个未知答掉两个。
> 本文由旧 `O`（VV 路径侦察）、旧 `M §3`（阶段 2 的未知）与旧 `I §5`（M4–M7）合并而来；
> 原文归档在 [archive/2026-10](../archive/2026-10/) 与 [research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)。
>
> **验证环境**：`E:\LeviLauncher\versions\1.26.51.01-agent2`（2560×1440 全屏）。
> **注意**：`1.26.51.01-agent2` 是另一个 agent 的并发工作区，部署/实验不要碰它；
> 主目录是 `E:\LeviLauncher\versions\1.26.51.01`。

## 0. 一句话

```
VV 下的 TAAU 每帧恰好 1 次声明 + 1 次 draw，跑在 factor 0.33、jitter 递增的常态上；
引擎每帧只发 1 次 ExecuteCommandLists、里面 48 个 command list ——「帧中间插一段 GPU 工作」
在队列层面第一次有了可操作的抓手（边界落点仍未确认）。
```

## 1. 触发条件（用户口述 + 实测钉死）

`options.txt` 三条同时满足，`taau` 才会真正跑：

| 键 | 值 | 说明 |
|---|---|---|
| `graphics_mode` | `2` | Vibrant Visuals（延迟渲染）；`3` = 开光追（走 RTX 帧渲染器，是另一条路径） |
| `upscaling_mode` | `0` | `0 = Taau`，`1 = Bilinear`。**Bilinear 根本不进 taau 分支** |
| `upscaling_percentage` | 非 0 且 < 100 | 实测用 `33`；`0` = 原生 100%，上采样无意义，taau 不触发 |

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
| 持有交换链后缓冲引用 | `ResizeBuffers` 失败 → 引擎 `abort()`（已发生：22:39 / 22:51 两次 `0xC0000409`） | **拿到的后缓冲引用必须当场 `Release`**；只在 `noteSwapChain()` 的调用内部检查、不常驻持有 |
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
