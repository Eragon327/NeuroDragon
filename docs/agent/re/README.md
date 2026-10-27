# docs/agent/re — RE 知识库（idb 之外的唯一真相）

**一句话**：`.i64` 是派生产物，仓库里的文本才是真相。agent 永远不需要"为了读一下"去开 idb。

## 为什么不是 idb

| 事实 | 数字 | 后果 |
|---|---|---|
| `Minecraft.Windows.exe` | 296 MiB | — |
| `Minecraft.Windows.exe.i64` | 3.67 GiB（12.7×） | 每次 load 都要读它 |
| 开着的 `.id0` + `.id1` | 2.63 + 1.16 GiB | 开一个会话 ≈7.5 GiB 磁盘 |
| IDA 打开数据库的模式 | 读**写** | 第二个打开者 = 抢锁或整份拷贝 |

所以：**一个锚点会话，其余全是读者**。全导入 / 自动分析全世界只做一次（就是你那个 GUI 会话），
产物经 `re_serve.py` 落成本目录里的文本。agent 要新函数就下单，不自己开 db。

## 目录

```
docs/agent/re/
  README.md                 本文件：协议与命名，别在这里存事实
  requests/<by>.jsonl       下单队列；每个 agent 一个文件，只追加
  served.jsonl              锚点会话的回执（成功/失败都记，只追加）
  builds/index.json         exe 路径+size+mtime -> sha256 缓存（296 MiB 只哈希一次）
  builds/<sha12>/meta.json  这个 build 的身份：sha256、exe、版本
  builds/<sha12>/addr_index.json   key -> rva / 函数名 / proto / 产物文件
  builds/<sha12>/decomp/    ★ 真正的资产：伪代码与交叉引用快照（文本）
  idb.local.json            .i64 路径（machine-specific，git-ignored）
  canon/funcs.jsonl         第二阶段：已命名/已定位的函数（签名、锚点、跨版本别名）
  canon/refuted.jsonl       第二阶段：已证伪的假设（死路）
```

`builds/<sha12>/` 里**没有** idb、没有 bin。idb 丢了可以从 exe + 签名 + 这些文本重建。

## 谁写什么

| 角色 | requests/ | builds/ | served.jsonl | canon/ | idb |
|---|---|---|---|---|---|
| 主 agent | 追加 | 只读 | 只读 | 写 | **不碰** |
| subagent | 追加 | 只读 | 只读 | 提议（追加） | **不碰** |
| 锚点会话（人按 Alt+F7） | 只读 | 写 | 追加 | 不碰 | 读，不保存 |

没有人手改 `builds/`、`served.jsonl`、`requests/*.jsonl` 里已有的行。**只有追加**。
理由：同一时刻可能有多个 agent 在写，改行 = 互相撕；追加 = 零锁。

## 键与命名规则

- `key`：`^[a-z][a-z0-9_]{1,47}$`（小写 snake_case）。**一个 key = 一个被分析对象**（函数 / 结构 / vtable）。
  一个函数只许有一个 key —— 两个 key 指向同一函数会被 `req add` 拒绝（那是同一个事实的两个名字）。
- build 目录：`builds/` + exe **sha256 的前 12 位**。地址只在同一个 build 内有意义，所以地址永远跟着 build 目录走。
- 产物文件名（由 `re_serve.py` 生成，不许手改）：

| want | 文件 | 内容 |
|---|---|---|
| `pseudo` | `<key>.c` | Hex-Rays 伪代码，头部 C 注释里带全部元信息 |
| `proto` | 不单独出文件 | 已写在每个文件的头部 |
| `xrefs` | `<key>.xrefs.txt` | 谁引用它（地址 + rva + 所在函数） |
| `disasm` | `<key>.asm` | 逐条反汇编（伪代码失败时的兜底） |
| `callees` | `<key>.callees.txt` | 它调了谁 |
| `strings` | `<key>.strings.txt` | 函数体内引用的字符串字面量 |

每个产物头部都有一段元信息（key / build / sha256 / exe / idb / resolve / rva / file_off / ea /
func / proto / why / requested_by / served）。它就是这条快照的出处，别删。

## 下单格式（`requests/<by>.jsonl`，一行一条）

```json
{"ts": "2026-10-27T12:40:00.123+08:00", "kind": "re-req", "id": "RQ-0001",
 "key": "taau_framecontext", "resolve": {"kind": "sig", "value": "48 89 5C 24 08 ..."},
 "want": ["pseudo", "proto", "xrefs"], "why": "确认 TAAU 拿到的 MV 是引擎自己产出的哪一份",
 "by": "agent", "sig_check": {"checked": true, "hits": 1, "file_off": "0xE412B40"}}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 自动 | `RQ-####`，由 `req add` 分配 |
| `key` | ✓ | 决定产物文件名 |
| `resolve.kind` | ✓ | `sig` / `rva` / `addr` / `name`，**四选一** |
| `resolve.value` | ✓ | `sig` 用 `48 89 ?? ..`；`rva` 用 `0x412B40`；`name` 用 idb 里的符号名 |
| `want` | 默认 | `pseudo,proto,xrefs` |
| `why` | ✓ | 这个函数回答什么问题。没有问题的解包请求一律拒绝 |
| `by` | 默认 `agent` | 决定落在 `requests/<by>.jsonl` |
| `sig_check` | 自动 | 下单时的离线扫描结果（命中数 + 文件偏移 + exe 身份） |
| `force` | 可选 | 已产出也要重跑（`re_kb.py req add --force`） |

**`sig` 必须唯一命中**：`req add` 会在下单时离线扫模块（不碰 idb，296 MiB 约 0.3 s），
0 次或多次都拒绝——一个靠模糊模式定位的事实比没有事实更糟。确实要弱条目才用 `--allow-ambiguous`。

## 一次完整回路

```bash
# 1. agent 下单（不需要 idb）
python scripts/agentflow/re_kb.py req add --key taau_framecontext \
  --sig "48 89 5C 24 08 ..." --why "确认 TAAU 的 MV 来源"

# 2. 人：在已经开着的那个 IDA GUI 里  File > Script file...  (Alt+F7)
#    选 scripts/agentflow/re_serve.py  ->  写 decomp/、追加 served.jsonl
#    （每次最多 40 条，再按一次 Alt+F7 继续；状态在 served.jsonl 里，可恢复）

# 3. 任何人读文本，不开 idb
python scripts/agentflow/re_kb.py status
python scripts/agentflow/re_kb.py served ls
```

`re_serve.py` **是读操作**：不保存数据库、不改名、不设类型、不 patch。它只往 `docs/agent/re/` 写文本。

## 第二阶段：canon（命名与死路）

第一阶段的 `decomp/` 是原始材料，只有先知道 key 才读得到。阶段二回答另一个问题：
**「我要找的那个函数是哪个」**——存命名、跨版本别名、以及走过的死路。

`canon/funcs.jsonl` 一条一个函数（只追加，同 key 最后一行生效）：

| 字段 | 说明 |
|---|---|
| `key` | 稳定标识 `^[a-z][a-z0-9_]{1,47}$`；一个 key 一个函数 |
| `label` | 文档 / IDB 里它原本的名字（可能带 `::`） |
| `addr` / `addr_space` | 地址 + 归属模块：`mac-1.26.32`（base `0x100000000`）/ `win-26.51`（`0x140000000`）/ `other-module` |
| `rva` | 仅 win：`addr - ImageBase`，构建内有效 |
| `sig` | **主键**：26.51 字节签名，写入前离线验唯一 |
| `anchors` | 签名漂移后的重搜依据：`prologue:<hex>` / `str:<字面量>` / `import:<dll!sym>` / `vtable:<槽>` |
| `mac_addr` / `mac_alias` | 仅 mac：符号化 IDB 的地址与符号名（命名的来源） |
| `proposed_name` | 想写回 idb 的名字 |
| `semantics` | 一句话：它干什么（不是它长什么样） |
| `confidence` | `observed`（文档/反编译里见过）/ `confirmed`（有自己的证据）/ `inferred`（推断） |
| `status` | `resolved-in-26.51`（唯一签名）/ `located-in-26.51`（只有 rva，绑构建）/ `located-in-other-module` / `unresolved-in-26.51`（只有名字） |
| `evidence` | `docs/...:line` 或 decomp 文件；没有证据不许写 |
| `origin` | `imported-from-docs`（文档抽的草稿）/ `authored` |

```bash
python scripts/agentflow/re_kb.py canon import        # 从 docs/ 抽地址做草稿（幂等，可重跑）
python scripts/agentflow/re_kb.py canon add --key taau_fb3d --addr 0x14b74fb3d \
  --semantics "..." --anchor "prologue:55 41 57 ..." --evidence docs/specs/vv.md:67
python scripts/agentflow/re_kb.py canon add --key taau_fb3d --amend --confidence confirmed ...
python scripts/agentflow/re_kb.py canon ls [--unresolved]
python scripts/agentflow/re_kb.py canon verify        # 重扫所有签名，报漂移
python scripts/agentflow/re_kb.py canon refute --key taau_fb3d \
  --claim "Windows 沿用 Mac 的 +0x10" --why "实测 Windows 是 +8" --evidence docs/specs/vv.md:81
```

硬规矩：

- **没有证据不许进 canon**（`--evidence` 必填）。文档是证据，猜测不是。
- **地址必须分模块**：`0x10b6a9ac1` 是 mac、`0x14b74fb3d` 是 win-26.51、`0x1f7aacc6010` 是运行期堆指针。
  导入时堆指针直接丢弃——它每次启动都变，不能当 key。
- **签名唯一才能当主键**；不唯一就补锚点，别硬塞。
- 修正是**追加同 key 一行**（`--amend`，最后一行生效），不回头改旧行。

`canon/refuted.jsonl` 记死路：`{key, claim, why, evidence, exp}`。26.51 没有符号，
最贵的知识是「知道它不是什么」；不记，下一个 agent 会再走一遍。

## 开局怎么用（防重复分析）

0. `re_kb.py find <关键词|rva|签名>` —— **任何 RE 动作之前先查这个**。它同时搜 canon、
   已证伪清单、队列、快照和 docs/，最后一行给出「next」：已产出 / 已排队 / 只有名字 / 什么都不知道。
1. `re_kb.py status` —— 有哪些 build、哪些产物、有没有待办、idb 是不是有人在用。
2. 要找一个函数：先看 canon 里有没有 key；有就按 status 决定读快照 / 下单 / 自己导签名。
3. 没有才 `req add`。**不要为了"看一眼"去开 idb**，也不要新开 worker —— 那是 7.5 GiB 和一把锁。
4. 结论（"这个函数是 taau 的入口，因为…"）进 `canon/`，地址只说 rva + build，别在 research 文档里再抄一遍地址。

## 不做什么

- 不把 idb、`.id0/.id1`、exe、任何 bin 放进仓库（`.gitignore` 兜底，但别依赖它）。
- 不手改 `builds/` 下的文件：`re_serve.py` 会覆盖；要修正结论去 `canon/` 或 research。
- 不在 `served.jsonl` 里补写/改写历史行：状态靠"有没有成功回执"判断，改行等于伪造。
- 不用地址当主键。地址只在 `builds/<sha12>/` 内有效；跨 build 的主键是签名。
- 不让 subagent 跑 Alt+F7 / 开 idb：那是人独占的动作（AGENTS.md §12）。

## 与既有流程的关系

- 这是 `flow.py` 的补充，不是替代：`exp` 记"实机实验"，这里记"静态分析产物"。两者都不许当另一方的证据。
- `re_kb.py` 刻意不和 `flow.py` 合并进同一个 argparse：`scripts/agentflow/` 是 `sys.path[0]`，
  多一个模块就多一个遮蔽标准库的机会（本目录曾有个 `re.py`，直接让 `flow.py` 起不来）。
- 升级 schema 时：新增字段可以（读方忽略未知字段），改字段语义必须同时改
  `re_kb.py` / `re_serve.py` / 本文件，并在 `served.jsonl` 里用新 `kind` 区分代际。
