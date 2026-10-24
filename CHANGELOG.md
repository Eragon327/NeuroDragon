# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed（2026-10-24：截图反馈进聊天栏、文件落游戏根目录、截图键可改）

- 截图结果的反馈从屏幕底部 2.2 s 的 ImGui 小条改为**游戏原生聊天栏**一行（`ui::notice::chat` →
  `ClientInstance::getGuiData()->displayClientMessage`，纯客户端消息、不发包），并**带绝对路径**：
  原提示太不明显，而且「已保存」不告诉人存到哪。
- PNG 落盘目录从 `mods/NeuroDragon/screenshots/` 改为**游戏根目录** `<version>/screenshots/`
  （与 `mods/`、`logs/` 同级、随 exe）。路径在 `Screenshot.cpp::makePath()` 里就做
  `std::filesystem::absolute`——`getModDir()` 返回的是相对路径（旧日志是 `./mods/…`）。
- 「高级」页的**截图键改为可改**（与面板键 / NR 键共用同一个一次性捕获机制，改完点「应用」）。

### Changed（2026-10-24：分屏对比只留一个，且不再跨会话保留）

- 删掉「超分 → 锐化」里的**分屏对比**（`sr.compare` / `sr.compareSplit`、RCAS shader 的 `Split` 分支、
  面板开关与「分割位置」滑杆、3 个 i18n 键）：分屏只保留「神经渲染」页判断 NR 的那一个。理由：TRP
  也只有一个 Compare（放在 NR resolve、会话内不保存）；两个都开会出现两条缝、四段画面，而且两个页面
  上「左半 = 原图」的含义并不相同。锐化改为**整帧开关**判断效果（实测 0.672 时 30% 像素变化 >2/255，
  整帧开关即可辨认）。
- `nr.compare` 改为**会话内**：`loadOrCreate()` 加载后清零（分割位置照旧保存），与 TRP 的 Compare 一致
  ——否则忘关分屏会把一条暗缝带到下一次启动，而屏幕上没有任何东西解释它。
- 旧 `config.json` 里残留的 `sr.compare*` 键在加载时被忽略（字段已删，没有迁移）。

### Changed（2026-10-24：默认替换模式改为 6（TRP Ratio），移除「只求值」入口）

- `nr.replace` 默认值改为 **6（TRP Ratio）**：高光取自原图、按原图亮度重基准；面板下拉第一项即它。
- 「只求值（0）」这一模式**整个移除**：面板没有入口，运行时代码也不再保留它——**任何不是 5 的值都按 6 处理**，
  配置不做迁移（用户明确「不需要向后兼容」）。增益 Boost 滑条只在 `Replace=5` 时出现。

### Measured（2026-10-24：网络预设 0/1 真的换了网络，上一版慢 80%）

- 同一场景 1485×835、输入 100%：`evaluate` 中位数 **3.18 → 5.85 ms**（+2.6 ms，约 +80%），
  NR 总时长 3.3 → 5.9 ms；encode / composite / 引擎 DLSS 两侧不变 ⇒ 换的是网络本身，不是别的批次。
- 结论：开关保留、**默认仍是 0（默认网络）**；面板提示已写明实测代价，避免误选。

### Fixed（2026-10-24：TEST 构建的探针日志根本没写进 latest.log）

- 诊断构建里所有 `NR_DEBUG` / `ND_DEBUG` 行都是 debug 级，而 LeviLamina 的 logger 默认级别会丢弃它们：
  `latest.log` 里一条 `[nr]` 调试行都没有（只有一条被误报成 “encode shader compile failed” 的警告）。
  ⇒ 之前靠日志判定的探针（P2 网络预设、P4 引擎是否每帧重写 Color）实际上一直是盲的。
  现在 **TEST 构建**把 logger 级别与 flush 都设为 Debug；发布版保持默认级别与 flush 策略。
- `D3DCompile` 在成功时也会返回带 warning 的 diagnostics blob，旧代码把这种情况打成
  “compile failed”；现在只有 HRESULT 失败才报错，成功时写 “compile diagnostics”。
- 诊断页「本帧实际写入」新增「网络预设」回读，P2 需要它确认到底加载了哪个网络。

### Added（2026-10-24：自适应曝光、求值失败计数）

- 「TRP Ratio 参数」区新增「自适应曝光」（默认关）+ ±2 EV 偏置：用画面平均亮度自动定白点，
  室内外 NR 看到的亮度更一致。实现是一个 1×1 `R32_FLOAT` white point 纹理 + 一个 8×8 组 /
  64×64 网格的亮度归约 pass（log 空间平滑，约 1 秒稳定，白点夹在 `[0.031, 4]`），全程 GPU、不回读。
  描述符每 pass 从 4 个扩到 6 个（4 SRV + 2 UAV），**开关关闭时仍走原来的常量白点路径**。
- 诊断页新增「求值失败次数」：失败的那一帧不参与合成，计数能解释「某些帧没有 NR」而不用翻日志。
- 诊断页「本帧实际写入」新增「自适应曝光」回读。
- 说明（2026-10-24 实测）：自适应曝光**肉眼难以分辨**——Ratio 合成里 `base / white` 与 `result * white`
  基本抵消，真正被改变的是 NR 看到的归一化亮度，而 NR 在当前内容上的增量只有 1.5–2.5%。
  开关默认关、文案已写明；后续要判定需要把 1×1 白点纹理回读出来做可测量对比（docs/L P7）。

### Added（2026-10-24：宽域 / 细节拆分、失败求值守卫、模型文件选择）

- 「TRP Ratio 参数」区新增「宽域强度 / 细节强度」：把 NR 变化（以档位计）拆成宽域分量与局部对比
  分别缩放，`1.00 / 1.00` = 不改；宽域分量用结果网格上 4 个宽跨度采样估计，不需要额外 pass 或纹理。
  只作用于 `Replace=6`。诊断页「本帧实际写入」新增「宽域 / 细节」回读。
- 求值失败的那一帧不再参与合成：输出纹理里留着上一帧结果，合成它等于把旧帧注入当前帧；
  跳过一帧合成看不出来，比注入旧内容安全。
- 「神经渲染」页新增「模型文件」：列出模组目录里的 `nvngx_dlssnr*.dll`，选中的文件在下次启动时
  替换游戏目录里的模型（core 只认 `nvngx_dlssnr.dll` 这个名字）。选中文件不存在时回落默认、
  替换失败时保留原文件，两种情况都只写 warn，不会让模组加载失败。

### Fixed（2026-10-24：面板把 Replace=6 当成「未上屏」）

- 顶部状态文字与管道条的 NR 节点只认 `replace == 5`，所以选 TRP Ratio（6）时明明已经合成上屏，
  面板却显示「NR 未上屏」、NR 节点也不点亮。现在 5 与 6 都算上屏，只有「只求值」（0）显示未上屏。

### Added（2026-10-24：Ratio 合成加入 HueGuard，批 2.2 第一项）

- Replace=6（TRP Ratio）的合成里加入 TRP 的灰区保护：原图接近中性色时，结果的色度被约束在
  原图自己的色相轴上（不旋转、不过饱和），原图本身有色度时保护按 OkLab 0.02–0.08 释放。
  这是质量保护而非旋钮，无开关；`replace=5`（增益合成）不受影响。

### Added（2026-10-24：NR 输入缩放 50/66/100，批 2.1）

- 「神经渲染」页新增「输入分辨率」：NR 在渲染尺寸 × 50/66/100% 上推理，再合成回全分辨率。
  低分辨率图像写在原尺寸分配的左上角；深度/运动矢量仍用全分辨率（310.x 运行时的重建契约，
  TRP 的 `FeatureSession` 就是这么设 subrect 的），因此不需要额外的降采样 pass。
  编码与合成各加两个 shader 模式（缩放时 10/13、5/6），**100% 仍走原模式、画面数值不变**。
- 诊断页「本帧实际写入」新增「输入缩放」：显示实际生效的百分比与特征尺寸（回落时也看得到）。
- 缩放创建失败不会让 NR 整局失效：写一行 warn 后自动回落到 100% 重建，NR 继续工作。
- 皮肤结构滑条从正式版面板移除：实测在 MC 画面上 `-1/0/1` 无差异，TRP 把它和自动皮肤遮罩配对，
  而我们的输入没有皮肤分类；参数默认值仍是 1.0，TEST 构建保留滑条用于对照（docs/L P1）。

### Added（2026-10-24：NR 剩余功能批 1——死旋钮接活 + 可验证性）

- **「替换模式」选 6（TRP Ratio）现在真的上屏**：合成 pass 过去只在 `replace == 5` 时调用，
  选 6 只求值不上屏，白点 / 迁移 / 色彩 / 最大比值四个旋钮等于假的。现在 5 与 6 共用同一段
  合成，只是 shader 模式不同（2 = 增益，4 = Ratio）。
- 诊断页新增「本帧实际写入」：替换模式、合成模式、输入编码、Style、强度、局部色调 / 结构、
  皮肤结构、Reset、MV 缩放、分屏位置。数值来自真正写进 NGX 参数对象的变量，不是面板草稿。
- 诊断页新增分阶段 GPU 计时：编码 pass / 网络求值 / 合成 pass，与「神经渲染」总时长同一帧，
  三者之和应接近总时长。
- 「风格」区新增「皮肤结构」（`-1` = 跟随局部结构，`0–2` = 独立强度）。运行时是否接受 `-1`
  由 `sweep=3`（每个测量窗轮流写 `-1/0/1`）判定，不靠猜。
- 「输入」区新增「网络预设」（默认 / 上一版）：只在创建 NR 时读取，改完需要重启游戏。
- 「神经渲染」页新增「分屏对比」：左半保持引擎原图（无 NR）、右半是合成结果，交界一条暗线。
  这是判断「NR 到底改了什么」的唯一直接手段。
- 高级页热键改为三行：面板开 / 关、NR 开 / 关（默认 `[`）、重置 NR 历史（默认 `]`）。
  后两个在面板关闭时生效，同时擦掉 NR 与引擎 DLSS 的时序历史。
- NR 关闭（面板「应用」或热键）与卸载时，把引擎自己的 `Color` 交还它的参数对象，避免画面停在
  最后一帧合成结果、卸载后参数对象指向已释放纹理；若引擎把我们的纹理原样交回（说明它没有每帧
  重写 `Color`），会写一行 warn 便于定位。

### Added（2026-10-24：成品帧截图，F2）

- 按 `F2` 把当前成品帧存成 PNG 到 `mods/NeuroDragon/screenshots/`：内容 = 屏幕所见
  （DLSS/NR 之后、含 HUD、**不含面板**、输出分辨率）。日志一行 `[shot] 已保存 …`。
- 时机在 Present 钩子里、画面板之前；面板开着按也不会拍到面板。PNG 编码在后台线程，
  Present 线程只做读回与格式转换（第一次实机同步版本是 395 ms/张，可见卡顿）。
- 按键后底部弹出 toast「截图已保存 / 截图失败（见日志）」——第一次实机时按键没有任何反馈。
- 键位在 `config.json` 的 `ui.screenshotKey`（默认 `VK_F2`=0x71），「高级」页只读显示。
- 这是 SR 的 A/B 取证工具：配合「分屏对比」，一张图直接能看到左原图/右锐化。

### Added（2026-10-24：RCAS 锐化，超分之后、UI 之前）

- 「超分」页新增「锐化」：RCAS 开关（默认关，会改画面属 opt-in）+ 0–1 强度（默认 0.672），
  应用即生效，不需要重建 DLSS feature。
- **强度放大的是锐化增量（0–4 倍，默认 0.672 ≈ 2.7 倍）**：第一次实机测得同帧差分只有
  mean 0.38/255——RCAS 把 lobe 上限压在 −0.1875、再乘 0.672，在已经很锐的 DLSS 输出上看不出来。
  现在 lobe 用满量程，强度直接放大增量；拉满 1.0 是 4 倍，可能出现白边，按需调低。
- 新增**分屏对比**：左半原图、右半锐化，中间 2 像素黑线；一帧内左右相邻，小屏或远程串流上也能确认效果。
- 位置在引擎 DLSS 之后、引擎调色与 UI 合成之前，所以 HUD 与文字不会被锐化。DLSS 自带的 `Sharpness`
  在 3xx 运行时已废弃（实测无效），这是独立的一遍后处理。
- 引擎 DLSS 的输出是**线性 HDR**，而 RCAS 只对 [0,1] 有解：`hitMax=(1−maxRGB)/(4·minRGB−4)`
  在「有通道 >1、有通道 <1」时为正，`lobe` 被夹到 0——直接移植会让亮部不锐化。所以这一遍先按
  Reinhard（`x/(1+x)`，与 NR 编码同一条曲线）映射到 0–1、锐化、再解回线性。
- 引擎的 `Output` 只做 `ALL → COPY_SOURCE → COPY_DEST → ALL` 的往返，不进入 UAV 状态：
  先拷到私有纹理，RCAS 写私有目标，再拷回。窗口尺寸连续 12 帧稳定才重建私有纹理对。
- 面板回读：已写入强度 / 锐化帧数 / RCAS GPU 中位数（GPU 计时新增第 3 段）。

### Changed（2026-10-24：应用即重建 feature；config 运行时只写不读）

- 不加「重载」按钮：底部仍是 保存为默认 / 应用 / 放弃。`config.json` 只在启动时导入一次，
  运行期间**只写不读**——`config::reload()` 已删除，install 里的那次重读也删掉。
- 「应用」在检测到模型预设变化时会强制引擎重建 DLSS feature：先请求另一个质量槽、再切回映射槽；
  重建期间 sizing 回调把渲染尺寸和动态范围钉在当前比例，所以分辨率不动，只有模型被重载。
- 其他参数仍是每帧读取，应用后立即生效。

### Added（2026-10-23：DLSS 模型预设，还原 TRP 的超分选项）

- 「超分」页新增「DLSS 模型（预设）」：驱动默认 / E / F / J / K / L / M，写入
  `DLSS.Hint.Render.Preset.{DLAA,Quality,Balanced,Performance,UltraPerformance,UltraQuality}`
  六项（保留引擎设置的 `0x80000000` 位），与 TRP 的 "DLSS model preset" 等价。
  只在 CreateFeature 时生效，改完需要一次重建（换一档内部分辨率或重进世界）。
- **驱动默认现在会真正生效**：参数对象跨 feature 复用，旧实现只在 `preset > 0` 时写，
  选驱动默认会保留上一次的 K/L/M；现在 0 也写，等于把六个 hint 清回驱动默认。
- **没有做锐化覆盖**：当前 NGX SDK 里 `NVSDK_NGX_DLSS_Feature_Flags_DoSharpening` 已标为
  `SR_DEPRECATED_SHARPENING` 并注明 "Sharpness is not supported"，运行时忽略 `Sharpness` 参数；
  实测写入 0/0.5/1.0 画面无变化，因此不提供这个旋钮（锐化由模型预设本身决定）。
- 说明：66% 与 100% 默认使用同一个模型（K），区别只是渲染比例：66% 先渲染 1707×960 再放大，
  100% 原生 2560×1440 走 DLAA；模型选择与比例选择相互独立。

### Added（2026-10-23：内部分辨率 33–100% 真正生效，不需要 F11）

- 「超分」页 →「内部分辨率」下拉（跟随引擎 / 33 / 50 / 58 / 66 / 100）同时驱动两处：
  引擎的 DLSS 质量槽（决定 DLSS 输入尺寸）和 FrameBuilder 的**场景因子表**（决定世界渲染矩形）。
  两边始终一致，所以 66/100 不再出现下半黑/截断。
- 场景因子表：`FrameBuilder::getUpscalingFactor()`（虚表槽 83）按 `index` 查表
  `[0.5, 0.58, 0.667, 0.3334, 0.75]`，RT 下返回该值，否则 1.0；世界渲染矩形按它算。
  面板选值后每帧把 `table[index]` patch 成目标值（`.rdata` 用 `VirtualProtect` 包一次），
  「跟随引擎」时恢复原值。
- 质量槽写入有门控：引擎创建 DLSS 之前不写（否则引擎会跳过 DLSS，NR 也不跑），创建后再写，
  引擎自己检测到变化并重建——实测不需要改窗口/F11。
- 场景因子表**等 DLSS 尺寸追上目标后再改**，切换时不再出现中间几帧的表/feature 错位。
- 实测（2026-10-23 23:07，TEST）：100% 时 `[desc] Depth` 从 1485×836 变为 2560×1440，画面正常。
- **75% 档位已移除**：DLSS 没有 75% 原生槽，只能借槽 + 覆盖 Out 尺寸，但引擎会按槽位的动态范围
  夹住 Out（q=5 夹到 2534/2560 → 变 100%，q=3 夹到 853），行为不稳定，用户决定不提供。
  现在 33/50/58/66/100 与引擎原生槽一一对应（q=3/0/1/2/5），槽位唯一、无需强制重建，
  回调也不再覆盖 Out 尺寸。旧配置里的 75 会被当作「跟随引擎」。
- 同时删除旧的两组实验旋钮（`qualityOverride` / `factorOverride`），配置只剩 `sr.scalePercent`。

### Added（2026-10-23：内部分辨率覆盖（实验））

- 新配置 `sr.scalePercent`（面板「超分」页 →「内部分辨率（实验）」下拉）：0 = 跟随引擎，其余
  33/50/58/66/75/100 = 在引擎的尺寸回调里返回"输出尺寸 × 比例"，由引擎按这个尺寸重建渲染目标。
  它只在该回调被调用时生效，而引擎只在**重建上采样配置**时调用它（改窗口尺寸/分辨率、进入世界），
  所以改档后需要一次重建——面板提示里写明了。
- 实测（2026-10-23 19:33）：引擎在 2560×1369 输出下调用回调 2 次、返回 1485×794（58%）、预设 K、
  flags 0x3、动态范围 50%..100%；改窗口尺寸会让回调 +1。游戏内没有可改的超分档位，所以这个下拉是目前
  唯一能改渲染比例的地方。

### Fixed（2026-10-23：内部分辨率上限收到 58%，>58% 会读到未绘制区域）

- 实测：比例 > 58%（引擎自己的因子）时画面下半变黑——引擎仍按 58% 渲染，我们只是把更大的尺寸告诉
  DLSS，它去读了没画的区域。下拉已移除 66/75/100，只保留"跟随引擎 / 33 / 50 / 58"，并在提示里写明
  向上调需要改引擎自己的渲染矩形（docs/I §11）。
- 另：「应用并重建」改为保持临时尺寸 6 帧再恢复（同一消息泵里立刻恢复会让引擎看不到尺寸变化，
  且一帧内两次 resize 会留下半张黑画面）。

### Added（2026-10-23：回调调用栈；「应用即重建」试过后移除）

- TEST 构建的 `[sr] cb` 日志加了调用栈（`[sr] cb stack: exe+…`），用来确认是谁调用引擎的尺寸函数，
  为"更干净的触发方式"找依据。
- 「应用并重建」按钮曾用来强制一次重建（窗口 +1 px 保持 6 帧再恢复）。实机是一次明显卡顿（两次
  swapchain 重建 + NR 跟随），按用户要求**整段移除**；改档后请按 F11 切一次全屏或拖一下窗口边缘。

### Added（2026-10-23：超分页 + 引擎 SR 尺寸回调的只读观测）

- 面板新增「超分」页：只读显示引擎 SR 的渲染尺寸 / 输出尺寸 / 比例 / 质量档（`PerfQualityValue`）/
  网络预设字母 / `Sharpness` / 创建标志，以及引擎尺寸回调被调用的次数、入参（输出尺寸）、
  它返回的渲染尺寸与锐化值、动态分辨率范围（docs/I §10）。
- 引擎的 `DLSSOptimalSettingsCallback` 被一个**只读**包装器替换：原函数照常调用、返回值一个字节都不改，
  只做记录；`--test=y` 构建会打出 `[sr] cb #N display=… q=… -> render=… sharp=…`。
  它是引擎计算渲染尺寸的地方（`base+0xdfc6c00`，docs/I §10.3），这个包装器也是将来「任意 R_in」的落点。
- 「超分」页新增「立即清除 SR 历史」按钮：只写一帧 `Reset=1`（引擎自己的键），切视角/传送后画面发糊时用。
- 顺手修掉一个测量错误：参数对象的浮点读回是槽 14，而 `paramGetResource` 以前把 14 当资源兜底——
  真命中时会把 `Sharpness`（0.35）当指针返回。现在资源只走 9 / 8。

### Changed（2026-10-23：卸载 NR 会写日志；面板在 NR 关闭/卸载后仍有基础监控）

- 「保存为默认」且 NR 关闭时，卸载会写一行 info：`NR 已卸载：feature、纹理与模型都已释放（重新启用会重建）`。
- 面板监控不再依赖 NR feature：引擎的渲染/输出尺寸由 probe 每个求值帧发布（`nrPublishEngineFrame`，
  `Output` 只在资源指针变化时 `describe`）；GPU 时间戳对象在第一次求值时创建（`nrEnsureStamps`，
  不随 NR 创建/卸载）；引擎 DLSS 的计时在「面板可见 / 测量窗口 / Replace」时记录。所以 NR 关闭或卸载后，
  渲染尺寸、输出尺寸、DLSS GPU、光栅帧率/帧时间都还在，只有 NR 专属的两项显示「已关闭」。
- 状态行新增「已关闭」（`ui.stats.disabled`），不再把「关闭」显示成红色「未就绪」；管线图里
  「引擎 DLSS」节点在计时可用时点亮（它属于引擎，NR 关着也在跑）。

### Added（2026-10-23：关掉「启用神经渲染」= 卸载 NR）

- `nr.enabled = false` 以前只跳过求值，feature 与 264 MB 模型继续常驻；现在 probe 每帧检查配置，
  关闭即调 `nrUnload()`：feature + output + 编码/解码纹理进退役队列，等游戏直接队列的 fence 之后
  真正释放（队列未知则保留，不冒险），`created/attempted` 复位。重新启用自动走正常创建路径重建
  （模型重新载入 ~0.3 s）。
- 编码 PSO / 根签名 / 描述符堆不随卸载销毁：`ensureEncoder` 拆成「管线」与「两张纹理」两段
  （新增 `ensureEncodeTextures`），重建时只补纹理。
- 触发点**只有「保存为默认」**：`applyDraft(true)` 在配置为关闭时调 `nrRequestUnload()`（一次性原子
  标志，渲染线程下次求值消费）。**「应用」不卸载**——它只改内存配置，NR 停止求值但 feature 与模型
  继续常驻，方便调参时来回勾选；保存为默认才真正释放。发布版不写日志。

### Changed（2026-10-23：NR 创建的启动日志压成一首一尾；面板在小分辨率下不再缩小）

- 「创建 NR 功能」一次操作原来写四行（父阶段首/尾 + 内层「加载 NR 模型」首/尾 +「NR 已就绪」）。
  现在只有父阶段的一首一尾：`创建 NR 功能（纹理、着色器、模型）：开始…` →
  `…：完成，用时 386 ms`（不带尺寸）。`log.stage.nrModel`、`nr.info.ready` 两个键退役。
- 分辨率变化不再写任何日志（`nr.info.resized` 退役，只在 TEST 构建留 debug 行），面板也不显示
  重建次数（`PanelStats.rebuilds`、`ui.stats.rebuilds` 删除）；重建失败与达上限的两条 warn 保留。
- 面板缩放下限从 0.7 改成 1.0：视口高度不足 1080p 时保持 1080p 的物理尺寸（更大的视口照旧按比例
  放大，`[-] 100% [+]` 仍是用户自己的覆盖）。原因：720p 下 0.7 倍文字看不清，且 ImGui 软件光标
  按字号绘制，只剩 ~10 px 几乎看不见；软件光标另加 `style.MouseCursorScale = 1.3`。
- `src/neurodragon/ui/Glyphs.h` 重新生成（300 个非 ASCII 字形），docs/F 的缩放与坑同步。

### Added（2026-10-23：NR 跟随任意分辨率——尺寸变化即重建 feature）

- 之前只在 1485×835 上验证过（= 2560×1440 输出 + Balanced 档），检测到尺寸变化会跳过并告警；
  现在窗口尺寸、全屏、游戏内超分质量档改变引起的渲染分辨率变化，NR 会**当场重建**并继续上屏。
- 离线先确认了前提（`tools/ngx_probe.cpp` 新增 `--sizes=` / `--keep-alive`）：core 的
  `CreateFeature(0x12)` 在 640×360 / 1280×720 / 1115×627 / 1485×835 / 1920×1080 / 2560×1440 /
  3840×2160 **全部 0x1**，三个不同尺寸的 feature 可**同时存活**。
- 实现要点：先建新 feature 再退役旧的（反过来 `ReleaseFeature` 会卸载 264 MB 模型，每次改分辨率都要
  重载一次）；output/encoded/decoded 纹理只增不减（变小只改子矩形，省下每次 ~90 MB 分配）；退役对象在
  游戏直接队列上 Signal 一个 fence、等 GPU 空闲后释放（队列未知时留着，下次重建再试）；重建后合成增益
  从 0 渐入；每会话上限 64 次、两次重建至少隔 20 次求值，同一尺寸失败只记一次。
- 日志：只有重建失败 warn（`nr.warn.rebuildFailed`）与达上限 warn（`nr.warn.rebuildLimit`），
  成功不写行；`nr.warn.sizeChanged`、`nr.info.resized` 退役。重建次数既不进日志也不上面板
  （按作者要求）。文档 docs/D §6.19。

### Fixed（2026-10-23：启动时那 3.5–4.9 秒不是建设备，是销毁临时 D3D11 设备）

- **安装游戏内面板 3774 → 523 ms**，其中「创建临时设备与交换链」那一步
  3519 / 4869 / 3861 ms → **375 ms**。Present 槽位不再从「临时 D3D11 设备 + 位块链」取，
  改成「本来就要为 `ExecuteCommandLists` 建的临时 D3D12 设备 + 直接队列 + 隐藏窗口上的
  flip-model 链」；D3D11 那条路只留在回退分支（D3D12 或隐藏窗口 flip 链不可用时）。
- 怎么定的位：把这一步拆成 **设备 / 链 / 钩子 / 销毁** 四段单独计时（临时 info 行，已删）。
  创建侧合计只有 112 + 25 + 232 ms，整步却是 3861 ms —— 差额全在 `Release()`：
  **销毁临时 D3D11 设备**在这个进程里要 ~3.5 s；干净进程探针里同一步只要 9 ms，而销毁
  D3D12 设备 + 队列 + flip 链只要 62 ms。所以「3.9 s 全在
  `D3D11CreateDeviceAndSwapChain`」的旧结论错在把创建和销毁包进了同一个 stage。
- 临时链的输出窗口也换成了我们自己创建的隐藏 8×8 popup（`WS_POPUP`、不显示、不呈现），
  不再借用游戏窗口；`D3D11CreateDeviceAndSwapChain` 拆成 `D3D11CreateDevice` +
  `IDXGIFactory::CreateSwapChain`，两条路径共用 `installSwapChainHooks()`，vtable 槽位
  （8 / 13 / 22 / 39）和钩子体没动。
- 之后按作者要求**只留父标记**「安装游戏内面板」：这一步不再有 `log.stage.overlayDevices`
  子标记（它的 375 ms 已经算在父标记里），i18n 键删掉，`src/neurodragon/ui/Glyphs.h` 重新
  生成（302 个字形），`log.stage.*` 现 6 个键。
- 剩下的余量：375 ms 里 217 ms 是 MinHook 的 5 次 `MH_EnableHook`（每次挂起所有线程）。
  想再压可以全部 `MH_CreateHook` 后一次 `MH_EnableHook(MH_ALL_HOOKS)`（一次挂起），本轮没做。

### Changed（2026-10-23：dump、参数追踪与 debug 输出收进 TEST 构建）

- **诊断代码只在 `TEST` 构建里存在**（`xmake f --test=y`，开关与说明见
  `src/neurodragon/Test.h`）。发布构建（默认 `--test=n`）不产生 `<模组目录>/dump/*.bmp`、
  不分配 dump 回读缓冲，也不安装引擎 NGX 参数 setter 的四个 detour 与 CreateFeature wrapper——
  `[param]/[param4]/[ngxset]/[res]/[desc]/[eval]/[dump]/[init]` 这些行随之消失；
  `log::write` 的 level 3 在发布版直接丢弃，debug 参数也不再被求值。
- 保留：info/warn/error（含 i18n）、`log::Stage` 阶段标记、NR 创建/求值/增益合成、面板与
  GPU 计时。测量窗口本身还在，发布版只留计时、不再 dump；面板「诊断」页的
  `参数日志` 开关只在 `TEST` 构建显示。
- 验证：11 个 TU 在带 `-DTEST` 与不带 `-DTEST` 两种配置下均编译通过
  （clang-cl 22.1.7 + MSVC 14.51 / Windows SDK 10.0.26100）；两种配置下剩余警告相同，
  都是原有的死代码（`findAdapter` 等），没有新增警告。

### Added（2026-10-23：启动阶段的成对 info 标记，卡住时能说出卡在哪）

- 「可能卡一下」的步骤在开始前写一行 info、结束后写一行带毫秒的 info（`log::Stage`，
  `src/neurodragon/Log.h`），失败分支写「失败」。首轮实测后**只保留 ≥200 ms 的四处**：
  创建临时 D3D11 设备与交换链 **3519 ms**、安装游戏内面板 3774 ms、创建 NR 功能 542 ms、
  core 加载 264 MB 模型 304 ms。日志最后一行因此就是「卡在哪一步」。
- 那 3.9 s 的 enable() 黑箱由此定位到 `hookSwapChainEntryPoints()`；同一轮的
  `D3D12CreateDevice` 只有 123 ms。**当时写的「全在 `D3D11CreateDeviceAndSwapChain`」
  后来证明不对**：大头是同一个 scope 里的 `Release()`（见上方 2026-10-23 的 Fixed 条目）。
- 实测 <200 ms 的 11 个标记（模组加载 1、模型准备 0、渲染钩子 117、D3D12 队列 123、
  输入钩子 131、ImGui 图形 41、D3D11On12 设备 26、上下文与字体 14、DX11 后端 0、
  字体预热 16、着色器编译 44）已按阈值删除，对应 i18n 键一并删掉（`log.stage.*` 现 7 个）。
- 为此把模组日志的 flush level 提到 info：LL 的 `FileSink` 默认 `flushLevel = Off`，
  info 行留在 `ofstream` 缓冲里，游戏被强杀就一起丢——「开始」那行必须已经在盘上才算证据。
  写盘在 LL 的日志线程里，不在渲染线程（详见 docs/C §4）。

### Fixed（2026-10-23 下午四轮：子窗口内边距、文字裁切、拖动粘滞、热键要按两次）

- **子窗口根本没有内边距**：无边框的 `BeginChild` 默认**不使用** `WindowPadding`
  （imgui.h 原文："no padding by default for non-bordered child windows"），所以两栏文字
  一直贴在栏边——上一轮把 padding 调大没有效果就是这个原因。两个子窗口都补上
  `ImGuiChildFlags_AlwaysUseWindowPadding`。
- **统计行标签被上下裁掉一半**：`PushClipRect` 的起点取自 `GetCursorScreenPos()`，而
  `RenderText()` 实际画在 `CursorPos.y + CurrLineTextBaseOffset`（`AlignTextToFramePadding`
  把这个偏移设成 `FramePadding.y`），所以裁剪框比字形高出一截。改成**按宽度截断字符串**
  （UTF-8 安全，末尾补 `…`），不再用裁剪矩形。
- **拖动时元素粘滞**：原来在 `Begin` 之后 `SetWindowPos`——窗口边框动了，但两栏是按拖动前
  坐标定位的子窗口，于是错位。现在拖动在 **`Begin` 之前**把 `io.MouseDelta` 累加到 `gPos`，
  窗口、背景、两个子栏同一帧用同一个坐标；面板整体比鼠标晚一帧，肉眼不可见。
- **Esc 关闭后要按两次 End 才能再打开**：面板打开时热键的**抬起**被当作普通面板输入吞掉，
  `gToggleKeyDown` 一直是 true，下一次按下被当成键盘自动重复而忽略。现在抬起会清零；
  顺带让热键在面板打开时也能关闭面板。
- 标题条的可拖区域改成「几何判定 + `IsAnyItemHovered()`」，不再占一个 item，缩放按钮
  不会被拖拽区抢走点击。
- **右上角状态点位置**：圆点原来画在 `AddText` 的 `y` 上，而那是行框顶部（不是字形中心），
  所以看起来悬在「健康」上方。现在状态文字是真正的 ImGui item，圆点取它的
  `GetItemRect` 竖直中心，并且文字在缩放按钮那一行里居中。

### Changed（2026-10-23 下午三轮：只从标题条拖动、页签合并、子窗口内边距）

- **拖动**：窗口改成 `NoMove`，只有标题条（渲染管线图上方那一条）能拖，和 Windows 窗口
  一个逻辑；拖拽热区是一个覆盖标题条的 `InvisibleButton`（先提交），缩放/状态按钮后提交，
  所以仍能点；鼠标停在标题条上显示移动光标。其余区域点击不会拖动窗口。
- **页签**：`画面` 页删除，内容（开关、替换模式、增益、Style）并入 `神经渲染` 页顶部，
  现在是 `神经渲染 / 诊断 / 高级` 三个页签。
- **`求值次数` 从统计栏删除**（对玩家没有意义的内部计数）。
- **Style 改下拉**（0/1/2）：三个离散档位，滑条没有必要，而且 0 处把手会被裁一半。
- **子窗口安全区**：内边距 11→14，左栏 280→330、gutter 14→18（540→850 的窗口由两栏
  宽度推出来），统计行「标签 ... 数值」不再被裁掉；`statRow` 的标签加裁剪矩形，
  长标签不会钻到右对齐的值下面；滑条把手在 0/1 两端内缩半个把手宽度。
- 附带：`ui.tab.image` / `ui.image.*` / `ui.stats.evals` 三个键从两份语言文件删除，
  新增 `ui.neural.enableSection/enable/replace/gain`。

### Fixed（2026-10-23 下午二轮：面板可读性 + GPU 计时「无样本」）

- **全角标点显示成 `?`**：字表只收 `4E00-9FFF` 的汉字，而 `，（）：…` 在 FF00/3000 区，
  一个都没进图集。`scripts/collect_glyphs.py` 改成收**所有非 ASCII 字符**（现 294 个），
  中英文稿一起扫，改完文案要重跑。
- **面板 GPU 两个数永远是「无样本」**：`nrStampReset()` 会在每个测量窗口结束时把**已发布的
  中位数一起清零**，而面板读的正是这两个原子量。现在 `nrStampReset()` 只清原始样本，另加
  `nrStampClearReported()` 专给面板的「清空计时样本」按钮；中位数改为每 30 帧发布一次，
  并且 `Replace != 0`（逐帧求值）时每帧都打时间戳，面板才有实时数字。计时堆没建起来时
  显示「计时未启用」而不是「无样本」。
- **面板太大、控件贴边、滑条过长**：全屏面板改成**可拖动浮窗**——默认 790×700 逻辑
  （1440p ≈ 1053×933）、停在左上角；背景空白处按住即可拖动，拖出屏幕会保留 120px 可见；
  内边距 18 逻辑、左右两栏固定宽（280 / 460）、行宽上限 440，滑条不再横跨半个屏幕；
  正文字号密度 0.9。
- **帧时间折线自动缩放**：按峰值在 16.7/25/33.3/50/66.7/100/150/250 ms 之间取档，平滑过渡，
  不再固定 0–50 ms（掉帧时曲线贴顶看不出形状）。

### Changed（2026-10-23 下午：面板快捷键 = End、配置只在启动导入、暂存式应用）

- 面板快捷键改为 **End**（不再是 F10），并可在面板 `高级 → 热键` 里**点按重绑**：点一下按钮，
  再按目标键（Esc 取消）、修饰键单独按不算。绑定存在 `config.json` 的 **`ui.openKey`**
  （Windows 虚拟键码，默认 `0x23` = VK_END），和其余配置一样**只在启动时导入**。
- **删掉 hot reload**：渲染线程里每 ~2 s 重读 `config.json` 的 `reloadConfigIfDue()` 删除，
  `nrCreate()` 里那一次 `config::reload()` 也删除。现在改文件对运行中的游戏不再有任何影响。
- 面板改成**暂存式**：所有控件编辑的是一份 `draft`，`config::get().nr` 不动；底部
  **`应用`** 只把 draft 写进内存（本次运行生效、不落盘），**`保存为默认`** 才额外写
  `config/config.json`，**`放弃`** 把 draft 退回当前运行值。关闭面板丢弃未应用的改动
  （会弹一条提示条）。诊断页三个开关也走同一套暂存逻辑。
- 顶部状态点/管线图/左统计栏显示的是**正在运行的值**，不是 draft —— 应用前它们不会变。
- 语言：面板所有文案改走已有的 `ll::i18n`（`src/lang/{zh_CN,en_US}.json` 的 `ui.*`，
  两边各 106 个键，占位符一致），不再硬编码中文。字体图集改由
  `scripts/collect_glyphs.py` 扫 `src/lang/*.json`（现 284 个汉字），改文案后要重跑。

### Added（2026-10-23：游戏内面板）

- 新增**顶层覆盖层面板**：DXGI `Present`/`Present1`/`ResizeBuffers*` 钩子 +
  `D3D11On12`（绑定游戏自己的 D3D12 direct queue）+ ImGui 的 DX11 后端，画在游戏 HUD **之上**，
  主菜单也能开。`End` 开关，`Esc` 关闭，打开时吞掉游戏输入并补发「已按下」的抬起事件，
  关闭时把光标收回窗口中心再交还游戏。
  路线与实现细节参照本地已验证的两个覆盖层：`D:\Documents\Mangrove\src\mangrove\ui`
  与 `D:\Documents\LHolo\src\overlay`（同一套 Present 钩子 + 11on12 + Win32/DX11 后端）。
- 新目录 [src/neurodragon/ui/](src/neurodragon/ui/)：
  `Overlay`（钩子/设备/窗口过程/帧计时/提示条）、`InputGuard`（拦 `MouseDevice::feed` 与
  `HIDControllerGameCoreDesktop::$onKeyDown/$onKeyUp`，F11 放行）、`Theme`（TRP 风格配色、
  微软雅黑 2x 图集、按 1080p 高度缩放）、`Menu`（顶部管线图 + 四个页签 + 左统计栏 + 右设置栏 +
  底部 放弃/应用/保存为默认）、`Widgets`（自绘勾选框、滑条、下拉、管线节点与箭头、帧时间折线）。
- 面板的配置语义（当天下午改成暂存式，见下一条 Changed）：编辑 draft，`应用` 只进内存、
  `保存为默认` 才落盘、`放弃` 退回运行值；`config::suppressReload` 已随 hot reload 一起删除。
- 面板数据来自真实测量：`nr::nrPanelStats()`（渲染/输出尺寸、feature 状态、求值次数、
  最近错误码、NR 与引擎 DLSS 的 GPU 中位数，后者由 `reportOne` 以原子量发布）+
  `Overlay::frameStats()`（`Present` 间隔算出的帧率/帧时间/折线）。
- 字体图集按**本面板实际用到的汉字**构建（`scripts/collect_glyphs.py` → `ui/Glyphs.h`，
  当前 229 个字形），不拉 2 万字全表。
- 已知：`Replace=6`（TRP Ratio）目前在代码里不会调用合成（`compositeGain` 只挂在 `replace == 5`），
  所以面板的替换模式只列 `0 / 5 / 6`，并在 6 的说明里写明"实际等同于只求值"，等接线后再改文案。

### Removed（2026-10-22 晚：删掉进世界的等待门）

- 删掉 `gInWorld` 与整套"进世界才算数"的等待逻辑：不再有 `join 事件 → gInWorld` 的门、
  不再要求 `n >= 基准帧 + 2` 才建 feature、不再有 `levelSettled`（进世界后等 120 帧）、
  也不再让 `Replace=5` 上屏前等 `建feature + 120 帧 / 进世界 + 120 帧`。
  现在：**引擎一旦交出真实的 Color / Depth / MV / Output，当帧就建 feature 并开始上屏**。
- 关卡切换仍保留两个效果（都不是延迟）：`nrNotifyLevelLoaded()` 给引擎 DLSS 发一次 `Reset=1`，
  以及 dump/计时的窗口计划重新锚定到当前帧（`gWindowAnchor`）。
- 已知取舍：进入世界/刚建完 feature 的那几帧不再被跳过；如果出现短暂黑帧或被 NR 的初始输出污染，
  说明那 120 帧其实在挡这个，届时再只针对"上屏"加回很小的保护。

### Changed（2026-10-22 晚：dump 不再污染游戏根目录）

- 所有 BMP dump 改写进 **`<模组目录>/dump/`**（`dumpFilePath()` 按需建目录，`CreateFileW` 写绝对路径），
  不再落在进程工作目录 —— 也就是游戏根目录。
- 模组目录未知时直接跳过写盘：dump 是诊断，不是交付物。
- 游戏根目录里已有的 80 个 BMP（845 MB）已全部移进 `mods/NeuroDragon/dump/`，根目录现在没有 `*.bmp`。

### Changed（2026-10-22 晚：配置改用 ll::config）

- 不再读任何 txt：`nr_swap.txt`（存在=开）与 `nr_params.txt`（key=value 覆盖）全部退役，
  改由 **`<模组目录>/config/config.json`** 承载，读写走 `ll::config::loadConfig/saveConfig`
  （与 CoralFans 同款：聚合 struct + `version` 字段 + `boost::pfr` 反射）。
- 新 struct [src/neurodragon/Config.h](src/neurodragon/Config.h)：`config::Config{version, nr}`，
  `config::Nr` 里是原来的所有开关（`enabled` / `replace` / `gainBoost` / `style` / `intensity` /
  `localToneStrength` / `localStructureStrength` / `depthInverted` / `sweep` / `mvecScale` /
  `nrEncode` / `whitePoint` / `transferStrength` / `colourStrength` / `maxRatio` /
  `resetEveryFrame` / `srResetEveryFrame` / `paramLog`）。
- 默认值按“NR 是唯一功能”定：**`enabled = true`**、`replace = 5`（增益合成上屏）、`gainBoost = 1`。
  文件缺失/缺键/版本旧 → 自动补默认并回写；文件坏 → 记 error 后重写默认，不会让模组崩。
- 实时调参保留原节奏：渲染线程每 ~2 s `reload()` 一次，`replace` / `gainBoost` 变了会打一条 debug。
- **没有迁移逻辑**：`config.json` 是唯一来源，删掉它就回到默认值；旧的 `nr_params.txt` /
  `nr_swap.txt` 不再读也不再被保留（只有作者用过 txt 版）。`version` 字段保留是因为
  `ll::config` 的 `IsConfig` 概念要求它，不是为了给旧格式做兼容。

### Changed（2026-10-22 晚：自写打包器）

- 去掉 `@levibuildscript/modpacker`（只拷 DLL / PDB / manifest.json），换成
  [scripts/after_build.lua](scripts/after_build.lua)：`bin/NeuroDragon/` 现在含
  `NeuroDragon.dll`、`.pdb`、格式化后的 `manifest.json`、**`lang/*.json`**。
- **NR 模型（266 MB）不进包**：发布时手动放到 `mods/NeuroDragon/nvngx_dlssnr.dll`；
  模组在 `load()` 时把它硬链接到游戏根目录（core 只在那儿找，docs/D §6.17）。
- 坑：Lua 的 `gsub` 返回 `(text, count)` 两个值，直接塞给 `io.writefile` 时第三个参数会被当成
  options 表，报 `attempt to index a number value (local 'opt')` —— 先赋值截断再写。

### Changed（2026-10-22 晚：用户可见输出改 i18n）

- 发布版用户能看到的行改走 `ll::i18n`：`<mod>/lang/{zh_CN,en_US}.json`，用法与 CoralFans 一致
  （`"nr.xxx"_tr(args...)`）。覆盖范围只有会活到正式版、用户真的要读的那些：
  **NR 开关 / 就绪 / 模型落地、分辨率变化、硬链接退回复制、NR 与引擎 DLSS 求值失败、
  NR 创建失败、NGX 导出缺失/挂钩失败**。
- 诊断细节保持原文、不加 i18n：指针/格式/句柄、HDR→LDR 各阶段、`nr_params` 解析、
  GetFeatureRequirements、资源与参数往返、dump 与计时——这些是给 issue 报告看的，
  用户既读不懂也无从处置。
- `getReason()` 实装：NGX 返回码 → `nr.result.*` 本地化文本，接在建 feature 的 debug 行与
  三处用户可见失败（创建失败 / NR 求值失败 / 引擎 DLSS 求值失败）。
- 打包：xmake `after_build` 把 `src/lang/*.json` 复制到 `bin/NeuroDragon/lang/`
  （`@levibuildscript/modpacker` 只拷 DLL / PDB / manifest.json）。
- 坑：`D3D12Bridge.cpp` **不能**直接用 `"_tr"` —— `ll/api/i18n/I18n.h` 会拉进引擎头，
  与 `<d3d12.h>` 撞 `D3D12_FEATURE_DATA_*` 重定义；那一侧改用
  `neurodragon::log::writeKey()`（内部同样 i18n lookup + `fmt::vformat`）。

### Changed（2026-10-22 晚：日志收敛）

- **日志分级按“用户需不需要”重排**：`info` 只留三条一次性事实（NR 开关状态、模型以复制方式落地、
  NR 就绪 + 尺寸 + 首次创建耗时）；`warn` 只留“影响不大或能自愈”的（硬链接失败改复制、
  HDR→LDR 不可用 / encode 失败、nr_params 解析失败、尺寸变化跳窗、NGX 尚未加载）；
  `error` 只留影响功能运行的（缺模型、游戏目录不可写、create/evaluate 失败、NGX 导出缺失或挂钩失败）。
  其余 37 处全部降为 `debug`（钩子地址、资源/参数往返、dump、逐窗统计、GPU 计时、初始化上下文等）。
- `nrLog` 新增 level 3 = `debug`（LL 默认过滤 debug，实测历史日志里一条 `[debug]` 都没有）。
- 删除死代码：`probeOwnDevice`（私有设备实验）、`loadRuntimeModule` / `runtimeFileName` 与
  `nr_runtime.txt` 覆盖机制（core 自己加载模型后不再需要）；部署目录里的 `nr_runtime.txt` 已删除。
- 启动信息改成结论式：`NR 已启用（游戏目录存在 nr_swap.txt）` / `NR 未启用：…没有 nr_swap.txt…`。
- **所有输出统一走 LeviLamina 的 logger**：`utils::logger()` 是唯一入口（`NativeMod::current()->getLogger()`）；
  删除 `nrSetLogger` / `NrLogFn` 注入机制与 `nrLogSink`——`D3D12Bridge.cpp` 通过不依赖 ll/引擎头文件的
  `neurodragon::log::write()` 落地到同一个 logger（`<d3d12.h>` 与 `_HeaderOutputPredefine.h` 冲突，
  不能直接 include LL 头）。排查确认源码里不再有 `printf` / `cout` / `OutputDebugString` 等旁路输出；
  `nrLog` 缓冲 512 → 1024 字节。
- 删除 `RtDispatchProbe.cpp`：`doAllRayTracing` 的 `[rt]` info 级探针（答案早已写进 docs/D §6.15，
  每帧多一次 atomic 自增，纯残留）。

### Added（2026-10-22 晚：NR 模型随包）

- **`nvngx_dlssnr.dll` 随包**：不再要求用户手动放到游戏根目录。模组在 `load()` 里按
  **硬链接 → 复制 → 明示日志** 把它落到 `Minecraft.Windows.exe` 同级：同卷硬链接零拷贝、
  零额外磁盘占用；跨卷或不支持时复制；两者都不行就打印“游戏目录不可写 + 目标路径”，
  NR 报不可用但**不崩**。目标已存在时跳过并打印大小，**绝不覆盖**。
- 依据（ngx_probe，2026-10-22，每次只改一个变量）：core 只从 **exe 同目录** 或 Init 的
  `FeatureCommonInfo::PathListInfo` 加载模型；`SetDllDirectoryW` / 当前目录 / `AddDllDirectory` /
  NGX `dataPath` 全部仍 `0xBAD0000B`。详见 docs/D §6.17。

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
