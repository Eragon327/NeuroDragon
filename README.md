# NeuroDragon

把 Theo's Render Pipeline（TRP，Skyrim SE）的全链路移植到 Minecraft Bedrock 客户端（LeviLamina client）：

```
场景原生输出 → NR 神经渲染 → 超分 → 原生 UI 合成 → 帧生成 → 显示器
```

## 范围

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
| A. 白盒取证：RenderDragon 通道图与接入点 | ✅ 完成 | [docs/A-renderdragon-passmap.md](docs/A-renderdragon-passmap.md) |
| B. Windows 侧符号/钩子落点确认 | ✅ 无 PDB 定位链 + RT 路径定位完成 | [docs/B-windows-probe.md](docs/B-windows-probe.md) |
| C. 工程骨架（xmake + ImGui + 探针） | ✅ xmake 工程 + 探针/钩子 + **面板已实现并部署**（4 页签、`End` 开关，见 [docs/F](docs/F-ui-plan.md)） | `src/neurodragon/probe/`、`src/neurodragon/ui/` |
| D. NR 接入（NGX feature 0x12） | ✅ **跑通并上屏**：core 建 feature + 逐帧 evaluate（**~3.5 ms**，1485×835）+ HDR→LDR 编码 + 增益合成，`Replace=5` 时结果进引擎 DLSS 放大到 2560×1440。2026-10-22 修掉“编码纹理被描述符堆竞争冻结”的根因（§6.16） | [docs/D-rt-dlss-nr-plan.md](docs/D-rt-dlss-nr-plan.md) |
| E. 超分（DLSS SR/DLAA） | ✅ 引擎自带 + **内部分辨率 33/50/58/66/100、DLSS 模型预设、应用即重建**已实现；**自建 RCAS 锐化已实现（待实机，见 J §9）**；78% 未做（原因见 J §4） | [docs/J-sr-implementation.md](docs/J-sr-implementation.md) |
| F. UI 分层 + FG | 🕓 第三阶段（原判「不排期」，2026-10-24 排进路线，见 [docs/M](docs/M-roadmap.md) §4）；UI 分层已由面板的 Present 方案覆盖 | — |
| 下一步优先级 | 四阶段顺序与完成度见 [docs/M](docs/M-roadmap.md)；功能对照与代价见 [docs/K](docs/K-trp-feature-gap.md) | — |

> **阶段顺序与完成度以 [docs/M](docs/M-roadmap.md) 为准**：阶段 1（NR + SR 主体）用户判定为
> **「一半」**，之后是 VV 接管 TAAU → FG → RR。上面这张表是取证/实现里程碑，不等于阶段完成度。

## 驱动要求

- **NR 需要 NVIDIA 驱动 ≥ 617.42**：核心 `_nvngx.dll` 的 feature 表里才出现 `dlssnr`
  （610.88 的核心没有，`core CreateFeature(0x12)` 返回 `OutOfDate`）。
- 驱动**不提供**模型：DriverStore 里只有 `_nvngx.dll` / `nvngx.dll` / `nvngx_dlssg.dll`，
  没有 `nvngx_dlssnr.dll`，所以模型必须随包。
- **模型怎么落地**：`nvngx_dlssnr.dll` 放在**模组目录**里，模组在 `load()` 时把它**硬链接**到
  `Minecraft.Windows.exe` 同级（同卷零拷贝、零额外空间；跨卷或失败则复制；两者都不行就打印
  目标路径让用户手动放）。文件名**必须是 `nvngx_dlssnr.dll`**——core 按这个名字找，
  用版本名（`nvngx_dlssnr_310.9.16.dll`）会得到 `0xBAD0000B`。
- 为什么必须落到 exe 目录：ngx_probe 实测（2026-10-22，同一台机器每次只改一个变量）core 只认
  **exe 同目录** 和 **Init 的 `FeatureCommonInfo::PathListInfo`**；`SetDllDirectoryW`、
  当前目录、`AddDllDirectory`、NGX 的 `dataPath` 全部仍返回 `0xBAD0000B`。
- 建 feature 必须走 **core** 的 `CreateFeature`；模型自己的 `Init_Ext` 在本机恒为 `PlatformError`。


## 取证环境

- IDA Pro GUI + DSH `ida` MCP（`idb_list` 取 session id）
- Windows 客户端：`E:\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe`（1.26.5101.0，**无 PDB**，但 RTTI 类型名可用，见文档 §7.8）
- IDB：`D:\Documents\DecompileData\version\minecraft-edu\minecraft-edu.i64`
- 来源：Minecraft Education **1.26.32.0** macOS 包（Mach-O，imagebase `0x100000000`），**全符号化**
- 目标：Bedrock 客户端 **1.26.51**（LeviLamina 26.51.7），**同为 clang 构建**

> 本目录下的 `.ida/*.json` 是从 IDB 导出的原始取证数据（callees / xrefs / decompile），不要删，后续 Windows 侧比对要用。

## 本地验证

客户端已搬到 `E:\LeviLauncher`（`base_root` 同步改过），E: 盘普通权限可写、`RUNASADMIN` 已移除，
所以「构建 → 部署 → 启动 → 读日志」可以整条链路自己跑，不再需要提权：

```bash
cd D:/Documents/NeuroDragon && xmake build -y      # 增量 <1s / 全量约 15s
# bin/NeuroDragon/ = DLL、PDB、manifest.json、lang/
cp -r bin/NeuroDragon/* "E:/LeviLauncher/versions/1.26.51.01/mods/NeuroDragon/"
grep -E "NR 已启用|NR 就绪|NR 创建失败|：开始|：完成|：失败" "E:/LeviLauncher/versions/1.26.51.01/logs/latest.log"
```

最后那条 grep 里的「开始/完成/失败」是启动阶段的成对 info 标记
（`log::Stage`，见 docs/C §4）：**实测 ≥200 ms 的步骤**在开始前和结束后各留一行（后者带毫秒），
现在只有四处——临时 D3D11 设备与交换链（3.5 s）、面板安装、NR 创建、core 加载模型。
游戏卡死时日志的最后一行就是卡住的那一步。

### 诊断构建（TEST）

BMP dump、NGX 参数追踪（`nr.paramLog`）、`[res]/[desc]/[eval]` 等诊断日志和所有 debug 级
输出都只在定义了 `TEST` 的构建里存在（[src/neurodragon/Test.h](src/neurodragon/Test.h)）。
默认构建是发布版：不写 `dump/`、不钩引擎的 NGX 参数 setter、不含任何 debug 行。

```bash
xmake f -p windows --test=y && xmake build -y   # 诊断构建（TEST 已定义）
xmake f -p windows --test=n && xmake build -y   # 发布构建（默认）
```

`-p windows` 不能省：本机在 msys bash 里跑 `xmake f` 不带它会把平台重配成 `msys`，
随后构建不出模组（2026-10-24 实测）。

发布版保留：info/warn/error 行、`log::Stage` 阶段标记、NR 创建/求值/合成、面板与 GPU 计时。

打包器是 `scripts/after_build.lua`（替换 `@levibuildscript/modpacker`，多拷一个 `lang/`）。
**NR 模型（266 MB）不进包**：手动放到 `mods/NeuroDragon/nvngx_dlssnr.dll`，模组在 load 时
硬链接到游戏根目录（core 只在那儿找，见 docs/D §6.17）。

开关与参数在 `mods/NeuroDragon/config/config.json`（`ll::config`，首次运行自动生成）：
`nr.enabled` 是总开关，`nr.replace` 选上屏模式（**6 = TRP Ratio，默认**；5 = 增益合成；旧的 0「只求值」已从面板移除），其余见 [src/neurodragon/Config.h](src/neurodragon/Config.h)。

细节、判据与边界见 [docs/D-rt-dlss-nr-plan.md](docs/D-rt-dlss-nr-plan.md) §6.16。

## 关键结论（一句话）

`dragon::framerenderer::modules::taau` 就是「3D 之后、UI/色调映射之前」的那个上采样模块，输入里直接带 **`InputBufferMotionVectors`**；接入点不是"在帧图里找缝"，而是**包住这个函数**。

## W1（6 项确认）

| # | 问题 | 结论 |
|---|---|---|
| 1 | taau 采样深度？ | **不采样**（无 `BindFramebufferDepth`，不引用 `SceneDepth`；Windows 1.26.51 签名同构已验证）→ NR 要深度得另钩通道 |
| 2 | MV 生产者 | **不是 MotionOnly pass**（那是 frame component）；推断为 G-buffer color 附件 |
| 3 | 描述符 → `ID3D12Resource` | 中间只有 **bgfx 句柄**，D3D12 资源需自建 handle→resource 桥 |
| 4 | Windows 走 Taau/Fsr1 | **PC 上只有 Taau/Bilinear，没有 Fsr1**（13 个平台配置实测）；tier1-3 Performance=bilinear，其余 taau；当前档位待运行时打印 |
| 5 | present | **bgfx `flip()`**（Mac 实测）；Windows 后端已确认是 bgfx **D3D12**，DXGI Present 在 bgfx 内部 |
| 6 | `UpscalingCameraData` | prev/cur VP + 亚像素抖动/valid history + 世界原点；精确布局待 Windows |

详见 [docs/A-renderdragon-passmap.md](docs/A-renderdragon-passmap.md) 第 7 节。

## 文档

**先看 [docs/README.md](docs/README.md)**：每篇的状态（权威 / 参考 / 历史）与「结论冲突时听谁的」。

| 文档 | 状态 | 内容 |
|---|---|---|
| [docs/README.md](docs/README.md) | 索引 | 文档状态表、权威顺序、TRP 源码读法 |
| [docs/A-renderdragon-passmap.md](docs/A-renderdragon-passmap.md) | 参考 | RenderDragon 通道图、taau 契约、接入点方案、W1 结果 |
| [docs/B-windows-probe.md](docs/B-windows-probe.md) | 参考 | 无 PDB 的 taau 定位链（字符串→xref→`.pdata`）与运行时探针 |
| [docs/C-levilamina-api-notes.md](docs/C-levilamina-api-notes.md) | 参考 | LL API 速查（hook / signature / 线程 / 日志 / config），只记本项目用到的 |
| [docs/D-rt-dlss-nr-plan.md](docs/D-rt-dlss-nr-plan.md) | **NR 事实权威** | RT 路线与 NR 接入的实测：参数表、地址、坑、A~D 步骤 |
| [docs/F-ui-plan.md](docs/F-ui-plan.md) | 历史（已实现） | 面板设计：为什么用 Present 钩子、输入吞噬、设置模型 |
| [docs/G-nr-customization-plan.md](docs/G-nr-customization-plan.md) | 历史（被 K 取代） | 按 TRP 0.4.5 面板逐项判定「能不能做」的推理 |
| [docs/H-sr-plan.md](docs/H-sr-plan.md) | 历史（被 I/J 取代） | SR 可行性：三种「能」、预设四层 |
| [docs/I-sr-final-plan.md](docs/I-sr-final-plan.md) | 部分有效 | 控制层 L1–L6、R_in/R_sr/R_out、VV 迁移 TAAU（未开始部分仍权威） |
| [docs/J-sr-implementation.md](docs/J-sr-implementation.md) | **SR 权威** | 内部分辨率、模型预设、重建的实现、证据与坑 |
| [docs/K-trp-feature-gap.md](docs/K-trp-feature-gap.md) | **路线权威** | TRP 0.5.3 的 SR/NR 全量对照、可做项、代价与排期 |
| [docs/L-nr-remaining-plan.md](docs/L-nr-remaining-plan.md) | **执行权威** | NR 剩余功能的批次、验收判据、探针记录与约定 |
| [docs/M-roadmap.md](docs/M-roadmap.md) | **阶段路线权威** | 四阶段顺序（NR/SR → VV 接管 TAAU → FG → RR）、每阶段状态与开工前必答的未知 |
