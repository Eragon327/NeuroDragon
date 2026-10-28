# 0005. VV 的 SR 插入点上移一层：写 `arg10`，复用引擎的显示变换

- **状态**：已采纳（2026-10-28，用户拍板「直接着手 R3」）；同日**进入实施**——阶段 A（纯色写 `arg10`）
  未跑，实施的阶段划分与判据见 [specs/vv.md](../specs/vv.md) §35
- **相关**：[specs/vv.md](../specs/vv.md) §34、
  [research/2026-10-28-vv-structural-review.md](../research/2026-10-28-vv-structural-review.md)、
  [decisions/0001-taau-insertion-point.md](0001-taau-insertion-point.md)

## 背景

阶段 2 前 30 个实验（EXP-0025 – EXP-0054）反复出现同一组症状：颜色冻结、黑面、显示尺寸面、
偏暗/偏亮/饱和、F11 后无法重捕。复盘（EXP-0054 之后）把它们归到三笔结构性债：身份靠形状猜、
suppress 断掉角色来源、**在后缓冲上覆盖，等于把引擎的显示变换一起替换**。

最后一笔在 2026-10-27/28 的证据：

- 引擎有显示分辨率的 HDR 中间面（`2560×1440 fmt26`，仅 RT，双缓冲），declarator 的 `arg10`
  链解析出的两个 native 与日志里的两张逐位相同、逐帧交替；
- 48-list 世界帧里，`25:v2560x1440 d1`（无 DSV）不写后缓冲，`44:v2560x1440 z d1` 才是后缓冲写者
  （B1c 的 tail-8…5 切点图是「上一帧内容」，证明 25 之后后缓冲未被写过）；
- EXP-0045 三批 dump 是**同一指针**跨 8800×（0.1316 / 0.00235 / 20.73），而引擎自己的 F2 输出
  在同一状态下仍是正常亮度（EXP-0046）——说明下游存在一级会做归一化的显示变换。

现状（suppress TAAU + 在 44 之后覆盖后缓冲）等于把 TAAU 与那一级一起替换，然后用一次拟合的
幂函数 + 自算白点去复刻它。复刻的对象是**自适应**的，固定近似不可能收敛；EXP-0048–0053
的「白点」全线都是在为这件事付利息。

## 决策

**把 SR 的插入点上移一层：我们写 `arg10`（TAAU 的输出目标，依赖名 `Color0`），让引擎自己的
下游后处理/合成把我们的结果映射到后缓冲。**

- 切点：**`Color0` 写入 list 之后立刻**（EXP-0055：48-list 帧里是该 list 25；不是「44 之前」，
  44 写的是另一张 RTV）；
- 输出：`arg10`/`Color0`（R11G11B10，`flags=0x1` 无 UAV ⇒ **PS + RTV**，不是 compute + copy）；
- 删除：`vvEncode`、白点 compute、4×4 曝光纹理、final 的 exposure 分支、后缓冲
  `PRESENT→COPY_DEST` 往返、`finalRgba8` 的显示尺寸 copy；
- **第一阶段不 suppress**：TAAU 照跑，我们只在它之后覆盖 `arg10`。这样材质绑定每帧都在，
  输入身份不掉线；代价是 TAAU 的 GPU 时间还在，F1 不成立。
- 第二阶段再 suppress，前提是先找到**与 suppress 正交的角色通道**（或状态机 + 有界转发）；
  另一个未知：suppress 后这条 list 是否仍把 `Color0` 绑成 RTV。

## 前提与判据（未证之前不写实现）

| # | 前提 | 判据 | 不成立意味着 |
|---|---|---|---|
| V1 | `arg10` 被下游消费 | ✅ **已证**：写入者 = 前置无 DSV list（EXP-0055，48-list 帧为 25）；消费者 = `LuminanceHistogram` / `Bloom`×2 / `LocalExposure`，走共享帧图节点引用、无 GPU 复制（EXP-0056）。剩下 `WaitOnUpscalingOutput` 登记语义与 RQ-0004..0007 的静态复核 | 若下游完全不使用，回查后处理链 |
| V2 | ✅ 静态已确认（2026-10-28 反编译）：`arg10` = `H + w*(C−H)`，与 `InputFinalColor` 同域、不做归一化 | — | 无需再测（运行时只需确认写的就是下游读的那张，并入 V5） |
| V3 | 我们能安全写 `arg10` | PS+RTV 写入；状态由 `Color0` 写入 list 留下（RT），写 RT、留 RT，让下一个消费者自己做 barrier | 状态不可控 ⇒ 退 `CopyResource`（仍是 RT 目标） |
| V4 | ✅ 静态已确认：TAAU 是纯 temporal resolve，无曝光/tonemap/encode、无深度、无 VP 使用 | — | 无需再测 |
| V5 | 双缓冲语义正确 | `decl-pre/post` 两次采样（EXP-0032 已证 decl 阶段可读） | 写错缓冲 ⇒ 画面滞后一帧（可见） |

V1 已由 EXP-0055 / EXP-0056 关闭，V2/V4 由反编译静态确认；**只剩 V3/V5 要靠阶段 A 的一次实写**
（[specs/vv.md](../specs/vv.md) §35.1 的 A1–A3）。

## 被否的替代

- **继续补白点/编码**：复刻自适应函数，不可收敛（EXP-0048–0053 已付 6 个实验）。
- **读 TAAU shader 复现**（research 的「High」方案）：方向对、成本 1 天 + 工具，但它回答的是
  「TAAU 里有什么」；R3 走的是「写它的输出」，不需要复现。保留为**阶段 A 失败后**的路线。
- **SDR 路**（用已 tonemap 的 8bit 面喂 DLSS）：可去掉整条曝光债，代价是量化与丢 HDR 高光；
  作为兜底，不在本轮。
- **从 main 重开做 R3**：main 是 agent2 的祖先、只有 1087 行旧探针，重开=重拉 5k 行；
  改为 `r3` worktree 从 agent2 尖端切出（2026-10-28 已建）。

## 后果

**正面**：曝光/色调/编码不再由我们复刻；后缓冲状态与黑边问题消失；F2（同帧对比）第一次有物理
意义——TAAU 版与 DLSS 版走同一条显示变换。

**负面**：第一阶段 TAAU 仍在跑（F1 不成立）；`arg10` 是引擎资源，写它要承担状态/双缓冲的风险；
`VvBypass`/`VvTone` 这两套实现进入「待删」状态（AGENTS §4：不许长期并行）。

## 止损

命中任意一条即退回「读 shader」路线，不再在 R3 上加补丁：

- 实现后把 `arg10` 写成纯色、引擎画面**不受影响**（= 阶段 A 的 A1）；
- `arg10` 的状态连续两次调试层报错（= A3；先试 `CopyResource` 兜底，仍报错即停）；
- 双缓冲写错、画面滞后一帧且按帧重取后仍不消失（= A2）。

## 2026-10-28 反编译补充

- `Upscaling.material.bin`（SM5.0 DXBC 与 SM6.3 DXIL 逐段对照一致）确认：TAAU = 3×3 邻域方差约束
  + Catmull–Rom 历史重采样 + 当前帧权重 ≤0.1；**无深度、无 tonemap/exposure/encode、不使用 VP**。
  ⇒ V2/V4 关闭。
- **MV 是上游输入**：`t0=InputBufferMotionVectors`，`.zw`，Y 翻转、渲染分辨率归一化；
  `historyPos = displayCenter - mv*RenderResolution/ratio`。
  ⇒ 实现放弃 depth+VP 重建，改绑引擎 arg11（详见 [specs/vv.md](../specs/vv.md) §6 的 2026-10-28 更正）。
- 新增待判项：深度方向——代码 `near/(1-d)`（标准 Z）与 `DepthInverted=1` 互相矛盾，需一次判定。
- **EXP-0055 更正管线位置**：`Color0` 的写入者是**显示分辨率、无 DSV、单 draw** 的前置 list
  （48-list 帧为 25），后续带 DSV 的合成 list（44）写的是另一张 RTV。⇒ 切点 = 写入者之后，
  不是 44 之前；且 44 是否采样 `Color0` 未证（中间还有后处理）。
- **EXP-0056 补上下游**：`arg10` 出 TAAU 后以**共享帧图节点引用**进入 `LuminanceHistogram`、
  `Bloom`（两次）、`LocalExposure`；每帧除 TAAU 自身绑定外恰好 3 次后续材质引用，
  `CopyTextureRegion` / `CopyResource` 两条路径在采样帧内均无命中 ⇒ 下游是「引用 + shader
  后处理」，不是先复制一份再由副本消费。⇒ V1 关闭（尾巴：`WaitOnUpscalingOutput` 的登记语义、
  RQ-0004..0007 的静态复核）。
- 证据：[research/2026-10-28-taau-decompiled.md](../research/2026-10-28-taau-decompiled.md) §9–§10、
  `docs/agent/evidence/arg10-list-binding-EXP-0055.log`、
  `docs/agent/evidence/arg10-downstream-EXP-0056.log`。

## 实施顺序

2026-10-28 定为 [specs/vv.md](../specs/vv.md) §35 的 **阶段 A–D**：

1. **阶段 A**：纯色写 `arg10`（TAAU 照跑），只回答 V3/V5；A1 画面不受影响即停止实施；
2. **阶段 B**：DLSS 输出走 PS+RTV 写 `arg10`，仍不 suppress；删白点 compute / `vvEncode` /
   `finalRgba8` / 后缓冲 `PRESENT → COPY_DEST → PRESENT`；
3. **阶段 C**：MV 改绑引擎输入（`{z, -w} * RenderResolution`），按材质角色区分通道，
   然后删 VP / 相机原点重建与自算 MV；深度语义单列未知；
4. **阶段 D**：不 suppress 下先回归（F11 / 切世界 / Stop/Start），再处理角色通道与 suppress 回退。

本 ADR 只保留决策本身；判据与止损以 §35 为准。原先记的「第一刀：删零引用死代码」仍是
阶段 A 之前的低风险清理项，但不阻塞 A。
