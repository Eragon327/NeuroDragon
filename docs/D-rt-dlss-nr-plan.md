# D. RT 路线 + 复用引擎 DLSS 的 NR 接入规划

> 目标客户端 `1.26.5101.0`
> （`E:\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe`——测量时在
> `D:\Program Files\LeviLauncher\…`，同日搬盘，文件不变；
> PE 时间戳 `0x6aa482fd`，`.text` = RVA `0x1000` + `0xe5a5ab6`）。
> 机器：NVIDIA GeForce RTX 5070 Laptop（RT/DLSS4 可用）。

## 0. 一句话结论

**超分白送、NR 已跑通、只剩色彩**：

- 引擎自己加载 NGX、每帧调 DLSS（RT 下）：1485×835 → 2560×1440，实测 **~1.64 ms**；
- 我们自己的 NR（NGX feature `0x12`）**已经能在游戏内建起来并逐帧 evaluate**：
  **~3.5 ms/帧**（1485×835，1:1），输出画面与场景对得上（树/草/岩浆/手臂都在）；
- 唯一剩下的问题是**色彩与合成**：直接输出比 Color 暗（§6.13 给了三条路线）。

**接入的正确姿势**（§6.12 定稿）：驱动 **≥ 617.42**；`nvngx_dlssnr.dll`（**规范名**）放**应用目录**
（游戏根目录）；用 **core** 的 `CreateFeature(0x12)`，**不要**走模型自己的 `Init_Ext`。

阅读顺序：§6.12（现在怎么做）→ §6.13（实测数据）→ §6.14（下一步）。§6.3 / §6.10 是被推翻的中间假设，
只留作过程证据。

## 1. 已验证的事实

| # | 事实 | 证据 |
|---|---|---|
| 1 | RT 与 VV 是**两条完全独立的渲染管线** | RT 下 VV 全部锚点（taau / taau-caller / rendererB:*）命中 0；VV 下 taau 53854 次 |
| 2 | **RT 下 DLSS 真的在跑** | `NVSDK_NGX_D3D12_CreateFeature` 1~2 次、`EvaluateFeature` 每会话 4k~12k 次 |
| 3 | 引擎启动即加载 NGX | `_nvngx.dll`（DriverStore）+ `nvngx_dlss.dll`（版本目录，54 MB）在 0.3 s 内先后 LoadLibrary |
| 4 | 四张输入纹理可取：`SetPtr` 日志与参数对象 `Get` **两边逐位一致**（见 §6.5） | `Color 0x…74550 / Depth 0x…4d250 / MV 0x…8d0 / Output 0x…d250`，`Output` 双缓冲交替 |
| 5 | 参数可读且完整 | 见 §2 参数表（jitter / exposure / MV 像素尺度都在） |
| 6 | DLSS 的调用点在游戏侧可定位 | `EvaluateFeature` 的调用者 = `base+0xe413610`（thunk 内），其真正调用方 = 函数 `base+0xdfc82b0`（`0xad18` 大小） |

## 2. 参数表（引擎每帧写入 NGX 的内容，实测）

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

⇒ 引擎具备 **DLSS Ray Reconstruction 的完整参数位**（当前全 0）。RT 路线上 RR 可能比普通 SR 更合适，
是独立的一条可评估路线，不与 NR 冲突。

## 3. 关键代码位置（1.26.5101.0）

| 位置 | 说明 |
|---|---|
| `base+0xdfc82b0`（`0xad18`） | 游戏侧 RT DLSS 阶段：逐参数 `SetPtr/SetF/SetUI`，然后调 thunk 触发 Evaluate |
| `base+0xe4135a0`（thunk） | 包装 `EvaluateFeature` 的调用；返回地址固定为 `base+0xe413610` |
| `base+0xe413740` | SDK inline `SetPtr` 的编译产物 → `vtable[1]`（指针/资源） |
| `base+0xe4136e0` | `SetUI` → `vtable[3]`（unsigned） |
| `base+0xe413630` | `SetF` → `vtable[6]`（float） |

三者签名（`.text` 内唯一，已用于挂钩）：

```
SetPtr 48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 4C 8B F1 49 8B F8 48 8B F2 48 8B 58 08 48 8B CB
SetF   48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 48 8B F1 0F 29 74 24 20 48 8B FA 0F 28 F2
SetUI  48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 4C 8B F1 41 8B F8 48 8B F2 48 8B 58 18 48 8B CB
```

## 4. 踩过的坑（都真实崩过游戏，别再犯）

1. **NGX 参数对象：`Set` 组顺序与 SDK 头文件不同；读资源实测走 `Get` 8/9**（反汇编 + 进世界那轮实测）。
   本机 runtime 的 `Set` 槽位（游戏侧 helper → `[rax+off]`）：`0xe413740` 指针/资源 → **1**、
   `0xe4136e0` unsigned → **3**、`0xe413630` float → **6**；SDK 头文件是 `1`=float、`6`=ID3D12Resource\*，
   两者 **1 与 6 互换**。`Get` 侧 `[slot]` 扫描结果：**8 与 9 都返回同一个资源指针（ret=0x1）**，
   `10..15` 全是 `ret=0xBAD00010`，`11` 读 `Width` 成立。
   ⇒ 早期按 `0xe4137a0` 的 `[rax+0x70]` 猜「资源走 14」**是错的**（该 helper 操作另一种参数对象）。
   读资源顺序：**8 → 9 → 14 兜底**，并打印返回值。
2. **禁止盲探测参数对象**：依次调用 slot 0..7 写假键，可能命中 `Reset`/非预期重载，把参数表清脏
   → `nvngx_dlss` 读 `0x10000004A` 崩溃。
   ⇒ 只读探测是安全的；但任何**写**操作必须先确认槽位语义，且不要照抄头文件（§1 已给实测表）。
3. **不要在 `LoadLibraryW/ExW` 的 detour 里挂钩**：那时持有 loader lock，`ll::memory::hook` 挂起线程会死锁。
   正确做法：LoadLibrary 只记录，NGX 挂钩放到旁观线程，等两个模块就绪 + 150 ms 后再挂。
4. `kernel32!GetTickCount64` 只是转发 stub，真实现在 `kernelbase.dll`（否则计数恒 0）。
5. `#include <d3d12.h>` 与引擎自带头文件冲突（`D3D12_RAYTRACING_GEOMETRY_DESC` 重定义）
   ⇒ D3D12 代码必须放**独立编译单元**，不与 LL/引擎头混编。

## 5. 规划（NR 接入，按风险递增）

| 步骤 | 内容 | 验收标准 | 风险 |
|---|---|---|---|
| **A** | 新建 `src/neurodragon/nr/D3D12Bridge.cpp`（只 include `<d3d12.h>`），由 `Color` 取 `ID3D12Device` 与 `D3D12_RESOURCE_DESC`，建同规格纹理 | 日志打印 device/格式/尺寸，画面不变 | 低 |
| **B** | **纯色替换**：`SetPtr("Color")` 时替换成我们的纯色纹理（只动我们的资源状态） | **画面变纯色** = 替换链路通 | 中 |
| **C** | pass-through：把引擎 `Color` 拷贝到我们的纹理后再替换 | 画面恢复正常 | 中高（要动引擎资源的 barrier 状态） |
| **D** | 换成 NR feature `0x12`（`nvngx_dlssnr.dll`），接 `DLSSNR.*`（`MVecScale` 用引擎给的 `MV.Scale`；**不需要 jitter / exposure**） | NR 效果可见 | **已完成**（§6.13：建 feature + evaluate + 计时；剩色彩/合成） |

安全措施：B~D 全部放在开关后面（`config/nr_swap.txt` 存在才启用），默认不碰渲染。

待确认/开放问题：

- `Output` 双缓冲交替 —— 替换 `Color` 不影响它，但 barrier 要按帧处理；
- `Color` 在 Evaluate 时的资源状态未知（DLSS 按 SRV 读，推测是 `NON_PIXEL_SHADER_RESOURCE`）；
  C 步可能需要先试 `ALL_SHADER_RESOURCE → COPY_SOURCE`，失败则改用「自建副本 pass」；
- DLSS-RR 参数位全 0 —— 已查明原因（引擎自带降噪，不用 RR），不属当前范围，见 §7；
- NR 来源已定：直接套用 NVIDIA 的 NGX feature `0x12`（`nvngx_dlssnr.dll`），**不写自定义 compute**
  —— 自己写一个 compute 滤镜得到的不是 NR，是另一个滤镜。契约、可直接拿的值与唯一的未知见 §6。

## 6. NR 模块化契约（TRP 实证， 补）

结论两句：**能直接套用 `nvngx_dlssnr.dll`** —— 它就是 NGX 的一个 feature runtime，与
`nvngx_dlss.dll` 同级，导出同一套 `NVSDK_NGX_D3D12_*` 入口；**但不能整块搬 TRP 的 NR 模块**
—— 那 2,765 行里大半是 Skyrim 专属（runtime 身份与内核包、双 pass + resolve、子矩形、
UI mask、D3D11→D3D12 交接）。第一版只要 600–900 行骨架。

### 6.1 可以直接拿的

| 项 | 值 | 出处（TRP 仓库 `D:\Documents\theosrp`） |
|---|---|---|
| 入口来源 | `nvngx_dlssnr.dll` **自己的**导出：`NVSDK_NGX_D3D12_Init` / `GetCapabilityParameters` / `CreateFeature` / `EvaluateFeature` / `ReleaseFeature`（**不走** `_nvngx.dll` 的） | `src/FrameGen/NeuralRenderingFeatureSession.cpp` |
| feature / api version | `0x12` / `0x15` | 同上 L31-32 |
| app id（先借用跑通） | GUID `f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11`，CmsId `0x0876232C`，engine version `0.1.0-neural-rendering-source` | 同上 L28-31 |
| 每帧 Set（资源） | `DLSSNR.Color` / `DLSSNR.MVec` / `DLSSNR.Depth` / `DLSSNR.Output`；可选 `DLSSNR.ControlMask` / `Backbuffer` / `UI` / `UIAlpha` | 同上 L389-396 |
| 每帧 SetF | `DLSSNR.MVecScaleX` / `DLSSNR.MVecScaleY`（像素单位，引擎已给） | 同上 L425-426 |
| 不需要的 | **jitter / exposure**：NR 参数表里没有这两项（引擎仍写 `Jitter.Offset.*`、`DLSS.Pre.Exposure` 给 DLSS，NR 不吃） | 同上 |
| 内核包 | `.trpk` 只服务 RTX 20/30/40；**50 系用原始 kernel，不需要** | `CHANGELOG.md` 0.4.4 |
| 打包体积 | 只带 `nvngx_dlssnr_310.9.16.dll`（266 MB）；legacy 310.8（165 MB）与 int8（310 MB）不必带 | `CHANGELOG.md` 0.4.4 / 0.4.5 |

### 6.2 必须自己提供的

- **device**：从 `Color` 的 `ID3D12Resource::GetDevice` 取（§1 事实 4 的四张纹理本就是 `ID3D12Resource`，
  所以 `docs/A` 的 W1-3「bgfx handle → resource 桥」在本路线上**不需要做**）；
- **command list**：用游戏自己的（`EvaluateFeature` thunk 第一参数）；
- **同规格输出纹理**：格式沿用引擎 Color（linear HDR），NR 要吃未色调映射的辐射度；
- **历史 reset 时机**：维度切换 / 传送 / 加载 / 开菜单（NR 有内部历史，错了就是拖影）；
- **runtime 落盘位置**：`LoadLibrary` 全路径放自己的目录，**别**放游戏根目录去抢 `_nvngx`。

### 6.3 二次 Init 的问题（**已解，**：不是关键；真正卡点是驱动版本 + 模型文件名 + 必须走 core create，见 §6.12）

引擎启动 0.3 s 内已用 `_nvngx.dll` 初始化过 NGX，我们再 Init 一次行不行？

| 方案 | 做法 | 风险 |
|---|---|---|
| **A** | 不 Init，直接用引擎的 capability 参数 `CreateFeature(0x12)` | 最省；可能 `NotInitialized`（runtime 未绑到 device） |
| **B** | 用 `nvngx_dlssnr.dll` 自己的 `Init`（本项目 app id） | TRP 的做法；可能 `ALREADY_INITIALIZED`，不同 app id 是否允许**未验证** |
| C | 抢在引擎之前 Init | 会破坏引擎 DLSS，**不做** |

先写探针打 B 的返回码，失败再试 A。这个结果决定 NR 是「两周内可见」还是「要动引擎初始化顺序」。

### 6.4 第一版骨架（不碰交换链、不碰 UI）

```
Install   LoadLibrary(dlssnr) → 取 5 个导出 → Init(device) → GetCapabilityParameters
首帧      CreateFeature(游戏 cmdlist, 0x12, params[W,H])
每帧      [在 EvaluateFeature thunk 内，用游戏 cmdlist]
          Color→SRV → 录 NR 写 nrOut → set DLSSNR.* → Evaluate
          → param->SetPtr("Color", nrOut)      ← slot 1 语义已验证（§4 坑 1）
          → 调原 EvaluateFeature
退出      ReleaseFeature
```

比 §5 的 A~D 再省一层：**不需要独立队列、不需要跨队列 fence**（TRP 的 `CreationContext` 那套
queue/allocator/fence 是给 CreateFeature 用的，逐帧工作在游戏 cmdlist 上就够了）。
§5 的 B / C 两步仍然保留 —— 它们是「纯色 → 恢复」的两次可见验收，成本低、能提前暴露 barrier 问题。

### 6.5 实测：`SetPtr` 与参数对象读回**一致**，但有一次全 0

| 运行 | 日志 | 结论 |
|---|---|---|
| 13:13（尚无 `SetPtr` 钩子） | `[dlss] #1 … w=1485 h=835 color=0x0 depth=0x0 mv=0x0 out=0x0`，一直持续到 `#3600` | 参数对象 `Get` 读到空 |
| 13:46（有 `SetPtr` 钩子） | `[param] SetPtr Color = 0x1f4bfa667d0`、`Depth = 0x1f4bec19350`、`MotionVectors = 0x1f4bec25ad0`、`Output = 0x1f4bec092d0`；同一运行 `[dlss] #1200 … color=0x1f4bfa667d0 depth=0x1f4bec19350 mv=0x1f4bec25ad0 out=0x1f4bec0c250` | **逐位相同** |

⇒ 已经能下的结论：

1. **`SetPtr` 的 value 就是运行时最终持有的那个指针**（`Set` → `Get` 往返一致），GPT 担心的
   「间接层 / 类型不符导致读回的不是同一个东西」**不存在**；
2. 仍缺的只有两件事：`GetDesc()`（证明它是 `ID3D12Resource*`，而不是别的 COM 对象）；
   以及 13:13 那次全 0 的解释（最可能是当时还在主菜单、DLSS 资源尚未绑定 —— 判据是
   「进世界前后各打一行」，一次运行即闭环）；
3. `Output` 双缓冲确认（`…0c250` / `…092d0` 逐帧交替）；同时说明现有「值变就打印」= **每帧一条日志**
   （13:47:14.293/.299/.305 连续三帧），会拖时序、要先去重；
4. `[anchor]` 结束统计：`EvaluateFeature` 3300 次、`CreateFeature` 1 次、`Init` 0 次
   ⇒ 引擎的 `Init` 早于我们的钩子，`CreateFeature` 每会话一次。

反汇编（`0xe413740` 起，`Minecraft.Windows.exe` 1.26.5101.0）：

```
mov rax,[rcx]      ; param->vtable
mov rdi,r8         ; value（第三参数）原样保存
mov rsi,rdx        ; name
mov rbx,[rax+8]    ; → vtable[1]（指针/资源重载）
…                  ; CFG 检查
mov r8,rdi / mov rdx,rsi / mov rcx,r14 / jmp rax
```

⇒ **`value` 以 r8 原样直传给 `vtable[1]`，没有 `&value` 间接层**；GPT 担心的「还有一层间接指针」不存在。
仍未证的只剩「这个指针是不是 `ID3D12Resource*`」，而它只能靠 `GetDesc()` 回答。

按证据排出的下一步（全部可在一台机器上闭环）：

1. 读资源改「**14 → 8 → 9**」并打印 `(slot, retcode, value)` 一次，把槽位钉死（14 是引擎自己 `0xe4137a0` 用的）；
2. 对 `SetPtr` 记的指针与 `Get` 读出的指针各调一次 `GetDesc()`（新 TU，只 include `<d3d12.h>`）
   —— 打印 Format / Dimension / W×H / MipLevels / Flags / SampleDesc；两者应完全一致；
3. `SetPtr` 去掉「值变就打印」，改「前 32 次 + 每 600 次」，并带帧号 + `GetCurrentThreadId()`；
4. `Evaluate` 钩子里补：帧号、`cmdList`、`handle`、**原函数返回值**；
5. 菜单 → 进世界各打一行对照，解释 13:13 的全 0。

在 2 通过之前，**不要**按「value 就是资源」设计任何替换。

### 6.6 离线验证：`nvngx_dlssnr.dll` 的 NGX 入口形状

对 `nvngx_dlssnr_310.9.16.dll`（266 MB）做 PE 解析，两条关键事实：

| 观察 | 含义 |
|---|---|
| `NVSDK_NGX_D3D12_Init` 的 RVA `0x13810` 处是 **`mov eax, 0xBAD00001; ret`**（与 `D3D11_Init`、`VULKAN_Init`、`CUDA_Init` 同一地址） | **模型 DLL 不实现 Init**，返回「未实现」哨兵值。Init 必须走核心 `_nvngx.dll` |
| 导出 55 个 `NVSDK_NGX_*`（D3D12/D3D11/VULKAN/CUDA 的 Init/CreateFeature/EvaluateFeature/GetScratchBufferSize/PopulateParameters…），导入只有 `VERSION/ADVAPI32/USER32/KERNEL32`，**不导入 `_nvngx.dll`** | 模型与核心之间是运行期动态加载，不是静态依赖 ⇒ 核心按自己的搜索路径加载 `nvngx_dlssnr.dll`（TRP 的 `ModulePathHook` 就是劫持这条路径来强制版本） |

⇒ §6.3 的 A/B 两方案要按这个事实重写：

- **A'**：不自己 Init，直接对**核心**已初始化的设备 `CreateFeature(0x12)`，让核心去加载模型（我们负责把 `nvngx_dlssnr.dll` 放到核心能搜到的位置）；
- **B'**：自己 `LoadLibrary` 核心的副本 + `Init`（本项目 app id）—— 仍然可能 `ALREADY_INITIALIZED`；
- 断言：**不要**指望从模型 DLL 拿 `Init`（它只会返回 `0xBAD00001`）。

### 6.7 阶段 1：只读探针（已实现并编译，）

`src/neurodragon/probe/RenderProbe.cpp` + 新 TU `src/neurodragon/nr/D3D12Bridge.{h,cpp}`。
**全部只读，不替换任何资源、不创建 feature、不跑 GPU 命令。**

| 新日志 | 回答的问题 |
|---|---|
| `[res] Color f=… p=… dev=… dim=… fmt=… 1485x835 arr=… mips=… msaa=… flags=…` | 这四个指针**是不是有效的 `ID3D12Resource`**、格式/尺寸/用途；`dev` 给出设备身份（后续 NR 必须同设备） |
| `[res] <key> f=… -> set / null` | 解释 13:13 那次全 0：是「进世界后才绑定」还是别的 |
| `[eval] f=… #… tid=… cl=… h=… c/d/mv/o=… ret=…` | 帧号、线程、命令列表、feature handle、**原函数返回码** |
| `[eval] first f=… slots color=… depth=… mv=…` + `[slot] Color slot=8..15 ret=… value=…` | 一次性把「哪个 Get 槽位读资源」钉死（14 优先，8/9 兜底） |
| `[create] feature=… cl=… params=… ret=… handle=…` | 引擎创建的到底是哪个 feature（0 = SR）、用哪个参数对象 |
| `[param] SetPtr … f=… tid=… = 0x…` | 前 32 帧全打、之后 1/600 采样；不再每帧刷屏 |

闸门：**`[res]` 与 `[eval]` 同时给出非 0 指针 + 合法 `GetDesc` 之前，不碰 `SetPtr("Color")`。**



### 6.8 实测结论：色彩缓冲是 2560 纹理里的 **1485×835 子矩形**

方法：在 `EvaluateFeature` 的钩子里用引擎自己的 command list 录一次
`CopyTextureRegion`（`ALL_SHADER_RESOURCE → COPY_SOURCE → 回 ALL_SHADER_RESOURCE`），
读回 BMP（`D3D12Bridge::queueColorDump/flushColorDump`，第 2400/2800 帧抓，2600/3000 帧写）。

| 目标 | 全图均值/最大 | 左上 1485×835 | 右侧条（x≥1485） | 底部条（y≥835） |
|---|---|---|---|---|
| `Color`（DLSS 输入） | 23 / 158 | **68 / 158** | **0 / 0** | **0 / 0** |
| `Output`（DLSS 输出） | 68 / 156 | 80 / 156 | 84 / 134 | 49 / 121 |

⇒ 结论（已有图为准，不再靠推测）：

1. **场景只渲染在 `Color` 的左上角 1485×835**，其余是纯黑 ⇒ `DLSS.Render.Subrect.Dimensions=1485×835` 是**真的**，不是装饰参数；
2. DLSS 从 1485×835 超分到整张 2560×1440（`Output` 四块区域全部有内容）⇒ **SR 是真超分**，不是 1:1；
3. 因此 **NR 的正确推理尺寸是 1485×835**（约 1.24 MP；TRP 720p 单 pass 4.94 ms 线性外推 ≈ 6.7 ms @4080S，本机估 10–13 ms）；
4. 置换 Color 时：纹理与格式必须与原件一致（2560×1440 `R16G16B16A16_FLOAT`），NR 结果写在**左上角 1485×835**，其余区域保持原样即可；
5. 「控两头强制 720p」收益只有 26%（0.92 vs 1.24 MP），**不做**。

坑：主菜单里光追没开，`Color` 全黑 —— 必须**进存档**才能抓到内容。

### 6.9 阶段 2：NR feature 的测量装置（已实现，）

目标：**只验证「feature 0x12 能不能在我们这个进程里建起来并真跑出东西」+ 拿到 GPU 耗时**，
不替换 Color、不改画面。代码：`src/neurodragon/nr/D3D12Bridge.{h,cpp}`、`src/neurodragon/probe/RenderProbe.cpp`。

**流程**

1. `<游戏目录>\nr_swap.txt` 存在才启用（默认关）；日志首行 `[probe] install (nr_swap=on)`。
2. 第一个 Color/Depth/MV/Output 齐全且帧号 ≥30 的帧：`nrCreate`
   - 从 Color 取 `ID3D12Device`；输出纹理**复制引擎 Color 的 desc**（2560×1440 `R16G16B16A16_FLOAT`），
     flags 只留 `ALLOW_UNORDERED_ACCESS`，初始 `COMMON`（TRP 同款）；
   - timestamp query heap（8 槽 × 2 组 × 2 个）+ READBACK buffer（常驻 Map）+ `GetTimestampFrequency`
     （临时建一个 DIRECT queue 读，读完即放）；
   - 参数对象用**核心** `_nvngx.dll` 的 `GetCapabilityParameters` 自己取一份；先和引擎那份比 vtable
     （`[nr] params vtable … same=1` 才算槽位可信），再写创建参数，最后**读回 `DLSSNR.Width` 校验**；
   - `CreateFeature(cmdlist, 0x12, params, &handle)` 用探针保存的 **original** 导出（不能走 detour，否则递归）。
3. 之后每帧（窗口内）：`nrRecord` 写资源子矩形 → 输入 `ALL_SHADER_RESOURCE → NON_PIXEL_SHADER_RESOURCE`、
   输出 `COMMON/ALL_SHADER_RESOURCE → UNORDERED_ACCESS` → `EvaluateFeature` → 全部复位。
4. 计时：NR 与**引擎 DLSS** 各夹一对 timestamp，`ResolveQueryData` 进常驻 map 的 READBACK，
   帧号 `% 8` 分槽、**第 3 帧后 harvest**。本项目拿不到游戏 queue，无法 `Signal` fence，所以
   「GPU 跑完」是按帧数推断的：样本按 `0.02–200 ms` 过滤，不合法就丢并计数（`dropped=`）。
5. 窗口：**以「客户端进存档」为锚**（`ll::event::client::ClientJoinLevelEvent` / `ClientExitLevelEvent`）——
   菜单里 Color 全黑，所以菜单期间**不建 feature、不开窗口**；进存档的事件到达后重新锚定基准帧，
   从第 2 帧起**每 900 帧开一个 60 帧窗口，共 10 个**（约 2 分 15 秒的覆盖）；每窗第 30 帧录
   `nr_dump_w<k>.bmp`（我们的输出）、第 32 帧录 `color_ref_w<k>.bmp`（引擎 Color）；窗口结束后打印
   `[nr] gpu nr ms …` / `[nr] gpu engine-dlss ms …`。离开存档会作废当前窗口并再次重新锚定。

**写入的参数**（全部经实测槽位：1=指针/资源、3=uint、6=float；读回 11=uint）

| 时机 | 键 | 值 |
|---|---|---|
| 创建 | `DLSSNR.Width/Height` | 1485 / 835（引擎 `Width/Height`） |
| 创建 | `DLSSNR.ScalingRatio` | 1.0 |
| 创建 | `DLSSNR.Hint.Render.Preset` | 0 |
| 创建 | `PerfQualityValue` / `CreationNodeMask` / `VisibilityNodeMask` | 2 / 1 / 1 |
| 每帧 | `DLSSNR.Color` / `Depth` / `MVec` / `Output` / `Backbuffer` | 引擎资源；`Backbuffer` = 我们的 Output |
| 每帧 | `DLSSNR.{Color,Output,Backbuffer}Subrect` | 0,0 / 1485×835 |
| 每帧 | `DLSSNR.{Depth,MVec}Subrect` | 0,0 / 1485×836（整张 guide 纹理，TRP 同款） |
| 每帧 | `DLSSNR.UI / UIAlpha / ControlMask` 及其子矩形 | null / 0 |
| 每帧 | `DLSSNR.MVecScaleX/Y` | 1485 / 835（像素单位） |
| 每帧 | `DLSSNR.Enabled/Style/UseAutoMask/UICorrection/DepthInverted/Reset` | 1 / 0 / 0 / 0 / 1 / 窗口首帧 1 |
| 每帧 | `DLSSNR.Intensity/LocalTone/LocalStructure/SkinStructure` | 1.0（TRP 默认值） |

**已知假设与未钉的点**（下一轮若画面不对，先看这里）

- `DepthInverted=1` 是**猜的**（反向 Z），错了只影响画面、不影响耗时；
- 运行时：用 **310.9.16（266 MB）**，从 `E:\V12\MO2\mods\DLSS5+6倍帧生成\SKSE\Plugins\TheosRenderPipeline\NVIDIA\nvngx_dlssnr_310.9.16.dll` 复制到游戏根目录并**改名为 `nvngx_dlssnr.dll`**（核心按这个名字找模型；`.trpk` 只服务 20/30/40，`_int8` 与 165 MB 的 310.8 都不用）。310.x 契约里 `Enabled/Style/Reset/DepthInverted/UseAutoMask/UICorrection/Hint.Render.Preset` 是 **int**（槽 4），`Width/Height/各 Subrect` 是 **unsigned**（槽 3）——两边都按 TRP 的类型写；
- `Color` 子矩形小于纹理（1485×835 ⊂ 2560×1440）是**本项目独有**用法（TRP 里 Color 子矩形恒等于 Width/Height）；
- `Backbuffer` 复用 Output：TRP 对 reconstruction 契约就是这么做的，旧运行时读的是 composed/输入；
- 计时没有 fence（见上），64 帧窗口里若 GPU 掉队超过 3 帧，`dropped` 会升高而不是给出错数。

**验收顺序**：`[nr] CreateFeature(0x12) ret=0x1` → `[nr] 窗口 #k 首帧 … recorded=1` →
`[nr] dump nr_dump_w<k>.bmp written …` → `[nr] gpu nr ms …`。若 BMP 与 `color_ref_w<k>.bmp` **完全一致**，
说明 1:1 模式下运行时只是空转，P-A 路线当场作废。

### 6.10 排查过程：被排除的假设

| 探针 | 结果 |
|---|---|
| `[nr] level 监听` / 窗口锚定 | ✅ 事件门控正常（菜单不建 feature、不开窗口） |
| `params vtable same=` | ✅ 1（自取对象与引擎那份同一个类） |
| `DLSSNR.Width` 往返 | ✅ 1485 |
| `GetFeatureRequirements(0x01)`（DLSS SR 对照） | ✅ `ret=0x1 supported=0 minHW=0x160 minOS=10.0.0` |
| **`GetFeatureRequirements(0x12)`（NR，core 那份）** | ❌ `ret=0xBAD0000C`（OutOfDate），`supported=0`、`minHW=0`、`minOS=` 全空 |
| `runtime`（模型 create，未 Init） | ❌ `0xBAD00002`（PlatformError） |
| `runtime`（模型 Init_Ext 之后 create） | ❌ `Init_Ext=0xBAD00002` → `CreateFeature=0xBAD00002` |
| `core`（core create） | ❌ `0xBAD0000C` |
| GPU 计时 | ✅ `engine-dlss median 1.55–1.68 ms`（n=60×5 窗，1485×835→2560×1440） |

**⚠️ 先前的错误结论（已作废）**：曾据「core 二进制里没有 `dlssnr` 字符串 + 运行时报 OutOfDate」推断
「本机驱动不带 NR，需要更新驱动」。**这是错的** —— 模型 DLL 是**应用自己 LoadLibrary** 的，core 的
feature 名字表根本不参与；而且同机同驱动（610.88）的 TRP 日志给出了反证：

```
[DLSSNR Source] runtime build=310.x version=310.9.16.0 size=266424944 …nvngx_dlssnr_310.9.16.dll
[DLSSNR Source] public NGX initialized result=0x00000001 loader=…DriverStore\…\_nvngx.dll   ← 同一个 core
[DLSSNR Source] feature initialized output=1280x720 render=1280x720 ratio=1.0000 preset=0 build=310.x handle=…
[DLSSNR Source] evaluation recorded count=600 reset=false … style=1 intensity=1.000 …
```

（`C:\Users\Eragon\Documents\My Games\Skyrim Special Edition\SKSE\TheosRenderPipeline.log`）

⇒ 真正的差别是**谁初始化了 NGX 会话**：TRP 自己做 `NVSDK_NGX_D3D12_Init_with_ProjectID`（结果 `0x1`），
所以模型 `Init_Ext` 和 `CreateFeature` 都认它；MC 里 **引擎先初始化了这个进程的 NGX**（游戏自己的
app id / API 版本），我们再拿模型去 create 就是 `PlatformError` / `OutOfDate`。

⇒ 下一步实验（一次性把三种接法都打出来）：① 模型 create，**不**调 Init_Ext；② 模型 `Init_Ext` 后
create；③ core create。若都不行，就试「私有 core」：把 `_nvngx.dll` 复制一份、`LoadLibrary` 后用
自己的 project id 初始化，得到**第二个 NGX 会话**再建 feature（TRP 相当于天然独占一个会话）。

### 6.11  晚：独立探针（不需要游戏）+ 四个问题的答案

**工具**：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.{cpp,bat}`（按约定放在模组仓库外层）。
`run_probe.bat` 一键运行：自建 D3D12 设备 → 从模组目录加载 runtime → ①不初始化 core 直接 `Init_Ext`；
②自己 `Init_ProjectID` 成功后再 `Init_Ext` → `GetCapabilityParameters` → `CreateFeature(0x12)`。
`_nvngx.dll` 自动在 DriverStore 里找（inf 目录名随驱动更新变化，不要硬编码）。

**结果**：两条路径的 `Init_Ext` 都是 `0xBAD00002`，而 core 的 `Init_ProjectID` / `GetCapabilityParameters`
都是 `0x1`。⇒ **NR 运行时的失败与 Minecraft 进程无关，干净进程里同样复现**。

**顺带修掉的实 bug**：`NVSDK_NGX_D3D12_Init_ProjectID` 的实际参数顺序是
`(projectId, engineType, engineVersion, dataPath, device, version, info)` —— **version 在 info 之前**，
SDK 头文件正好相反。按头文件调用时 core 把 `info` 指针当 32 位 version、把 `0x15` 当
`FeatureCommonInfo*` 解引用 → 崩溃在「读取位置 0x0000000000000015」。**游戏内 22:30 那次崩溃就是这个**，
不是之前推断的“core 不允许二次初始化”。（证据：IDB `_nvngx.dll!0x1800666E0` 反编译，其内部
`sub_180066240` 的第 5 个参数按 +0 Path / +8 Length / +16 InternalData / +24 LoggingCallback /
+32 MinimumLevel / +36 DisableOtherSinks 解引用。）

| 问题 | 答案 |
|---|---|
| **A. 独立进程能否复现** | 能。干净进程 + 自建设备 + core init 成功 + 预加载 `nvngx_dlss.dll` 后，`Init_Ext` 仍 `0x02` |
| **B. 是不是只错过 hook** | 不是。游戏里 core 的 `CreateFeature` 返回 `0x1`（Init 已发生）；独立进程自己 Init 也 `0x1` |
| **C. PlatformError 的来源** | 数值确认 `0xBAD00002`；NGX 在这条路径不写日志；文件/注册表/模块/core 调用四条追踪均为 0 次 |
| **D. 是否追对入口** | 是。引擎走 core 的 D3D12 导出、feature=1；NR 走 runtime 自己的导出 |

**明天的执行顺序（全部无需进游戏）**

1. **feature id 扫描**（10 分钟）：对 `0..0x20` 调 core 的 `GetFeatureRequirements` / `CreateFeature`，
   确认 core 到底认不认 NR，以及 **RR=13** 是否可用（RR 若可用，是另一条可走的线）。
2. **复刻 TRP 的 load 前置**（2–4 小时）：读 `D:\Documents\theosrp` 的 `NeuralRuntimeKernels.cpp`
   （`ApplyRuntimeKernels` / `NeedsRuntimeKernels` 对 Blackwell 的真实返回值）、`NvidiaHostStartup.cpp`、
   `ModulePathHook.cpp`，逐项加进探针。
3. **驱动升级验证**（需要机主操作）：官方文档写 DLSS-NR 需 ≥615.64；升级后跑 `run_probe.bat`
   即可 10 秒判定，无需游戏。
4. 只有 1/2/3 有一个成功，才谈 **host 进程形态**（共享纹理 + 共享 fence + 流水线，+1 帧延迟，
   价值是崩溃隔离）。

### 6.12 ：驱动 617.42 之后的**正确接法**（已解锁建 feature）

**结论（探针实测，可重复）**：NR feature 能建起来，必须同时满足三条——

1. **驱动 ≥ 617.42**：核心 `_nvngx.dll`（32.0.16.1742）的 feature 名字表里出现了 `dlssnr`；
   610.88 的核心（32.0.16.1088）没有。
2. **模型必须叫 `nvngx_dlssnr.dll`、放在应用目录**（`Minecraft.Windows.exe` 同级）：
   用版本名 `nvngx_dlssnr_310.9.16.dll` 时 core create 返回 `0xBAD0000B`（找不到/初始化不了该库），
   改名后立刻 `0x00000001`。**这一点和之前的“不能改名”直觉相反，是 core 的硬要求。**
3. **必须用 core 的 `CreateFeature`**（core 自己加载模型），不要用 runtime 自己的 `Init_Ext`/`CreateFeature`
   ——后者在任何情况下都返回 `0xBAD00002`（TRP 那条“模型自己初始化”的路径在本机从来不成立）。

探针证据链（同一台机器，只改一个变量）：

| 条件 | core `CreateFeature(0x12)` |
|---|---|
| 610.88，无模型 | `0xBAD0000C` OutOfDate |
| 617.42，应用目录无模型 | `0xBAD0000B` UnableToInitializeFeature |
| 617.42，应用目录有 `nvngx_dlssnr.dll` | **`0x00000001`，handle 非空** |
| 617.42，只放版本名 DLL | `0xBAD0000B`（名字是硬要求） |

**因此模组已改为**：不预加载模型、优先用 **core** 的 `CreateFeature`/`EvaluateFeature`（参数写入与
资源转场不变），游戏根目录放 `nvngx_dlssnr.dll`。进游戏验证时只需重新创建 `nr_swap.txt`。

### 6.13  23:33：**游戏内首次跑通 NR**（实测数据）

日志（`logs/latest.log`，窗口 #0–#2，每窗 60 帧）：

```
[nr] core CreateFeature(0x12) ret=0x00000001 handle=0x…
[nr] 窗口 #0 首帧 指南=1485x836 特征=1485x835 recorded=true
[nr] dump nr_dump_w0.bmp written max=1.069 nz=5186944
[nr] gpu nr ms median=3.523 mean=3.557 min=2.990 max=4.653 n=60
[nr] gpu engine-dlss ms median=1.642 mean=1.631 min=1.430 max=1.773 n=60
[nr] ReleaseFeature ret=0x00000001
```

- 三个窗口的 NR 中位数：**3.523 / 3.493 / 3.566 ms**（1485×835，1:1）；同帧引擎 DLSS：1.642 / 1.681 / 1.722 ms。
- 画面：`nr_dump_w2.bmp` 与 `color_ref_w2.bmp` 对比（下表裁 1485×835）——**场景结构完整可辨**
  （树、草、岩浆、手臂都在），但整体偏暗、偏灰。⇒ NR 真的跑起来了，不是空转。
- 无崩溃；禁用时 `ReleaseFeature` 成功。

**偏暗的原因（下一步）**：TRP 的输出不是直接当最终图像用——它用**自己的 resolve kernel** 把
`modelResult`（NR 输出）与原始颜色重新合成（`ResolveKernel::Ratio` / `Residual`），还有 HDR→LDR 的
encode 步骤。NR 的 62 个参数里**没有** HDR/色彩空间开关（已从 DLL 里提取完整 `DLSSNR.*` 列表），
所以这层必须由我们自己做。三条可选路线：

1. 直接把 NR 输出当 Color（先看 DLSS 超分后的观感，最省事）；
2. 复刻 TRP 的 resolve：NR 输出 + 原始 Color 用 compute 合成（相当于一个后处理 pass）；
3. 先做 HDR→LDR 的 encode（tone map）再喂 NR，输出再 decode 回来。

### 6.14 现状、未解问题与下一步（读这一节就够）

**已完成**

- feature 建起来了（`core CreateFeature(0x12)=0x1`），每帧 evaluate 成功，GPU 计时闭环；
- 三个窗口实测：NR **3.52 / 3.49 / 3.57 ms**，同帧引擎 DLSS 1.64 / 1.68 / 1.72 ms（n=60/窗）；
- 生命周期干净：软禁用 + 模块 PIN，`ReleaseFeature=0x1`，禁用/退出不再崩；
- 计时装置、`nr_swap.txt` 开关、`nr_runtime.txt`（已不需要）、BMP dump 都在。

**未解**

| 问题 | 现状 | 影响 |
|---|---|---|
| NR 输出偏暗 | 直接当图像用会暗、偏灰；TRP 是「NR 结果 + 原色」自己 resolve，还做 HDR→LDR encode | 决定观感，必须先解决 |
| `DepthInverted` | 暂按 `1`（反向 Z）写死，未验证 | 影响深度相关的细节 |
| 参数调优 | `Style=0 / Intensity=1.0 / Local*Strength=1.0` 用的是 TRP 默认 | 影响风格强度 |
| 尺寸变化 | 只测过 1485×835；换分辨率需重建 feature（现在会跳过并告警） | 全屏/改分辨率要处理 |

**下一步三条路线（成本递增）**

| 路线 | 做法 | 成本 | 风险 |
|---|---|---|---|
| **A** 直接替换 Color | 把 NR 输出（左上 1485×835）当 DLSS 的 Color 喂进去，先看观感 | 约半天 | 画面可能偏暗，但能立刻“看见 NR” |
| **B** 复刻 TRP 的 resolve | 写一个小 compute，把 NR 输出与原始 Color 按 ratio/residual 合成 | 1–2 天 | 需要 shader 管线（目前工程里还没有 compute pass） |
| **C** HDR→LDR encode/decode | 喂 NR 前 tone map + gamma，输出再反变换 | 半天–1 天 | 可能一步解决偏暗，但不一定还原 TRP 观感 |

建议：**先 A 拿到可看的画面，再按观感决定 B 还是 C**。

### 6.15 编码链路、出图口径，以及“到底是不是 NR”

**修掉的 bug（害了两次运行）**：`D3D12_ROOT_CONSTANTS` 是三个字段
`{ShaderRegister, RegisterSpace, Num32BitValues}`，我写成 `{0, 2}` 把 2 当成了 space，
导致 `Num32BitValues=0` → 根签名和 shader 的 `cbuffer(b0)` 对不上 →
`CreateComputePipelineState` 返回 `E_INVALIDARG (0x80070057)`。用独立探针
`ngx_probe --cs` 做三变体（no-root / table / table+cbuffer）定位，改 `{0, 0, 2}` 后立刻 `0x0`。

**新增：HDR→LDR encode pass**（§6.14 route C）。compute shader（运行时 `D3DCompile`
编译内嵌 HLSL）把引擎 `Color`（线性 HDR，峰值 ~69）按 `Reinhard + gamma 2.2` 写进私有
RGBA16F 纹理，feature 读这张图；引擎 Color 用完转回 `ALL_SHADER_RESOURCE`，不影响引擎 DLSS。

**dump 口径拆成三域**：`linear`（Reinhard+gamma）、`encoded`（直写）、`linear-ldr`（仅 gamma），
每次 flush 打印 `max/mean/above1`；`color_ref_*` 现在 dump 的是**NR 实际看到的输入**。

| 指标（w1，同一帧） | 编码前 | 编码后 |
|---|---|---|
| `\|NR − 输入\|` 平均绝对差 | 4.11% | **1.48%** |
| 通道增益 R/G/B | 0.89 / 0.92 / 0.80 +0.119 | 0.92 / 0.92 / 0.87 +0.038 |
| 三通道均值（输入 vs 输出） | 0.230/0.279/0.252 vs 0.230/0.279/0.257 | 同左 |

⇒ “发蓝、发灰”的主体来自**域不匹配**（HDR 直接喂 LDR 特征 + dump 少 gamma），不是滤镜；
编码后残留的冷色只是一点点对比/色温差异。

**“是不是 NR”的判定（按可信度递增）**

1. **代码里没有像素写入**：输出纹理的唯一写入者是 NGX 的 `evaluate`（无 shader、无 copy）；
2. **`Enabled` 冻结测试（两次复现）**：`Enabled=0` 的窗口输出**不跟随画面**——
   输入均值 0.353、输出仍停在 0.736（跨窗口 |Δ| = 0.38，而同窗 |NR−输入| 只有 0.016）；
   无状态滤镜做不到；
3. GPU 计时（3.3–3.4 ms）夹在 `evaluate` 前后，同帧引擎 DLSS 1.65–1.72 ms。

**Style 0/1/2 sweep**：窗口间差异 1.3–1.9%，与 NR 自身帧间抖动同量级，**无系统性差异**——
这个构建可能忽略 `Style`（或只作 hint）。因此 Style 不是“是否 NR”的判据，改用上面第 2 条。

**参数覆盖机制**（不改代码即可做实验）：`<游戏目录>\nr_params.txt`，支持
`Style` / `Intensity` / `LocalToneStrength` / `LocalStructureStrength` / `Enabled` / `DepthInverted`，
以及 `Sweep=1`（每窗循环 Style 0–2）、`Sweep=2`（偶数窗 Enabled=1、奇数窗 Enabled=0）。

**遗留（用户已判定可接受）**：NR 的增量很小（同窗 1.5–2.5%），与 MC 的 16× 素材信息密度低一致；
`DepthInverted=1` 仍是假设，未单独验证。

### 6.16 Route A：把 NR 结果交给超分

#### 引擎的 NGX 参数契约（实测）

用 `SetPtr / SetU32 / SetF32` 的钩子把引擎在 `EvaluateFeature` 之前的全部写入记下来，得到它
驱动自己 DLSS SR 的完整契约。命名**无前缀**（不是 `DLSS.*`）：

| 参数 | 引擎的值 | 说明 |
|---|---|---|
| `Width` / `Height` | 1485 / 835 | 渲染分辨率（不是 2560×1440） |
| `OutWidth` / `OutHeight` | 2560 / 1440 | 放大目标 |
| `MV.Scale.X` / `MV.Scale.Y` | 1485 / 835 | 运动向量是**归一化**的，按分辨率缩放 |
| `DLSS.Render.Subrect.Dimensions.Width/Height` | 1485 / 835 | 输入子矩形 |
| `DLSS.Input.{Color,Depth,MV,Translucency}.Subrect.Base.X/Y` | 0 | 输入从 (0,0) 开始 |
| `DLSS.Output.Subrect.Base.X/Y` | 0 | 输出从 (0,0) 开始 |
| `DLSS.Enable.Output.Subrects` | 0 | 关闭输出子矩形 |
| `DLSS.Feature.Create.Flags` | 3 | **创建时**生效；不设就没有子矩形感知的输入路径 |
| `PerfQualityValue` | 1 | 质量档 |
| `DLSS.Pre.Exposure` / `DLSS.Exposure.Scale` | 1 / 1 | 曝光 |
| `Sharpness` | 0.35 | |
| `Reset` | 0 | **无前缀**；重置时序历史的唯一键 |
| `CreationNodeMask` / `VisibilityNodeMask` | 1 / 1 | |
| `DLSS.Hint.Render.Preset.*` | Quality/DLAA/Balanced/UltraQuality = `0x8000000B`，Performance = `0x8000000D`，UltraPerformance = `0x8000000C` | preset 提示 |

每帧被重写的是 `Output`、`Jitter.Offset.X`、`Jitter.Offset.Y`；`Color/Depth/MotionVectors` 与
上表其余项只在创建时写一次。`Output` 在**两张纹理之间交替**（双缓冲）。

#### 已确认的规则（每条都有运行证据）

1. **不要写引擎的 `Color`。** 它参与引擎的时序链，写进去一帧近黑内容后整条链永久死掉
   （之后每帧读回都是 0）。
2. **不要 transition 引擎拥有的纹理**（`Color/Depth/MV/Output`）。NGX 自己管理这些资源的状态；
   我们把它 `ALL → UAV → ALL` 来回切会打乱引擎 feature 的内部跟踪，之后引擎自己就画不对。
3. **一个进程里只能有一个 DLSS SR 实例。** 我们 `CreateFeature(1)` 自建第二个 SR 之后，
   引擎那个 feature 就再也产不出正确结果：它的 `EvaluateFeature` 仍然返回 1，但输出不再更新
   （表现为“我们停止写 Output 后画面冻结”，且只有重启进程才能恢复）。
4. 求值必须用钩子的 **original** 指针。`GetProcAddress` 拿到的地址是我们自己的 detour，
   会递归到栈溢出（`0xC00000FD`）。
5. 我们的 SR 里必须按上面的契约设：`MV.Scale.*`（否则历史不重投影 → 内容固定在屏幕上）、
   `DLSS.Feature.Create.Flags=3`（否则子矩形输入路径没开 → 竖条纹垃圾）。
6. **`Reset` 无前缀**：`DLSS.Reset` 不被读取，跨存档/跨关卡的时序残留需要写 `Reset=1`（一次性）。

#### 最终结论（2026-10-22 20:34，已修复并实机验证）

**模式收敛到两个**：`0` = 关闭（NR 只求值，不上屏）；`5` = 增益合成后把引擎参数对象的
`Color` 指向我方纹理，由引擎自己的 DLSS 放大上屏。其余模式（1–4、6、7–9）已删除。

**真正的根因不是合成方式，而是编码纹理被冻结。** 编码 pass 与合成 pass 共用同一个
4 槽描述符堆、同一帧各写一次；命令列表先记录后执行，GPU 执行时才读描述符内存，于是编码
dispatch 实际用了合成 pass 那一套描述符（目标 = `gDecodedColor`），随后又被合成覆盖。
结果 **`gEncodedColor` 只在第一帧被写过，之后永久不变**：NR 一直处理同一张旧帧，旧帧的
结构被当作“增益”乘到实时画面上——这就是那个“画面里没有的地形/手”的幽灵。

判定手法（可复用）：把中间纹理按窗口 dump 出来逐字节比较。修复前 `color_ref` 相隔 11 秒的
两个窗口**完全相同（0.000/255）**；修复后差 41.6/255。

修复：描述符堆扩到 8 槽，**0–3 给编码 pass，4–7 给合成/解码 pass**，各自绑定自己那一段的
GPU 句柄。验证（同帧）：`nr2` vs `color_ref` = 2.23/255（NR 在跑），`fed` vs `base` =
1.86/255（合成生效，NR 本身的增量就很小）。

**因此 4e083ac 时 NR “会变”**：那时还没有合成 pass，同一个堆一帧只被写一次。回归是
Route A 把合成塞进同一个堆时引入的。

`MVecScale` 同时修正为调用方传入的渲染尺寸（曾经默认 1.0；引擎的 MV 是归一化的，正确值
是 1485/835）。`DepthInverted=1` 仍是未单独验证的假设。

#### 规划：接管引擎那一次创建（**已被模式 2 的实测否决，仅留作备选**）

> 模式 2 证明换指针可行，本节这条路线不再是主线。留在这里的原因只有一个：如果将来发现
> “每帧换指针”在某些帧上有副作用（而不是机制不成立），接管 create 是退路。

1. 钩住引擎的 `CreateFeature(1)`：**不转发**，用引擎传进来的参数对象替它创建我们的 feature，
   把我们的 handle 作为返回值交回去（`ret=1`）；
2. 从此引擎每帧的 `EvaluateFeature(handle)` 就是我们的 feature，运行在引擎自己的命令流里，
   用引擎自己设好的 jitter / MV / Output / 尺寸，我们不再手动摆弄任何引擎资源；
3. 我们只做一件事：evaluate 前把 `Color` 换成我们的合成图；关卡加载/过场期间不换（喂引擎原图），
   这样加载期画面完全由引擎自己的逻辑决定，不会损坏；
4. 进存档时发一次 `Reset=1`，鬼影不会跨存档；
5. 模式语义随之简化：`0` = 直接透传（我们的 SR 仍存在，但不换色），`8` = 换色上屏。

验收：进存档无损坏、跨存档无鬼影、切回 `0` 画面正常（不冻结），`8` 下画面是我们的 SR 结果。

## 7. RR 地基：`doAllRayTracing`（第二阶段，不在当前范围）

> RR = NGX feature **13**（`0x0D`），runtime `nvngx_dlssd.dll`，参数 `DLSSD.*` +
> `DiffuseAlbedo` / `SpecularAlbedo` / `Normals` / `Roughness` / `DiffuseHitDistance`。
> Skyrim/TRP **结构上不可能有 RR**（光栅化，没有 noisy radiance），所以「TRP 没有、无参考」
> 是必然的，不是难度问题。MC RT 是光追，这是本平台相对 TRP 唯一结构性的优势。

Mac edu 1.26.32 全符号 IDB 实测：RenderDragon 的 RT 降噪集中在一个函数里

```
dragon::framerenderer::drawcommands::doAllRayTracing(
    ComputeCommandContext&, unsigned short, RayTracingFeatureConfiguration const&,
    RayTracingResources const&, ServerResourcePointer<ResolvedMaterialResource>,
    span<RtLightInfo>, span<uint>, unsigned, unsigned)
```

- Mac base `0x10b72e700`，大小 `0x1746`；
- 引用字符串：`TemporalDenoising`、`FinalizeDenoising`、`AdaptiveDenoiserCalculateGradientsInline`、
  `AdaptiveDenoiserGenerateReferenceInline`；
- `dragon::rendering::RayTracingResources` 持有 `Diffuse2Denoised` / `DiffuseChroma2Denoised` /
  `Specular2Denoised` / `AdaptiveDenoiserGradients{0,1}` / `AdaptiveDenoiserReference` /
  `AdaptiveDenoiserPlaneIdentifier`（最后一个还被 `getDebugModeInfo` 引用）。

三条结论：

1. **引擎自带降噪** ⇒ §2 表里 `DLSSD.*` 那批参数位全 0 是「引擎不用 RR」，不是「没写完」。
2. **RR 的插入点不是 `SetPtr("Color")`**：现在的 Color 是**降噪之后**的 DLSS 输入，喂给 RR 只会
   双重降噪；真 RR 要 `doAllRayTracing` 内部的噪声 radiance + G-buffer。⇒ 当前 NR 设计与 RR
   是两条独立路线，**不复用**注入点。
3. 这些地址来自 Mac/Metal 版，**Windows 1.26.51 必须按字符串 / RTTI 重新定位**，不能套偏移。

真到那一步，唯一决定性问题：**能否拿到 `RayTracingResources`（或它背后的噪声 radiance / G-buffer）**
—— 1–2 天可在 IDB 里答掉；答不上来，RR 就是不可做的，NR 才是本平台唯一的神经渲染路线。

## 8. 参考文档

- [A-renderdragon-passmap.md](A-renderdragon-passmap.md)：RenderDragon 通道图、taau 契约、W1 结论
- [B-windows-probe.md](B-windows-probe.md)：无 PDB 定位链、探针演进、每次运行记录、参数表
- [C-levilamina-api-notes.md](C-levilamina-api-notes.md)：LL API 速查（hook / signature / 线程 / 日志）

外部参考（本机）：

- `D:\Documents\theosrp`：TRP 0.4.5 源码（NR / FG / SR 的完整实现，本文所有「TRP 实证」均出自此处）
- `E:\V12\MO2\mods\DLSS5+6倍帧生成`：TRP 的 Universal 打包（含 `nvngx_dlssnr_310.9.16.dll` 266 MB
  与 `nvngx_dlssnr_310.9.16.trpk` 12 MB）。注意：**包内 INI 是默认值**（`Provider=0`、
  `XeFGMultiplier=2`），所谓「6 倍帧生成」不在这个包里，是 TRP 菜单里的乘数 + MFG 解锁。
