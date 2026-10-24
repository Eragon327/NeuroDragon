# 文档索引

**规则：结论冲突时按「状态」列取权威。** 历史文档不删——里面的实测数据（地址、日志、反汇编、
崩溃现场）还能查；但不要再按它们的结论做事。

| 文档 | 状态 | 一句话 | 现在该看它的什么 |
|---|---|---|---|
| [A. RenderDragon 通道图](A-renderdragon-passmap.md) | 参考（数据有效） | Mac/Metal 全符号 IDB 里的帧结构与 taau 契约 | 地址、结构、UI 边界；「接入点 = taau」已被实际路线取代 |
| [B. Windows 探针](B-windows-probe.md) | 参考 | 无 PDB 的「字符串 → xref → `.pdata`」定位链 | 定位方法本身 |
| [C. LL API 速查](C-levilamina-api-notes.md) | 参考 | 本项目用到的 LeviLamina API + 坑 | 写代码时查签名/线程/日志 |
| [D. NR 接入规划](D-rt-dlss-nr-plan.md) | **NR 事实权威** | RT 路线、引擎 DLSS 参数表、NR 接入全过程的实测 | §2 参数表、§6.12–6.20 的实测与坑；被后续推翻的结论见文首横幅 |
| [F. UI 与设置系统](F-ui-plan.md) | 历史（已实现） | 面板为什么用 Present 钩子、输入吞噬、设置模型 | 设计理由 |
| [G. NR 客制化对齐](G-nr-customization-plan.md) | 历史（被 K 取代） | 按 TRP **0.4.5** 面板逐项判定「能不能做」 | 逐项推理过程；最新对照看 K |
| [H. SR 可行性与预设](H-sr-plan.md) | 历史（被 I/J 取代） | 三种「能」的划分、预设四层 | 可行性的判断框架；机制看 J |
| [I. SR 最终规划](I-sr-final-plan.md) | 部分有效 | 控制层 L1–L6、R_in/R_sr/R_out、VV 迁移 TAAU | 未落地的 L3–L6 与 VV 迁移；M1/M3 已由 J 落地 |
| [J. SR 实现](J-sr-implementation.md) | **SR 权威** | 内部分辨率、模型预设、重建的实现、证据与坑 | 要动 SR 先看这篇 |
| [K. TRP 0.5.3 对照](K-trp-feature-gap.md) | **路线权威** | TRP 的 SR/NR 全量功能对照 + 可做项 + 代价 + 排期 | 决定下一步做什么 |
| [L. NR 剩余功能](L-nr-remaining-plan.md) | **执行权威** | 批 1–3 的范围、验收判据、探针记录与工作约定（批 4「位置轴」见 N） | NR 剩余功能怎么做、做到哪了 |
| [M. 阶段路线](M-roadmap.md) | **阶段路线权威** | 四阶段顺序（NR/SR → VV 接管 TAAU → FG → RR）、每阶段状态与开工前必答的未知 | 先做什么、某阶段到什么程度算完 |
| [N. NR 位置轴](N-nr-placement-plan.md) | **位置轴路线** | NR 在放大器前/后两档（Before / After upscaling）的语义、P0–P4、判据与验收 | 要做「NR 放到放大之后」时先看这篇 |

## 读 TRP 源码

- 仓库在 `D:\Documents\theosrp`，**工作树停在 0.4.5**，`origin/main` 是 0.5.3。
- 读新版一律用 `git show origin/main:<path>`（例：`git show origin/main:CHANGELOG.md`），
  不要在工作树里找 0.5.x 的代码。
- 版本对不上的时候以 `CHANGELOG.md` 的版本号为准，别按文件名猜。

## 更新约定

- 新结论写进**当前权威文档**（SR → J，NR 事实 → D，功能对照与代价 → K，NR 执行 → L，
  **阶段顺序与完成度 → M**，**位置轴 → N**），不要只写进聊天。
- 被取代的文档只在文首加横幅指向新文档，不删内容。
- 新增文档后同时更新本索引与根 `README.md` 的文档表。
