# NeuroDragon

把 Theo's Render Pipeline（TRP，Skyrim SE）的全链路移植到 Minecraft Bedrock 客户端（LeviLamina client）：

```
场景原生输出 → NR 神经渲染 → 超分 → 原生 UI 合成 → 帧生成 → 显示器
```

## 范围（2026-10-20 用户定稿）

**做**（= TRP 的渲染链路本身）：

- NR 神经渲染（NGX feature `0x12`，`nvngx_dlssnr.dll`）
- 超分：DLSS SR / DLAA（XeSS 作为 AMD 路径，视 SDK 可行性）
- 原生分辨率 UI 合成
- 帧生成：DLSS-G（XeFG 视 SDK 可行性）
- ImGui 设置界面、预设/配置持久化、性能与诊断探针

**不做**：

- **ReShade 互操作层**（ReShade 本体是独立第三方软件，不属于 TRP；TRP 只提供了可选的
  `ReShadeIntegration` 胶水，本项不移植。TRP 仓库里的 `ReShadeIntegration.*` / `ReShadeSwapChain.h`
  原样留给 TRP，不动）
- ENB / Community Shaders 这类 Skyrim 专属兼容层
- 与 TRP 无关的额外功能

## 当前状态

| 阶段 | 状态 | 产出 |
|---|---|---|
| A. 白盒取证：RenderDragon 通道图与接入点 | ✅ 完成（2026-10-20） | [docs/A-renderdragon-passmap.md](docs/A-renderdragon-passmap.md) |
| B. Windows 侧符号/钩子落点确认 | 待开始 | — |
| C. 工程骨架（xmake + ImGui + 探针） | 待开始 | — |
| D. NR 接入（NGX feature 0x12） | 待开始 | — |
| E. 超分（DLSS SR/DLAA） | 待开始 | — |
| F. UI 分层 + DLSS-G | 待开始 | — |

## 取证环境

- IDA Pro GUI + DSH `ida` MCP（`idb_list` 取 session id）
- Windows 客户端：`D:\Program Files\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe`（1.26.5101.0，**无 PDB**，但 RTTI 类型名可用，见文档 §7.8）
- IDB：`D:\Documents\DecompileData\version\minecraft-edu\minecraft-edu.i64`
- 来源：Minecraft Education **1.26.32.0** macOS 包（Mach-O，imagebase `0x100000000`），**全符号化**
- 目标：Bedrock 客户端 **1.26.51**（LeviLamina 26.51.7），**同为 clang 构建**

> 本目录下的 `.ida/*.json` 是从 IDB 导出的原始取证数据（callees / xrefs / decompile），不要删，后续 Windows 侧比对要用。

## 关键结论（一句话）

`dragon::framerenderer::modules::taau` 就是「3D 之后、UI/色调映射之前」的那个上采样模块，输入里直接带 **`InputBufferMotionVectors`**；接入点不是"在帧图里找缝"，而是**包住这个函数**。

## W1（6 项确认，2026-10-20）

| # | 问题 | 结论 |
|---|---|---|
| 1 | taau 采样深度？ | **不采样**（无 `BindFramebufferDepth`，不引用 `SceneDepth`；Windows 1.26.51 签名同构已验证）→ NR 要深度得另钩通道 |
| 2 | MV 生产者 | **不是 MotionOnly pass**（那是 frame component）；推断为 G-buffer color 附件 |
| 3 | 描述符 → `ID3D12Resource` | 中间只有 **bgfx 句柄**，D3D12 资源需自建 handle→resource 桥 |
| 4 | Windows 走 Taau/Fsr1 | **PC 上只有 Taau/Bilinear，没有 Fsr1**（13 个平台配置实测）；tier1-3 Performance=bilinear，其余 taau；当前档位待运行时打印 |
| 5 | present | **bgfx `flip()`**（Mac 实测）；Windows 后端已确认是 bgfx **D3D12**，DXGI Present 在 bgfx 内部 |
| 6 | `UpscalingCameraData` | prev/cur VP + 亚像素抖动/valid history + 世界原点；精确布局待 Windows |

详见 [docs/A-renderdragon-passmap.md](docs/A-renderdragon-passmap.md) 第 7 节。
