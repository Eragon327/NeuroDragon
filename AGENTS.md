# AGENTS.md — 本仓库的执行纪律

## 0. 开工前（30 秒）

```bash
python scripts/agentflow/flow.py status        # 目标 / 阶段 / 验收项 / 未关闭实验
python scripts/agentflow/flow.py doctor        # 有没有人绕过流程（脏部署、失效豁免、陈旧实验）
```

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
- 本仓库**不要 commit**：改动留在工作区，由用户提交。

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
- 测试实例是 `E:\LeviLauncher\versions\1.26.51.01-agent2`，**不要**部署到没有 `-agent2` 的那个版本。
- 引擎的渲染目标在模组加载前就建好了：RTV/DSV 清单要等一次 F11 重建才有渲染分辨率资源。
- 改了 DLL 必须重开游戏或 reload，旧进程里跑的还是旧镜像。
