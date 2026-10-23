# J. SR 内部分辨率：实现、证据与坑（2026-10-24）

> 本文是 `docs/H-sr-plan.md`、`docs/I-sr-final-plan.md` 的落地记录：结论都有一局游戏 + 日志/反汇编证据。
> 与旧文冲突时以本文为准（旧文的"任意百分比做不了"已被 FrameBuilder 因子表推翻）。

## 1. 最终行为

「超分」页 →「内部分辨率」：跟随引擎 / 33 / 50 / 58 / 66 / 100。

每个比例 = DLSS 的原生质量槽，同时驱动两边：

- **DLSS 输入尺寸**：引擎自己的 optimal-settings 答案（不覆盖 `OutWidth/OutHeight`）；
- **场景渲染矩形**：FrameBuilder 的因子表。

「DLSS 模型（预设）」：驱动默认 / E / F / J / K / L / M，写六个 `DLSS.Hint.Render.Preset.*`（保留
`0x80000000`）。应用时若预设变化，强制一次 feature 重建（ctx 换槽 + 尺寸钉住），不需要换分辨率或重进世界。

**75% 不提供**：DLSS 没有原生槽，借槽 + 覆盖 Out 会被槽位动态范围夹住（见 §4）。
**锐化不提供**：运行时已废弃 `Sharpness`（见 §5）。

## 2. 两个尺寸来源（缺一不可）

| 侧 | 谁决定 | 我们改哪里 |
|---|---|---|
| DLSS 输入 | 引擎 optimal-settings 回调（q 槽） | ctx 的 q（`+0xe806dc`），shim 每帧复制到渲染器 |
| 场景渲染矩形 | FrameBuilder 因子表 | `getUpscalingFactor()` 的 `table[fb+0x57c]` |

`FrameBuilder::getUpscalingFactor()` = **虚表槽 83**（我们自己的调用编译成 `call [rax+0x298]`；
按头文件顺序推的 62 是错的）。RT 下按 `[fb+0x57c]` 查表：

```
[0.5, 0.58, 0.667, 0.3334, 0.75]   // RVA 0xE9776E0（1.26.51.01）
index > 4 或非 RT → 常量 1.0
```

patch 表项（`.rdata` 用 `VirtualProtect` 包一次）→ 场景按该比例渲染。实测 100%：
`[desc] Depth` 从 `1485x836` 变为 `2560x1440`，画面正常（2026-10-23 23:07）。
「跟随引擎」时恢复原表值。

## 3. 两个写入者：ctx 才是权威

- `srDriveFrameContext`（每帧 shim，`ctx+0xe806dc`）是真正到达帧构建器的路径；
- `srDriveEngineRequest`（evaluate，渲染器 `+0x3c56428`）每帧会被 shim 用 ctx 覆盖。

所以任何"强制重建"的 nudge **必须写 ctx**。2026-10-24 实测：先写渲染器 → 66↔75 无效；改 ctx 后生效。

## 4. 槽位与动态范围（为什么只有 33/50/58/66/100）

| 槽 | 比例 | 动态范围（2560×1440 下） | 备注 |
|---|---|---|---|
| 3 | 33.3% | 853×480 .. 853×480 | 固定 |
| 0 | 50% | ~1280×720 .. 2560×1440 | |
| 1 | 58% | 1280×720 .. 2560×1440 | Balanced |
| 2 | 66.7% | 1280×720 .. 2560×1440 | Quality |
| 5 | 100% | 2534×1426 .. 2560×1440 | DLAA |

75% 没有原生槽：借 q=5 会被夹到 2534（变 100%），借 q=3 会被夹到 853；只有 q=1/2 的范围能装 1920，
但 66 已占 q=2、q=1 是 58。行为不稳定，2026-10-23 用户决定不提供。

## 5. 不会变成功能的东西

- **Sharpness**：`nvsdk_ngx_defs.h` 里 `DoSharpening` 已标 `SR_DEPRECATED_SHARPENING`
  （"Sharpness is not supported"）。实测写入 0/0.5/1.0 后回调读数仍是 0.35、画面无变化。
  要锐化只能做后处理 pass（CAS/RCAS）。
- **75%**：见 §4。
- **config 热读**：运行期间 config 只写不读（`config::reload()` 已删除，install 里的重读也删掉）；
  文件改动需重启。

## 6. 模型预设与重建

- `srApplyCreateOverrides` 在 CreateFeature 之前写六个 `DLSS.Hint.Render.Preset.*`（保留 `0x80000000`）；
  **0 也写**，否则参数对象复用会让"驱动默认"保留上一次的 K/L/M。
- 预设只在 CreateFeature 读，所以需要一次 feature 重建：
  - `srRequestFeatureRebuild()` 记下当前渲染尺寸；
  - phase 1 用另一个质量槽（写 ctx），phase 2 回到映射槽；
  - 重建期间 sizing 回调把 `OutWidth/OutHeight` 与动态范围钉在记录的尺寸，
    否则临时槽的动态范围会夹住它（q=3→853，q=5→2534）；
  - 「跟随引擎」也支持：基准 q 用 ctx 里的值。
- 只读区会短暂看到临时槽（已加 ~90 帧读数保持，仍偶发）；计划直接删掉只读区。

## 7. CreateFeature 的调用路径（为直接调用留档）

栈（2026-10-24）：

```
exe+dfc7102   ← sizing 0xdfc6c00 + 0x502
exe+dfb606a   ← 0xdfb5ff0 + 0x7a
exe+dfc545b   ← shim 0xdfc53c0 + 0x9b
exe+b725025 … 游戏侧
```

sizing 入口先比较 active/requested mode、factor、feature handle、display size；只有状态变化或 handle 为空
才会走到 +0x502 的 CreateFeature（`0x14e412d30`）。因此直接调用 sizing 而不改状态不会重建——nudge 是必要的。

## 8. 当前 UI 与待办

- 「超分」页：内部分辨率、DLSS 模型、引擎配置只读区（待删）、尺寸回调统计。
- 底部：保存为默认 / 应用 / 放弃（没有重载按钮）。
- 待办：删只读区；后处理锐化 pass；组合预设；NR 侧 T1.x（见 `docs/G`）。
