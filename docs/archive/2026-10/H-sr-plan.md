# H. SR（超分）可行性 + 自定义预设规划

> **2026-10-24 状态：历史文档。** 机制与最终结论见 [`docs/J`](J-sr-implementation.md)，
> 控制层与 VV 迁移规划见 [`docs/I`](I-sr-final-plan.md)，优先级见 [`docs/K`](K-trp-feature-gap.md)。
> 本文保留的价值是「三种能」的判断框架与预设四层的定义；§0 的「任意百分比做不了」已被推翻。

> **仅规划，未改任何代码。**
> 判断依据：`docs/D` §6.16（引擎 SR 参数契约，实测）、`docs/G` §4（现有"做不到"清单）、
> TRP 源码与 NGX SDK 头（`D:\Documents\theosrp`）、本机 `options.txt` 与客户端 SDK 头。
> 结论区分三种"能"：**现在就能（参数写值）** / **要一次重建才生效** / **结构性做不到**。
>
> **后续（2026-10-23）**：用户把范围扩到"更全的控制 + 将来 VV 用 DLSS 替换 TAAU"，
> 完整路线、分辨率三层模型（R_in / R_sr / R_out）与里程碑见 **`docs/I-sr-final-plan.md`**；
> 本文保留为可行性判断与"预设四层"的定义。
>
> **落地（2026-10-24）**：任意比例（33/50/58/66/100）与 DLSS 模型预设已实现，最终机制、证据与坑见
> **`docs/J-sr-implementation.md`**。本文 §0 的"任意百分比做不了"已被 FrameBuilder 因子表推翻；
> 75% 因槽位动态范围夹取不提供，锐化因运行时废弃 `Sharpness` 不提供。

## 0. 一句话结论

1. **"自建 SR"是死路，但"SR 可调"完全能做，而且真的不用接管 RT 线。**
   我们已经有引擎的 `EvaluateFeature` 钩子和它的参数对象（vtable 同源 `same=1`），
   改预设/锐化/重置就是在**引擎自己的调用序列里改值**——这和 docs/D §6.16 里
   "接管 `CreateFeature(1)`、返回我们的 handle"是两件事，后者不必要。
2. **自定义 SR 预设 = 四层参数的组合**：比例档（游戏设置 `upscaling_percentage`，6 档）＋
   网络预设字母（J/K/L/M，创建期）＋ 锐化（待 A/B，可能得自家做）＋ 重置策略（一次性 `Reset=1`）。
   没有一层需要"重写超分"。
3. **真正做不到的只有两种**：① 进程内第二个 DLSS SR 实例（实测会把引擎的顶掉）；
   ② 让我们自己的 SR 成为最终画面（UI 在 SR **之后**由引擎合成，我们在 Present 只看到已放大 + 已合成 UI 的画面）。
4. **任意百分比（比如 70%）做不了也不值得**：比例是游戏自己的设置，只有 0/33/50/66/75/100；
   本机 `upscaling_percentage:0`（Automatic）实测落在 1485×835 = 58% = DLSS Balanced。改这个之外的数字
   要动引擎的 render-size 计算 = 接管级别。
5. **窗口化完全兼容，不需要强制全屏**：SR 是相对比例——输出 = 窗口客户区（引擎给），渲染 = 输出 × 比例，
   我们只改比例。代价是"预设只能存比例、不能存 1440p 这种绝对分辨率"，所以要按窗口面积自动分档（§5）。

## 1. 先把"SR 不能做"拆成四个不同的问题

| # | 说法 | 实际是什么 | 结论 | 依据 |
|---|---|---|---|---|
| 1 | 自建 DLSS SR | 我们自己 `CreateFeature(1)` 再 evaluate | **不行**：一个进程只能有一个 DLSS SR，第二个会把引擎的顶掉（引擎 evaluate 仍返回 1 但输出不再更新） | docs/D §6.16 规则 3；模式 7–9 实测冻结/加载画面损坏 |
| 2 | 我们自己的 SR 当最终图像 | 把自家放大结果送屏 | **不行**：UI 在 SR 之后由引擎合成，Present 拿到的是"已放大 + 已合成"的画面；UI 纹理我们没有 | docs/F §1；docs/G §4 |
| 3 | 我们自己的整屏后处理 | 在最终画面上做锐化/增强 | **能**：Present 钩子是我们自己的（覆盖层就在上面画） | docs/F §1 的 11on12 管线 |
| 4 | 改引擎 SR 的行为 | 预设字母 / 比例 / 锐化 / Reset | **能**（比例受 6 档限制） | docs/D §6.16 参数表；本机 `options.txt` |
| 5 | 用 NR 当 SR | NR 承担放大 | **不做**：NR 契约是 1:1（`DLSSNR.ScalingRatio=1.0`），且即使能放大，仍卡在第 2 条的所有权问题 | docs/D §6.9 创建参数表 |

> 所以 "SR 完全不能做吗" 的正确答案是：**"自建 SR" 不能，"自定义 SR 预设" 能**。
> docs/G §4 里 "MC 的 SR 也被引擎独占，塞不进去" 这句只对"自建"成立，容易被读成"SR 一律不能碰"。

## 2. 我们手里现成的东西（不需要新钩子）

| 资产 | 位置 | 对 SR 的用处 |
|---|---|---|
| `EvaluateFeature` 钩子，拿到引擎 `params`/`Color`/`Output`/`Width/Height` | `probe/RenderProbe.cpp:581`、`:596-606` | 每帧读回 SR 状态；每帧写引擎参数 |
| 在**引擎自己的参数对象**上写值（不是新建对象） | `nr/D3D12Bridge.cpp:2194-2260`（NR 已经在这么干） | 写 `Sharpness`/`Reset`/预设提示 |
| 引擎 SR 参数契约（实测） | docs/D §6.16 表 | 知道有哪些键、何时写 |
| 游戏侧 `CreateFeature` 包装的签名（`kSigCreateWrapper`） | `probe/RenderProbe.cpp:408`，目前只在 `#ifdef TEST`（`:548`） | 创建期覆盖预设字母（提发布版即可） |
| 引擎 DLSS 的 GPU 计时（已在面板） | `nr/nrStamp*` + docs/F | "改预设到底有没有用"的性能判据 |
| 面板 / `ll::config` / i18n | docs/F、`ui/Menu.cpp`、`src/lang/*.json` | 预设 UI 全部现成 |
| 游戏自己的比例设置 | `options.txt` 里 `upscaling_percentage` / `upscaling_mode` / `gfx_upscaling` / `target_resolution`；枚举见 LL 包 `mc/client/options/UpscalingQuality.h`（0/33/50/66/75/100） | 比例档的唯一来源 |

## 3. 关键事实核对（实测或 SDK 文本，写方案前先钉死）

| 事实 | 内容 | 对方案的影响 |
|---|---|---|
| 引擎预设提示值 | `Quality/DLAA/Balanced/UltraQuality = 0x8000000B`、`UltraPerformance = 0x8000000C`、`Performance = 0x8000000D`；低位 11/12/13 = 预设 K/L/M（DLSS 4.x transformer 预设），高位 `0x80000000` 是引擎加的标记 | 覆盖时**保留高位、只换低位**；离线先验证带不带高位都能创建 |
| 预设是**创建期**参数 | docs/D §6.16：除 `Output`/`Jitter` 外，其余只在创建时写一次 | 改字母要等一次重建；改比例本身会触发重建 ⇒ 两者放进同一个预设包 |
| 引擎 flags = 3 | `IsHDR \| MVLowRes`（`nvsdk_ngx_defs.h:286-287`），**没有** `DoSharpening`(1<<5) | `Sharpness=0.35` 很可能根本不起作用（SDK 把该 flag 标成 deprecated，注释 "Sharpness is not supported"）⇒ 锐化要么靠这个参数被验证为有效，要么只能自家 pass |
| `PerfQualityValue=1` | = Balanced（TRP `DLSSFeatureParameters.h:36` 的映射），只是给 NGX 的档位提示 | 改它**不会**改引擎渲染尺寸；尺寸由游戏设置决定（docs/D §6.19 已见"超分质量档改变渲染分辨率"） |
| 比例目前 = 0.58 | 1485/2560 = 835/1440 = 0.58 = DLSS Balanced；`options.txt` 里 `upscaling_percentage:0` = Automatic | `Automatic` 是"让引擎按档位取最优"，58% 不是常量；改档位 → 引擎重建 SR（docs/D §6.19 已见"超分质量档改变渲染分辨率"） |
| NR 与引擎 SR **共用一个参数对象** | `D3D12Bridge.cpp` 每帧在同一对象上写 `Width/Height`/`MV.Scale`/`Color`；`OutWidth/OutHeight` 故意不碰（写错会让引擎 SR 只放大 1485 的 patch） | SR 侧新增写入必须走同一处，并确认不会覆盖引擎每帧要用的键；`Reset` 我们要写的是**无前缀**的那个（`DLSSNR.Reset` 是 NR 的） |
| 单实例只限制 DLSS | 别家 SR（FSR 等）不占 NGX 实例 | 但所有权问题（第 1 节第 2 条）一样存在 ⇒ 换实现也救不了 |
| `DLSSOptimalSettingsCallback` 是 NGX 参数 | `nvsdk_ngx_defs.h:721`；exe 里有这个字符串 | 引擎可能经这个回调决定"最优渲染尺寸" ⇒ S4 的线索（任意比例的唯一轻量可能） |

## 4. 方案：三层 + 一个只做验证的第四层

### S1 — 读数 + 活的参数覆盖（0.5–1 天，低风险，先做）

| 项 | 做法 | 用户能看到什么 |
|---|---|---|
| **S1.1 SR 状态页** | 面板加"超分"页：渲染尺寸 / 输出尺寸 / 实际比例 / `PerfQualityValue` / 预设字母 / `flags` / 引擎 DLSS GPU ms（已有） | 现在 SR 到底在跑什么档、花了多少 ms，一目了然 |
| **S1.2 Reset 热键** | 在 Evaluate 前写 `Reset=1` **一帧**，下一帧写回 0（不能常置，否则历史永远不累积） | 切视角/传送/长时间停顿后画面"糊住了"，按一下清掉 |
| **S1.3 锐化覆盖** | 每帧写 `Sharpness`（同对象，1 行）；先 A/B 验证在 flags 没有 `DoSharpening` 时是否真起作用 | 若有效：0–1 的锐化滑条；若无效：记下"参数已死"，转 S3 |
| **S1.4 预设字母覆盖（只读回）** | 先只把引擎写入的预设读出来显示，不改 | 先确认"档位 ↔ 字母"的对应关系，再决定要不要能改 |

验收：面板数值与 `[ngxset]` 参数日志一致；Reset 热键肉眼可见；Sharpness A/B 有结论（有效/无效）。

### S2 — 预设包（1–2 天；这才是"自定义 SR 预设"的产品形态）

一个"SR 预设"= 一组值，面板里可存/可切/可设默认：

| 层 | 取值 | 生效时机 | 备注 |
|---|---|---|---|
| 比例档 | `Automatic` / 33% / 50% / 66% / 75% / 100%（游戏 `upscaling_percentage`） | 改了就触发引擎重建 SR（待实测确认） | 只能在游戏给的 6 档里选；写游戏设置是**探路项**（见下） |
| 网络预设字母 | 驱动默认 / J / K / L / M（E、F 是已废弃旧 CNN，可以留但标注） | **创建期**，要等下一次重建 | 覆盖 `DLSS.Hint.Render.Preset.*`，保留 `0x80000000` 位 |
| 锐化 | `Sharpness` 参数（若 S1 证明有效） | 每帧 | 无效就走 S3 |
| 稳定策略 | 进世界 / 传送 / 视角切换后自动 `Reset=1` | 一次性 | 复用 `nrNotifyLevelLoaded` 已有钩子 |
| NR 绑定 | 预设顺带切 NR 的 Style/Intensity/Local* | 每帧 | 让"画质档"是一整套观感，而不是两个独立开关 |

**探路项（半天内能出结论）**：能不能在运行时改游戏的比例设置？
- 最好：客户端 `Options` 对象可写（LL 是否给门面未确认）→ 面板直切，引擎立刻重建；
- 次好：只能写 `options.txt` → 提示"重启后生效"；
- 最差：只读好显示，切换让玩家去原生设置改（那预设包就只剩字母+锐化+NR）。
判据现成：改比例后引擎的 `Width/Height` 会变，而 NR 已经会跟着重建（docs/D §6.19）——看日志就知道引擎重建没重建。

验收：面板切"性能/均衡/画质"→ 渲染尺寸变化 + 预设字母变化 + NR 自动跟随；同一地点 A/B 能看出细节/鬼影差别。

### S3 — 自家锐化（1–2 天，独立于 RT 线）

两条落点，二选一（或都做，用配置切）：

| 落点 | 做法 | 优点 | 代价/风险 |
|---|---|---|---|
| **前置**（推荐先试） | 在现有合成 pass 里加一个 RCAS/CAS-like 分支，锐化后的图仍作为引擎 DLSS 的 `Color` | 复用 Route A 已验证的"换 `Color` 指针"链路；**UI 不会被锐化**；代价 ~0.1 ms @1.24 MP | 锐化在放大前，DLSS 可能放大锐化伪影 |
| **后置** | 在 Present 钩子里对 backbuffer 跑一次全屏锐化（11on12 + PS） | 等价于 TRP 的 RCAS 位置（放大后）；效果最直接 | **UI/文字也会被锐化**（要么接受，要么做 UI mask，后者做不到）；1440p ~0.1–0.3 ms；动 11on12 生命周期要重测 |

> 只有 S1.3 证明 `Sharpness` 参数无效时才值得做 S3；TRP 也是因为 DLSS 自带锐化不可用才自己写 RCAS。

### S4 — 任意比例（只验证 30 分钟，不排进主线）

线索：引擎在 exe 里带 `DLSSOptimalSettingsCallback`（NGX 参数，`nvsdk_ngx_defs.h:721`）。
如果引擎确实用这个回调决定"给定输出尺寸 + 档位 → 渲染尺寸"，钩它就能得到任意比例、零渲染器改动。
验证：钩回调 → 打印调用次数与入参/出参 → 对照 1485×835 是否来自它。
不成立就放弃，**不要**去改引擎的 upscaling 配置或 render target 尺寸（那是接管级别）。

## 5. 窗口化不是障碍：输出 = 窗口客户区，渲染 = 输出 × 比例

**不需要强制全屏。** 先分清三个"分辨率"（完整模型见 `docs/I` §1）：**R_in**（内部渲染，DLSS 输入）、
**R_sr**（放大目标）、**R_out**（呈现/后缓冲）。窗口化下 R_out = 窗口客户区，R_in = R_out × 比例；
**能且值得控制的是 R_in**（docs/I 的 L1–L4），R_out 默认不控（强制它会让 UI/鼠标坐标一起缩放）。

- `OutWidth/OutHeight` = 交换链/窗口客户区像素（全屏时是显示模式尺寸，窗口时就是窗口大小）；
- `Width/Height` = 引擎按比例算出的渲染尺寸（`Automatic` 时按 DLSS 最优，本机 58%）；
- 我们只改**比例**，绝对像素只读（`nrPublishEngineFrame` 已经在发布 `Width/Height/Output`）。

| 概念 | 2077 / 老滚（输出固定） | MC 窗口化 | 我们的做法 |
|---|---|---|---|
| 输出分辨率 | 玩家选的显示器分辨率 | 窗口客户区，随拖动变化 | 默认不控（强制 = UI 一起缩放，见 docs/I §3） |
| 渲染分辨率 | 输出 × DLSS 比例 | 窗口 × `upscaling_percentage` | 先只改比例（6 档）；任意像素见 docs/I 的 L3 |
| 预设语义 | "1440p + 性能档" | **只能存比例**，像素随窗口走 | 面板显示"58% → 928×522 → 1600×900（窗口）" |
| 绝对分辨率预设 | 可以 | **可能**（L3 回调 / VV 的 `target_resolution`） | 先做 docs/I 的 K3 探路，成立就做像素滑条 |

**窗口化特有的三个问题（2077 没有，必须单独设计）**：

1. **小窗口下 SR 不划算**：DLSS 成本随输出像素走、NR 成本随渲染像素走。1280×720 窗口 58% ⇒ 743×418，
   DLSS 约 0.3 ms、NR 约 0.5–0.7 ms —— NR 的固定开销占比反而更高，而 418p 放大的伪影最明显。
   ⇒ 预设要带"按窗口面积自动分档"：<900p 建议 Native/DLAA 或关 NR；1080p 档 66–75%；1440p 58–66%；4K 50%。
   这也是 MC 比 2077 更适合做"自动档"的地方：**窗口像素我们自己知道，代价随时可算**。
2. **拖动窗口会反复触发重建**：resize → 引擎重建 DLSS feature；我们的 NR 目前是"尺寸变了就重建"
   （先建后拆、≥20 次求值间隔、每会话上限 64，docs/D §6.19）。拖动时如果每帧都变，会顶到上限、白付重建成本。
   ⇒ 加"尺寸稳定 N 帧后才重建"的消抖；拖动期间沿用旧 feature 或暂停 NR 上屏；上限改成按"稳定后重建次数"计。
3. **窗口比显示器小时，输出就是窗口本身**：DWM 把整块窗口缩放到屏幕，观感上限 = 窗口像素，不是显示器分辨率。
   面板必须显示"比例 / 渲染像素 / 窗口像素"三个数，否则 4K 屏用户会以为自己输出是 4K。

**其它窗口化边界**（都要显式判）：最小化/被遮挡时 Present 不跑或尺寸为 0；DPI 缩放时客户区物理像素 ≠ 逻辑尺寸
（一律读引擎参数，不要自己算）；跨屏拖动；纹理"只增不减"的内存上界（拖到 4K 再缩小会留着 4K 级纹理）。

**要不要强制全屏？不要。** 强制全屏既不解决"绝对分辨率"（输出仍只是显示器尺寸、比例仍是相对的），
又会丢掉 MC 的常态玩法（窗口化 + 多开）。真想要"窗口 1080p、内部渲染 1440p"这种超采样，看 `docs/I` §2：
① 钩 `DLSSOptimalSettingsCallback` 直接改 R_in（L3，先验证）；② 30 分钟探路 `target_resolution`（K2）；
③ 改引擎 render-size（L4，接管级，最后手段）。

## 6. 做不到 / 不该做（逐条给理由）

| 项 | 为什么不做 |
|---|---|
| 第二个 DLSS SR 实例 | 单实例，实测顶掉引擎；加载画面也会坏 |
| 我们自己的 SR 直接送屏 | UI 在 SR 之后合成，Present 只给已合成画面；UI 纹理我们没有 |
| 跳过引擎 evaluate、把我们的 SR 写进 `Output` | 技术上可行（同一 cmdlist），但等于接管输出，且**收益为负**：我们写不出比 DLSS 更好的 SR。只值得当实验，不值得当功能 |
| 用 NR 承担放大 | 契约是 1:1；即使放大成立，仍受"最终图像所有权"限制 |
| 任意百分比比例 | 直接改要动引擎 render-size；但若 `docs/I` 的 K3（`DLSSOptimalSettingsCallback`）成立，任意 R_in 反而是低成本项——先验证再定 |
| VV 路线上接 DLSS | 已列入 `docs/I` 的 M4–M6（用户定了"VV 用 DLSS 替换 TAAU"）：VV 不占 NGX SR 实例、插入点 = taau，但要做 bgfx→D3D12 桥、另找深度源，先侦察（M4）再承诺 |
| 写引擎 `Color`/`Output` 纹理做 SR | 已两次踩爆（整屏黑、时序链打死），docs/D §6.16 规则 1/2 |

## 7. 建议顺序（每步都能自己看见）

1. **离线 30 分钟**（`tools/ngx_probe.exe`）：预设字母 E/F/J/K/L/M、带/不带 `0x80000000` 位，feature 1 能否创建；
   比例 33/50/66/75/100 各建一次。→ 先把 S1/S2 的参数取值范围钉死，避免在游戏里试错。
2. **S1.1 + S1.2**（半天）：读数 + Reset 热键。纯加法，不改变任何渲染行为。
3. **S1.3 A/B**：`Sharpness` 到底生不生效 → 决定 S3 做不做。
4. **S2 探路**：改比例能不能让引擎重建（先用手改 `options.txt` / 原生设置验证），再决定面板能不能直切。
5. **S2 预设包**：把比例/字母/锐化/NR 绑成一套。
6. **S3**：只在锐化参数无效时做（先前置、后后置）。
7. **S4**：单独 30 分钟，结论不管正负都写进文档，不排主线。

## 8. 需要你拍板的点

1. **"SR 预设"你要的是哪一层？** ① 比例档（性能↔画质）② 网络预设字母（J/K/L/M，细节 vs 鬼影）
   ③ 锐化 ④ 整套预设包（①②③ + NR 绑定）。我建议 ④，但先做 ① 的读数和 ② 的只读。
2. **能接受"字母要等一次重建才生效"吗？** 切比例会顺手触发重建；只改字母不会立刻变。
   不接受的话就得接管引擎的 create（docs/D §6.16 备选路线），那是另一档成本。
3. **比例切换走哪条**：面板直写游戏设置（要先探路，可能踩 Options 门面）／只读显示 + 提示去原生设置改？
4. **S3 自家锐化要不要做？** 它不依赖 DLSS，但后置方案会连 UI 一起锐化；前置方案可能被 DLSS 放大伪影。
5. **窗口化要不要做"按面积自动分档"？** 我建议要（小窗口高比例、大窗口低比例），否则同一个预设在不同窗口
   大小上效果/代价完全不同；实现只要读引擎给的 `OutWidth/OutHeight`，不需要碰渲染器。

## 9. 参考

- 本项目：`docs/D-rt-dlss-nr-plan.md` §6.16（引擎 SR 契约）、§6.19（尺寸变化即重建）；
  `docs/G-nr-customization-plan.md` §4；`src/neurodragon/probe/RenderProbe.cpp:408/548/581`；
  `src/neurodragon/nr/D3D12Bridge.cpp:1657-1664/2194-2260`。
- TRP：`src/DLSSBackend.cpp:285-295`（六个 `Hint.Render.Preset` 一次写全）、`src/DLSSPreset.h`（J/K/L/M 语义）、
  `src/DLSSFeatureParameters.h:30-45`（档位映射）、`src/DRS.cpp`（自己的动态分辨率）。
- NGX SDK：`.dependencies/Streamline/external/ngx-sdk/include/nvsdk_ngx_defs.h`
  （`:70-88` 预设枚举、`:281-294` flags、`:721` OptimalSettingsCallback）。
- 本机设置：`E:\LeviLauncher\versions\1.26.51.01\Minecraft Bedrock\Users\<id>\games\com.mojang\minecraftpe\options.txt`
  （`upscaling_percentage` / `upscaling_mode` / `gfx_upscaling` / `target_resolution` / `graphics_mode`）；
  枚举来自 LL 26.51.7 包内 `mc/client/options/UpscalingQuality.h`。
