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
| 每帧 command list 总数 | **48–51** | 引擎把一帧切成 ~48 个 list 再一次性提交 |
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
| 2 | bgfx 句柄 → `ID3D12Resource` | ⬜ 未完成；**已证否**「依赖对象里直接有 D3D12 资源」（§4.1），桥必须从 bgfx 侧搭；渲染目标清单已能靠 F11 重建抓到（§5.1） |
| 3 | 深度从哪来 | 🟡 有候选：渲染分辨率下两个深度面（格式 41/54），taau 的 4 个输入里确实没有深度；下一步读准格式并与延迟着色对上 |
| 4 | 写输出目标是否被后续 pass 依赖 | ⬜ 未做；清单里没有显示分辨率的 HDR 面 ⇒ 怀疑 TAAU 直接写后缓冲，若成立「写输出」就是写后缓冲，比预想简单 |
| 5 | VV 的尺寸语义 | ✅ `factor=0.33` × 显示尺寸；`target_resolution` 是绝对目标，DRS 换算成 factor（与 [sr §3](sr.md) 的 R_in 口径一致） |

**新增结论（对实现最关键）**：引擎**每帧只调一次 `ExecuteCommandLists`，但一次带 ~48 个
command list**。钩子可以把这个调用**拆成「前缀 + 我们自己的 list + 后缀」**分别在同一个队列上执行，
因此「在 TAAU 之后、调色/UI 之前」插入我们自己的命令**在队列层面是可行的**——前提是能识别
「哪一段 list 边界正好在 TAAU 之后」。

## 7. 下一步（按风险排序）

1. **用「内容」认纹理，不再依赖槽号**：渲染目标清单已经有了（§5.1），把显示分辨率那几个 8 bit 候选
   （以及渲染分辨率的 10/11/12）在 present 时拷到回读缓冲、写成 BMP，肉眼一看就知道哪个是 TAAU 的输出
   （放大后的画面、无 UI）、哪个是 HDR 场景色。这条路只用 `CopyResource` + fence，**不碰任何未验证的槽**。
2. **槽位标定（若 1 不够用）**：用我们自己的 command list 反推槽号——但 2026-10-24 22:0x 的第一次尝试
   （32 个「记录并转发」detour）在 `nvwgf2umx.dll` 里崩了（AV @0x2b88），说明批量挂钩本身有风险；
   若再来，改成**一次只挂一个槽 + 只调用一次**，并且先在临时设备上做，确认无崩溃再上引擎设备。
3. **边界实验**（用户可见的验收）：在候选 list 边界把目标纹理清成纯色——画面变纯色而 UI 仍在
   ⇒ 边界对了；等价于 M5，但**不需要**先建 DLSS。配合 48 个 list 可以逐帧扫。
4. **深度源**：844×475 的 41/54 两个面里必然有深度（清单已给出候选）；下一步把它们的格式读准
   （`DXGI_FORMAT` 41/54）并与延迟着色的 `BindFramebufferDepth` 对上。

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
  `__uuidof` / `d3d12.h` / BMP 回读都在这里）、`ui/Overlay.cpp` 里的回调
  （`onPresent` / `noteSwapChain` / `setGameQueue` / `onExecuteCommandLists`）。
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
