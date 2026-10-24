# NeuroDragon

**给 Minecraft Bedrock 客户端加一层神经网络渲染与超分控制。**

NeuroDragon 是一个 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 客户端模组。它接进游戏原生的
RenderDragon 管线，在 3D 世界画完之后、UI 与色调映射之前插入 NVIDIA 的神经渲染（DLSS-NR），
并把 DLSS 超分的档位、模型预设和锐化交到玩家手里。

| | |
|---|---|
| 游戏 | Minecraft Bedrock **1.26.51**（1.26.5101.0） |
| 加载器 | LeviLamina **client** `26.51.*` |
| 显卡 | NVIDIA RTX（NR 在 RTX 50 系上验证；20/30/40 系通常需要额外的内核包，本模组不提供） |
| 驱动 | NR 需要 **≥ 617.42** |
| 系统 | Windows 10 / 11 x64 |

## 功能

- **神经渲染（NR）**：把 `nvngx_dlssnr.dll` 作为 NGX feature `0x12` 跑在世界画面上，
  结果与原始颜色合成后交给引擎的 DLSS 放大上屏。带历史、按帧求值，可按分辨率变化自动重建。
- **位置轴**：NR 可以站在放大器的两侧——**放大前**（处理渲染分辨率，默认，约 3.5 ms @1485×835）
  或**放大后**（处理输出分辨率，约 9.8 ms @2560×1440，画面更完整但更贵）。
- **超分控制**：内部分辨率 33 / 50 / 58 / 66 / 100（跟随引擎或指定），DLSS 模型预设
  默认 / E / F / J / K / L / M，应用即重建，不需要重进世界。
- **RCAS 锐化**：在引擎 DLSS 之后、调色与 UI 之前做锐化，HUD 与文字不受影响。
- **游戏内面板**：`End` 打开，四个页签（神经渲染 / 超分 / 诊断 / 高级），改完按「应用」生效
  （需要重启才生效的项，面板会标注）。
- **诊断**：分屏对比（左原图 / 右 NR）、A/B 热键、GPU 分段计时、尺寸与参数回读。
- **截图**：`F2` 保存当前成品帧（含 HUD、不含面板）到游戏目录的 `screenshots/`。

## 安装

1. 关闭游戏。
2. 把 `NeuroDragon` 文件夹放进游戏的 `mods/`：

   ```
   mods/NeuroDragon/
   ├── NeuroDragon.dll
   ├── manifest.json
   └── lang/
   ```

3. **放入 NR 模型**：把 `nvngx_dlssnr.dll`（NVIDIA DLSS Neural Rendering 运行时，约 266 MB）
   放进 `mods/NeuroDragon/`。模型不随模组发布，需要自己准备。
4. 启动游戏。模组会在加载时把模型**硬链接**到 `Minecraft.Windows.exe` 同级目录（同卷零拷贝；
   跨卷则复制）。若游戏目录不可写，日志会打印目标路径，手动复制即可。

> 模型文件名**必须是 `nvngx_dlssnr.dll`**——NGX 核心按这个名字找它，带版本号的名字会被拒绝。

## 使用

- `End` 打开/关闭面板（可在「高级 → 热键」重绑），面板打开时 `Esc` 关闭。
- **首次启用 NR**：`神经渲染` 页勾选「启用神经渲染」→ 按「应用」。
- **替换模式**：
  - `6 TRP Ratio`（默认）：按比例/增益合成，高光取自原图；
  - `5 增益`：把网络增益直接叠到画面上，可用「增益」滑条放大，便于观察。
- **输入分辨率**：50 / 66 / 100%。低于 100% 时只在低分辨率上跑网络，guide 仍是全分辨率，
  合成仍在全分辨率做——省 GPU，画面损失用分屏对比判断。
- **热键**：`[` 开/关 NR，`]` 重置 NR 与 DLSS 的历史（擦掉残影），`F2` 截图。
  `F11`（切全屏）始终放行，面板画歪时用它恢复。
- **保存**：面板的「应用」只改本次运行；「保存为默认」才写入
  `mods/NeuroDragon/config/config.json`。
  关闭 NR 时，只有「保存为默认」会真正卸载模型与显存（重新启用会再载入，约 0.3 s）。

## 已知限制

- **VV（Vibrant Visuals）管线还没接管**：当前只在光追（RT）渲染路径上工作；
  VV 下用 DLSS 替换 TAAU 是下一个阶段。
- **「放大后」位置较贵**：100% 时约 9.8 ms（1440p），66/50 的降本档还没做。
- **75% / 78% 内部分辨率不提供**：DLSS 没有对应的原生质量槽，强行借用会被动态范围夹住。
- **mip LOD 偏置、帧生成、光线重建**：未实现。
- **20/30/40 系**：NR 通常需要 TRP 风格的 `.trpk` 内核包，本模组不带，也不做这类内核 patch。
- 只在 **RTX 5070 Laptop + 1.26.5101.0 + LeviLamina 26.51** 上实测；其他组合未验证。

## 从源码构建

需要 xmake、Visual Studio 2022（MSVC x64）与 LeviLamina 的 xmake 仓库（已配置在 `xmake.lua`）。

```bash
xmake f -p windows --test=n     # 发布构建（默认）；--test=y 是诊断构建
xmake build -y
```

产物在 `bin/NeuroDragon/`（DLL、PDB、`manifest.json`、`lang/`），复制到
`<游戏版本目录>/mods/NeuroDragon/` 即可。

- `--test=y`：编译进 BMP dump、NGX 参数追踪、`[res]/[eval]` 等诊断日志；
  发布构建不含这些代码，也不写 `dump/` 目录。
- `-p windows` 不能省：在 msys bash 里不带它会把平台重配成 `msys`，随后构建不出模组。
- 首次构建会拉取 `levilamina`、`imgui`、`minhook` 等依赖。

## 目录

```
src/neurodragon/
├── nr/       # NR：NGX feature 生命周期、编码/合成、位置轴
├── sr/       # 超分：内部分辨率、模型预设、RCAS 锐化
├── ui/       # 游戏内面板：Present 钩子、D3D11On12、输入、截图
├── probe/    # 渲染探针：taau / NGX / 帧统计
├── input/    # 热键
└── utils/    # 日志、杂项
scripts/      # 构建后打包、字形收集
```

## 许可

[CC0 1.0 Universal](LICENSE)。

## 致谢

- **Theo's Render Pipeline（TRP）**：NR / 超分 / 锐化的设计参考与 `NVSDK_NGX` 参数契约来源。
- **LeviLamina / LeviMC**：客户端模组加载器与 SDK。
- **Mangrove / LHolo**：Present 钩子 + D3D11On12 覆盖层的实现参考。
- **Dear ImGui / MinHook**：面板与挂钩。
