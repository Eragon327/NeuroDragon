# 0004. 代码约定：翻译、头文件顺序、日志边界、配置版本

- **状态**：已采纳（2026-10-24）
- **相关**：[specs/ui.md](../specs/ui.md)、[specs/nr.md](../specs/nr.md)、
  [src/neurodragon/Macro.h](../../src/neurodragon/Macro.h)

## 背景

项目里有一批「不按这个来就一定踩坑」的写法：i18n 的运行时行为、D3D12 头与引擎头的冲突、
日志工具的分工、`ll::config` 的版本语义。它们之前散在旧 `docs/README.md` 的「代码约定」一节，
不属于任何单一主题的 spec；文档改版后需要有稳定的落点。

## 决策

### 1. 翻译一律 `"key"_tr(args…)`，没有例外

用 `ll::i18n_literals::operator""_tr`。注意它**不校验参数个数**：`fmt::format_string` 校验的是
键名本身，而键名里永远没有 `{}`；参数对不上只会在运行时由 `fmt::vformat` 抛异常。

- 缺键时**静默返回空串**（`ll::i18n::get` 缺键给空）。「键一定存在」只能靠离线检查保证。
- 审计键时要按**字符串字面量**扫，**不要按调用形式扫**：2026-10-24 只扫 `_tr(` 就把当时走
  `log::text` 的 `ui.toast.shotSaved` 误判成了死键。
- 新增/修改界面上出现的文案：`src/lang/{zh_CN,en_US}.json` 两份都要写、占位符一致，
  然后重跑 `python scripts/collect_glyphs.py`（`ui/Glyphs.h` 是生成物）。

### 2. 同时用 `<d3d12.h>` 和引擎头时的 include 顺序

```
<d3d12.h>  →  neurodragon/Macro.h  →  引擎头（ll/*、mc/*）
```

`mc/_HeaderOutputPredefine.h` 用 `#ifndef <类型名>` 去 guard 三个空壳 D3D12 结构体
（`D3D12_FEATURE_DATA_D3D12_OPTIONS` / `…_ARCHITECTURE` / `D3D12_RAYTRACING_GEOMETRY_DESC`）；
类型名永远不是宏，所以 guard 恒真、空壳恒被定义，在 `<d3d12.h>` 之后必报 `redefinition`。
`Macro.h` 用自引用宏把头文件里的 guard 判假，空壳不再生成，其余出现处展开成自身故无影响。

- 根因与替换写法都在 `Macro.h` 的注释里；旧 `D §4 坑 5` 记的是这条坑的历史。
- **仍建议能分开编译单元就分开**：D3D12 代码放独立 TU，只 include `<d3d12.h>`。

### 3. `log::text` / `log::writeKey` 只服务 `log::Stage`

`Log.h` 声明、`Utils.cpp` 实现，**不是**第二种翻译写法：它们存在是因为 `Stage` 在头文件里、
拉不了 i18n 头。新代码不要用它们；正常日志用 `getSelf().getLogger().info("key"_tr(...))`。

### 4. `ll::config` 的 `version` 语义

聚合 struct + 整数 `version`。**增删键就要 bump version**，不只在语义变化时 bump：
`loadConfig` 只在 version 不同时补缺失字段，不 bump 的话旧文件会按原样反序列化、在新键上失败。
本项目**不做值迁移**（旧的 `ui.openKey` 等直接丢弃，热键回到默认）。

### 5. 工作约定（与代码直接相关的部分）

- 不 commit：改动留在工作区，由用户自己提交。
- 新开关必须可回读验证（面板「诊断 → 本帧写入」），否则等于假开关。
- 渲染线程（我们的 detour）里不做游戏状态操作；要改设置/读世界数据抛给
  `ClientThreadExecutor`。
- 渲染线程里只做原子计数 + 日志；不分配、不阻塞、不调引擎。

## 理由

- `_tr` 的缺失参数是**运行时异常**，且缺键静默为空：这两条都不能靠类型系统兜底，所以需要
  「按字面量扫」的离线检查——这是踩过误判后写下的。
- `Macro.h` 的写法不是风格偏好：不按这个顺序 include 就直接编译失败，而失败信息（结构体重定义）
  指向引擎头，容易误判成项目自己的问题。
- `log::Stage` 的特殊性来自头文件依赖，不是设计喜好；放任它扩散会产生第二套翻译路径。
- `version` 的 bump 规则如果不写死，第一次加键就会让老配置文件静默丢字段。

## 后果

- 每个新文案 = 两条 JSON + 重跑字形脚本 + 一次 `--test=y` 构建自检（成本固定）。
- D3D12 相关代码天然倾向独立 TU；跨 TU 的接口只暴露不带头文件的 POD/句柄。
- 代码审计脚本必须扫字面量，不能扫调用形式。
