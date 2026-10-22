# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **NR feature 跑通**：游戏内用 **core** 的 `CreateFeature(0x12)` 建成 feature 并逐帧 evaluate，
  1485×835 1:1 推理，实测 **median 3.52 / 3.49 / 3.57 ms**（n=60/窗）；同帧引擎 DLSS 1.64–1.72 ms。
  `src/neurodragon/nr/D3D12Bridge.cpp` 负责纹理/参数/求值/状态转场，`probe/RenderProbe.cpp` 负责窗口、
  BMP dump 与开关。
- GPU timestamp 计时：NR 与引擎 DLSS 同帧各一对查询，环形 readback + 3 帧后收割；日志给
  median/mean/min/max 与被丢弃样本数。
- 开关 `<游戏目录>\nr_swap.txt`（默认关，删除即恢复原状）；`<模组目录>\nr_runtime.txt` 指定运行时文件名
  （现已不需要，core 自己从应用目录加载）。
- 独立探针 `D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.exe`（`run_probe.bat`）：
  不需要游戏即可验证驱动/core/运行时组合，自动在 DriverStore 里挑选**最新**的 `_nvngx.dll`。

### Added

- HDR→LDR 编码 pass（Reinhard + gamma 2.2，运行时编译 HLSL 的 compute shader）：网络现在读的是
  编码后的 1485×835 图，而不是引擎的线性 HDR Color。`|NR − 输入|` 从 4.11% 降到 1.48%。
- dump 三域（linear / encoded / linear-ldr）+ 每次 flush 的 `max/mean/above1` 统计；
  `color_ref_*` 改为 dump「NR 实际看到的输入」，对照终于同域。
- 参数覆盖文件 `<游戏目录>\nr_params.txt`（Style/Intensity/LocalToneStrength/Enabled/DepthInverted，
  以及 `Sweep=1` Style 循环、`Sweep=2` Enabled 冻结），实验不需要重新编译。

### Added：Route A，NR 结果交给引擎 DLSS 放大

- **`Replace=1`（`nr_params.txt`，默认关）**：NR 求值后把 1485×835 结果反变换回线性 HDR
  （gamma 2.2 解码 → 反 Reinhard，NaN/Inf 清洗）写进**自己的**纹理，然后把**引擎自己的 NGX 参数对象**
  的 `Color` 指向它；引擎的 DLSS 照常把它放大到 2560×1440 —— **NR 只做 1:1，不承担放大**。
- encode pass 扩成双向（`mode=0` 编码 / `mode=1` 反变换），根常量 2 → 3 个；用
  `ngx_probe --cs`（三个变体）离线验过 `CreateComputePipelineState=0x0`。
- `Replace=1` 时 NR **每个入世帧都求值**（`nrReplaceContinuous()`），并有 **120 帧闸门**
  （刚进存档/刚建 feature 的几帧不替换）。
- 新增 `out_after_wN.bmp`：引擎 DLSS 的 Output 在它 evaluate 之后 dump，用来证明替换真的喂进了超分。

### Findings：NGX DLSS SR 的参数契约与共存限制

- 参数名**无前缀**（`Width`/`Height`/`OutWidth`/`OutHeight`/`MV.Scale.X|Y`/`Reset`），
  `DLSS.Feature.Create.Flags=3` 只在创建时生效，子矩形 `DLSS.Input.*.Subrect.Base.*` 全 0，
  `Output` 每帧重写且在两张纹理间交替，`Jitter.Offset.X|Y` 每帧变化。
- **不要写引擎的 `Color`**（会把时序链写死）；**不要 transition 引擎拥有的纹理**
  （会打乱 NGX 对资源的内部跟踪，之后引擎自己画不对）。
- **一个进程只能有一个 DLSS SR 实例**：自建第二个之后，引擎的 `EvaluateFeature` 仍返回 1
  但输出不再更新（停止我们的写入后画面冻结，只有重启进程能恢复）。
- 结论：要接管超分，必须**接管引擎那一次 `CreateFeature(1)`**（用自己的 feature 顶替并返回我们的
  handle），而不是另建一个实例。见 `docs/D-rt-dlss-nr-plan.md` §6.16。

### Fixed

- compute PSO 的根常量初始化漏了一个字段（`D3D12_ROOT_CONSTANTS{0,2}` → `{0,0,2}`），
  导致 `CreateComputePipelineState` 返回 `E_INVALIDARG`；用 `ngx_probe --cs` 三变体定位。
- `NVSDK_NGX_D3D12_Init_ProjectID` 的参数顺序：真实为 `(…, device, version, featureInfo)`，
  SDK 头文件相反；按头文件调用会崩在「读取位置 0x15」。
- 禁用模组时崩溃（`0xC000001D`，跳进已释放的跳板）：改为**模块 PIN + 软禁用**——钩子常驻但透传，
  不再在别的线程可能执行 detour 时 `unhook`。
- 加载期偶发死锁：移除了对 `LoadLibraryW/ExW`、`GetProcAddress`、`CreateFileW`、注册表的诊断钩子
  （它们会在 loader lock 内被执行）。

### Findings

- **不要写引擎的 `Color`**：第一版 Route A 直接 `CopyTextureRegion` 覆盖它，进存档后整屏永久黑
  （之后每窗 `color_ref` 都是 `max=0 nz=0`，`nr_dump` 只剩黑底 + ROI 边框）。Color 参与引擎的
  时序链，加载期写进一帧近黑内容就把链打死。改为「把引擎参数对象的 `Color` 指针指向自己的纹理」。

- `Enabled=0` 的窗口输出不跟随画面（输入均值 0.353 → 输出仍 0.736），两次复现：写入只能来自
  NGX 的 evaluate，排除“我们代码加的滤镜”。
- `DLSSNR.Style` 0/1/2 在窗口间的差异（1.3–1.9%）与 NR 自身帧间抖动同量级，无系统性差异。
- NR 的增量本身很小（1.5–2.5%），与 MC 16× 素材的信息密度一致；`DepthInverted=1` 仍是未验证的假设。

- NR 能跑的三条硬要求：驱动 **≥ 617.42**（核心 feature 表里才有 `dlssnr`）、模型必须叫
  **`nvngx_dlssnr.dll`** 并放在**应用目录**、建 feature 必须走 **core** 的 `CreateFeature`。
  模型自己的 `Init_Ext` 在本机恒为 `PlatformError`（`0xBAD00002`），与 TRP 的用法不同。
- 失败可在**独立进程**复现（自带设备 + core 初始化成功 + 预加载 DLSS 模型，仍然 `0x02`），
  所以问题不是 Minecraft 进程环境。
- 当前未解：NR 输出直接使用时偏暗（TRP 会用自己的 resolve kernel 与原始颜色合成，并做 HDR→LDR encode）。
- `DepthInverted=1`、`Style/Intensity/Local*Strength` 的取值仍是 TRP 默认，未单独验证。

### Fixed（2026-10-22 傍晚：NR 的运动向量缩放）

- **`DLSSNR.MVecScaleX/Y` 一直写成 1.0。** 引擎的 `MotionVectors` 是归一化的，它自己给 DLSS 用的
  是 `MV.Scale = 1485/835`；NR 按“像素空间”解释，历史就永不重投影——光源周围的半透明光晕
  固定在屏幕像素上、不随视角移动（`Replace=5` 的症状）。现在默认取调用方传入的渲染尺寸，
  `MVecScale=` 仍可覆盖两个轴。
- 新增 `NrEncode=0|3`：强制网络输入域（0 = Reinhard+gamma，3 = TRP shoulder+sRGB），
  用它在任意 `Replace` 模式下单独验证“域不匹配”和“时序”。
- 新增 `fed_wN.bmp` + `[dump] fed` 统计：这是**实际喂给引擎 DLSS** 的那张图。画面冻结时，它若
  停更就是我们这边写挂；它若在变而引擎 `Output` 不变，才是 upscaler 不更新。


### Fixed（2026-10-22 晚：幽灵的真正根因）

- **编码纹理只在第一帧被写过。** 编码 pass 与合成 pass 共用同一个 4 槽描述符堆，同一帧各写
  一次；命令列表先记录、后执行，GPU 执行时读到的是最后写入的那套描述符，于是编码 dispatch
  实际写进了 `gDecodedColor`，随即被合成覆盖。`gEncodedColor` 从此永久冻结，NR 一直在处理
  同一张旧帧——旧帧结构被当成“增益”乘到实时画面，就是那个“画面里没有的地形/手”的幽灵。
  修复：堆扩到 8 槽（0–3 编码、4–7 合成/解码），各自绑定自己的 GPU 句柄。
- `DLSSNR.MVecScaleX/Y` 修正为调用方传入的渲染尺寸（1485/835）。此前默认 1.0，而引擎的
  `MotionVectors` 是归一化的，等于历史永不重投影。
- 验证：同帧 `nr2` vs `color_ref` = 2.23/255，`fed` vs `base` = 1.86/255；跨窗 `color_ref`
  差 41.6/255（修复前逐字节相同）。

### Removed（2026-10-22 晚：收敛成一个能用的东西）

- 删除自建 DLSS SR（旧 `Replace=7/8/9`，约 700 行）与所有“写引擎资源/借还/直拷”的早期模式
  （旧 1–4、6）。模式只剩 `0`（关）与 `5`（增益合成上屏）。
- 删除对应的一次性诊断开关（`Control`/`Stamp`/`DLSSPreset`/`Sharpness`），保留 `NrEncode`、
  `NRReset`、`MVecScale`、`ParamLog`、`GainBoost` 与按窗口的 dump。
