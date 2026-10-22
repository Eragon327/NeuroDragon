# F. UI 与设置系统规划

> 状态：**设计稿，未实现**。用户正在做 mod 的标准化（目录/命名/配置），本文件里凡标注
> 「待定案」的点都等标准出来后再落代码，避免返工。

## 0. 一句话

把 NR 的参数从「改 txt + 重启」变成「游戏内面板实时调」，并为后续阶段（Style 调参、
预设、诊断）提供一个统一入口。

## 1. 现状

- 参数来源：`<游戏目录>\nr_params.txt`，模组每秒重读一次；
  支持 `Replace` / `GainBoost` / `Style` / `Intensity` / `LocalToneStrength` /
  `LocalStructureStrength` / `DepthInverted` / `NrEncode` / `NRReset` / `MVecScale` / `ParamLog`。
- 没有任何 UI，也没有预设；每次调参都要切出游戏、改文件、等重读。
- 渲染侧已经具备条件：我们在 NGX `EvaluateFeature` 的钩子里能拿到**引擎的 command list**、
  `Color`（DLSS 输入）与 `Output`（DLSS 输出）资源，并且已经在这根 list 上做编码/合成。

## 2. 目标与非目标

**目标**

- `F10` 开关面板；打开时吞掉游戏输入，关闭立刻恢复。
- 控件与现有参数一一对应，改动**即时生效**，不需要改 txt。
- 预设：命名保存/加载，落在 mod 目录。
- 诊断页：NR / 引擎 DLSS 的 GPU 计时、当前分辨率与 Replace 状态、手动触发一次 dump。

**非目标（本阶段）**

- 不做全屏设置菜单、不做多语言。
- 不替换游戏自带的设置界面。
- 不做 HUD 之上的绘制（那需要 hook Present/swapchain，另议，见 §7）。

## 3. 渲染方案（已定，用户已确认位置）

**画到引擎的 `Output` 上，在引擎自己 evaluate 之后的同一根 command list 里。**

- ImGui `v1.91.0-docking`（dx11/dx12 变体）——xmake 包里已有（levilamina 的依赖），无需自己拉。
- 位置：**3D 之上、HUD 之下**（HUD 都在屏幕边缘，中间的面板不会被挡）。
- 为什么不用 Present hook：省掉 swapchain/backbuffer 生命周期问题，复用现成资源与 command list；
  代价只是面板会被 HUD 覆盖，将来要盖 HUD 再升级。
- 关键点：
  - 首帧创建 ImGui 上下文、字体图集、SRV 堆（字体）与 1 个 RTV（Output）；
  - 每帧：`Output` 从 NGX 之后的状态 → `RENDER_TARGET` → `RenderDrawData` → 还原状态；
  - 面板关闭时不产生任何额外 GPU 工作；
  - 不阻塞主线程，不等待 GPU。

## 4. 输入方案

- `ll::event::input::KeyInputEvent`（**Cancellable**）：`F10` 开关；面板打开时吞掉其它按键（保留 `F10`/`Esc`）。
- `ll::event::input::MouseInputEvent`（**Cancellable**）：x/y/dx/dy 与按钮，直接喂给 ImGui IO；打开时吞掉。
- 顺序很重要：**先让 ImGui 处理，再决定是否 cancel**，否则面板自己的点击也会被吞。
- 不做自己的 WndProc 钩子：LL 的事件已经能拿到坐标与按键，少一个可能打架的钩子。

## 5. 设置模型

- 内存里的 `Settings` 是唯一真源；`nr_params.txt` 降级成「启动导入 / 调试导出」。
- 面板改动 → 立即写入运行参数并标记 dirty（写盘节流，例如 2 秒或关闭面板时）。
- 持久化格式与文件名：**待定案**（跟着用户的标准走）。
- 预设：命名槽，保存一组参数；先给三档内置（`TRP 默认` / `低` / `高`）。

## 6. 里程碑（每个都以“用户能亲手验证”为验收）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M1** 骨架 | 能画出窗口 + 一个 `GainBoost` 滑条 + 一个开关；`F10` 开合；输入被吞 | 拖滑条，画面立刻变亮/变暗；关闭后键鼠恢复正常 |
| **M2** 控件铺满 | `Replace` 开关、`Style`、`Intensity`、`Local*Strength`、`DepthInverted`、`NrEncode`，全部实时生效 | 面板里关掉 NR，画面立刻回到引擎原图 |
| **M3** 持久化 + 预设 | 保存/加载、启动自动读取 | 重启游戏后设置还在 |
| **M4** 诊断页 | GPU 计时、分辨率/模式显示、dump 按钮 | 面板上的毫秒数与日志里的一致 |

M1 约半天，M2–M4 各半天。

## 7. 风险与对策

- **ImGui 的 dx12 backend 与本工程的 `set_exceptions("none")` / `EHs`** 可能冲突：先做最小链接
  验证（能编译、能画出三角形），再铺控件。
- **`Output` 在 NGX 之后的状态未知**：NGX 可能把它留在 `UNORDERED_ACCESS` 或
  `ALL_SHADER_RESOURCE`，我们先按已知状态 transition，必要时加一次 debug 校验日志。
- **输入吞噬**：若 LL 事件的坐标/按钮与原版语义有出入，先在日志里打原始值对齐一次。
- **面板与 NR 求值的顺序**：同一根 command list，先画 UI 或先合成都不影响数值；固定成
  「合成 → 引擎 evaluate → 画 UI」，避免以后改顺序时踩状态。

## 8. 与「标准化」的接口

- UI 代码建议独立成 `src/neurodragon/ui/`，与 `nr/`、`probe/` 平级，便于后续替换或裁剪。
- 本文档中标「待定案」的三处（配置文件格式/名称、模块划分、面板样式）都以用户的标准为准；
  代码只依赖一个 `Settings` 结构体 + 一组回调，样式与文件格式改起来不碰逻辑。
- 若标准化后目录结构变化，M1 只受影响的是 include 路径与文件位置，不影响设计。
