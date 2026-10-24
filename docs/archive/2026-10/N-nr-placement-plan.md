# N. NR 位置轴：Before upscaling / After upscaling

> **来源**：用户 2026-10-24 口述。原文要点：
> 「只做 `[SourceDLSSG] NRBeforeUpscaling` 的两档（面板 Placement 里的 Before upscaling 和
> After upscaling）」；「前 NR：NR 在 DLSS/CS 放大之前、按渲染分辨率处理世界色，结果拷回给放大器当输入。
> 最终图像仍归放大器所有，UI 在放大之后叠加」；「后 NR：NR 在放大之后、按输出分辨率处理放大后的世界色，
> 结果直接就是最终画面；原生 UI 在 NR 之后叠加，帧生成的 HUD-less 实时帧也取自 NR 输出，等于 NR 接管了
> 最终图像」；「这两个，其他不用」。
>
> **效力**：本文管「位置轴」这一条的路线、判据与验收。实现落地后：NR 契约与实测 → 写回
> [D](D-rt-dlss-nr-plan.md)；功能对照状态 → [K](K-trp-feature-gap.md)；阶段完成度 → [M](M-roadmap.md)；
> 执行约定沿用 [L §0](L-nr-remaining-plan.md#L8-L22)。
>
> **状态（2026-10-24 晚）**：P0 完成（§3.1）；P1 实现（§3.2）并完成一次实机验收（§6.1：
> 后档 NR 9.78 ms、无错误/回落、写回成功）；**release 已部署主目录**（SHA-256 `fd90f578…`）。
> §6 第 2/4/5 条（分屏、快速转视角鬼影、窗口缩放）在 release 里继续观察；P3（66% 降本）等后续拍板。
> 按用户裁定，上一版规划里的其它项
> （RCAS/截图的实机验收、ToneBands 保真、白点回读、换 `nvngx_dlss.dll`、78%、mip LOD 偏置）**全部不排**；
> 其中只有「After 档与 RCAS 的先后顺序」会在 P2 处理。

## 0. 一句话

把「NR 站在放大器的哪一侧」做成可切换的两档：**Before upscaling**（现状，NR 处理渲染分辨率的
`Color`，结果进放大器）与 **After upscaling**（新增，NR 处理输出分辨率的 `Output`，结果写回
`Output`、成为调色与 UI 之前的最终 HDR 图）。两档互斥，同一帧只跑一个。

## 1. 两档的语义（用户定义 → 我们的落点）

| | 前 NR（Before upscaling，现状） | 后 NR（After upscaling，新增） |
|---|---|---|
| 输入 | 引擎 `Color` 1485×835 线性 HDR（峰值实测 ~69） | 引擎 `Output` 2560×1440 线性 HDR（引擎 DLSS 刚写完） |
| NR 工作尺寸 | 渲染分辨率（可 ×50/66/100，见 L 批 2.1） | **输出分辨率**（默认 100%） |
| guide | `Depth`/`MVec` 1485×835，`DLSSNR.MVecScaleX/Y` = 渲染尺寸 | 同左（**未知：guide 比 color 小，运行时接不接受**，见 §3 P0） |
| 结果去向 | 合成后把引擎参数对象的 `Color` 指针指向我方纹理 | 合成后 `COPY_DEST` **写回引擎 `Output`** |
| 之后谁接手 | 引擎 DLSS 放大 → 调色 → UI 合成 | 引擎调色 → UI 合成（**不再放大**，NR 输出即最终 HDR 图） |
| UI 被 NR 处理吗 | 不被（UI 在放大之后合成） | 不被（同理，两档一样） |
| 成本（1440p） | 3.5 ms @1485×835；66% ≈1.5、50% ≈0.9（L 批 2.1 的**预期**值） | ≈9.6 ms @100%（网络 8.75 实测 + 编码/合成/写回 ≈0.8）；66% ≈4.6、50% ≈3.0（估算） |

两点需要说清：

- **「拷回」的等价物**：前 NR 现在没有真的拷回放大器输入，而是把引擎参数对象的 `Color` 换成我方纹理
  （[D §6.16](D-rt-dlss-nr-plan.md#L518) 的 Route A）。对放大器是同一件事，且不碰引擎资源——已验证，不改。
- **「接管最终图像」的边界**：Bedrock 的调色与 UI 合成在 SR **之后**（[J §9](J-sr-implementation.md#L105)），
  所以我们接管的是**调色前的最终 HDR 图**，调色/UI/Present 仍归引擎。这正好符合 TRP 的意图（UI 不被 NR 处理），
  也是阶段 3 帧生成要的 HUD-less 源（[M §4.2](M-roadmap.md#L88)）。

## 2. 为什么这次敢重开 After 档

旧结论是「不做」，两条都在本仓库里：

- [G §4](G-nr-customization-plan.md#L110)：「Placement：放大后跑 NR —— 引擎拥有 SR 与最终图像；放大后跑 NR
  就得接管 `EvaluateFeature` 的输出、UI 合成与 present」⇒ 判结构性不可做；
- [K §3.5](K-trp-feature-gap.md#L171)：「2560×1440 上 NR ≈ 8.75 ms，四倍成本；TRP 自己也建议优先『前』」。

**判据变了（2026-10-24）**：[J §9](J-sr-implementation.md#L119) 的 RCAS 已经在引擎 `Output` 上做过
`ALL → COPY_SOURCE → COPY_DEST → ALL` 的往返，两次实机画面正常、窗口缩放不崩。⇒ After 档只需要
「读 `Output` + 写回 `Output`」，**不需要接管 UI 合成与 present**。

于是 After 档从「结构性不可做」降级为「一个待探针回答的契约未知」：**guide（Depth/MV，渲染分辨率）
比 color（输出分辨率）小，310.x 运行时接不接受**。成本仍按 ≈9.6 ms 记（这条老结论不变，只是不再等于「不做」）。

## 3. 路线（P0–P4）

| 步 | 内容 | 判据（先写判据再动手） | 代价 | 状态 |
|---|---|---|---|---|
| **P0 探针**（离线，不进游戏） | 扩 `ngx_probe` 的 `--after` 组（设计见下） | ① create=1；② 两帧 eval 都=1；③ 输出非退化（非全黑/非 NaN/非常量）；④ 两帧不同（时序在跑）；⑤ 两档 `MVecScale` 的 dump 差异记下来 | 0.5–1 天 | **已完成**（2026-10-24，结论见 §3.1） |
| **P1 After 最小闭环**（TEST） | `nr.placement` + 面板「位置」两档；After 路径：读 `Output` → 编码 → NR → Ratio 合成 → 写回 `Output`；切位置重建 feature + 双侧 Reset + 失败回落 Before | 分屏：左=`Output` 原图、右=NR 合成（输出分辨率上可辨）；切回 Before 画面与开关前一致；不花屏/不冻结/不崩 | 1–1.5 天 | **已完成并实机验收（2026-10-24，§6.1）** |
| **P2 顺序与读数** | After 写回之后再跑 RCAS（若开）；诊断页回读位置 / work×guide×输出尺寸 / 编码·求值·合成·写回四段计时 | 顺序固定「NR → RCAS」；回读值 = 实际写入；关 RCAS 时画面不受影响 | 0.5 天 | **已完成（并入 P1，随 P1 验收）**：顺序固定 `NR → RCAS`；诊断页回读位置/域/guide；写回计时并入合成 stamp |
| **P3 降本档**（可选） | After 也支持 work = 输出 × 50/66，合成仍在输出分辨率 | 66% 成本 ≈4.6 ms 且画面可接受 | 0.5 天 | 未开工（拍板点见 §7；等后续拍板） |
| **P4 收尾** | `--test=y` + `--test=n` 双构建 → 部署 `E:\LeviLauncher\versions\1.26.51.01\mods\NeuroDragon`；文档回写 D/K/M + CHANGELOG | 发布构建同样能切两档 | 0.5 天 | **已完成**：release 已部署（`fd90f578…`）；文档已回写 |

### P0 探针设计（离线，不进游戏）

- 工具：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp`（已有自建设备 + core Init +
  `CreateFeature(0x12)` 尺寸扫描 + ratio kernel；改工具不进 mod 仓库，见 L §0 与工作区分层约定）。
- 要加 `--after` 组：建 3 张纹理（color / output = 2560×1440 的线性内容，depth / MV = 1485×835），
  按 `D3D12Bridge.cpp` 的**同名参数**置：
  `DLSSNR.Width/Height` = 输出尺寸；`DLSSNR.ColorSubrect*` / `OutputSubrect*` / `BackbufferSubrect*`
  = 输出尺寸；`DLSSNR.DepthSubrect*` / `MVecSubrect*` = 渲染尺寸；`DLSSNR.MVecScaleX/Y` 跑**两档**
  （渲染尺寸 / 输出尺寸）；连跑 2 帧（第 2 帧色内容轻微变化），dump 两次 output 并打印 create/eval 返回码。
- 判据：见上表 P0 行；两档 `MVecScale` 的差异记进本文件，作为实机 A/B 的先验。
- **判否时的退路**（P0 出结论当天就要选，不要悬着）：
  - **O1** 自升采样 guide：depth 双线性、MV 拷贝并按输出尺寸重标定 scale（1–2 天，保住「按输出分辨率处理」的定义）；
  - **O2** 降采样后按渲染尺寸跑、只在输出分辨率合成（1 天，便宜但语义偏离用户定义）。

**为什么先做探针**：这是 After 档唯一的未知（guide 尺寸契约），用离线探针答掉，就不需要
「改一版、进游戏看一眼、再改一版」（L §0 约定 1–2）。

### 3.1 P0 结果（2026-10-24，`ngx_probe --after=all`）

探针位置：`D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp`（新增
`--after[=before|guide|output|all]`；`--out=WxH` 设输出尺寸、`--after-render=WxH` 设 guide 尺寸）。
走 **core 自己的 `CreateFeature(0x12)`**（模组同一条路径），三张合成纹理、跑 2 帧后回读 Output 统计。

| 变体 | color/output | guides | MVecScale | create | eval f0/f1 | 输出 mean | 跨帧 meanAbsDiff |
|---|---|---|---|---|---|---|---|
| before（对照） | 1485×835 | 1485×835 | 1485×835 | 1 | 1 / 1 | 0.802 → 0.825 | 0.130 |
| after-guide | 2560×1440 | 1485×835 | 1485×835 | 1 | 1 / 1 | 0.768 → 0.793 | 0.169 |
| after-output | 2560×1440 | 1485×835 | 2560×1440 | 1 | 1 / 1 | 0.768 → 0.787 | 0.166 |

（输出均为 `R16G16B16A16_FLOAT`；三档 `nan=0 inf=0`，跨帧 500 万+ 个分量变化。）

结论：

1. **guide < color 的 after 契约成立**（本机 core-create 路径，与模组同一条路）⇒ 不需要 O1/O2，
   直接按 §3 P1 实装；
2. 两档 `MVecScale` 的 frame1 输出不同（hash 不同、diff 0.169 vs 0.166）⇒ 运行时确实消费这个值；
   首帧两档 after 的 hash 相同（首帧不依赖尺度），后续帧才分开。**默认取 guide 尺寸**（TRP 的
   310.x 契约同款），配置 `nr.mvecScale` 仍可覆盖；
3. **补测（2026-10-24 晚）**：模组每帧写的是「非前缀 `Width/Height` = guide、`DLSSNR.Width/Height`
   = feature」，而上面首轮探针写的是 display。给探针加 `--after-wh=guide` 复现模组的精确组合后，
   两档 after 的 create/eval、hash、mean、跨帧 diff 与首轮**逐位相同** ⇒ 运行时不吃这两个非前缀键；
   模组实际参数组合已离线验过，P1 的实机只剩观感/成本/Output 状态三项要确认；
4. 后面只剩实机确认观感与成本，不需要再为「接不接受」做离线实验。

### 3.2 P1 实现记录（2026-10-24 晚）

**代码（工作区未提交）**

- `Config.h`：`nr.placement`（0 = 放大前 / 1 = 放大后），`version` 2 → 3。
- `nr/D3D12Bridge.{h,cpp}`：`domain`（前=渲染色 / 后=引擎 `Output`）与 `guide`（Depth/MV=渲染尺寸）
  分离；后档读 `Output` → 编码 → NR → Ratio 合成到私有纹理 → **`CopyTextureRegion` + `D3D12_BOX`
  写回 `Output`**（`gDecodedColor` 只增不减，窗口缩小时 `CopyResource` 的尺寸约束不成立）；
  切位置重建 feature（先建后拆）+ 双侧 Reset + 还引擎 `Color` 指针；after 创建/重建失败设置
  `afterFallback` 并**保持旧 feature**（不把 NR 整档关掉），回落走 core→runtime 完整梯子；
  after 运行中 `Output` 缺一帧时跳过该帧，不再来回切档；`Output` 非 RGBA16F 时回落并告警。
- `probe/RenderProbe.cpp`：位置轴拆成「引擎求值前（before）/ 后（after）」；after 在
  `nrStampEnd(1)` 之后、`out_after` dump 与 RCAS 之前，顺序固定 `NR → RCAS`；TEST dump 的
  `base` 在后档改成引擎 `Output`。
- `ui/Menu.cpp` + `src/lang/{zh_CN,en_US}.json` + `scripts/collect_glyphs.py`：位置两档、成本提示、
  回落提示、诊断页回读「位置 / NR 处理尺寸 / 指南尺寸」；i18n 键 290 / 290（en、zh）对齐，
  glyphs 480 个。
- 构建/部署：`--test=y` 与 `--test=n` 均通过；TEST 用 `xmake build -y -r` 干净重编、部署后
  SHA-256 `5b3b9f36…`。实机验收后又修了 TEST 诊断（后档 `base` dump 改到写回前、`域/特征`
  标签改成 `引擎渲染`），重编 release 并部署主目录：DLL SHA-256 **`fd90f578…`**；
  TEST 备份 `.bak-test-placement`，改动前备份 `.bak-before-placement`。产物留档
  `D:\Documents\NeuroDragonWorkSpace\backup\NeuroDragon-placement-release-20261024.dll`
  （同一 `fd90f578…`）。

**两轮独立审查（DSH code review）**

1. 第一轮发现并修复：后档写回用 `CopyResource` 在窗口缩小时源/目标尺寸不一致（改为
   `CopyTextureRegion` + box）；after 回落只重跑 core 梯子（补 runtime）；回落文案承诺的
   「重新启用」不会重试（文案改为重启）；`Output` 格式不符的回落静默（补日志 + 用户提示）。
2. 第二轮确认四项修好，并再修两条不阻塞项：placement 重建失败会逐帧重试（改为记录失败目标并
   保持旧 feature）；after 运行中 `Output` 瞬缺会来回重建两次（改为跳过该帧）。

**放入实机验收前的唯一未知**：引擎 `Output` 在 `EvaluateFeature` 返回时是否稳定处于
`ALL_SHADER_RESOURCE`。旧会话日志（2026-10-24 17:39）里 RCAS 已对同一资源做过
`ALL → COPY_SOURCE → COPY_DEST → ALL` 往返，`sharp_before_672` / `sharp_after_672` 都成功写出
（max 48 → 199、nz 11.4M），所以这条前提已有实机先验；后档只是把同一往返换成
「读 Output → 私有纹理 → CopyTextureRegion 写回」。

## 4. 改动落点（已核对代码）

| 文件 | 要改什么 |
|---|---|
| `src/neurodragon/Config.h` | `nr.placement`：0 = 放大前（默认）/ 1 = 放大后 |
| `src/neurodragon/nr/D3D12Bridge.{h,cpp}` | 参数集**已经**按 feature / guide 分离（`DLSSNR.*Subrect` 用 feature 尺寸、`Depth`/`MVec Subrect` 用 guide 尺寸，`D3D12Bridge.cpp:2640-2689`），所以 After 档主要是三件：① 输入资源从引擎 `Color` 换成 `Output`；② 纹理按 **`Output` 的 desc** 分配（现在统一按 `Color` 的 desc，`:1049-1077`、`:1862`、`:2183`）；③ 合成落点从「换引擎参数对象的 `Color` 指针」（`:2532`）换成「`COPY_DEST` 写回 `Output`」（照 `sr/SrSharpening.cpp` 的 source/target 模式） |
| `src/neurodragon/probe/RenderProbe.cpp` | After 路径放在 `gNgxEvalOriginal` 返回 1 之后、RCAS 调用点（`:1132`）之前，顺序固定「NR → RCAS」；切位置时置一次双侧 Reset |
| `src/neurodragon/ui/Menu.cpp` + `src/lang/{zh_CN,en_US}.json` + `scripts/collect_glyphs.py` | 面板「位置」两档 + 诊断页回读（位置 / work / guide / 输出尺寸） |
| `D:\Documents\NeuroDragonWorkSpace\tools\ngx_probe.cpp` | 只 P0 用；不进 mod 仓库 |

## 5. 风险与处置

| 风险 | 表现 | 处置 |
|---|---|---|
| 运行时不接受 color > guide | create 返回非 `0x1`，或 eval 后输出全黑/花屏 | **P0 先答**；按 §3 的 O1 / O2 选退路 |
| 写 `Output` 与引擎后续 pass 打架 | 花屏 / 冻结 | RCAS 已证明可行；失败自动关 After 并回落 Before，不留死状态 |
| 切位置时历史错位 | 拖影 / 重影 | 切位置强制双侧 `Reset=1`，切换后首帧不合成 |
| NR 历史建立在「已放大」的图像上 | 快速转视角出现鬼影 | 验收单列一条；严重则加「移动时降低合成强度」作为收尾项 |
| 100% 成本 ≈9.6 ms（≈78% 帧预算） | 帧率明显下降 | After 档的固有代价；P3 给 66/50 降本档，面板直接显示成本 |

## 6. 验收（一次进游戏，5 条）

1. 面板切「放大后」→ TEST 日志出现
   `[nr] create：placement=after 域 2560x1440（guide 1485x835）→ 特征 2560x1440（100%）`，
   诊断页「位置」= 放大后、「NR 处理尺寸」= 2560 x 1440、「指南尺寸」= 1485 x 835。
2. 开「分屏对比」→ 左半是放大后的原图、右半是 NR 合成，分界线可拖。
3. GPU 读数：After 档 NR 总量 ≈9–10 ms（100%），明显高于 Before 的 3.5 ms——**这是预期，不是 bug**。
4. 快速转视角 / 移动：无拖影、无重影、无残影。
5. 面板切回「放大前」→ 画面与开关前一致；**先在 after 档把窗口缩小再切换/继续运行**，
   验证写回框不越界（不花屏、不冻结、不崩）；期间改内部分辨率同样不崩。

### 6.1 实机验收记录（2026-10-24 21:45–21:47，主目录 TEST 构建）

- 21:46:40 从 before 切到 after；重建日志
  `特征 1485x835 → 2560x1440（placement after，域 2560x1440，guide 1485x835，100%，第 1 次重建）`，
  窗口 #1 首帧 `placement=after ... recorded=true`。
- 窗口 #1（240 帧）：NR median **9.782 ms**（evaluate 9.229 / encode 0.115 / composite 0.427），
  引擎 DLSS 1.841 ms；`evaluate` 无一失败，无 create/rebuild 失败、无 `afterFallback`、无崩溃。
- dump：`color_ref_w1` vs `nr2_w1` mean 6.98/255（NR 确实改变了画面）；`fed_w1`（合成结果）
  与 `base_w1` 逐字节相同——该版 dump 在写回**之后**抓 base，所以这是「写回成功」的证据，不是 no-op。
  已把 TEST dump 改为写回前抓 base、把 `域/特征` 标签改成 `引擎渲染`，供后续实机判断。
- 用户观察：后档有可感卡顿，与 9.78 ms 相符。
- **未显式跑**：分屏对比、快速转视角鬼影、窗口缩小/内部分辨率切换（§6 第 2/4/5 条）。
  这三项在 release 构建里继续观察；出现鬼影/花屏按 §5 处置。

## 7. 拍板状态

| 事项 | 状态 |
|---|---|
| 只做位置轴，其它项不排 | ✅ 已拍板（2026-10-24） |
| P0 先做 | ✅ 已拍板（2026-10-24）；2026-10-24 晚开工 |
| After 档默认工作尺寸 = 100%（严格按用户定义） | ✅ 已拍板（选项 C）：100% 先上，66% 留作 P3，验收后再定 |
| guide 被拒时走 O1（自升采样 guide） | ✅ 已拍板（选项 A）：仅在 P0 判否时付这 1–2 天 |
| 切档用重建 feature（一次 0.3–0.5 s） | ✅ 已拍板（选项 A）；不做双 feature 常驻 |
| After 档下 RCAS 默认关 | 拟定（RCAS 本身默认关，顺序固定 NR → RCAS） |
| 位置切换面板 Apply 即时生效（走现成先建后拆重建） | 拟定 |
| 部署目录 | ✅ 已拍板（2026-10-24）：主目录 `E:\LeviLauncher\versions\1.26.51.01`；**agent2 是另一个 agent 的并发工作区，不碰** |
| P2 实机验收 | ✅ 主路径通过（2026-10-24 21:46，§6.1：后档 9.78 ms、无错误/回落、写回成功）；分屏/鬼影/窗口缩放留 release 观察 |
| release 部署 | ✅ 2026-10-24，主目录 DLL = `fd90f578…`；TEST 备份 `.bak-test-placement`，改动前备份 `.bak-before-placement` |

## 8. 不做（沿用现有文档，不因本文改变）

多 pass / 对象遮罩 / peripheral / fused（[K §3.5](K-trp-feature-gap.md#L162)）、Style 3–7、天气时段、
UI mask 与 `ControlMask`（[G §4](G-nr-customization-plan.md#L110)）、HDR 输出、`.trpk`/INT8/MFG 解锁
（[M §4.4](M-roadmap.md#L106)）。

## 9. 变更记录

- 2026-10-24：首版。路线据用户口述的位置轴两档；旧结论（G §4 / K §3.5「不做」）的判据变化登记在 §2；
  P0 的探针设计与判据先写在这里再开工；按用户裁定，其余 NR/SR 待办全部不排。
- 2026-10-24（晚）：用户拍板：「按推荐来」——后档先 100%（66% 留 P3）、P0 判否才走 O1、切档用重建、
  部署主目录 `1.26.51.01`（agent2 是并发工作区，不动）。P0 开工。
- 2026-10-24（晚，收工点）：P0 完成（§3.1）；P1 代码完成并两轮审查（§3.2），TEST 已部署主目录，
  待 §6 的五条实机验收；发布构建已编译验证、未部署；P2 顺序/回读已并入 P1，P3 等验收后拍板。
- 2026-10-24（晚，验收）：实机跑通 after（§6.1）：重建成功、NR 9.78 ms、无错误/回落、写回成功；
  release 已部署主目录（`fd90f578…`）。分屏/鬼影/窗口缩放三项留 release 观察；P3 仍等拍板。
