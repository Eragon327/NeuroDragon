# AGENTS.md — 本仓库的执行纪律

## 0. 开工前（30 秒）

```bash
python scripts/agentflow/flow.py brief         # 开局唯一必跑：阶段/构建/等你跑的/未提交归属
python scripts/agentflow/flow.py status        # 目标 / 阶段 / 验收项 / 未关闭实验（长视图）
python scripts/agentflow/flow.py doctor        # 有没有人绕过流程（脏部署、失效豁免、陈旧实验）
```

- 这轮只要碰**地址 / 签名 / hook / 反编译**，就再多跑一条：
  `python scripts/agentflow/re_kb.py status`，并用 `re_kb.py find <关键词>` 查已有的结论（§13）。
  别为了"看一眼"去开 idb。
- 长任务**必须先有契约**：`docs/agent/contract.json`（goal / criteria / stage / out_of_scope）。
  没有就补，别在对话里口头约定。
- **阶段只能通过 `claim` 前进**，且必须有证据 + 一次更晚的 `review`。
- 中间模块跑通 ≠ 目标完成。目标完成只认 contract 里 `kind: "final"` 的验收项。

## 1. 一次实验 = 一条账本记录

```bash
python scripts/agentflow/flow.py exp new \
  --unknown U-ngx-params \
  --hypothesis "005 的原因是参数键名不是点分形式" \
  --change "把 Jitter/MV 的写入改成点分键名" \
  --expect-pass "eval=1，画面变锐" \
  --expect-fail "仍是 005，则看 NGX feature 日志点名了哪个参数" \
  --evidence-plan "%LOCALAPPDATA%\\Temp\\NeuroDragon\\NGX\\nvngx_dlss_*.log"

python scripts/agentflow/flow.py exp close EXP-0007 --pass \
  --result "eval=0x00000001" --evidence <原始日志路径> \
  --conclusion "..." --next "..."
```

`exp new` 会强制三件事：
1. **一台实验只验证一个未知项**；`unknown` 相同就会累计次数。
2. 上一次**失败**之后再来一次，必须带 `--postmortem`（上一轮证据说明了什么）和
   `--from-evidence <文件>`，且该文件必须**晚于**上次记录 —— 这就是「先分析证据再动手」的可检查形式。
3. 同一未知项超过 `max_attempts_per_unknown`（默认 3）需要 `--user-authorization "<谁/在哪说的>"`。

`exp close` **必须有证据文件**（日志 / dump / 截图），否则只能算推测。失败还必须写 `--next`。

- 步骤往往实验开了才想清楚：`exp update EXP-xxxx --runbook "进世界|走动|说一声"`（`--tier`、`--expect-s` 同理）。
- 「没跑 / 现在不行」用 `exp cancel EXP-xxxx --why "…"`：不需要证据、不占 attempt 次数、不算失败。
  **别为了收尾塞假证据**——那比不记录更坏。

**没有可失败的假设的改动不开 exp**（重构、文档、门禁、typo）：跑 `verify` 就够了。
`exp` 记的是未知项，不是「我改过东西」。

## 2. 实机预算与停止条件

一次「构建 → 部署 → 让人开游戏 → 贴日志」算一次实机。进入实机前，逐条自问并写进 `exp new`：

1. 这次要消除的**唯一**未知项是什么？
2. 它能不能靠静态检查 / 构建 / 启动自检 / 已有日志解决？
3. 这次改动有没有引入**新的未验证变量**（顺带改了别的东西）？
4. 成功判据是什么？失败后第一条诊断路径是什么？
5. 成本：一次构建约 1 分钟，一次实机约 5–10 分钟。

**失败后不许直接猜下一版。** 先读原始日志/代码 diff，提出有证据的假设，再进 `exp new`。
没有新增证据的重复实机，流程会拒绝；被拒绝时不要绕过，先把证据补上。

## 3. 构建与部署

- **构建**：`xmake -y`（msys 下配置要 `-p windows`）。`xmake` 里挂了硬门禁
  `flow.py gate build`，源码一违反下面任一条，构建直接失败（不是警告）：
  - NGX 参数名不在 `scripts/agentflow/ngx_keys.txt` 里（该表由运行时 DLL 字节生成）；
  - DLSS-SR 的 evaluate 路径漏了必需参数（`ngx_required.json`，`Jitter.Offset.*` 有实证）；
  - `src/neurodragon/vv/` 里的 HLSL 编不过（d3dcompiler_47，`main` / `cs_5_0`）。
- **部署**：只能 `python scripts/agentflow/flow.py deploy --exp EXP-xxxx`。它会要求：
  当前源码树有一次通过的 `verify`、实验是打开的、构建比源码新、没有未批准的豁免；
  然后把 DLL/PDB/manifest/lang 拷过去并记账。**不要手 `cp`** —— `doctor` 会对不上账并报出来。
- 已豁免的偏差：`scripts/agentflow/waivers.json`，必须写理由 + 过期日期 + 由谁批准；
  部署带豁免的构建还需要显式 `--allow-waived "<这次跑仍然值得的理由>"`。
- **commit 由 Agent 敲，但必须先拿到批准**：`flow.py commit propose`（生成消息 + 前置检查）→
  用户回 ok → `flow.py commit apply --approved "<原话>"`。没批准不提交；只 `git add` 不需要批准。
  提交标准见 §9。

## 4. 已验证 ≠ 永久有效

- 已被 `accept` 的验收项，证据文件一改，`claim` 就会拒绝（证据过期）。
- 动到已验证行为的代码，要重跑 `flow.py verify` 并在 `exp` 里说明影响面；
  回归了就先用 `review`/`doctor` 定位引入点，不要叠加猜测性修复。
- 两套功能重叠的实现（如 `vv/VvBypass` 与 `vv/VvSr`）不许各自演进：改一套，另一套要么删、要么写明为何保留。

## 5. 阶段门禁与审查

阶段（对应 contract 的 S1–S6）：契约 → 静态检查+构建 → 核心功能实跑 → 输出可见 → 目标语义（替换而非覆盖） → 回归交付。

```bash
python scripts/agentflow/flow.py verify            # 跑检查并记账（同一源码树）
python scripts/agentflow/flow.py review --record   # 重跑检查 + 重算证据哈希（审查者用这个）
python scripts/agentflow/flow.py claim --stage 3   # 前进阶段；证据/审查不够就拒绝
python scripts/agentflow/flow.py claim --final     # 只认 kind=final 的验收项
```

- 审查者**不读执行者的自述**：跑 `review`，看它重算的哈希与检查输出；必要时再跑 `doctor`。
- 如果执行与审查是同一个模型，角色分离不构成独立验证 —— 用原始日志、确定性检查、不同路径复核来降低共同犯错。
- 想改目标/扩大范围：`contract set` 需要 `--user-authorization`；**改计划文档不算授权**。

## 6. 证据怎么写

- 证据是**原始产物**：`latest.log` 的相关段、NGX feature 日志、dump 的 `.bin`、截图、构建日志。
  不要手写「已通过」当证据。
- 未证实的写「假设 / 待验证」；只验了一部分就写清楚哪部分没验。
- 结论分三类，别混：**已确认**（有原始证据）/ **未解决**（列出下一步判据）/ **已证伪**（保留反例，防止重走）。

## 7. 这个仓库的既有约束（别踩）

- Hook 用 LeviLamina 的 8 个 Hook 宏，不要手写 MinHook 注册（热函数会在 `enable()` 里死锁）。
- 测试实例由**本检出**的 `scripts/agentflow/config.local.json` 给出（git-ignored，`instance` 字段）：
  `agent2` 检出用 `E:\LeviLauncher\versions\1.26.51.01-agent2`，`main` 检出用 `E:\LeviLauncher\versions\1.26.51.01`，
  `r3` 检出用 `E:\LeviLauncher\versions\1.26.51.01-r3`。
  没有这个文件时 `deploy` 直接拒绝；**不要**把实例写回 tracked 的 `config.json`，也不要用 `--instance` 指向别人的实例。
- 引擎的渲染目标在模组加载前就建好了：RTV/DSV 清单要等一次 F11 重建才有渲染分辨率资源。
- 改了 DLL 必须重开游戏或 reload，旧进程里跑的还是旧镜像。
- **`scripts/agentflow/` 是 `sys.path[0]`**：放在那里的脚本文件不能和标准库同名。
  加过一个 `re.py`，`flow.py` 立刻起不来（`argparse` → `import re` 命中了它，`re.compile` 不存在）。
  新文件先过一眼标准库名字（`re` / `json` / `types` / `code` / `copy` / `io` / `abc` …）。

## 8. 人机契约（谁负责什么）

**请求实机前，Agent 必须给全这五条**，缺一条就别说「请跑一次」：

1. 这次消除的**唯一**未知项 + 账本编号（`EXP-xxxx`），以及跑的是哪个构建（部署时间/哈希）。
2. 3–5 步操作，含「**不要做什么**」（别点别的按钮、别改设置）。
3. 预期看到什么；以及失败时**分别意味着什么**——由 Agent 读日志判定。
4. 崩了/不对时用户做什么（多数情况是「什么都别做，说一声」）+ 怎么退回已知状态。
5. 预计耗时。

**Agent 不可以**：

- 让用户判断 Agent 自己能测的东西（「是不是更锐」「是不是更清楚」）——画质只能当**次要信号**，不能当验收判据。
- 把能从日志确认的事甩给用户负责（例：F11 有没有生效，看 `[vvsr] sr pick: … colourTargets=`）。
- 无新证据就再问一次同一个未知项（`exp new` 会拒绝）。
- 让用户调试（试点别的按钮找原因、改配置试）。

**用户只有三条义务**：照做（偏离就说明）→ 报可见异常 → 给一个结论词。不用解释技术原因、不用截图、不用判断是不是"对"。

| 用户说 | 含义 | Agent 必须做 |
|---|---|---|
| 「好了」 | 步骤做完，无可见异常 | 读日志下结论；**日志说失败就以日志为准**，不能拿「用户说好了」推翻 |
| 「好了，看不出区别」 | 跑完，画面无可见变化 | 先看 `eval=`；成功则是 MV=0/Reset=1 的预期，不记为验收 |
| 「崩了，在 X 之后」 | 客户端退出/黑屏 | 读日志尾与 crash 文件，不猜 |
| 「不对：…」 | 偏色/HUD 丢失/闪烁/按钮不在 | 记进账本，先定位引入变更，不叠加猜测 |
| 「没跑」/「现在不行」 | 拒绝或推迟 | **不算失败**；关掉实验或改走不需要实机的路径 |
| 「我改了步骤：…」 | 偏离脚本 | 本轮不作该判据的证据，重跑或降级为探索 |

用户的话要**原样**进账本（`exp close --result "<原话>"`），不许改写成「已验证」。

## 9. commit 标准（`flow.py commit`）

- **一个 commit = 一个通过 `verify` 的状态 + 一条账本理由**。类型由文件组决定：
  `exp`（`src/`）、`flow`（`scripts/agentflow`、`xmake.lua`、`AGENTS.md`）、`docs`（specs/roadmap/CHANGELOG）。
  三者混在一起就既 review 不了也回滚不了——`9bc5c7b`（17 文件 2102 行）就是反例。
- subject 一句祈使句 ≤72 字符，说的是**可检查的行为**；`update/improve/enhance/misc/changes`
  会被工具拒绝。其余 trailer（Exp/Hypothesis/Evidence/Verify/Deploy）由 `commit propose` 从账本填，
  agent 不手写——**消息能说的东西不超过账本里有的东西**。
- 有 open exp 动 `src/` 时拒绝提交；要中途留存用 `--checkpoint`（生成 `wip(EXP-xxxx):`，`doctor` 会标出）。
- `commit propose` 自己会跑一次 `verify`；`tree_digest` 是内容哈希，**提交不会让它失效**（旧实现会）。
- 证据随 exp commit 一起进；`bin/` 永不进；>512 KB 文本 / >2 MB 图会打 `Note:` 提示。
- 提案 → 用户 ok → `apply --approved "<原话>"`。内容在批准后被改过，`apply` 会拒绝。

## 10. 跨 session 交接（`flow.py brief` / `handoff`）

- 开局只跑 `flow.py brief`：阶段 / 构建 / 等你跑的 / 上次 next 有没有动 / 未提交文件归属 / 本次已花实机。
- 会话结束**只有状态变了才写**：`flow.py handoff write --next "..." [--awaiting ...] [--note ...]`。
  字段三个，写不出就说明没什么可交接。
- 不维护 `HANDOFF.md`：第二份真相必然漂移，新 session 会信它而不信账本。账本 + `brief` 是唯一来源。
- `brief` 里 `dirty` 的「未归属」= 上一轮改了没记账的手尾：先确认，再决定提交或丢弃。
- L2 请求用 `flow.py runbook` 一次列全，不要一个 exp 打断一次。

## 11. 测试分级（谁跑、谁判定）

| 级 | 触发 | 判定 | 前置 |
|---|---|---|---|
| L0 | agent | 退出码 / 日志（`verify`、构建、`smoke --replay`） | 无 |
| L1 | agent（`smoke` 启动客户端） | 日志断言（`scripts/agentflow/smoke/*.json`） | verify + deploy；同树失败重跑要 `--replicate`；每 session 预算 |
| L2 | 用户 | 日志断言 + 一个结论词 | `exp new --tier L2 --runbook "步骤1|步骤2|步骤3"` |

- L1 默认同意，但有闸：游戏已在跑 → 默认拒绝（`--interrupt` 才会杀）；一次只允许一个 launch lease；
  `l1_max_per_session` 用完要 `--user-authorization`。默认同意 ≠ 无限连点。
- smoke 只看启动后日志增量（锚定 byte offset）——4.4 MB 的 `latest.log` 里还躺着上一局的失败，
  不锚定就会把旧的 `ret=0xbad00005` 当成本次的。
- L2 的判据必须是日志断言；人只提供**触发**和**异常**（偏色/闪/HUD 丢了）。能加一行断言解决
  的 L2 必须降成 L1；反过来为了不写断言把 L1 抬成 L2 = 违规。
- L2 触发没发生时（没进世界/没按步骤），`smoke` 记 ABORTED 并**保持实验打开**，不记失败。

## 12. 协作与 IDA（MCP / 锚点会话）

- **Subagent 默认使用 Deepseek Flash**。
- **委派必须按“块”而不是按“步骤”**：主 agent 亲自完成路线判断、实验设计、文件/地址定位、代码串联与最终证据复核；subagent 只承担边界清晰、上下文少、可一次产出整块结论的任务（如离线反编译、批量日志/二进制分析、独立代码审查）。
- **不派发薄任务**：纯跑命令、读一段已知日志、改一处局部代码、提出单个 follow-up 这类工作由主 agent 完成。只有预计需要独立上下文达到数百行以上，或能并行覆盖多个不相交问题，才委派。
- **subagent 一次必须拿到完整背景**：目标、已知路径/地址/文件名、判据、禁止事项和预期输出一次写清；后续不要靠连续追问补上下文。默认承接原任务的 agent 继续修正，而不是新起一个。
- **IDA 默认不能并发**：同一时刻只跑一个 IDA 打开/分析任务；不要并发 `idb_open`、不要让多个 subagent 同时操作 IDA。确需并行时先证明是隔离的进程与数据库，否则仍串行。
- **MCP 连的库不一定是你要的库**：本机同时开着两个 GUI ——
  `minecraft-edu.i64`（Mac 1.26.32 全符号，`127.0.0.1:13337`，imagebase `0x100000000`）与
  `Minecraft.Windows.exe.i64`（26.51，`127.0.0.1:13338`，imagebase `0x140000000`）；
  **MCP 默认连 13337**。判据永远是 `ida_nalt.get_input_file_path()`，不是端口、不是窗口标题。
  `re_serve.py` 会用 `config.local.json` 的实例比对并打 WARNING。
- **subagent 不跑 Alt+F7、不开 idb**：它的产出写进 `docs/agent/re/requests/`（§13），由人服务队列。
- 打开/关闭 IDB 也属于有副作用的操作：记录意外保存或解包产物；只读分析不得主动改名、改类型、patch 或保存。

## 13. RE 知识库（idb 之外的真相）

**判据**：`docs/agent/re/` 里的文本是真相，`.i64` 是派生产物。26.51 的 idb 是 3.67 GiB，
开着时 `.id0/.id1` 再加约 3.8 GiB；IDA 打开即写模式，**第二个打开者 = 抢锁或整份拷贝**。
协议全文在 `docs/agent/re/README.md`，这里只列必须照做的。

```bash
python scripts/agentflow/re_kb.py status              # build / 队列 / canon / idb 是否有人在用
python scripts/agentflow/re_kb.py find taau           # ★ 动手前必跑：canon/死路/队列/快照/docs
python scripts/agentflow/re_kb.py sig --rva 0xb74f860 # rva -> 最短唯一签名（离线，不开 idb）
python scripts/agentflow/re_kb.py req add --key k --sig "48 89 ..." --why "它回答什么问题"
python scripts/agentflow/re_kb.py canon add --key k --addr 0x14... --sig ... --evidence docs/x.md:12
python scripts/agentflow/re_kb.py canon verify        # 新 build 来了先跑：哪些签名失效
```

| 谁 | requests/ | builds/（快照） | canon/ | idb |
|---|---|---|---|---|
| 主 agent | 追加 | 只读 | 追加 | **不开** |
| subagent | 追加 | 只读 | 只读 | **不开** |
| 锚点会话（人按 Alt+F7 跑 `re_serve.py`） | 只读 | 写 | 不碰 | 读，不保存 |

- **一个锚点会话**：全导入 / 自动分析全世界只做一次，产物落成文本（`builds/<sha12>/decomp/`）。
  要新函数就 `req add`，**不要为新函数再开一个 db**。
- **签名必须唯一**：`req add` / `canon add` 下单时就离线扫模块，0 次或多次都拒绝。
  实测教训：序言 `55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC` 在 26.51 exe 里出现 **48182 次**，
  只能当挂钩前的健全性校验，不能当定位器；`taau` 的入口签名要到 **48 字节**才唯一。
- **地址必须分模块**：mac 1.26.32（`0x10…`，base `0x100000000`）/ win 26.51（`0x14…`，`0x140000000`）/
  其它 DLL（`0x18…`）/ 运行期堆指针（`0x1f…`，每次启动都变，直接丢弃）。
  把 26.51 的地址记成 mac 的，下一个 agent 就会去错库里找。
- **没有证据不许进 canon**（`--evidence` 必填）；改正是**追加同 key 一行**（`--amend`，last wins）；
  key 改名用 `canon rename`（留墓碑行，旧名照样查得到去向）。
- 结论进 `canon/funcs.jsonl`，证伪的假设进 `canon/refuted.jsonl`。
  **不要在 research 文档里再抄一遍地址**——复制出来的第二份必然漂移。
- 静态分析的产物不是实验证据：`exp` 记实机，`canon`/`decomp` 记静态，两者不能互相冒充。
