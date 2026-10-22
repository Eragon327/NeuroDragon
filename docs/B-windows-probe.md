# B. Windows 侧定位与探针（无 PDB）

> 目标：不依赖 PDB / 符号表，在运行时把 `dragon::framerenderer::modules::taau` 找出来并挂钩，
> 顺便把 W1-4 剩下的「到底跑 Taau 还是 Bilinear」用一行日志钉死。

## 1. 定位链：字符串 → 代码交叉引用 → `.pdata` 定界

```
"InputBufferMotionVectors" 等 3 个材质输入名（.rdata，唯一）
   └─ 全 .text 扫 `lea r64,[rip+disp32]`，取目标等于这些字符串的位置
        └─ 用 .pdata（x64 unwind 表）反查「包含该指令的函数」的精确 [begin,end)
             └─ 校验：同一函数内还能找到另外 2 个输入名的引用 ⇒ 就是 taau
```

实测（`E:\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe`，1.26.5101.0；测量当时在
`D:\Program Files\LeviLauncher\…`，2026-10-21 搬盘，二进制相同）：

| 项 | RVA |
|---|---|
| `InputFinalColor` 字符串 | `0xebffe5e` |
| `InputBufferMotionVectors` 字符串 | `0xebe61c0` |
| `InputTAAHistory` 字符串 | `0xebb4b9b` |
| 三处代码引用（各**唯一**） | `0xb751167` / `0xb7511d8` / `0xb751249` |
| **taau** | `0xb74f860` – `0xb751693`（大小 `0x1e33`） |
| 相邻的 TAA 绘制回调（`TAAUpscalingParameters` 引用所在） | `0xb752820` – `0xb753f87` |

反汇编交叉验证（`0xb751167` 起）：

```
0xb751167  lea rdx, [rip+0x34aecf0]   ; "InputFinalColor"
0xb75116e  lea rdi, [rbp+0x330]       ; MaterialUniformName 的栈缓冲
0xb751178  call <PathBuffer<std::string>::PathBuffer(char const*)>
```

与 Mac 版 taau 里 `Core::PathBuffer(...)+MaterialUniformName(...)` 的形态逐条对应。

> 注意：`Minecraft.Windows.exe` 在 2026-10-20 22:55 被改写过（同目录有 `.bak`，311,215,952 B ← 311,249,032 B）。
> 上表基于**当前** exe；定位器本身不依赖固定偏移，改版只会让「函数边界校验」失败而不是挂错地方。

## 2. 探针实现（`src/neurodragon/probe/RenderProbe.cpp`）

`probe::install()`（在 `NeuroDragon::enable()` 里调用）：

1. 用 `GetModuleHandleW(nullptr)` + `IMAGE_NT_HEADERS64` 取 `.text` / `.rdata` / `.pdata` 三段；
2. `findBytes()` 在 `.rdata` 找 3 个输入名（`memchr` + `memcmp`）；
3. `findRipRefs()` 单次扫 `.text`，收集所有 `48/4C 8D /r` 且 mod=00、rm=101 的 RIP 相对引用，命中三串之一即记录；
4. `functionBounds()` 用 `.pdata`（按 `BeginAddress` 有序，二分）取精确函数区间；
5. 校验「同一函数内 ≥2 个输入名引用」，再 `ll::memory::hook(target, detour, &original, Normal)`；
6. detour 镜像 taau 的 12 个参数（4 个寄存器 + 8 个栈），打印后 `call` 原函数。

日志（前 3 次 + 每 300 次）：

```
[RenderProbe] taau @ base+0xb74f860（大小 0x1e33）
taau #1 mode=Taau factor=1.000 jitter=0 dynScale=1.000 viewport=1920x1080+0,0
```

- `mode` = `UpscalingData::mUpscalingParameters.mUpscalingMode`（`0=Taau, 1=Bilinear, 2=Fsr1, 3=Fsr1DebugBilinear`）
- `factor` = `mUpscalingFactor`；`jitter` = `mCurrentJitterIdx`（+52）；`dynScale` = `mDynamicResolutionScale`（+56）
- 定位失败时会打 `stringFound=… refs=… inside=…` 三个计数，用于区分"串变了/引用变了/边界变了"。

## 3. 验证步骤

> 2026-10-21 起客户端在 `E:\LeviLauncher\versions\1.26.51.01`，普通权限可写，不用再提权
> （完整回路与边界见 [E-local-loop.md](E-local-loop.md)）。

1. 关掉游戏，把 `bin/NeuroDragon/`（`NeuroDragon.dll` + `manifest.json`）拷到
   `E:\LeviLauncher\versions\1.26.51.01\mods\NeuroDragon\`；
2. 启动 1.26.51.01，进一个世界；
3. 看 `E:\LeviLauncher\versions\1.26.51.01\logs\latest.log`：

| 日志现象 | 含义 | 下一步 |
|---|---|---|
| 完全没有 `RenderProbe` 行 | 模组没被加载 | 查 `mods/NeuroDragon/manifest.json` 与 LL 版本/日志里的 "已加载 N 个模组" |
| `未定位到 taau（stringFound=… refs=… inside=…）` | 定位链断在某一步 | 按计数判断：`stringFound=false`→输入名改了；`refs=0`→lea 形态变了；`inside<2`→函数边界变了 |
| `taau @ base+0x…` + `taau #1 mode=…` | 成功 | **`mode` 就是 W1-4 的答案**；`Bilinear` 说明当前档位没走 TAAU |

## 4. 已知边界

- 该探针**只做定位 + 打印**，不改渲染结果；`disable()` 会 `unhook` 并打印累计调用次数。
- 若某次更新后 `inside<2`，用离线脚本（同一套算法，见本文件 §1）重新确认字符串/引用，再决定是否改锚点。
- 回调（`0xb752820`）已经能定位到，但它是 `__func::operator()` 的 vtable 槽，暂未挂 —— 等 P1 真正要拿纹理时再上。

## 5. 第二轮：taau 0 命中 —— 改用「无签名锚点」找真实路径

现场（2026-10-20 23:26 / 23:36 两次启动）：

```
RenderProbe: taau @ base+0xb74f860（大小 0x1e33）   ← 地址正确
RenderProbe: taau 钩子已安装（每 300 次打印一次）
RenderProbe: 已卸载，累计调用 0 次                  ← 但一次都没命中
```

**离线复核**（同一 exe）：

| 事实 | 值 |
|---|---|
| taau 的直接调用点（`E8`） | **只有 1 处**：`0x52a1bf4`，在函数 `0x5255450`（大小 `0x6075f`）里 |
| 该函数自身的调用者 | 0 个直接调用（虚函数，经 vtable） |
| 引用 `Upscaling` 材质的代码 | 3 处：`0xb74f860`(taau)、`0x8db1100`、`0x5e0cfe0`(bgfx caps 表) |
| TAAU 的 10 个 uniform 名（`SubPixelJitterAndValidHistory` 等） | **只被 `0xb752820`（taau 的绘制回调）引用** ⇒ 全二进制只有一份 TAAU 实现 |

⇒ 结论：**不是钩子坏了，是调用 taau 的那个渲染函数（`0x5255450`）在这台机器上根本没跑**。
要么当前渲染路径换了实现，要么 `upscaling_mode` 实际解析成了 bilinear（那样不会走 taau 分支）。

### 探针改造：不需要签名的通用 thunk

`src/neurodragon/probe/UniversalHook.h` 在运行时生成一段机器码：保存 GPR/xmm/flags →
调用 `anchorReport(idx)` → 原样恢复 → `jmp` 回原函数（尾跳），所以**不需要知道目标函数签名**。
本地自测（`build/thunk_test.cpp`，`RESULT=PASS`）验证了：6 参数（含 double/xmm 与栈参数）保真、两次调用计数为 2、尾跳返回原值。

锚点表（全部经 `.pdata` 校验为函数起点）：

| RVA | 名称 |
|---|---|
| `0xb74f860` | taau |
| `0xb752820` | taau-callback |
| `0x5255450` | rendererA:taau-caller |
| `0x8db1100` | rendererB:Upscaling-material |
| `0x53211d0` | rendererB:SceneDepth |
| `0x8cab340` | rendererB:deferred-pass |
| `0x8cc6630` | rendererB:radiance-upscale |
| `0x8cd4120` | rendererB:pass-2 |
| `kernel32!GetTickCount64` | **对照**（必然每帧被调，用来证明 thunk/挂钩链路本身有效） |

### 判读

| 日志 | 含义 |
|---|---|
| `control:GetTickCount64` 首次命中 | thunk 与 LL hook 链路有效 |
| `rendererA:taau-caller` 命中、`taau` 0 | 渲染器在跑，但**没走 taau 分支**（多半 effective mode = bilinear） |
| `rendererA:taau-caller` 也 0 | 实机不是这条渲染路径 → 去 `rendererB:*` 里找 |
| `rendererB:*` 有命中 | 实机走的是 B 簇；下一步在 B 簇里找真正画 `Upscaling` 材质的那个 pass |
| 全部 0（含 control） | 挂钩链路有问题（LL hook 与游戏自身 hook 冲突之类），再查 |

退出游戏时 `disable()` 会打印每个锚点的累计命中数，一次运行即可看清全部。

## 6. 为什么第一轮是 0 命中（已定案）

不是钩子的问题：**当时实际跑的是 Bilinear**。

- 活动档案 `Minecraft Bedrock/Users/9587534086357524579/.../options.txt`（23:37 写入）里
  `upscaling_mode` 由 `1` 变成 `0`；`Users/Shared` 那份是 10-11 旧档仍为 `1`。
- `UpscalingParameters::Mode` 是 `Taau=0, Bilinear=1`，与选项值同序 ⇒ **0=Taau、1=Bilinear**。
- 23:26 / 23:36 两次运行读到的都是 `1`（Bilinear）→ 引擎走非 taau 分支 → 钩子 0 命中，完全自洽。
- 顺带确认：**bilinear 路径根本不调用 `modules::taau`**（不是"调用了但走了 bilinear technique"）。

⇒ 现在档案里是 `upscaling_mode:0`（Taau），下一轮探针应当能看到 `taau` 锚点命中。

## 7. 为什么 mode=Taau 了还是不命中：`factor < 1.0` 门控（反汇编确认）

在 `rendererA:taau-caller`（签名见上）里，taau 的调用块前面有两个门控字节：

```asm
; 0x5255939  计算门控
mov   rax, [rsi+0xd08]          ; UpscalingData*
movss xmm0, [rip+0x9351718]     ; = 1.0f
ucomiss xmm0, [rax+4]           ; 比较 1.0 与 mUpscalingFactor
mov   r15d, [rax]               ; r15d = mUpscalingMode
seta  [rbp+0x7360]              ; active = (1.0f > factor)   ← 只有 factor<1 才算"在超分"
test  r15d, r15d
sete  [rbp+0x6d3f]              ; gate_taau = (mode == 0)
mov   eax, r15d
and   eax, 0xfffffffe
cmp   eax, 2
sete  [rbp+0x68ab]              ; gate_fsr  = (mode == 2 或 3)
...
; 0x52560cb
movzx eax, [rbp+0x7360]
and   [rbp+0x6d3f], al          ; gate_taau &= active
and   [rbp+0x68ab], al          ; gate_fsr  &= active
...
; 0x52a1ad6
cmp   byte ptr [rbp+0x6d3f], 0
je    +0x...                    ; 为 0 就跳过整个 upscaling 块（含 call taau @ 0x52a1bf4）
```

结论：

- `mode` 读自 `UpscalingData+0`，`0 = Taau`（与 `UpscalingParameters::Mode` 同序，也和选项值同序）；
- **`mUpscalingFactor` 必须 < 1.0**（即渲染分辨率低于输出分辨率）才会真正调用 taau；
- 你当前 `options.txt`：`upscaling_percentage:0` + `target_resolution:1`（= 原生）→ factor = 1.0
  → `active=false` → 门控关掉 → **taau 0 命中**。这与 16261 次 caller / 0 次 taau 完全吻合。

要把 P1 点亮：在游戏里把**渲染分辨率/目标分辨率/超分比例**调到低于窗口（例如目标 720p 而窗口 1080p，
或把 Upscaling Percentage 调离 0），让 factor < 1.0 即可。

> 另外顺带看到一对字符串：`Upscaling Mode: DLSS` / `Upscaling Mode: Off` / `Upscaling Mode: Fallback`
> 与 `ADVANCED_UPSCALING` / `DEFERRED_UPSCALING_MODE` 等材质宏 —— 这个 Windows 版本里可能已经存在
> 一条 DLSS 超分路径，后续值得单独确认（可能改变 E 阶段的落点）。

## 8. 探针改动（本版）

- 锚点**改为签名匹配**（`ll::memory::Signature` + `.text` 范围），不再硬编码 RVA；签名均在
  1.26.5101.0 上验证过唯一性（`taau-callback` / `Upscaling-material` 需要 64 B 才唯一）。
- 修掉 control 误判：`GetTickCount64` 不在本模块 `.pdata` 里，之前被"必须是函数起点"的检查拒绝，
  现在 control 直接按 `GetProcAddress` 挂钩。
- 每个锚点启用时打印解析到的 `base+0x...`，退出时打印各锚点命中数。

## 9. 第三次运行（2026-10-21 11:46）：P1 通了

```
[anchor] taau                         命中 53854 次
[anchor] taau-callback                命中 53854 次
[anchor] rendererA:taau-caller        命中 53854 次
[anchor] rendererB:radiance-upscale   命中 107708 次   (2x)
[anchor] rendererB:pass-2             命中 53854 次
[anchor] rendererB:Upscaling-material 命中 0 次
[anchor] rendererB:SceneDepth         命中 0 次
[anchor] rendererB:deferred-pass      命中 0 次
[anchor] control:GetTickCount64       命中 0 次        ← 见下
```

- **taau : taau-callback : caller = 1 : 1 : 1** ⇒ 每帧的 VV 帧图确实构建了 TAAU pass，P1 落点成立；
  这也反证门控（`mode==Taau && factor<1`）当时是开的。
- `rendererB:*` 里只有 `radiance-upscale` / `pass-2` 在跑，另外两个是死代码分支 —— 后续不用再管。
- **control 0 是坑不是 bug**：`kernel32!GetTickCount64` 只是转发 stub，真体在 `kernelbase.dll`，
  钩 stub 收不到调用。已改为优先 `kernelbase!GetTickCount64`。

## 10. 第四次运行（12:14）：8 个签名全 miss，但 control 装上了

```
[anchor] taau 签名未命中（版本漂移？）
... 8 个全 miss ...
RenderProbe: 锚点已安装 1/9        ← 只有 control
```

而磁盘上的 `Minecraft.Windows.exe` 与 `.bak` 的 `.text` **都能命中同样的签名**（都在
`0xb74e860` / `0x5254450`）。⇒ 那次运行**主模块不是我们分析的那个 exe**（用户切了"光线追踪"启动方式，
很可能走了另一个构建/另一个可执行文件）。

已加两件事，下次日志即可定案：

1. 启用时打印 `module=<exe 全路径> ts=<PE 时间戳> .text=<rva>+<size>`；
2. 签名全 miss 时，自动回退到「字符串 → lea rip → `.pdata`」定位 taau，并标注 `taau(string)`。

## 11. 第五次运行（12:32，RT 模式）：签名全 miss 的真正原因

```
RenderProbe: module=D:\...\1.26.51.01\Minecraft.Windows.exe ts=0x6aa482fd .text=0x1000+0xe5a5ab6
[anchor] 8 个签名全 miss
[anchor] taau(typed,string) -> base+0xb74f860     ← 字符串回退找到同一地址
```

⇒ **不是换了 exe**（module 路径/时间戳/`.text` 大小都与分析目标一致，回退也落在 `0xb74f860`），
是我用 `ll::memory::Signature::resolve` 的那条路本身没工作（8/8 全 miss，而字节其实一致）。

已改为**自带扫描器**（固定字节 + `??` 通配，`memchr` 找锚字节后逐字节校验），并在字符串回退成功时
附带打印函数头 16 字节（`bytes=…`）作为交叉证据。

## 12. RT + DLSS 现状（2026-10-21）

本机 GPU：**NVIDIA GeForce RTX 5070 Laptop**；版本目录里有 **`nvngx_dlss.dll`（54 MB）**；exe 内有整套
NGX/DLSS 标记串（`DLSS.Input.MV.Subrect.*`、`DLSS.Input.Depth.Subrect.*`、`DLSS.Hint.Render.Preset.DLAA|Quality|…`、
`DLSSOptimalSettingsCallback`、`Upscaling Mode: DLSS`、`error: failed to load nvngx`）。

⇒ **DLSS 超分是引擎自带能力**，RT 路径很可能直接在用（Minecraft 的 RTX 路径历史上就是 DLSS2）。

为确认「RT 下到底走不走 DLSS」，探针新增：

- 启用时打印 `nvngx_dlss.dll` 是否已加载；
- 挂 `kernelbase!LoadLibraryW / LoadLibraryExW`，任何含 `nvngx/dlss/ngx` 的 DLL 加载都会打 `[dll] …`。

若确认 DLSS 在跑，下一步就是挂 `NvSDK_NGX_D3D12_EvaluateFeature`（从已加载的 nvngx_dlss.dll 取导出），
拿到 color/depth/MV/output 参数 —— 那就是 NR 插在 DLSS 之前的天然位置，也是「复用 DLSS」的落点。

## 13. 第六次运行（12:40）：签名修复 + DLSS 确认在加载

```
[anchor] taau(typed) -> base+0xb74f860            ← 自带扫描器生效，8 个全部命中
...
[dll] nvngx_dlss.dll 启用前未加载
[dll] LoadLibraryExW C:\Windows\System32\DriverStore\...\_nvngx.dll        (tag=nvngx)
[dll] LoadLibraryExW D:\...\1.26.51.01/nvngx_dlss.dll                       (tag=nvngx)
RenderProbe: 锚点已安装 9/9
```

- 自带签名扫描器替代 `ll::memory::Signature` 后，8 个锚点稳定命中（此前 8/8 miss 是解析器用法问题）。
- **引擎在启动阶段就加载 NGX**：先 `_nvngx.dll`（驱动库里的 NGX core），再 `nvngx_dlss.dll`（游戏目录里的 DLSS 插件）。
  这是标准 NGX SDK 初始化序列 ⇒ DLSS 是引擎真实能力，不是残留字符串。

本版据此新增：`_nvngx.dll` 加载完成后，自动解析并挂 `NVSDK_NGX_D3D12_{EvaluateFeature, EvaluateFeature_V2,
CreateFeature, Init}`（通用 thunk，只计数 + 首命中日志），用于回答「DLSS 到底每帧在不在跑」。

判断与下一步：

| 现象 | 含义 | 下一步 |
|---|---|---|
| `NVSDK_NGX_D3D12_EvaluateFeature` 每帧命中 | DLSS 在跑 ⇒ 可直接复用 | 换成 typed hook，dump Color/Depth/MV/Output 参数 → NR 插在 DLSS 之前 |
| 只有 `CreateFeature`/`Init` 命中 | DLSS 初始化了但没启用（当前画质/模式不走 DLSS） | 找开关（`Upscaling Mode: DLSS` 对应选项） |
| 一个都不命中 | 引擎从 `nvngx_dlss.dll` 内部调，不走 NGX core 导出 | 改挂 `nvngx_dlss.dll` 内部或找 NGX 包装层 |

## 14. 第七次运行（12:48，RT）：**DLSS 在跑，且 RT 与 VV 完全不同路径**

```
[ngx] hooked NVSDK_NGX_D3D12_EvaluateFeature @ 0x7ffd125c6f90
[ngx] hooked NVSDK_NGX_D3D12_CreateFeature  @ 0x7ffd125c6e30
[ngx] hooked NVSDK_NGX_D3D12_Init           @ 0x7ffd125c65a0
...
[anchor] taau / taau-callback / rendererA / rendererB:*   全部 0 次
[anchor] control:GetTickCount64                           16241 次
[anchor] NVSDK_NGX_D3D12_EvaluateFeature                  12348 次
[anchor] NVSDK_NGX_D3D12_CreateFeature                    1 次
[anchor] NVSDK_NGX_D3D12_Init                             0 次（在我们挂上前已调用）
```

结论：

1. **RT 模式下 DLSS 真在跑**：`CreateFeature` 1 次（建 feature），`EvaluateFeature` 12348 次（每帧级别）。
   ⇒ 「复用引擎 DLSS」成立，不需要我们自己实现超分。
2. **RT 与 VV 是两套完全不同的渲染路径**：VV 的 taau/帧图锚点在 RT 下全 0（符合预期，不是坏了）。
3. `Init` 为 0 是因为 `_nvngx.dll` 的 Init 在我们挂钩之前就调用过了 —— 不影响结论。

下一步（本版已实现）：把 `EvaluateFeature` 换成 **typed hook**，dump 参数结构体头部
（`w/h/tw/th`）+ 前 16 个指针字，用来定位 `Color / Depth / MotionVectors / Output` 四个资源
—— 那就是 NR 插在 DLSS 之前的落点。

## 15. DLSS 参数读取（12:51 这轮 dump 为什么是乱码，以及正确做法）

12:51 的 `[eval]` 打印里 `w/h/tw/th` 是垃圾值，原因是我按「参数结构体头部是宽高」去解读了 —— 但现代
NGX 的 API 是：

```c
// nvsdk_ngx.h:702（本机副本：theosrp/.dependencies/Streamline/external/ngx-sdk/include）
NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList *InCmdList,
    const NVSDK_NGX_Handle    *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters,   // ← 不透明键值容器，不是宽高结构体
    PFN_NVSDK_NGX_ProgressCallback InCallback);
```

输入是通过 `NVSDK_NGX_Parameter` 的 setter 塞进去的（`SetD3d12Resource(params,"Color",res)`、
`SetUI(params,"Width",w)` …，键名见 `nvsdk_ngx_defs.h`）。所以正确读法不是解内存布局，而是**走它的 vtable**：

```c
// nvsdk_ngx_params.h:  Set 重载 [0..7] / Get 重载 [8..15] / Reset [16]
//   Get(ID3D12Resource**) == 14,  Get(unsigned int*) == 11
using GetUI  = int(*)(void*, const char*, unsigned int*);
using GetRes = int(*)(void*, const char*, void**);
w   = vt[11](param, "Width", &w);
col = vt[14](param, "Color", &col);   // 还有 Depth / MotionVectors / Output
```

本版 `ngxEvaluateDetour` 已改成这样读，每 600 次打印：

```
[dlss] #n w=… h=… color=0x… depth=0x… mv=0x… out=0x…
```

拿到这四个 `ID3D12Resource*` 后，NR 的插入点就确定了：在 `EvaluateFeature` 之前把 `Color`
（以及需要的 Depth/MV）替换成 NR 的输出，再让引擎的 DLSS 超分。

## 16. 第八次运行（12:59）：`Width/Height` 读到了，但资源是 0

```
[dlss] #1    w=878  h=613   color=0x0 depth=0x0 mv=0x0 out=0x0     ← 窗口
[dlss] #3600 w=1485 h=835   color=0x0 depth=0x0 mv=0x0 out=0x0     ← 之后切了全屏
```

- `Width/Height` 通过 `Get(unsigned int*)`（vtable slot 11）读到了，且随窗口/全屏变化 ⇒ 参数对象读法正确。
- 四个资源全 0 ⇒ 引擎不是用 `SetD3d12Resource`（slot 14）存的，多半是 `SetVoidPointer`/`Set(void*)`。
  本版已同时读 `Get(void**)`（slot 15），并挂 NGX core 的三个**参数写入**导出：
  `NVSDK_NGX_Parameter_SetVoidPointer / SetD3d12Resource / SetUI`，直接记录 `键名=值`（前 24 次 + 每 600 次）。
- 另：`CreateFeature` 命中 2 次 —— 这版游戏创建了两个 NGX feature（可能有第二个 DLSS 实例）。

## 17. 第九次运行（13:18/13:24）：DLSS 四张输入全部拿到，且参数可写

```
[ngx-probe] Get(Color) slot 8 -> 0x1f6b3f9a050     (slot 9 同值)
[ngx-probe] Get(Output) slot 8 -> 0x1f6b3fb84d0
[ngx-probe] Get(Depth) slot 8 -> 0x1f6b3fc09d0
[ngx-probe] Get(MotionVectors) slot 8 -> 0x1f6b3fb6850
[ngx-write] Set(Color) -> 0x1f6b3f9a050 完成（参数可写）
```

- **槽位实测**：这个 NGX 实现里资源从 Get **slot 8/9** 取（10~15 全空），与 SDK 头文件的
  `Get(ID3D12Resource**)==14` 不一致 —— 以实测为准。
- `NVSDK_NGX_Parameter_Set*` 是 SDK 内联 helper，不走导出，所以 `[ngx-set]` 永远不会出现；
  但 `Set("Color", …)` 的 vtable 写入实测可用（游戏未崩），这就是替换 DLSS 输入的开关。
- 本版新增 `[res]`：用 ID3D12Resource 的 `GetDesc`（vtable slot 10，结构体返回走隐藏指针）
  打印 Color/Depth/MV/Output 的 尺寸/格式/采样/Flags。注意**不能**直接 `#include <d3d12.h>`，
  会和引擎自带头文件冲突（`D3D12_RAYTRACING_GEOMETRY_DESC` 重定义），所以手写 ABI + SEH 保护。

下一步（NR 接入）：
1. 用 `[res]` 的格式建同规格纹理；
2. 「纯色替换」测试：只把我们纹理 `Set("Color", ours)`（不动 Color 的资源状态）→ 画面变纯色即链路通；
3. pass-through 拷贝（需要处理 Color 状态屏障）；
4. 换成 NR 着色器，再接 jitter/exposure 参数。

## 18. 真 vtable 布局（从游戏自己的内联 helper 反汇编得出）

崩溃的根因找到了：**这个 NGX 版本的 vtable 顺序和 SDK 头文件不一样**。游戏把 SDK 的 inline Set helper 编译成了三个小函数，
它们各自 tail-call 参数对象的虚函数：

| 游戏内 helper | tail-call 的槽位 | 实际语义 |
|---|---|---|
| `0xe413740` | `vtable[1]` | `Set(指针/资源)` |
| `0xe4136e0` | `vtable[3]` | `Set(unsigned)` |
| `0xe413630` | `vtable[6]` | **`Set(float)`** |

我之前的「写测试」用 `vtable[6]` 传资源指针 —— 实际那是 `Set(float)`，指针被转成浮点/截断，
DLSS 再把 `Color` 当 `ID3D12Resource*` 解引用 → `D3D12Core`/`nvngx_dlss` 的 AV。两次崩溃都是这个原因。

**新路线（安全）**：直接挂游戏的三个 helper（`0xe413740/0xe413630/0xe4136e0`，签名已验证唯一），
记录 `键名 → 值`，不再碰 NGX 的不透明参数对象。等看清 `Color/Output/Width/Jitter` 的写入后，
再在同一个小函数里**替换 `Color` 的值**（它的入参就是 `(param, name, value)`，改 `value` 即可）。

另：`Output` 在两次采样之间交替（`0x…4d250` / `0x…46050`）⇒ 双缓冲/swapchain 轮转，NR 侧要按帧重新绑。

## 19. 完整参数表（13:46 实测）：NR 需要的东西全在

```
SetUI  PerfQualityValue=1  RTXValue=0  DLSS.Feature.Create.Flags=3  Reset=0
SetPtr Color / Output(双缓冲交替) / Depth / MotionVectors
SetF   Jitter.Offset.X=-0.125  Jitter.Offset.Y=-0.27778
SetF   Sharpness=0.35  DLSS.Pre.Exposure=1  DLSS.Exposure.Scale=1
SetF   MV.Scale.X=1485  MV.Scale.Y=835        ← 单位是像素，不是归一化
SetPtr TransparencyMask / ExposureTexture / DLSS.Input.Bias.Current.Color.Mask (0)
SetPtr GBuffer.{Albedo,Roughness,Metallic,Specular,Subsurface,Normals,
               ShadingModelId,MaterialId,Attrib.8-15} (0)
SetPtr MotionVectors3D / IsParticleMask / AnimatedTextureMask / DepthHighRes /
       Position.ViewSpace / RayTracingHitDistance / MotionVectorsReflection (0)
```

要点：

1. **NR 插入点确认**：`paramSetPtrDetour` 里 `name == "Color"` 时改 `value` 即可替换 DLSS 输入。
2. 引擎已经提供 **jitter（亚像素抖动）**、pre-exposure、曝光缩放、MV 像素尺度 —— NR 侧不用自己算抖动。
3. 存在一整套 **DLSS-RR 输入**（`GBuffer.*`、`RayTracingHitDistance`、`MotionVectorsReflection`），
   当前都是 0 ⇒ 这个版本具备 Ray Reconstruction 的参数位。RT 路线上这比普通 SR 更合适，后续值得单独确认。
4. `Output` 每帧在两个指针间交替 ⇒ 双缓冲，替换 Color 时不涉及 Output，但 barrier/同步要按帧处理。

下一步（NR 接入）：
- A：新增独立编译单元 `nr/D3D12Bridge.cpp`（只 include `<d3d12.h>`，避免与引擎头冲突），从 `Color` 取 device/desc；
- B：纯色替换测试（可见）；
- C：pass-through 拷贝；
- D：NR compute + 接 jitter/exposure。
