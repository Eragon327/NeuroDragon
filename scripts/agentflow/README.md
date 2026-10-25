# agentflow

执行纪律工具。协议在仓库根的 [AGENTS.md](../../AGENTS.md)，这里只讲实现与维护。

## 它拦什么

| 失败模式（2026-10-25 的实例） | 机制 | 硬/软 |
|---|---|---|
| 中间模块跑通当目标完成 | `claim --final` 只认 `kind: "final"` 的验收项 + 证据 + review | 硬（命令拒绝） |
| 没证据的重复实机 | `exp new`：同一 unknown 上次失败 → 必须 `--postmortem` + 晚于上次的 `--from-evidence`；`deploy`：同树同 unknown 再跑必须 `--replicate --why` | 硬 |
| 绕过前置检查直接进游戏 | `xmake` 的 `before_build` 跑 `gate build`；`deploy` 要求当前树有通过的 `verify` | 硬（构建失败 / 部署拒绝） |
| 已验证契约被后续改动破坏 | `accept` 记录证据哈希；证据一变 `claim`/`review` 立即拒绝 | 硬 |
| 假设写成事实 | `exp close` 必须有证据文件；失败必须有 `--next` | 硬 |
| 未授权扩大范围 | `contract set` 必须 `--user-authorization`，写入账本 | 硬（命令拒绝） |
| 声称通过但没有执行 | `review` 重跑检查 + 重算哈希；`claim` 要求 review 晚于证据 | 硬 |
| 手抄 DLL 绕过部署 | `deploy` 记 sha256；`doctor` 比对实际部署文件 | 检测（不能阻止 cp） |

## 文件

- `flow.py` 命令行；`checks.py` 静态检查；`ngx_keys.txt` 由运行时 DLL 字节生成；
  `ngx_required.json` DLSS-SR evaluate 的必需参数（每条带证据说明）；`waivers.json` 有期限的豁免；
  `config.json` 实例路径与预算。
- 工作区状态：`docs/agent/contract.json`（契约）、`docs/agent/ledger.jsonl`（只追加账本）、
  `docs/agent/evidence/`（原始产物）。

## 检查怎么加

`checks.py` 里每个 check 返回 findings（`check/id/level/message/where`）。
**不允许「看不见就通过」**：抽不出 HLSL、找不到 evaluate 函数、键表为空，都要报 error。
加检查前先问：这个错误类别真的发生过、或真会在本仓库发生吗？没有就別加。

现有三条检查的来历：

- `ngx.unknown-name`：`JitterOffsetX` 写成裸名，运行时只认 `Jitter.Offset.X`；
  键表用 `flow.py keys --dll <dll> --verify/--add` 维护，只能从 DLL 字节里加，不许手编。
  本机有测试实例的 DLL 时，gate 还会直接拿运行时字节复核源码里用到的每个名字，
  手改键表绕过也会被抓（有测试）。
- `ngx.missing-required`：eval 路径删掉了 float 写入，于是必然缺 `Jitter.Offset.*`。
- `shader.compile`：`src/neurodragon/vv/` 里的 HLSL 用 d3dcompiler_47 离线编（`main` / `cs_5_0`），
  和运行时同一套参数。NR/SR 的 shader 片段与引擎代码拼接，明确列为 out-of-scope 警告。

## 已知限制（别假装它们是硬的）

1. **不能阻止 bash**：权限是 `danger-full-access`，`cp` 到游戏目录、直接开游戏都拦不住。
   能做的只有 `doctor` 事后发现「部署哈希对不上」。
2. **启动期自检没有做**：根签名 64 DWORD 预算、寄存器重叠、DSV 钩子缺失、枚举与 shader 常量不一致，
   这些要么是运行期错误、要么需要改 mod 代码加自检。静态检查覆盖不到，AGENTS.md 里只能作为纪律要求。
3. **豁免机制是双刃**：`waivers.json` 可以合法压掉一条错误，但有过期日、要写理由、部署还要额外确认；
   过期后自动恢复为错误（有测试覆盖）。仍然需要人定期看它。
4. **契约的「完成」判定依赖人写好的验收项**；验收项写得太松，门禁就只能拦流程、拦不住内容。
