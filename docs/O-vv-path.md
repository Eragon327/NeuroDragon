# O. VV 路径侦察（阶段 2 / M4）

> 目标：把 `docs/M-roadmap.md` §3 的五个未知答掉，先只读、不改画面。
> 本文只记**实测**结论；机制来源另外标注（macOS IDB = 1.26.32，Windows = 1.26.5101.0）。
> 验证环境：`E:\LeviLauncher\versions\1.26.51.01-agent2`（2560×1440 全屏）。

## 0. 一句话

```
VV 下的 TAAU 每帧恰好 1 次声明 + 1 次 draw，跑在 factor 0.33、jitter 递增的常态上；
引擎每帧只发 1 次 ExecuteCommandLists、里面 48 个 command list —— 「帧中间插一段 GPU 工作」
在队列层面第一次有了可操作的抓手（待确认边界落点）。
```

## 1. 触发条件（用户口述 + 实测钉死）

`assets` 里的 `options.txt` 三条同时满足，`taau` 才会真正跑：

| 键 | 值 | 说明 |
|---|---|---|
| `graphics_mode` | `2` | Vibrant Visuals（延迟渲染）；`3` = 开光追（走 RTX 帧渲染器） |
| `upscaling_mode` | `0` | `0 = Taau`，`1 = Bilinear`。**Bilinear 根本不进 taau 分支**（[A §7.4](A-renderdragon-passmap.md) 的历史结论在此再次成立） |
| `upscaling_percentage` | 非 0 且 < 100 | 2026-10-24 用 `33`；`0` = 原生 100%，此时上采样无意义，taau 不触发 |

实测：设成上面三行后，**连主菜单**都在跑 taau（菜单背景全景也走 VV 上采样），所以后续探针不需要进世界也能验证 hook 是否活着。

## 2. Windows 侧定位（无 PDB，离线可复现）

锚点 = 代码自己构造的材质 uniform 名，每个名字在 exe 里**只有 1 处 RIP 相对引用**：

| 字符串 | 命中函数 | RVA | 结论 |
|---|---|---|---|
| `InputFinalColor` / `InputBufferMotionVectors` / `InputTAAHistory` / 材质 `Upscaling` | `dragon::framerenderer::modules::taau`（declarator） | `0xb74f860` | 每帧 1 次，拿到 viewport / `UpscalingData` / 4 个 framebuffer 依赖 |
| `SubPixelJitterAndValidHistory` / `ResolutionRatiosAndFPEpsilon` / `TAAUpscalingParameters` / `TAAUpscaling` | TAAU 的 draw callback（`std::function` 目标） | `0xb752820` | 每帧 1 次，纹理已解析 |

- 工具：`D:\Documents\NeuroDragonWorkSpace\tools\vv_locate.py`（PE 解析 → 找串 → 扫 `.text` 的 RIP 相对引用 → 用 `.pdata` 反查函数边界），`vv_disasm_args2.py`（capstone 反汇编 + 追参数）。
- 两个函数的序言都是 `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC`，所以探针在安装前先校验 15 字节，不匹配就拒绝挂钩并报错（`[vv] taau prologue mismatch`）。
- 结构对照：Mac `taau` 0x1348 / 回调 0x10b2；Windows 0x1e33 / 0x1767（MSVC + 19 个补丁的体量差，语义一致）。

### 2.1 参数基址（踩过的坑）

`taau` 序言是 **8 个 push + `sub rsp, 0xd58` + `lea rbp, [rsp+0x80]`**，所以「第一个栈参数」不是 `[rbp+0xd00]` 而是 **`[rbp+0xd40]`**（漏算 8 个 push 的 0x40）。按 0xd40 起算，参数 5–12 = viewport / `UpscalingData` / `UpscalingCameraData` / `MeshFilter` / 4×`BindFramebufferColor`。

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

Mac 1.26.32 读的是 `+0x10`。**两者布局差 8 字节，跨版本迁移时必须以本地反汇编为准**。

### 2.3 四个依赖的语义（按源码顺序，与 Mac 一致）

| 参数 | 依赖 | 材质 uniform | 语义 |
|---|---|---|---|
| arg9 | `[0]` | `InputTAAHistory` | 上一帧 TAAU 结果（渲染分辨率） |
| arg10 | `[1]` | `InputFinalColor` | 本帧颜色（渲染分辨率） |
| arg11 | `[2]` | （无名字） | **输出目标（显示分辨率）** |
| arg12 | `[3]` | `InputBufferMotionVectors` | 运动矢量 |

⇒ 探针日志里的标签（`history/color/output/motion`）已按此修正；先前那版把它们错标成 `color/history/motion/output`。

## 3. 运行时实测（2026-10-24 21:00 / 21:14 两次运行）

主菜单与进世界后一致的常数：

| 项 | 值 | 含义 |
|---|---|---|
| taau 声明 / draw | **1/帧 各一次** | 插入点是"每帧一次"的稳定事件，不是每对象 |
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

## 4. 依赖对象里能读出什么

`BindFramebufferColor + 0x8` 指向的对象，前 0x100 字节里有**可打印字符串**（生产者通道名），实测出现过：

- `DoDeferredLighting`（与 [A §7.2](A-renderdragon-passmap.md) 的"MV 是延迟着色 G-buffer 附件"一致）
- `PreDeferredLighting`

⇒ 这个对象携带资源来源信息，不是裸纹理。**它还不足以直接给出 `ID3D12Resource`**，所以 `bgfx 句柄 → ID3D12Resource` 这条桥（[A §7.3](A-renderdragon-passmap.md)）仍未打通（下一步，见 §6）。

### 4.1 依赖对象里没有 `ID3D12Resource`（已证否）

探针 v5 用"纹理扫描"验证过：把依赖对象、它 `+0x8` 指向的节点、以及 `+0x10` 那一跳的 qword 逐个当候选，用
"vtable 必须等于已知 `ID3D12Resource` 的 vtable"做过滤后调 `GetDesc` —— **四个依赖、三个层次，一次都没命中**。
所以帧图节点里存的是 bgfx 侧的标识，不是 D3D12 资源；桥必须从 bgfx 那一侧搭。

> 安全备注：早期版本只校验"vtable 落在某个已加载模块"，随机 qword 命中设备/队列对象时会调错虚函数直接崩客户端。
> 现在的过滤是"vtable 精确等于某个真实资源的 vtable"（取自 `CreateRenderTargetView` 的第一个参数），命中率为 0 但不会崩。

### 4.2 渲染目标清单：必须在"重建资源"之后才看得到

引擎的 D3D12 设备与渲染目标**在模组加载之前就建好了**，所以 `D3D12CreateDevice` / `CreateRenderTargetView` 钩子
在正常启动时只能看到 overlay 自己的临时设备和它的后缓冲。**按一次 F11（切全屏 → 引擎重建资源）之后，
44 个渲染目标立刻全部出现在日志里**——这是以后抓引擎资源的通用手法。

实测清单（窗口 2560×1440、factor 0.33）：

| 尺寸 | 格式（DXGI） | 判读 |
|---|---|---|
| 2560×1440 | 26 / 28（R8G8B8A8_TYPELESS / _UNORM_SRGB） | 交换链后缓冲（两块） |
| 844×475 | 10 / 11 / 12（R16G16B16A16_FLOAT / _UNORM / _UINT） | 渲染分辨率的颜色与 G-buffer |
| 844×475 | 26 / 28（RGBA8） | 渲染分辨率的 LDR 面 |
| 844×475 | 41 / 54 | 深度（D32 一类，待读准格式） |
| 422×225 … 13×7 | 26 | bloom 金字塔 mip 链 |

**关键异常**：清单里**没有**"显示分辨率的 HDR 目标"（不存在 2560×1440 的 10/11/12）。也就是说这一版 VV 的
TAAU **很可能直接写进后缓冲**（8 bit SDR），色调映射发生在 844×475 那一层——与 [A §2](A-renderdragon-passmap.md)
记录的 macOS 1.26.32 顺序（taau → 亮度 → bloom → 调色）不一致。这条**待确认**，确认方式见 §6。

## 5. 对阶段 2 的意义（哪些未知已答）

对应 [M §3](M-roadmap.md) 的表：

| # | 未知 | 结论 |
|---|---|---|
| 1 | VV 下 `taau` 是否命中 / NGX 是否已被引擎 Init | ✅ **命中，1/帧**；VV 下没有引擎 NGX 调用（`[sr] ctx` 只是渲染器配置对象），所以 DLSS 必须我们自己建 feature |
| 2 | bgfx 句柄 → `ID3D12Resource` | ⬜ 未完成；**已证否**"依赖对象里直接有 D3D12 资源"（§4.1），桥必须从 bgfx 侧搭；渲染目标清单已能靠 F11 重建抓到（§4.2） |
| 3 | 深度从哪来 | 🟡 有候选：渲染分辨率下两个深度面（格式 41/54），taau 的 4 个输入里确实没有深度；下一步读准格式并与延迟着色对上 |
| 4 | 写输出目标是否被后续 pass 依赖 | ⬜ 未做；但清单里没有显示分辨率的 HDR 面 ⇒ 怀疑 TAAU 直接写后缓冲，若成立"写输出"就是写后缓冲，比预想简单 |
| 5 | VV 的尺寸语义 | ✅ `factor=0.33` × 显示尺寸；`target_resolution` 是绝对目标，DRS 换算成 factor（与 [I §1](I-sr-final-plan.md) 的 R_in 口径一致） |

**新增结论（对实现最关键）**：引擎**每帧只调一次 `ExecuteCommandLists`，但一次带 ~48 个 command list**。钩子可以把这个调用**拆成"前缀 + 我们自己的 list + 后缀"**分别在同一个队列上执行，因此"在 TAAU 之后、调色/UI 之前"插入我们自己的命令**在队列层面是可行的**——前提是能识别"哪一段 list 边界正好在 TAAU 之后"。

## 6. 提交侧：帧是一次性的命令流，边界只能靠"哪段 list"来认

探针 v4 把 command list 的关闭时序也量了（`Close` 是最安全的一个槽：无参数、每次提交 48 次、转发不会错）：

| 事实 | 数值 |
|---|---|
| 一帧的 `Close` 次数 | **49**（与提交的 48 个 list 相当，多一个是收尾） |
| 这 49 次 `Close` 的时间跨度 | **约 3.3 ms**，全部发生在**同一个线程**（渲染线程） |
| 与 taau 回调的时间关系 | taau 回调（录制相）在 480 ms 之前；两者不在同一时间窗 |

⇒ **bgfx 并不是"边录边提交"**：整帧先在 CPU 上录完，再由渲染线程一次性把 48 个 list 编码/关闭/提交。
因此"在 taau 那一刻拿到正在录制的 command list"这条路不成立；能用的只有 §3 那条：**拆 `ExecuteCommandLists`**。

### 6.1 引擎用的是 Agility SDK 的 `D3D12Core.dll`（这是关键）

实测（`[vv] engine list vtable=… slot9=D3D12Core.dll+0x232dd0 slot12=… slot25=… slot28=…`）：

- 引擎提交的 command list，其 vtable 全部落在 **`D3D12Core.dll`**，不是系统 `d3d12.dll`；
- 所以钩 `d3d12.dll` 导出的 `D3D12CreateDevice` 只能抓到 overlay 自己的临时设备，**引擎的设备和它的 `CreateRenderTargetView` 一个都看不到**
  （之前"设备在模组加载前就建好"的解释是不完整的，真正原因是运行时不是同一个模块）；
- 正确入口：拿引擎提交的某个 command list 调 `ID3D12CommandList::GetDevice`（在独立 TU `probe/VvCalibrate.cpp` 里做，因为要用 `__uuidof`），
  再钩那个设备的 `CreateRenderTargetView`。**渲染目标清单从此可见**（仍需按一次 F11 触发引擎重建资源）。

### 6.2 槽 25 **不是** `OMSetRenderTargets`（实测否决，别再试）

在引擎的 D3D12Core 列表上钩槽 25，第一次调用收到的寄存器是 `rdx = 堆指针、r8 = 0xFC5D1543EC5F01` 这种"一个指针"的形状，
不是 `(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE*)`。转发后 `D3D12Core.dll` 把 `rdx` 当结构体指针解引用，
在 `0x6F214130 + 0xCC` 处 AV（用户调试器里能看到 `omSetRtvDetour` 行 251）。

⇒ **结论：这一版运行时不能按公开头文件的槽号去挂钩 `ID3D12GraphicsCommandList` 的方法（除槽 9 `Close` 已验证可用）。**
要拿绑定信息，只能先把槽号**测**出来（见 §7 第 2 条）。

**踩过的坑（重要）**：按公开的 D3D12 vtable 顺序去猜 `ID3D12GraphicsCommandList` 的方法槽是会翻车的。
`Close`（槽 9）猜对了；但**槽 25 猜成 `OMSetRenderTargets` 是错的**——它被调用时参数是
`this=0x…, a1=0xad3b5670, a2=0xfc5d1543ec5f01`（不是 `(UINT, 描述符数组)`），转发两次后客户端直接崩。
结论：**没有验证过的槽不要挂钩**；要认槽，先拿一个已知对象做标定（见 §7 第 2 条）。

## 7. 下一步（按风险排序）

1. **用"内容"认纹理，不再依赖槽号**：渲染目标清单已经有了（§6.1），把显示分辨率那几个 8 bit 候选
   （以及渲染分辨率的 10/11/12）在 present 时拷到回读缓冲、写成 BMP，肉眼一看就知道哪个是 TAAU 的输出
   （放大后的画面、无 UI）、哪个是 HDR 场景色。这条路只用 `CopyResource` + fence，**不碰任何未验证的槽**。
2. **槽位标定（若 1 不够用）**：用我们自己的 command list 反推槽号——但 2026-10-24 22:0x 的第一次尝试
   （32 个"记录并转发"detour）在 `nvwgf2umx.dll` 里崩了（AV @0x2b88），说明批量挂钩本身有风险；
   若再来，改成**一次只挂一个槽 + 只调用一次**，并且先在临时设备上做，确认无崩溃再上引擎设备。
3. **边界实验**（用户可见的验收）：在候选 list 边界把目标纹理清成纯色——画面变纯色而 UI 仍在 ⇒ 边界对了；
   等价于 [I §5](I-sr-final-plan.md) 的 M5，但**不需要**先建 DLSS。配合 48 个 list 可以逐帧扫。
4. **深度源**：844×475 的 41/54 两个面里必然有深度（清单已给出候选）；下一步把它们的格式读准（`DXGI_FORMAT` 41/54）
   并与延迟着色的 `BindFramebufferDepth` 对上。

## 7. 证据与工具

- 代码：`src/neurodragon/probe/VvProbe.{h,cpp}`（只读探针），`ui/Overlay.cpp` 里两处计数回调。
- 日志标记：`[vv] taau #N viewport=…`（每 300 次）、`[vv] taau #1/#2 …resText=[…]`（前两帧 dump）、`[vv] presents=… submits=… lists=…`（前 10 帧 + 每 600 帧）、`[vv] submit #N tid= lists= qpc=`（前 24 次）。
- 工具：`NeuroDragonWorkSpace\tools\vv_locate.py`、`vv_disasm_args2.py`、`rtti_names.py`、`vv_ngx_route.py`（均不依赖 IDA；capstone 走 pip 装）。
- 版本约束：所有 RVA 只对 `1.26.5101.0` 成立；换版本先用 `vv_locate.py` 重新定位（函数每帧都在跑，串锚点很稳）。
