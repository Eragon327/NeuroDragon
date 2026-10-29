# UI 与设置系统

> **状态**：现行设计，面板已实现并部署。本文由旧 `F` 迁来并更新（旧文归档在
> [archive/2026-10/F-ui-plan.md](../archive/2026-10/F-ui-plan.md)）。
> 早期「画到引擎 `Output` 上（HUD 之下）」的方案已作废，原因见 §1。

## 0. 一句话

把 NR/SR 的参数从「改 config.json + 重启」变成「游戏内面板调、按应用生效」，
并为诊断、A/B 提供统一入口。面板画在**所有游戏内容（含 HUD）之上**，主菜单也能开。

## 1. 渲染方案：为什么是 Present 钩子

早期设计是「画在引擎 DLSS 的 `Output` 纹理上、位于 HUD 之下」。这条路的两个代价——HUD 会盖住面板、
主菜单不开光追时钩子不跑——在本地已有更好答案（`D:\Documents\Mangrove` 与 `D:\Documents\LHolo`
都用同一套）：

- `MinHook` 挂 `IDXGISwapChain::Present`（虚表 8）/ `Present1`（22）/ `ResizeBuffers`（13）/
  `ResizeBuffers1`（39）；
- 钩 `ID3D12CommandQueue::ExecuteCommandLists`（虚表 10）只为拿到游戏的 direct queue；
- `D3D11On12CreateDevice(device12, ..., &gameQueue, 1, ...)` 建 D3D11On12 设备，ImGui 用 **DX11 后端**
  （`imgui v1.91.9` 的 xmake 包已把 `imgui_impl_dx11` / `imgui_impl_win32` 编进 `imgui.lib`）；
- 每帧：`GetCurrentBackBufferIndex` → `GetBuffer` 取 `ID3D12Resource` →
  `ID3D11On12Device::CreateWrappedResource(PRESENT, PRESENT)` → `AcquireWrappedResources` → 画 →
  `ReleaseWrappedResources` → `Flush` → 真正的 `Present`。

⇒ 面板画在后台缓冲上，**在所有游戏内容（含 HUD）之上**，主菜单也能开，且不受 `Output` 颜色空间 /
后处理影响。

## 2. 文件

| 文件 | 职责 |
|---|---|
| `ui/Overlay.{h,cpp}` | Present/Resize/队列钩子、11on12 设备、窗口过程子类化、帧计时、提示条、卸载 |
| `ui/InputGuard.{h,cpp}` | 拦引擎自己的输入通路（`MouseDevice::feed`、`HIDControllerGameCoreDesktop::$onKeyDown/$onKeyUp`），F11 放行 |
| `ui/Theme.{h,cpp}` | TRP 风格配色、微软雅黑 2x 图集（按实际用字构建）、按视口高度缩放（**下限 1.0**） |
| `ui/Menu.{h,cpp}` | 顶部管线图 + 四个页签 + 左统计栏 + 右设置栏 + 底部按钮条 |
| `ui/Widgets.{h,cpp}` | 自绘勾选框、滑条、下拉、管线节点/箭头、帧时间折线、页脚按钮 |
| `ui/Glyphs.h` | **生成物**：`scripts/collect_glyphs.py` 从 `src/lang/*.json` 抽出的非 ASCII 字集 |
| `ui/Screenshot.{h,cpp}` | Present 里抓后缓冲 → WIC PNG（[sr §8](sr.md)） |
| `ui/GameNotice.cpp` | 聊天栏通知（`ui::notice::chat`） |
| `sr/SrStatus.{h,cpp}` | 后端判定（`EngineDlss` / `VvDlss` / 无）+ 共用快照：尺寸、特征、新鲜度 |
| `src/lang/*.json` | 面板文案（`ui.*`，`zh_CN` / `en_US`，占位符一致） |
| `nr/D3D12Bridge` | 只读 `nrPanelStats()`：尺寸、状态、错误码、两张 GPU 中位数 |

单独编译单元：`ui/` 里只有 `Overlay.cpp` 碰 `<d3d12.h>`，`Menu/Widgets/Theme` 只碰 ImGui，
`D3D12Bridge.h` 不把 D3D 头带进别的 TU（[decisions/0004](../decisions/0004-code-conventions.md)）。

## 3. 界面

**可拖动浮窗**，默认 850×700 逻辑单位（1440p ≈ 1133×933），左上角 (26,26) 逻辑；
视口高度不足 1080p 时**不再缩小**（0.7 倍下字看不清、ImGui 软件光标只剩 ~10 px，小分辨率下整个面板
不可用），更大的视口仍按高度比例放大，`[-] 100% [+]` 是用户自己的覆盖；**只有标题条**能拖。

- 拖动量在 `Begin` **之前**用 `io.MouseDelta` 累加到 `gPos`，再 `SetNextWindowPos(gPos, Always)`
  ——窗口、背景、两个子栏同一帧用同一个坐标，不会出现「边框动了、子栏还留在原地」；面板整体比鼠标晚一帧。
- 命中判定是几何的（`IsMouseHoveringRect(标题条)` 且 `!IsAnyItemHovered()`），不占 item；
  悬停时把鼠标光标改成「移动」；拖出屏幕保留 120px 可见。

```
┌ 面板（圆角 6，深灰 #212224，橙色 #F0A33C 强调，内边距 18）──────────────────┐
│ NeuroDragon  ← 整条标题条都可以拖动          [-] 100% [+]   ● 健康         │
│ ┌引擎世界──┐ → ┌神经渲染──┐ → ┌引擎 DLSS─┐ → ┌输出────┐                  │
│ │1485x835  │   │NR 运行   │   │超分放大  │   │2560x1440│                  │
│ │光栅      │   │3.25 ms   │   │1.64 ms   │   │本面板    │                  │
│ [神经渲染] [超分] [诊断] [高级]                                            │
│ ┌ 左：帧统计 (330) ─┐ ┌ 右：当前页签的设置 (460) ──────────────────────┐ │
│ │ 光栅帧率 / 帧时间   │ │ 启用神经渲染 / 位置 / 替换模式 / 增益          │ │
│ │ 自动缩放折线        │ │ Style / 整体 / 局部色调 / 局部结构 / 输入分辨率 │ │
│ │ 渲染尺寸/输出尺寸   │ │ 输入：深度反转、运动矢量；编码；TRP Ratio      │ │
│ │ 状态/替换           │ │                                            │ │
│ │ NR GPU / DLSS GPU   │ │                                            │ │
│ └───────────────────┘ └────────────────────────────────────────────┘ │
│ 未修改                                          [放弃] [应用] [保存为默认] │
└──────────────────────────────────────────────────────────────────────────┘
```

- 页签四个：`神经渲染` / `超分` / `诊断` / `高级`。原来的 `画面` 页内容并入 `神经渲染` 页顶部。
  `超分` 页是超分的读数 + 设置（[sr §1](sr.md)）。
- **`超分` 页按「当前后端」分岔，不按配置分岔**（2026-10-29，`sr/SrStatus`）：顶部一行写
  **画面上真正在做什么**——`DLSS（VV）`（开关开、且上一帧我们的管线真的记录成功）、
  `TAAU（引擎）`（VV 路径但开关关着或还没注入成功）、`引擎 DLSS（光追）`、`无`。
  它不是"这条路能替换"，而是"这一帧谁在放大"；判定用的原始计数（TAAU 声明数、
  draw 跳过/转发/漏、DLSS 求值数）按用户要求只放在 `诊断` 页，正式页面不显示计数器。
  RT 与 VV 的控件语义不同，所以只有**语义相同**的分区共用：
  模型预设、重置、尺寸读数；内部分辨率两边都可控但机制不同（RT 写质量槽 + 因子表，
  VV 写引擎选项 `upscaling_percentage`）；锐化目前只挂在 RT 上，VV 下显示为禁用并说明原因。
- **VV 的总开关**（`sr.vvEnabled`，页面上是「用 DLSS 替换 TAAU」）：开 = 我们的 DLSS 写 arg10
  且引擎那条 TAAU draw 被跳过（draw 级 suppress，EXP-0067；不是整回调，那会连 MV 用的材质绑定
  一起没了）；关 = 完全回到引擎 TAAU。四个开关的先后顺序只写在 `vv::sr::setEnabled` 一处——
  开时先 arm 注入再 arm 跳过（跳过门只对「上一帧真的注入了」的帧生效），关时先恢复 TAAU 再停写。
  诊断页里那几个单步按钮保留为实验用的手动覆盖；`syncConfig` 只在配置值变化时动手，两者不打架。
  **判定依据的原始计数只在 `诊断` 页**（TAAU 声明数、draw `跳过/转发/漏`、DLSS 求值数）：
  声明数涨说明 VV 在跑（suppress 不改变它），draw 的三个数才回答「TAAU 是否真的没执行」；
  用 draw 计数当后端判据是错的——我们一 suppress，它就会显示「VV 没了」。
- **禁用规则**：没有后端时，除「内部分辨率」外的分区整体 `BeginDisabled`，读数保留（变暗）；
  内部分辨率单独放行，因为 100% 会把上采样整个关掉，这个下拉是唯一的回头路。
  判定只看计数是否在涨（`taauDrawStats().declared` / `srPanelStats().evalCount`），
  不看设置项——`upscaling_percentage = 100` 时设置看着是开的、TAAU 却一次都不跑（EXP-0081）；
  也不用 TAAU 的 **draw** 计数，因为我们自己的 draw 级 suppress 跳的正是那条 draw。
- **例外的那一个读设置的地方**：`graphics_mode` 与 `upscaling_percentage` 在进世界/每 2 s 读一次并
  缓存（`probe::options::knownValues`）。理由同上——100% 时一个逐帧信号都没有，只有设置本身能
  说明这台机器是 VV，页面才知道该把分辨率下拉留在 VV 语义下而不是变成死路。
- 两栏是**固定宽度**（330 / 460 逻辑，gutter 18），窗口宽度由它们推出来，滑条不会随屏幕变长；
  行宽另有一道上限（440 逻辑）。子窗口内边距 14 逻辑，**必须传
  `ImGuiChildFlags_AlwaysUseWindowPadding`**（无边框 child 默认没有 padding）；统计行标签超宽时按宽度
  截断成 `…`（不要用 `PushClipRect`，`RenderText` 的 `CurrLineTextBaseOffset` 会让裁剪框比字形高一截）；
  滑条把手在 0/1 两端内缩半个把手宽度。
- Style 是**下拉**（0/1/2），不是滑条：三个离散档位，滑条上的把手在 0 处只会被切一半。
- 帧时间折线按峰值自动取档（16.7/25/33.3/50/66.7/100/150/250 ms）并平滑过渡。
- 替换模式只列代码里真实存在的两个值：`5 增益合成`、`6 TRP Ratio`（默认）。
  「0 只求值」已删除（[nr §5.1](nr.md)）。

## 4. 输入

- `End` 开关（`config.json` 的 `ui.openKeys`，默认 `0x23` = VK_END）；面板打开时 `Esc` 关闭。
- 重绑：面板 `高级 → 热键` 点一下再按目标键（Esc 取消，单独修饰键不接受）。捕获发生在窗口过程里、
  早于 ImGui 与游戏，所以按键不会漏进任何一边。热键是**组合**（`input::KeyBind`，修饰键在前）。
- 面板打开时窗口过程吞掉键鼠消息并交给 ImGui；`InputGuard` 同时拦掉引擎自己的 HID 通路
  （两条通路都走，只拦一条会出现「面板开着但人还在走」）。
- 打开瞬间把游戏已收到的按键/鼠标键补发抬起；关闭时 `PostMessage` 让窗口线程把光标收回客户区中心
  再交还游戏（不能在 Present 线程直接动光标）。
- `F11`（切全屏）在两处都放行：面板画歪时这是唯一出路。
- 游戏内热键：`[` NR 开/关、`]` 重置 NR + 引擎 DLSS 历史、`F2` 截图（[sr §8](sr.md)）。

## 5. 配置语义（暂存式）

- `config/config.json` **只在启动时导入**，没有 hot reload（`reloadConfigIfDue` 已删）。
- 面板里的控件改的是一份 `draft`，运行中的 `config::get()` 不动；顶部状态点、管线图、左统计栏显示的
  仍是**正在运行**的值。
- 底部三个按钮：**`应用`** = draft 写进内存（本次运行生效，不落盘）；**`保存为默认`** = 应用 + 写
  `config/config.json`；**`放弃`** = draft 退回运行值。
- 关闭面板 = 丢弃未应用的改动（有改动时弹一条提示条）。
- 诊断页的开关也走同一套暂存逻辑，避免「一半实时一半暂存」。
- 底部状态行：`未修改` / `已修改 · 未应用` / `已应用` / `已保存为默认` / `已放弃改动`。
- 特例：NR 的「保存为默认」且配置为关闭时会真正卸载 feature 与模型；「应用」只停求值
  （[nr §5.4](nr.md)）。

## 6. 数据来源（都是实测值，没有估算）

| 面板显示 | 来源 |
|---|---|
| 光栅帧率 / 帧时间 / 折线 | `Overlay` 在 `Present` 里用 QPC 计时，240 点环形历史 |
| 引擎渲染尺寸 / 输出尺寸 | probe 每个求值帧发布（`nrPublishEngineFrame`），`Output` 只在资源指针变化时 `describe` |
| NR 工作尺寸 | `gNr.width/height`（feature 建立时的尺寸；后档 = 输出尺寸） |
| NR GPU / DLSS GPU | `reportOne` 算出的中位数，以 `std::atomic<double>` 发布（渲染线程写、面板读，避免读半截 vector） |
| 求值次数 / 错误码 | `gNr.evalCount` / `gNr.lastError`，外加求值失败计数 |
| SR 尺寸链 / 质量槽 / 预设 | [sr §9](sr.md) 的自检与遥测 |

## 7. 坑（都已踩过）

- `imgui` 的 xmake 包**只有** `imgui.lib` 与 `backends/imgui_impl_{dx11,win32}.h`；dx12 后端源码不在包里。
  这里走 DX11 后端 + 11on12，正好绕开。
- `<d3d11on12.h>` 与某些 SDK 的 `<d3d12.h>` 会重复定义三个结构体，`Overlay.cpp` 顶部用宏改名绕开
  （Mangrove 同款）。
- `D3D11On12CreateDevice` 必须拿到游戏**自己的** direct queue，否则画面不同步；队列只能从
  `ExecuteCommandLists` 钩子里取第一个 DIRECT 队列。
- `ResizeBuffers*` 之前必须先拆掉 11on12 侧所有引用，否则 Size mismatch。
- 字体图集：2 万字全表会拉出几十 MB 纹理 + 明显卡顿，所以只按实际用字建；字表由
  `scripts/collect_glyphs.py` 从 `src/lang/*.json` 生成，**收所有非 ASCII 字符**
  （只收 `4E00-9FFF` 会让 `，（）：…` 这类全角标点缺失、显示成 `?`）。**改文案（含英文稿）后要重跑**。
- 缩放与软件光标：`io.MouseDrawCursor` 的箭头按当前字号画，`FontGlobalScale = s * 0.45` 会把它一起缩小
  —— 720p 下旧的 0.7 倍只剩 ~10 px。现在 `scale` 下限 1.0，并把 `style.MouseCursorScale` 抬到 1.3。
- 监控不能寄生在 NR 上：引擎的渲染/输出尺寸由 probe 每个求值帧发布，GPU 时间戳对象在第一次求值时创建，
  否则 NR 一关面板就只剩帧率、尺寸全 0。这一层现在是**最小改动**，正式拆分见 §8。
- GPU 计时与面板：`nrStampReset()` 只清原始样本，**不要**在里面清已发布的中位数——测量窗口每 60 帧
  结束一次就会调它，清了面板就永远读「无样本」。要清已发布值用 `nrStampClearReported()`（面板按钮）。
  中位数每 30 帧发布一次，`Replace != 0` 时每帧打点。
- 字体图集/字体贴图的构建发生在**第一帧 `ImGui_ImplDX11_NewFrame()`**，所以面板关着时也要空跑 2 帧
  （`kWarmupFrames`），把这笔开销花在加载画面里。

## 8. 待办

- **监控与 NR 拆分（只规划、未实施）**：现状 `nr/D3D12Bridge.cpp`（~2600 行）同时装着五件事——NR feature
  生命周期、HDR↔LDR 编码/合成、GPU 时间戳环、引擎侧帧事实（`gEngineFrame`）、面板快照（`PanelStats`）。
  `ui/Menu` 通过 `nrPanelStats()` 读数据，所以「NR 卸载」和「监控可用」在 API 上纠缠。本轮只做了最小解耦
  （`nrPublishEngineFrame` / `nrEnsureStamps`），名字仍挂在 `nr::` 下。
  - 目标：监控的生命周期属于模组，NR 只是它汇报数据的一个子系统。
  - 阶段 1（1–2 h）：新增 `monitor/FrameMonitor.{h,cpp}`，搬 `publishEngineFrame` 与
    `Snapshot`；`D3D12Bridge` 只在创建/卸载/重建时调 `monitor::setNrState(...)`；`Menu` 改读
    `monitor::snapshot()`。
  - 阶段 2（1 h）：`StampRing` + `ensureStamps` + `nrStamp*` 整体搬进 `monitor`（改名
    `stampBegin(0=NR, 1=engine)`）；它本来就是引擎级基础设施。
  - 阶段 3（0.5 h）：删 `PanelStats` / `nrPanelStats()`；`ui/Menu.cpp` 不再 include `nr/D3D12Bridge.h`。
  - 不做：不动 NR 的 feature 寿命/重建/卸载逻辑；不拆 probe 的每帧编排。
  - 判据：每个阶段做完都能单独构建，并用面板对照一次（NR 开/关 × 1280×720 / 2560×1440）。
- 背景模糊（从后台缓冲降采样 + blur 再叠面板，+0.3~0.5 ms）。
- 预设：命名保存/加载（现在只有「保存为默认」）。
- 诊断页的「手动 dump 一次」按钮（需要桥里加触发入口）。
- 热键已升级为**组合键**（`Config.h` 的四个 `*Keys` 数组，修饰键在前）；不再支持单键配置文件，
  也不做迁移。

## 9. 参考

- 旧 `F`（面板设计原文）：[archive/2026-10/F-ui-plan.md](../archive/2026-10/F-ui-plan.md)
- 代码约定：[decisions/0004](../decisions/0004-code-conventions.md)
- 面板数据的一半来自 NR：[nr §6](nr.md)；SR 页：[sr §9](sr.md)
