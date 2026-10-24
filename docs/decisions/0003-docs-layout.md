# 0003. 文档改版：specs / decisions / research / archive

- **状态**：已采纳（2026-10-24）
- **相关**：[docs/README.md](../README.md)

## 背景

2026-10-20 到 10-24 之间，`docs/` 里堆了 14 篇按字母编号的文档（`A`–`O`）。它们带来三个问题：

1. **一个主题散在多篇**：NR 有 `D`（事实）、`G`（客制化）、`L`（剩余功能）、`N`（位置轴）；
   SR 有 `H`（可行性）、`I`（规划）、`J`（实现）。「现在该看哪篇」要靠每篇文首的横幅自己判断，
   `docs/README.md` 的状态列越写越长。
2. **计划文档不断增殖**：每开一批工作就新建一个 `*-plan.md`；做完之后它既不是设计也不是记录，
   但还在被引用。
3. **结论冲突靠人记**：同一个问题在不同时间有不同结论（例如「NR 放在放大之后 = 不做」后来改判），
   旧文档不删、只在文首加横幅，读者必须逐篇对照。

## 决策

文档按**生命周期**分目录，一个主题的「现行设计」只有一份：

```
docs/
├── README.md      # 唯一索引，只列 active；不放正文
├── roadmap.md     # 全局路线图，永远只有 1 份
├── specs/         # 当前有效设计，每个主题最多 1 份
├── decisions/     # ADR：稳定决策，少而精
├── research/      # 技术调研与实测笔记，文件名必须带日期
└── archive/YYYY-MM/  # 不再使用的旧文档，原文不动
```

配套六条规则（写进 [docs/README.md](../README.md)）：

1. `docs/` 根目录禁止新建文件，只允许改 `README.md` 与 `roadmap.md`；
2. 新建文档前先 `grep docs/specs/`：有同主题 → 追加；无 → 才新建，且必须在 README 登记；
3. 计划 / 方案 / remaining / placement 一律进对应主题的 specs，不再单独建 plan 文件；
4. 调研笔记放 `research/`，文件名带日期；
5. 实现记录、会议记录、临时分析 → `research/` 或 `archive/`，不进 specs；
6. 不删除文档，只许移动（`git mv`）。

## 理由

- **「一个主题一份」让权威唯一**：读者不需要先读横幅判断状态；冲突在写新结论时就地改。
- **specs 负责「现在怎么做」**，research 负责「证据」，archive 负责「曾经怎么想」——
  三者混在一起时，实测数据会被当成行动依据，旧结论会被当成现状。
- **ADR 只记稳定决策**：决策有生命周期（采纳/被取代），比「plan 文档」更适合承载
  「为什么这样定」；它不会随着批次推进而过期。
- **research 带日期**是因为调研笔记允许互相矛盾（保留现场），日期是它唯一的排序依据。
- **不删除只移动**：旧文档里的地址、日志、崩溃现场仍有取证价值；但归档后不再维护，
  README 也不再列它们。

## 迁移记录（2026-10-24）

| 旧文档 | 去向 | 理由 |
|---|---|---|
| `A-renderdragon-passmap` | `research/2026-10-22-renderdragon-passmap.md` | 白盒取证（IDB 证据），现行设计蒸馏进 `specs/renderdragon.md` |
| `B-windows-probe` | `research/2026-10-22-windows-probe.md` | 定位方法与九次运行记录 |
| `C-levilamina-api-notes` | `research/2026-10-23-levilamina-api-notes.md` | API 速查/参考 |
| `O-vv-path` | `research/2026-10-24-vv-path.md` | VV 侦察实测记录 |
| `D` / `G` / `L` / `N` | `archive/2026-10/` | 合并进 `specs/nr.md` |
| `H` / `I` / `J` | `archive/2026-10/` | 合并进 `specs/sr.md` |
| `F` | `archive/2026-10/` | 合并进 `specs/ui.md` |
| `K` | `archive/2026-10/` | 合并进 `specs/trp.md` |
| `M` | `archive/2026-10/` | 由 `docs/roadmap.md` 接替 |

新增：`docs/README.md`（重写）、`docs/roadmap.md`、`specs/*`（6 篇）、`decisions/*`（4 篇）。
`docs/O-vv-path.md` 此前未提交，迁移后按 `research/` 规则带日期。

## 后果

**正面**

- 入口固定：先 README，再 roadmap，再按主题进 specs；证据去 research/archive 查。
- 旧文档不再需要维护，也不会被误当成现行结论。
- 新工作只需要在 spec 里追加一节，不再产生 plan 文档。

**负面 / 代价**

- 归档文档里的相对链接仍指向旧字母文件名（`K-trp-feature-gap.md` 等），跨文档跳转会 404；
  需要时按 `archive/2026-10/` 下的文件名查。
- 合并后的 specs 比单篇旧文档长；这是「一个主题一份」的必然代价。
- 每次新增主题/决策都要登记，多一步手工维护。
