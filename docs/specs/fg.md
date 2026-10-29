# FG：VV 线 DLSS-G 帧生成

> **状态**：设计已定，尚未实施（2026-10-29）。
>
> **路线选择**：先做 **VV + DLSS-G 固定 2x + Streamline**，RT 线后排。
> **呈现语义**：按用户决定，**FG dispatch 放在全部游戏 UI 合成之后、原始 Present 之前**。
> pre-UI 只负责保存 hudless 与准备 Depth/MV，不在那里执行生成或呈现。
>
> 本文是当前 FG 的唯一实施依据。RenderDragon 的通用帧结构见 [renderdragon](renderdragon.md)，
> VV 的 SR 输入与资源桥见 [vv](vv.md)，Present 面板设施见 [ui](ui.md)。

## 0. 一句话

复用 VV 已验证的 Depth/MV 与 D3D12 注入链，在 P3/P5 保存最终 LDR hudless，
在 **P6 `flip()` / DXGI `Present`** 执行 DLSS-G dispatch；先解决交换链创建时机，
再标记输入，最后才开启固定 2x。

## 1. 决策与边界

### 1.1 先选 VV，不先选 RT

| 维度 | VV 线 | RT 线 |
|---|---|---|
| D3D12 颜色 / Depth / MV | 已有并实机验证；SR 使用 `depthR32`、像素单位 `mvRg32` | 引擎 NGX 参数可直接给原生资源，省去 bgfx 桥 |
| 命令列表注入 | 已有 R3 注入与提交边界 | 有 NGX evaluate 钩子，但没有 VV 的同一套帧图注入边界 |
| 26.51 RE 状态 | TAAU、`arg10`、MV 语义与下游消费已定位 | RTX `frameImpl` 在 Windows 26.51 仍未解析 |
| FG 共同缺口 | Streamline、交换链创建、UI 后调度、hudless、输入生命周期 | 相同 |
| 结论 | **复用面最大，先做** | 等 VV 的呈现层打通后再复用 |

RT 的“资源更直接”不足以抵消新的帧渲染器和 UI 边界 RE；两条线最困难的交换链与帧节奏工作相同。

### 1.2 第一版范围

- DLSS-G 固定 2x：`numFramesToGenerate = 1`。
- Streamline 加载 DLSS-G、Reflex、PCL；Reflex Low Latency 随 FG 开启。
- 先支持 VV 世界内渲染；菜单、加载帧和无有效世界输入时直通。
- 失败时关闭 FG 并走原始 Present，不以黑屏、重复帧或旧输入继续运行。

**不在第一版**：MFG/多倍帧、动态倍率、FSR-FG、XeFG、RT 线、驱动内核补丁、
`.trpk`/INT8 解锁。既有禁令见 [decisions/0002](../decisions/0002-no-kernel-patching.md)。

## 2. 外部代码给出的契约

调查版本：

- RTXPT：`303b408ab81790a5051abd7573830b7a4112eb8e`（v1.9.0），Streamline 2.14.16。
- OptiScaler：`e57377fc75831d5c2fbb58d6a057a75702d78a95`（2026-10-28）。
- 两个本地检出的相关子模块/DLL 均未完整落地，不能把“源码存在”当成“运行时已可加载”。

### 2.1 RTXPT：算法与每帧顺序

本机路径：`D:\Documents\RTXPT`。

- `Rtxpt/Sample.cpp:2397`：设置 `DLSSGOptions`，查询 `DLSSGState`、实际呈现倍率和输入完成 fence。
- `Rtxpt/Sample.cpp:3160`：在 tonemap 后、UI 前复制 `PreUIColor`。
- `Rtxpt/Sample.cpp:3663`：设置相机、jitter、深度方向、MV scale、reset 等常量。
- `Rtxpt/Sample.cpp:3714`：标记 Depth、MotionVectors、HUDLessColor。
- `Rtxpt/Sample.cpp:2865`：下一帧重写输入前等待 `inputsProcessingCompletionFence`。
- `Rtxpt/SampleUI.h:180`：FG 开启时关闭普通 Vsync/FPS limiter，帧节奏交给 FG/Reflex。

### 2.2 OptiScaler：交换链与资源生命周期

本机路径：`D:\Documents\OptiScaler`。

- `OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp:78`：创建交换链前升级 Streamline factory，
  启用 DLSS-G，并加入 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`。
- 同文件 `:599`：Depth/MV/Hudless 通过 `slSetTagForFrame` 按帧标记，
  区分 `eOnlyValidNow` 与 `eValidUntilPresent`。
- `OptiScaler/hooks/FG_Hooks.cpp:1157`：FG dispatch 位于原始 Present 之前。
- Resize 必须保留创建期添加的 tearing 标志，否则 DXGI 可拒绝尺寸变更。

OptiScaler 是 GPLv3 项目。本文只把它当行为参照；若要直接复制实现，先确认许可与发布方式，
不默认把它的全局钩子、资源追踪和 UI 系统一起搬进 NeuroDragon。

## 3. Mac / Windows 的 UI 后点位

### 3.1 Mac 1.26.32 已有符号

| 点位 | 地址 | 判断 |
|---|---:|---|
| `bgfx::mtl::RendererContextMtl::flip()` | `0x10b850b20` | **P6，推荐**。全部游戏 UI 后、真正 Present 前，每帧一次 |
| `DefaultUIRendererCompositorStage::postRender()` | `0x1040be980` | 不是最终边界；函数内部仍遍历 UI mesh 并绘制，返回也只代表该 stage 完成 |
| `GameRenderer::renderCurrentFrame()` | `0x1040c74c0` | 可做粗粒度尾部钩子，但不如 `flip()` 精确 |
| `FrameBuilder::endFrame()` | 虚调用 | 太早，后面还有 UI compositor stage |

证据：[renderdragon-passmap §7.5](../research/2026-10-22-renderdragon-passmap.md)。

### 3.2 Windows 26.51 对应关系

Windows 使用 bgfx D3D12 后端，语义对应 `bgfx::d3d12::RendererContextD3D12::flip()`。
第一版可以直接复用现有 DXGI `Present/Present1` 钩子执行 FG dispatch，避免为调度再解一层 bgfx；
交换链创建仍必须单独处理。

**输入标记与 FG dispatch 是两个位置，不要合并：**

```text
VV 渲染期：准备 / 标记 Depth、MV
P3/P5：保存 tonemap 后、UI 前的最终 LDR hudless
引擎 UI 合成
P6 flip / Present：FG dispatch → 原始 Present
```

如果只把 UI 后的最终帧当唯一颜色输入，链路实验可以做，但 HUD、准星和文字可能插值变形或闪烁；
正式路径仍保留 pre-UI hudless。

## 4. 当前已确认与未解决

### 4.1 已确认

- VV 的 DLSS-SR 已跑通，自有 D3D12 资源、引擎 MV、命令列表和提交边界可复用。
- `arg10` 是 tonemap 前的 HDR 中间面，**不是** RTXPT 的 `PreUIColor`，不能直接当 hudless。
- UI 在 SR 后合成；Mac 的 P6 `flip()` 是明确的 UI 后呈现边界。
- 现有面板已有 `Present/Present1/ResizeBuffers*` 与游戏 direct queue。
- 现有 NGX 日志出现过 `nvngx_dlssg.dll doesn't exist in any of the search paths`；
  驱动目录里存在文件不等于 NGX/Streamline 的搜索路径能找到它。

### 4.2 未解决，不能当事实写死

- Windows 26.51 的 P3/P5 精确签名，以及 Gameface/常驻 UI 是否都落在同一 hudless 边界之后。
- 游戏实际使用哪个 swapchain 创建入口，以及模组能否在它之前装好 factory 钩子。
- Streamline DLSS-G 与当前直连 NGX 的 VV DLSS-SR 是否能在同一进程稳定共存。
- SL 常量与 VV 矩阵、MV 坐标、reversed-Z 的完整对应。
- MPO/DWM、VRR、全屏切换与输入延迟的实机表现。

IDB 工作遵守现有协议：主 agent 使用 canon/已服务反编译快照；新函数由锚点会话服务，
不为 FG 单独并发打开库。

## 5. 目标架构

建议新增独立 `fg/` 模块，不把 FG 塞进 NR 或 VV SR：

```text
fg/Streamline.*     DLL 搜索、slInit/slSetD3DDevice、feature 函数绑定、日志与关闭
fg/Swapchain.*      factory/swapchain 创建、FG Present、Resize 与直通回退
fg/FrameInputs.*    帧号、Depth/MV/hudless 标记、SL 常量、输入 fence
fg/Config.*         开关、固定 2x、诊断状态（也可并入现有 config 体系）
```

与现有代码的接线：

- `NeuroDragon::enable()`：FG 的 factory 早期钩子必须先于 Overlay 的临时 DXGI 对象和晚期 Present 安装。
- `vv/VvSr.cpp`：只发布已经验证的 Depth/MV 与帧常量，不让 SR 拥有 FG 生命周期。
- P3/P5 新钩子：复制最终 LDR hudless；不拿 `arg10` 冒充。
- `ui/Overlay.cpp`：面板绘制后、原始 `Present/Present1` 前调用 FG dispatch。
- `NeuroDragon::disable()`：先停新帧与 FG，再按 GPU/fence 依赖释放输入、Streamline 和 SR。

## 6. 实施阶段

### F0：静态点位与启动时序（stop/go）

**唯一目标**：证明“能创建 FG 交换链，也能在正确边界取得 hudless”。

1. 由锚点会话定位 Windows P3/P5，确认最终 LDR 与全部 UI 的先后。
2. 确认实际 swapchain 创建 API（Hwnd / 旧式 / CoreWindow）。
3. 将 factory 早期钩子放到模组启用前段，记录它与游戏首次创建的先后。

**通过**：创建钩子命中游戏链；P3/P5 能取得正确 LDR hudless。  
**停止**：模组加载已经晚于链创建，或找不到可靠 pre-UI LDR 点。先修启动/点位，
不强毁现有交换链，不拿 `arg10` 或带 UI 后缓冲冒充正式输入。

### F1：Streamline 与交换链透传

- 固定 Streamline 版本，准备 `sl.interposer/sl.common/sl.dlss_g/sl.reflex/sl.pcl` 与可发现的 `nvngx_dlssg`。
- 使用 manual hooking、frame-based resource tagging、DXGI factory proxy。
- 创建 Streamline 包装链，但保持 `DLSSGMode::eOff`。
- 验证与现有直连 NGX 的 VV SR 共存。

**通过**：普通画面不变，FG feature 可用，连续 Present、F11、拖窗和全屏切换无新错误。  
**回退**：任何初始化失败都使用原 factory/原 Present，不影响 SR。

### F2：只标记输入，不开 FG

- 复用 `depthR32` 与像素单位 `mvRg32`，按 VV 的坐标契约设置 `mvecScale/depthInverted`。
- 从 TAAU current/previous VP 形成重投影矩阵；缺失投影参数单独核实，不填猜测默认值。
- P3/P5 复制并标记 hudless。
- 下一帧重写共享 Depth/MV 前等待 DLSS-G 输入完成 fence；正确后再考虑环形缓冲优化。
- 记录每次 `slSetConstants/slSetTagForFrame` 的返回码与帧号。

**通过**：标记持续成功，SR 输出不变，无 D3D12 错误、陈旧帧号或资源竞争。

### F3：开启固定 2x

- `DLSSGMode::eOn`、`numFramesToGenerate=1`、Reflex Low Latency。
- FG 开启时关闭普通 Vsync/FPS limiter。
- 顺序固定为：游戏 UI → NeuroDragon 面板 → FG dispatch → 原始 Present。
- 无有效世界输入、菜单、加载、Resize 或标记失败时直通。

**通过**：日志显示实际呈现 2 帧、真实帧持续推进、无 stale tag/设备错误；
HUD 不随生成帧扭曲，FG 关闭立即恢复原呈现。

### F4：回归与交付

覆盖静止、移动、快速转视角、开关 FG、进出世界、菜单、暂停、F11、拖窗、截图和长时间运行。
记录光栅帧率、实际呈现倍率、FG 错误、回退次数；观感只作次要信号，不替代日志判据。

## 7. 验收矩阵

| 场景 | 必须确认 |
|---|---|
| FG 开 / 关 | 实际倍率切换；关闭后原始 Present，无残留 FG 帧 |
| 静止 / 移动 / 快速转视角 | Depth/MV 新鲜，无持续旧输入；HUD/文字不变形 |
| 进出世界 / 菜单 / 加载 | 无有效输入时直通，不用菜单资源创建世界 FG |
| F11 / 拖窗 / 全屏 | tearing 标志一致；旧引用释放；Resize 不触发引擎 abort |
| 面板与截图 | 面板位于游戏 UI 后；截图语义明确，不把诊断面板当 hudless |
| 禁用 / 退出 | 先停帧再释放；无 Streamline、驱动或 SR 退出崩溃 |
| 运行时缺失 / 不支持 | 明确日志与 UI 状态，自动直通，不影响原游戏与 SR |

## 8. 参考

- [roadmap §4](../roadmap.md)：FG 为阶段 3。
- [renderdragon §4–§5](renderdragon.md)：P1–P6 与 bgfx Present。
- [vv §34–§35](vv.md)：R3、`arg10`、引擎 MV 与输出语义。
- [ui §1–§2](ui.md)：现有 Present/Resize/queue 设施。
- [2026-10-22 RenderDragon passmap](../research/2026-10-22-renderdragon-passmap.md)：Mac 符号与 P6 证据。
