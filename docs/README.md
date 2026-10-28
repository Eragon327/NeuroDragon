# NeuroDragon 文档

本目录只放**现在有效**的文档。历史文档不删除，移到 `archive/` 冻结；旧结论不再作为行动依据，
但其中的实测数据（地址、日志、反汇编、崩溃现场）永远可以查。

```
docs/
├── README.md          # 唯一索引 + 文档规则（本文件）
├── roadmap.md         # 全局路线图，永远只有 1 份
├── specs/             # 当前有效设计，每个主题最多 1 份
│   ├── renderdragon.md
│   ├── ui.md
│   ├── nr.md
│   ├── sr.md
│   ├── trp.md
│   └── vv.md
├── decisions/         # ADR：稳定决策，少而精
│   ├── 0001-taau-insertion-point.md
│   ├── 0002-no-kernel-patching.md
│   ├── 0003-docs-layout.md
│   ├── 0004-code-conventions.md
│   └── 0005-vv-r3-write-arg10.md
├── research/          # 技术调研与实测笔记，文件名必须带日期
│   ├── 2026-10-22-renderdragon-passmap.md
│   ├── 2026-10-22-windows-probe.md
│   ├── 2026-10-23-levilamina-api-notes.md
│   ├── 2026-10-24-vv-path.md
│   ├── 2026-10-28-vv-structural-review.md
│   └── 2026-10-28-taau-decompiled.md
└── archive/
    └── 2026-10/       # 2026-10 改版前的旧字母文档（D–N；A–C 已迁入 research/），原文不动
```

## 索引（只列 active）

| 文档 | 主题 | 一句话 |
|---|---|---|
| [roadmap.md](roadmap.md) | 路线 | 四阶段顺序（NR/SR → VV 接管 TAAU → FG → RR）、每阶段状态与开工前必答的未知 |
| [specs/renderdragon.md](specs/renderdragon.md) | 宿主引擎 | 帧结构、VV 通道清单、`taau` 契约、接入点 P1–P6、资源与呈现、W1 结论 |
| [specs/vv.md](specs/vv.md) | VV 路径（阶段 2） | 触发条件、插入点、命令流、已答/未答的未知、M4–M7 与 stop/go；§34–§35 为 R3（插入点上移、写 `arg10`、实施阶段 A–D） |
| [specs/nr.md](specs/nr.md) | NR | NGX feature `0x12` 的契约、参数表、编码/合成、位置轴、尺寸与生命周期、剩余工作与探针结论 |
| [specs/sr.md](specs/sr.md) | SR | 引擎 DLSS 的三层分辨率、内部分辨率、模型预设与重建、RCAS 锐化、截图、读数与待办 |
| [specs/ui.md](specs/ui.md) | UI / 设置 | Present 钩子 + D3D11On12 面板、输入吞噬、draft 配置语义、监控与拆分计划 |
| [specs/trp.md](specs/trp.md) | 对照与选型 | TRP 0.5.3 的 SR/NR 功能全表、我们已有/缺什么、代价、不做与原因 |
| [decisions/0001-taau-insertion-point.md](decisions/0001-taau-insertion-point.md) | ADR | 接入点选「包住 `taau`」，不是「在帧图里找缝」 |
| [decisions/0002-no-kernel-patching.md](decisions/0002-no-kernel-patching.md) | ADR | 不做 `.trpk` / INT8 / MFG 这类对驱动内核的逆向 patch |
| [decisions/0003-docs-layout.md](decisions/0003-docs-layout.md) | ADR | 文档改版：specs / decisions / research / archive 与六条规则 |
| [decisions/0004-code-conventions.md](decisions/0004-code-conventions.md) | ADR | 翻译一律 `_tr`、D3D12 与引擎头的 include 顺序、`log::text` 的边界 |
| [decisions/0005-vv-r3-write-arg10.md](decisions/0005-vv-r3-write-arg10.md) | ADR | VV 的 SR 插入点上移一层：写 `arg10`、复用引擎显示变换；含 V1–V5 判据与止损 |
| [research/2026-10-22-renderdragon-passmap.md](research/2026-10-22-renderdragon-passmap.md) | 取证 | Mac 符号化 IDB 读出的帧结构、`taau` 契约证据、W1 全文 |
| [research/2026-10-22-windows-probe.md](research/2026-10-22-windows-probe.md) | 取证 | 无 PDB 的定位链（字符串 → xref → `.pdata`）、探针演进、NGX 参数读取的九次运行 |
| [research/2026-10-23-levilamina-api-notes.md](research/2026-10-23-levilamina-api-notes.md) | 参考 | 本项目用到的 LeviLamina API、签名、线程/日志/配置约定与坑 |
| [research/2026-10-24-vv-path.md](research/2026-10-24-vv-path.md) | 取证 | VV 路径侦察：RVA、参数基址、依赖语义、提交结构、渲染目标清单 |
| [research/2026-10-28-vv-structural-review.md](research/2026-10-28-vv-structural-review.md) | 复盘 | EXP-0025→0054 的结构性归因（身份/自指/层错位）、同指针 8800×、帧形 25/44/45 证据 |
| [research/2026-10-28-taau-decompiled.md](research/2026-10-28-taau-decompiled.md) | 取证 | TAAU 全拆解：纯 temporal resolve、MV 契约（`.zw`/Y 翻转/渲染归一化）、无深度/无 tonemap、96 字节常量缓冲；§9–§10 为 `arg10` 写入者与下游消费（EXP-0055 / EXP-0056） |

## 文档规则

1. **禁止在 `docs/` 根目录新建文件**；根目录只允许改 `README.md` 和 `roadmap.md`。
2. **新建文档前先 `grep` `docs/specs/` 是否已有同主题文档**：有 → 追加到已有文档，不新建；
   无 → 才允许在 `specs/` 下新建，且必须在本 README 登记。
3. **计划、方案、remaining、placement 这类内容一律进对应主题的 specs**，不再单独建 plan 文件。
4. **调研笔记放 `research/`**，文件名必须带日期（`YYYY-MM-DD-slug.md`）。
5. **实现记录、会议记录、临时分析** → `research/` 或 `archive/`，不进 `specs/`。
6. **不删除文档，只许移动**（`git mv`），归档目录按年月分（`archive/YYYY-MM/`）。

补充约定：

- `specs/` 每个主题**最多 1 份**。主题固定为 `renderdragon` / `ui` / `nr` / `sr` / `trp` / `vv`，
  新增主题要在这里登记，并说明为什么不能追加到已有主题。
- `decisions/` 只记**稳定的决策**（"为什么这样定"），编号 `0001` 起递增；计划与待办不写这里。
- `research/` 是**证据**：实测日志、地址、反汇编、探针记录。可以带日期、可以互相矛盾（保留现场），
  但不要把它当行动依据。
- 归档的旧文档**不再维护**，文内链接可能仍指向旧字母文件名；需要找原文时按 `archive/2026-10/` 下的
  文件名查（`A`–`O` 与主题的对应见各 spec 的「参考」一节）。

## 结论冲突时听谁的

1. **顺序与完成度** → [roadmap.md](roadmap.md)；
2. **某个主题现在怎么做** → 对应的 `specs/*.md`；
3. **为什么这样定** → `decisions/`；
4. **原始实测数据、被推翻的中间假设** → `research/` 与 `archive/`（只作证据）。

旧文档里的结论若与 `specs/` 冲突，一律以 `specs/` 为准；旧文档保留原文是为了保住证据，
不是让它继续发号施令。
