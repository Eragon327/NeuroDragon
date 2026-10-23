# F. UI 与设置系统

> 状态（2026-10-23）：**面板已实现并部署**，`End` 开关（可重绑）。本文档描述的是实现后的样子，
> 早期「画到引擎 `Output` 上（HUD 之下）」的方案已作废，原因见 §1。

## 0. 一句话

把 NR 的参数从「改 config.json + 重启」变成「游戏内面板调、按应用生效」，
并为后续（预设、诊断、更多特征页签）留一个统一入口。

## 1. 渲染方案：为什么是 Present 钩子

早期设计是「画在引擎 DLSS 的 `Output` 纹理上、位于 HUD 之下」。这条路的两个代价
（HUD 会盖住面板、主菜单不开光追时钩子不跑）在本地已有更好答案：

`D:\Documents\Mangrove`（`src/mangrove/ui/Overlay.cpp`）与 `D:\Documents\LHolo`
（`src/overlay/ImGuiOverlay.cpp`）都已经做到**顶层**，用的是同一套：

- `MinHook` 挂 `IDXGISwapChain::Present`（虚表 8）/ `Present1`（22）/
  `ResizeBuffers`（13）/ `ResizeBuffers1`（39）；
- 钩 `ID3D12CommandQueue::ExecuteCommandLists`（虚表 10）只为拿到游戏的 direct queue；
- `D3D11On12CreateDevice(device12, ..., &gameQueue, 1, ...)` 建一个 D3D11On12 设备，
  ImGui 用 **DX11 后端**（`imgui v1.91.9` 的 xmake 包已把 `imgui_impl_dx11` /
  `imgui_impl_win32` 编进 `imgui.lib`，不需要拉后端源码）；
- 每帧：`GetCurrentBackBufferIndex` → `GetBuffer` 取 `ID3D12Resource` →
  `ID3D11On12Device::CreateWrappedResource(PRESENT, PRESENT)` →
  `AcquireWrappedResources` → 画 → `ReleaseWrappedResources` → `Flush` → 真正的 `Present`。

所以面板画在后台缓冲上，**在所有游戏内容（含 HUD）之上**，主菜单也能开，
且不受 `Output` 颜色空间 / 后处理影响。

## 2. 文件

| 文件 | 职责 |
|---|---|
| `ui/Overlay.{h,cpp}` | Present/Resize/队列钩子、11on12 设备、窗口过程子类化、帧计时、提示条、卸载 |
| `ui/InputGuard.{h,cpp}` | 拦引擎自己的输入通路（`MouseDevice::feed`、`HIDControllerGameCoreDesktop::$onKeyDown/$onKeyUp`），F11 放行 |
| `ui/Theme.{h,cpp}` | TRP 风格配色、微软雅黑 2x 图集（按实际用字构建）、按视口高度缩放（**下限 1.0**，小分辨率不再缩小） |
| `ui/Menu.{h,cpp}` | 顶部管线图 + 四页签 + 左统计栏 + 右设置栏 + 底部按钮条 |
| `ui/Widgets.{h,cpp}` | 自绘勾选框、滑条、下拉、管线节点/箭头、帧时间折线、页脚按钮 |
| `ui/Glyphs.h` | **生成物**：`scripts/collect_glyphs.py` 从 `src/lang/*.json` 抽出的非 ASCII 字集（现 300 个） |
| `src/lang/*.json` | 面板文案（`ui.*`，`zh_CN` / `en_US` 各 106 个键，占位符一致） |
| `nr/D3D12Bridge` | 新增只读 `nrPanelStats()`：尺寸、状态、错误码、两张 GPU 中位数 |

单独编译单元：`ui/` 里只有 `Overlay.cpp` 碰 `<d3d12.h>`，`Menu/Widgets/Theme` 只碰 ImGui，
`D3D12Bridge.h` 依旧不把 D3D 头带进别的 TU（docs/D §4 的约束不变）。

## 3. 界面

**可拖动浮窗**，默认 850×700 逻辑单位（1440p ≈ 1133×933），左上角 (26,26) 逻辑；
视口高度不足 1080p 时**不再缩小**（2026-10-23：0.7 倍下字看不清、ImGui 软件光标只剩 ~10 px，
小分辨率下整个面板不可用），更大的视口仍按高度比例放大，`[-] 100% [+]` 是用户自己的覆盖；
**只有标题条**（渲染管线图上方那一条）能拖，和 Windows 窗口一个逻辑。窗口是 `NoMove`，
拖拽是自绘状态机：

- 拖动量在 `Begin` **之前**用 `io.MouseDelta` 累加到 `gPos`，然后 `SetNextWindowPos(gPos, Always)`
  ——窗口、背景、两个子栏同一帧用同一个坐标，不会出现「边框动了、子栏还留在原地」；
  面板整体比鼠标晚一帧。
- 命中判定是几何的（`IsMouseHoveringRect(标题条)` 且 `!IsAnyItemHovered()`），不占 item，
  所以缩放按钮照样能点；悬停时把鼠标光标改成「移动」。
- 拖出屏幕保留 120px 可见。

```
┌ 面板（圆角 6，深灰 #212224，橙色 #F0A33C 强调，内边距 18）──────────────────┐
│ NeuroDragon  ← 整条标题条都可以拖动          [-] 100% [+]   ● 健康         │
│ ┌引擎世界──┐ → ┌神经渲染──┐ → ┌引擎 DLSS─┐ → ┌输出────┐                  │
│ │1485x835  │   │NR 运行   │   │超分放大  │   │2560x1440│                  │
│ │光栅      │   │3.25 ms   │   │1.64 ms   │   │本面板    │                  │
│ [神经渲染] [诊断] [高级]                                                   │
│ ┌ 左：帧统计 (330) ─┐ ┌ 右：当前页签的设置 (460) ──────────────────────┐ │
│ │ 光栅帧率 / 帧时间   │ │ 启用神经渲染 / 替换模式 / 增益                │ │
│ │ 自动缩放折线       │ │ Style（下拉）/ 整体 / 局部色调 / 局部结构      │ │
│ │ 渲染尺寸/输出尺寸   │ │ 输入：深度反转、运动矢量；编码；TRP Ratio     │ │
│ │ 状态/替换          │ │                                            │ │
│ │ NR GPU / DLSS GPU  │ │                                            │ │
│ └───────────────────┘ └────────────────────────────────────────────┘ │
│ 未修改                                          [放弃] [应用] [保存为默认] │
└──────────────────────────────────────────────────────────────────────────┘
```

- 页签只有三个：`神经渲染` / `诊断` / `高级`。原来的 `画面` 页内容（开关、替换模式、
  增益、Style）已并入 `神经渲染` 页顶部，页面本身删除。
- 两栏是**固定宽度**（330 / 460 逻辑，gutter 18），窗口宽度由它们推出来，所以滑条不会随
  屏幕变长；行宽另有一道上限（440 逻辑）。子窗口内边距 14 逻辑，**必须传
  `ImGuiChildFlags_AlwaysUseWindowPadding`**（无边框 child 默认没有 padding）；
  统计行的标签超宽时按宽度截断成 `…`（不要用 `PushClipRect`，`RenderText` 的
  `CurrLineTextBaseOffset` 会让裁剪框比字形高一截，把字切掉一半）；
  滑条把手在 0/1 两端内缩半个把手宽度。
- Style 是**下拉**（0/1/2），不是滑条：三个离散档位，滑条上的把手在 0 处只会被切一半。
- 帧时间折线按峰值自动取档（16.7/25/33.3/50/66.7/100/150/250 ms）并平滑过渡。
- 替换模式只列代码里真实存在的三个值：`0 只求值`、`5 增益合成（上屏）`、`6 TRP Ratio`。
  **注意 6 目前不会合成**（`compositeGain` 只挂在 `replace == 5`），界面里已如实标注。

## 4. 输入

- `End` 开关（`config.json` 的 `ui.openKey`，默认 `0x23` = VK_END）；面板打开时 `Esc` 关闭。
- 重绑：面板 `高级 → 热键 → 打开/关闭` 点一下，再按目标键（Esc 取消，单独修饰键不接受）。
  时序上捕获发生在窗口过程里、早于 ImGui 与游戏，所以按键不会漏进任何一边。
- 面板打开时窗口过程吞掉键鼠消息并交给 ImGui；`InputGuard` 同时拦掉引擎自己的 HID 通路
  （两条通路都走，只拦一条会出现「面板开着但人还在走」）。
- 打开瞬间把游戏已收到的按键/鼠标键补发抬起；关闭时 `PostMessage` 让窗口线程把光标
  收回客户区中心再交还游戏（不能在 Present 线程直接动光标）。
- `F11`（切全屏）在两处都放行：面板画歪时这是唯一出路。

## 5. 配置语义（暂存式）

- `config/config.json` **只在启动时导入**，没有任何 hot reload（`reloadConfigIfDue` 已删）。
- 面板里的控件改的是一份 `draft`，运行中的 `config::get().nr` 不动；顶部状态点、管线图、
  左统计栏显示的仍是**正在运行**的值。
- 底部三个按钮：**`应用`** = draft 写进内存（本次运行生效，不落盘）；
  **`保存为默认`** = 应用 + 写 `config/config.json`；**`放弃`** = draft 退回运行值。
- 关闭面板 = 丢弃未应用的改动（有改动时弹一条提示条）。
- 诊断页的三个开关也走同一套暂存逻辑，避免出现「一半实时一半暂存」的混乱。
- 底部状态行：`未修改` / `已修改 · 未应用` / `已应用` / `已保存为默认` / `已放弃改动`。

## 6. 数据来源（都是实测值，没有估算）

| 面板显示 | 来源 |
|---|---|
| 光栅帧率 / 帧时间 / 折线 | `Overlay` 在 `Present` 里用 QPC 计时，240 点环形历史 |
| 渲染尺寸 | `gNr.width/height`（feature 建立时的 1:1 尺寸） |
| 输出尺寸 | 第一次拿到引擎 `Output` 时 `describe()` 读到后缓存 |
| NR GPU / DLSS GPU | `reportOne` 算出的中位数，以 `std::atomic<double>` 发布（渲染线程写、面板读，避免读半截 vector） |
| 求值次数 / 错误码 | `gNr.evalCount` / `gNr.lastError` |

## 7. 坑（都已踩过）

- `imgui` 的 xmake 包**只有** `imgui.lib` 与 `backends/imgui_impl_{dx11,win32}.h`；
  dx12 后端源码不在包里。这里走 DX11 后端 + 11on12，正好绕开。
- `<d3d11on12.h>` 与某些 SDK 的 `<d3d12.h>` 会重复定义三个结构体，`Overlay.cpp` 顶部
  用宏改名绕开（Mangrove 同款）。
- `D3D11On12CreateDevice` 必须拿到游戏**自己的** direct queue，否则画面不同步；
  队列只能从 `ExecuteCommandLists` 钩子里取第一个 DIRECT 队列。
- `ResizeBuffers*` 之前必须先拆掉 11on12 侧所有引用，否则 Size mismatch。
- 字体图集：2 万字全表会拉出几十 MB 纹理 + 明显卡顿，所以只按实际用字建（300 个字形）；
  字表由 `scripts/collect_glyphs.py` 从 `src/lang/*.json` 生成，**收的是所有非 ASCII 字符**
  （只收 `4E00-9FFF` 会让 `，（）：…` 这类全角标点缺失、在界面上显示成 `?`）。
  **改文案（含英文稿）后要重跑**，否则新字会缺字形。
- 缩放与软件光标：`io.MouseDrawCursor` 的箭头是**按当前字号**画的，`FontGlobalScale = s * 0.45`
  会把它一起缩小 —— 720p 下旧的 0.7 倍只剩 ~10 px，鼠标几乎看不见（2026-10-23 用户反馈）。
  现在 `scale` 下限 1.0（小分辨率不缩小），并把 `style.MouseCursorScale` 抬到 1.3。
- 监控不能寄生在 NR 上：引擎的渲染/输出尺寸由 probe 每个求值帧发布（`nrPublishEngineFrame`，
  `Output` 只在资源指针变化时 `describe`），GPU 时间戳对象在第一次求值时创建（`nrEnsureStamps`），
  否则 NR 一关面板就只剩帧率、尺寸全 0（2026-10-23 截图）。
  这一层现在是**最小改动**，正式拆分计划见 §8 待办。
- GPU 计时与面板：`nrStampReset()` 只清原始样本，**不要**在里面清已发布的中位数 ——
  测量窗口每 60 帧结束一次就会调它，清了面板就永远读「无样本」。要清已发布值请用
  `nrStampClearReported()`（面板按钮）。中位数每 30 帧发布一次，`Replace != 0` 时每帧打点。
- 字体图集/字体贴图的构建发生在**第一帧 `ImGui_ImplDX11_NewFrame()`**，所以面板关着时
  也要空跑 2 帧（`kWarmupFrames`），把这笔开销花在加载画面里。

## 8. 待办

- **监控与 NR 拆分（只规划、未实施）**：把「帧统计 + 引擎尺寸/输出 + GPU 时间戳环 + 面板快照」
  从 `nr/D3D12Bridge` 抽成独立的 `monitor` 模块，NR 只负责 feature 生命周期并向它上报自己的
  计时；计划见 **§9**。
- 背景模糊（从后台缓冲降采样 + blur 再叠面板，+0.3~0.5 ms）。
- 预设：命名保存/加载（现在只有「保存为默认」）。
- 诊断页的「手动 dump 一次」按钮（需要桥里加一个触发入口）。
- 把 `Replace=6` 真正接到合成上（`compositeGain` 的条件），然后更新文案。
- 面板热键目前只认单键（不接受组合键）；若要 `Ctrl+...` 之类的组合，需要把
  `Overlay` 的热键判定改成和 `Mangrove::input::KeyBind` 一样按「组合」处理。

## 9. 监控与 NR 的拆分计划（2026-10-23 规划，**未实施**）

现状：`nr/D3D12Bridge.cpp`（~2600 行）同时装着五件事——NR feature 生命周期、HDR↔LDR 编码/合成、
GPU 时间戳环、引擎侧帧事实（`gEngineFrame`）、面板快照（`PanelStats`）。`ui/Menu` 通过
`nrPanelStats()` 读面板数据，所以「NR 卸载」和「监控可用」在 API 上是纠缠的。本轮只做了最小解耦
（`nrPublishEngineFrame` / `nrEnsureStamps` 让数据在 NR 关着时也有），名字仍挂在 `nr::` 下。

目标：监控的生命周期属于模组，NR 只是它汇报数据的一个子系统。

### 阶段 1：抽数据（低风险，约 1–2 h）

- 新增 `src/neurodragon/monitor/FrameMonitor.{h,cpp}`：
  - `publishEngineFrame(w, h, output)` —— 从 `D3D12Bridge` 原样搬（`describe` 的指针缓存一起走）；
  - `struct Snapshot { renderW/H, outputW/H, stampsActive, nrMs/engineMs, samples, featureCreated, enabled }`；
  - `snapshot()` 与 `setNrState(created, w, h, enabled)`。
- `D3D12Bridge` 只在创建/卸载/重建时调 `monitor::setNrState(...)`；
- `Menu` 改读 `monitor::snapshot()`，`PanelStats` 暂时保留作过渡。

### 阶段 2：抽时间戳（中风险，约 1 h）

- `StampRing` + `ensureStamps` + `nrStamp{Begin,End,Tick,Report,Reset,ClearReported}` 整体搬进
  `monitor`（改名 `stampBegin(0=NR, 1=engine)`）：它是**引擎级**基础设施，不属于 NR。
- NR 的计时仍由 probe 在 evaluate 前后用 `monitor::stampBegin/End(0)` 打点，只是换命名空间；
- `nrEnsureStamps` 删除（probe 改调 `monitor::ensureStamps`）。

### 阶段 3：面板只读快照（低风险，约 0.5 h）

- 删除 `PanelStats` / `nrPanelStats()`；`ui/Menu.cpp` 不再 include `nr/D3D12Bridge.h`；
- `ui` 只剩 `monitor`（外加 `Overlay` 的帧统计）依赖。

### 不做的事

- 不动 NR 的 feature 寿命/重建/卸载逻辑（刚稳定，且与监控无关）；
- 不把 probe 的「每帧编排」一起拆（风险高、收益低）；以后统计项再变多，才考虑
  `probe/FrameTelemetry.cpp`。

判据：每个阶段做完都能单独构建，并用面板对照一次（NR 开/关 × 1280×720/2560×1440），不需要一次全做完。
