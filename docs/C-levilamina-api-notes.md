# C. LeviLamina API 速查（本项目实际用到的部分）

> 权威文档在 `D:\Documents\LeviLamina\docs\main\contents\api_reference`（随 LL 版本走，别在这里抄全）。
> 本文只沉淀：**本项目用到的 API + 签名 + 踩坑**，方便写代码时不用再翻源码。
> 目标 LL：`levilamina 26.51.*`，`target_type = client`。
>
> ⚠️ **变体差异**：那份 api_reference 大量以**服务端**为例（`mc/server/...`、`ServerInstance`）；
> 本项目是 **client** 变体，以 `include/ll/api` 的实际头文件为准，已知差异见 §3（客户端是
> `ClientThreadExecutor`，不是 `ServerThreadExecutor`）。

## 1. 内存与钩子 —— `ll/api/memory`

| API | 签名（摘自 `memory.zh.md`） | 备注 |
|---|---|---|
| `hook` | `int hook(void* target, void* detour, void** originalFunc, HookPriority = Normal, bool suspendThreads = true)` | 返回 **0 = 成功**；target 是运行时算出来的地址时**只能用这个函数**（宏要编译期函数指针） |
| `unhook` | `bool unhook(void* target, void* detour, bool suspendThreads = true)` | 与 hook 成对，`disable()` 里调 |
| `HookPriority` | `Highest=0, High=100, Normal=200, Low=300, Lowest=400` | 数字越小越早执行 |
| 钩子宏 | `LL_STATIC_HOOK` / `LL_INSTANCE_HOOK` / `LL_AUTO_*` | 需要 `&Fn` / `&Cls::method`，**我们的 taau 用不上**（地址运行时才有） |
| `modify` | `void modify(void* ptr, size_t len, brstd::function_ref<void()>)` | 临时改内存保护后执行回调 |
| `addressCall` | `template<class R, class...Args> R addressCall(void const* address, Args&&...)` | 按地址直接调用 |
| `virtualCall` | `template<class R, class...Args> R virtualCall(void const* self, ptrdiff_t vIndex, Args&&...)` | 调虚函数（P1 阶段可能用） |
| `dAccess` | `template<class T> T& dAccess(void* ptr, ptrdiff_t off)` | 按偏移取成员 |

本项目的用法（`src/neurodragon/probe/RenderProbe.cpp`）：

```cpp
void* originalRaw = nullptr;
if (ll::memory::hook(target, reinterpret_cast<void*>(&detour), &originalRaw,
                     ll::memory::HookPriority::Normal) != 0) { /* 失败 */ }
gOriginal = reinterpret_cast<TaauFn>(originalRaw);   // 先落 void* 再 cast，避免 void** 强转的 UB
```

- `suspendThreads` 默认 `true`：我们在 `enable()`（渲染线程还没跑起来）里挂钩，安全。
- detour 跑在**渲染线程**：只做原子计数 + 日志，不碰游戏状态。

## 2. 签名与符号 —— `ll/api/memory/Signature.h` / `Symbol.h`

```cpp
ll::memory::Signature::parse("55 41 57 41 56 ? ? 48 81 EC")   // ? = 通配
    .view().resolve();          // 扫整张镜像
    .view().resolve(range);     // 或用 ll::utils::sys_utils::getImageRange() 限定范围
```

- `resolve()` 默认范围是**整个模块镜像**；本客户端 `.text` 就有 **230 MB**，扫描成本高 ⇒ 我们自己扫（`memchr` 找 `0x8D` + 手工校验 `lea r64,[rip+disp32]`）。
- **`memory::Symbol("?...@@...")` 在客户端不可用**：它依赖 `bedrock_runtime_data`（LL 的符号数据只覆盖服务端 bedrock_runtime），而 taau 是客户端 RenderDragon 的自由函数、无导出符号 ⇒ 只能靠「字符串 → 代码交叉引用 → `.pdata` 定界」定位（见 `docs/B-windows-probe.md`）。

## 3. 线程 —— `ll/api/thread`

| API | 用途 |
|---|---|
| `ClientThreadExecutor::getDefault().execute(fn)` | 把任务丢到**客户端主线程**（在那里访问游戏状态才安全）。客户端包同时带了 `ServerThreadExecutor.h`，但那是服务端语义，客户端 mod 用 `ClientThreadExecutor` |
| `ThreadPoolExecutor::getDefault().execute(fn)` / `executeAfter(fn, dur)` | 后台线程 / 延迟执行（返回可 `cancel()` 的句柄） |
| `GlobalThreadPauser` | 暂停所有游戏线程（改内存/hot patch 时用） |

约定：**渲染线程（我们的 detour）里不做任何游戏状态操作**；要改设置/读世界数据 → 抛给 `ClientThreadExecutor`。

## 4. 日志与模组 —— `ll/api/mod` / `ll/api/io`

```cpp
auto& log = ll::mod::NativeMod::current()->getLogger();   // 等价于本项目 getSelf().getLogger()
log.info("模组加载中");            // fmt::format_string<Args...>，编译期检查格式串
log.warn("内存不足: {} MB", 128);
log.debug(...); log.error(...);
log.setLevel(ll::io::LogLevel::Warn);
```

- 日志落在 `<version>/logs/latest.log`（本机即 `E:\LeviLauncher\versions\1.26.51.01\logs\latest.log`），行首形如
  `[2026-10-20 23:18:15.123 IFO][LeviLamina] …`（模组行用你自己的名字做 tag）。
- 格式串是**编译期检查**的：参数个数/类型不匹配直接编不过（这反而是个好事）。
- 目录：`getConfigDir()` / `getDataDir()` / `getWorldDataDir()` —— 后续存探针开关、NR 模型/缓存都放这里。

## 5. 配置 —— `ll/api/Config.h`

```cpp
struct MyConfig { int version = 1; bool probeEnabled = true; };   // 必须聚合 + 有整数 version
ll::config::loadConfig(cfg, getConfigDir() / "config.json");       // 不存在则写默认
ll::config::saveConfig(cfg, getConfigDir() / "config.json");
```

后续 ImGui 设置面板的持久化直接走这套（JSON）。

## 6. 本项目待用（先记在这）

- **P1 取纹理**：framegraph 依赖对象可用 `dAccess` / `virtualCall` 读，但最终纹理是 **bgfx 句柄**（不是 `ID3D12Resource`，见 `A-renderdragon-passmap.md` §7.3）。
- **UI 分层 / 事件**：`ll/api/event`（本文件未展开，用到再补）。
- **ImGui**：LL 不内置；客户端 mod 一般自己带一个（CoralFans 等），具体做法待定。
