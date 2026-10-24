# RenderDragon 通道图与接入点（白盒取证）

> **迁移记录（2026-10-24）**：本文原为 `docs/A-renderdragon-passmap.md`，文档改版后迁入 `research/`，
> 正文未改。文内的 `A/B/C…` 链接指向旧字母文档（D–N 在 `docs/archive/2026-10/`，A–C 已迁到 `research/`）；
> **现行设计看 [specs/renderdragon.md](../specs/renderdragon.md)**，本文只作为证据。
>
> 目标：在不碰 Windows 二进制的前提下，先从 **Minecraft Education 1.26.32.0 (macOS/Metal)** 的符号化 IDB 里把
> RenderDragon 的帧结构、上采样模块契约、UI 边界读清楚，作为 Windows 26.51 接线的依据。
>
> 所有地址来自 IDB，imagebase `0x100000000`。标注 **【确认】/【推断】/【待确认】**。

---

## 0. 一句话结论

```
接入点 = 包住 dragon::framerenderer::modules::taau（不是"帧图里找缝"）
```

`taau` 在语义上就是「3D 渲染完成之后、色调映射/UI 之前」的**时间上采样**：它拿到的是
`InputFinalColor`（渲染分辨率）、`InputBufferMotionVectors`、`InputTAAHistory`、`UpscalingData`、`UpscalingCameraData`。
这正是 DLSS SR / NR 需要的输入集合，而且 **MV 是引擎自己产出的**。

---

## 1. 渲染架构总览 【确认，startFrame/endFrame 为虚调用、先后顺序为推断】

```
GameRenderer::renderCurrentFrame              (0x1040c74c0)
  ├─ makeScreenContext / ViewRenderObject
  ├─ FrameBuilder::startFrame()               (ServiceLocator<mce::framebuilder::FrameBuilder>)
  ├─ FrameRenderer::frameImpl(...)             ← 整个帧图在这里构建（VibrantVisuals / RTX / Fancy 三选一）
  │     └─ dragon::framegraph::Declarator<RenderContext>
  │           ├─ declareResource<BackBufferColor / BackBufferDepth / TransientImage / ExternalImage>
  │           ├─ modules::* 与 bgfxbridge::draw*（阴影、场景、延迟着色、水、SSR、后处理、TAAU/FSR1）
  │           └─ UI（drawColorCorrectedUI / drawPersistentUI）
  ├─ FrameBuilder::endFrame(FrameBuilderContext&&)
  └─ UI 合成 stage（DefaultUIRendererCompositorStage / DefaultGameRendererCompositorStage）→ 呈现
```

三条帧渲染器路径（同一 `frameImpl` 语义）：

| 渲染器 | 地址 | 尺寸 | 说明 |
|---|---|---|---|
| `bgfxbridge::VibrantVisualsFrameRenderer::frameImpl` | `0x10b673570` | `0x44190` | 延迟渲染 + 光追可选（Vibrant Visuals），**主目标** |
| `bgfxbridge::RTXMinecraftFrameRenderer::frameImpl` | `0x10b5db420` | `0x12892` | RTX 路径 |
| `bgfxbridge::(anonymous)::_drawFrame` | `0x10b5cf180` | `0xb91` | Fancy（非延迟）路径，内部调 `drawScene/drawUI/...` |

`FrameBuilder` 是纯虚接口（SDK 头 `FrameBuilder.h`）：`startFrame/endFrame`、`queuePreFrameTask(std::function<void()>)`、
`custom(insertCallback)`、`isUpscalingAvailable/setUpscaling/isUpscalingEnabled/getUpscalingFactor/getRenderResolution`、
`createExternalTexture/wrapExternalTexture/readBackTexture`、`updateFramePacing`。全局入口 `renderDragonFrameBuilder()`（`0x10b6e8ac0`）。

---

## 2. VibrantVisuals 通道清单（按 frameImpl 内**声明**顺序）【确认】

> 帧图是声明式的：**声明顺序 ≠ GPU 执行顺序**，执行顺序由依赖决定。下表用于定位，不用于定序。

| # | 调用点 | 被调函数 | 作用 |
|---|---|---|---|
| 1 | `0x10b6739aa` | `bgfxbridge::createDeferredRenderingCameras` | 建立主相机/视口（吃 `shared_ptr<UpscalingData>`） |
| 2 | `0x10b673b1b` | `bgfxbridge::_copy` | 资源拷贝 |
| 3 | `0x10b678789` | `drawShadowCascade` | 级联阴影 |
| 4 | `0x10b678ef1` | `setupCascadedShadowMaps` | 阴影图合成 |
| 5 | `0x10b67efc8` | `modules::volumeScattering` | 体积雾/散射 |
| 6 | `0x10b68ce35` | `drawDeferredShadingPass` | 延迟着色（GBuffer → 光照） |
| 7 | `0x10b68d635` | `drawDeferredShadingPass` | 第二条（分屏/第二视图） |
| 8 | `0x10b68e51b` | `modules::calculateScreenSpaceReflections` | SSR |
| 9 | `0x10b6a98eb` | `bgfxbridge::_captureHDRScreenshot` | HDR 截图 |
| 10 | **`0x10b6a9ac1`** | **`modules::taau`** | **时间上采样（SR 插入点）** |
| 11 | `0x10b6aa4c4` | `modules::averageLuminance` | 平均亮度（自动曝光） |
| 12 | `0x10b6aa751` | `modules::postfx::buildBlurPyramid` | 模糊金字塔 |
| 13 | `0x10b6aaaee` | `modules::postfx::buildBlurPyramid` | 第二遍 |
| 14 | `0x10b6aac21` | `modules::postfx::applyBloom` | Bloom |
| 15 | `0x10b6af1d3` | `modules::colorPostProcess` | 调色/色调映射 |
| 16 | `0x10b6af49a` | `modules::fidelityfx::FSR1Upscale` | **空间上采样（备选 SR 路径，位于调色之后）** |
| 17 | `0x10b6afadf` | `bgfxbridge::drawColorCorrectedUI` | UI（色彩校正后） |
| 18 | `0x10b6afd44` | `gammaScreenColorPostProcess` | 屏幕 gamma |
| 19 | `0x10b6b58f4` | `modules::_drawGameface` | 前景 UI（Gameface/网页层） |
| 20 | `0x10b6b598c` | `drawPersistentUI` | 常驻 UI |

**两条上采样路线**【推断，需 Windows 确认】：

- **TAAU 路线**：`drawDeferredShadingPass → taau → bloom/调色 → gamma → UI`
- **FSR1 路线**：`drawDeferredShadingPass → bloom/调色 → FSR1Upscale → gamma → UI`

> 也就是说：Windows 端如果 `UpscalingParameters.mode` 是 `Taau`，**MV/历史/抖动全都现成**；
> 如果是 `Fsr1`，则只有空间上采样，DLSS 需要的时间输入要自己补。

---

## 3. `modules::taau` 契约【确认】

签名（由 Itanium mangled name 解析）：

```cpp
void dragon::framerenderer::modules::taau(
    framegraph::Declarator<framerenderer::RenderContext>& declarator,
    Core::CpuRingBufferAllocator<uchar,2,false,Core::CheckedRingBuffer<2,false>>& allocator,
    dragon::materials::MaterialResourceManager& materialMgr,
    mce::CheckedResourceService<ResolvedMaterialResource>& materialService,
    cg::math::Rect<ushort> const& viewport,
    dragon::rendering::UpscalingData const& upscalingData,
    dragon::rendering::UpscalingCameraData const& cameraData,
    frameobject::components::MeshFilter const& meshFilter,
    framegraph::dependency::BindFramebufferColor<RenderContext>&,   // x4
    ...);
```

函数内引用的输入名（字符串常量）【确认】：

| 字符串 | 地址 | 含义 |
|---|---|---|
| `InputFinalColor` | `0x10dd5252f` | 当前帧颜色（渲染分辨率） |
| `InputBufferMotionVectors` | `0x10dd5253f` | **运动矢量**（引擎产出，非外部注入） |
| `InputTAAHistory` | `0x10dd52558` | 上一帧 TAAU 结果（历史色） |
| `Upscaling` | `0x10dd4d4ba` | 材质/通道命名 |

`UpscalingData`（SDK 头 `mc/external/render_dragon/rendering/UpscalingData.h`）字段：

```
UpscalingParameters mUpscalingParameters;  // Mode{Taau,Bilinear,Fsr1}, factor, dynamicRes, MV filtering...
uint   mCurrentJitterIdx;                  // 抖动相位
float  mDynamicResolutionScale;            // 动态分辨率缩放
```

`UpscalingParameters`（SDK 头）里明确有 `mEnableMotionVectorFiltering`、`mUpscalingFactor`、`mEnableDynamicResolution`。
**⇒ 引擎内部有抖动与 MV，且渲染分辨率可被 factor 驱动。**

> **已定案（W1-1）**：taau **不采样深度**——依赖表里只有 4 个 `BindFramebufferColor` + 3 个
> `BindMaterialTexture`（颜色 / 历史 / MV），没有 `BindFramebufferDepth`，也不引用 `SceneDepth`。
> 详见 §7.1。

---

## 4. UI 边界【确认 + 推断】

- 帧图内已有 UI 通道：`drawColorCorrectedUI`、`drawPersistentUI`、`_drawGameface`，都在 gamma/调色之后。
- 帧图之外还有 UI 合成 stage：
  - `DefaultUIRendererCompositorStage::{preRenderUpdate,postRender}`（`0x1040be020`, `0x1040be980`）
    → `postRender` 里以 **屏幕空间正交矩阵**（`2/w`, `2/h`, NDC −1..1）遍历 UI 网格并 `mce::Mesh::renderMesh`，
    走 `GuiData`，使用 `OffscreenCaptureDescription`。这是"把 UI 画到最终 target"的地方。
  - `DefaultGameRendererCompositorStage::{preRender,postRender}`（`0x1040bd7c0`, `0x1040bd7d0`）——函数体仅 6 字节，空实现。
- `GameRenderer::renderCurrentFrame`（`0x1040c74c0`）内可见 `ScreenRenderer::blit`、`_checkAndDrawInputUI`，
  说明 UI 绘制由 stage 序列在帧图之后进行。

⇒ **TRP 的 "pre-UI boundary" 在 Bedrock 里有三处对应物**（由粗到细）：
`frameImpl 结束` → `endFrame` → `DefaultUIRendererCompositorStage::preRender`。

---

## 5. 接入点方案（P1–P6）

| 编号 | 钩住谁 | 能得到什么 | 适合 | 风险 |
|---|---|---|---|---|
| **P1** | `modules::taau` | 渲染分辨率颜色 + MV + 历史 + 抖动 + factor | **NR 前置 / DLSS SR** | 帧图依赖描述符→真实纹理需再解析 |
| **P2** | `fidelityfx::FSR1Upscale` | 调色后的低分辨率颜色 | 若 Windows 走 FSR1，作为 SR 落点 | 无 MV（空间上采样） |
| P3 | `drawColorCorrectedUI` / `drawPersistentUI` | UI 通道输入前的 hudless 图 | **DLSS-G 的 HUDLess/UI 分层** | UI 可能多来源（Gameface/常驻） |
| P4 | `FrameBuilder::endFrame` 返回后 | 整帧已提交、UI 将画的时刻 | **帧生成的"帧末"落点** | 需确认与 UI stage 的先后 |
| P5 | `DefaultUIRendererCompositorStage::preRender` | UI 合成前 | 原生 UI 合成 / 替换 | vtable 索引跨版本 |
| P6 | `RendererContextI::flip()`（bgfx）或 DXGI `Present1` | 最终呈现 | **帧生成 / 输出** | Windows 平台层，Mac 无参考 |

**推荐主路径**：`P1`（包住 taau，先原函数后我们的 NR/DLSS，或完全替换）→ `P3/P5`（UI 分层）→ `P6`（present/FG）。

理由：P1 天然满足 TRP 对"pre-UI present"的要求，且**不必**去解 bgfx 的命令批处理问题。

---

## 6. Windows 26.51 迁移（**同为 clang 构建**）

| 维度 | 结论 |
|---|---|
| 名称修饰 | Mac = Itanium（`__ZN6dragon...`）；Windows = MSVC（`?taau@modules@framerenderer@dragon@@...`）。语义名一致，可用 demangled 名在 Windows PDB 里定位。 |
| 代码形态 | 同 clang 家族 ⇒ 内联、模板实例化、寄存器分配形态接近，**Mac 版反编译可直接当"注释版源码"读**。 |
| 结构布局 | POD（矩阵/矩形/`UpscalingData`）布局可参照；含 `std::string/std::function/std::variant` 的类型**不可**照搬（libc++ vs MSVC STL）。 |
| 版本漂移 | 1.26.32 → 1.26.51，19 个补丁；函数体可能变，但 `frameImpl → taau → compositor` 结构稳定。 |
| 符号解析 | LeviLamina 的 `symbolprovider` + `bedrock_runtime_data` 可按名解析；`ll::memory::Symbol("?...@taau@...")` 或签名扫描。 |

**建议的 Windows 侧映射表（W1 产出）**：

> 实测补充（2026-10-20）：Windows 客户端**没有 PDB**，但 exe 保留 MSVC 修饰的 RTTI 类型名，
> 足以锚定 `taau` 的签名与绘制回调地址 —— 方法见 **§7.8**。

| 语义名 | Windows 符号（待填） | 钩法 | 验证方式 |
|---|---|---|---|
| `dragon::framerenderer::modules::taau` | `?taau@modules@framerenderer@dragon@@…` | `LL_HOOK` / MinHook | 断点命中次数 = 帧数 |
| `…::fidelityfx::FSR1Upscale` | `?FSR1Upscale@fidelityfx@modules@…` | 同上 | 同上 |
| `mce::framebuilder::bgfxbridge::VibrantVisualsFrameRenderer::frameImpl` | `?frameImpl@VibrantVisualsFrameRenderer@bgfxbridge@framebuilder@mce@@…` | 同上（巨大函数，只做边界） | 帧开始/结束 |
| `DefaultUIRendererCompositorStage::preRender` | `?preRender@DefaultUIRendererCompositorStage@@…` | vtable/虚函数 | UI 帧计数 |
| `renderDragonFrameBuilder` | `?renderDragonFrameBuilder@@YAPEAVFrameBuilder@…` | 直接调用 | 返回非空 |

---

## 7. W1 结果（2026-10-20；Mac 1.26.32 IDB 取证，结论分级）

| # | 问题 | 结论 | 置信度 |
|---|---|---|---|
| 1 | taau 是否采样深度 | **不采样**。依赖只有 4×`BindFramebufferColor` + 3×`BindMaterialTexture`（颜色/历史/MV），无 `BindFramebufferDepth`，不引用 `SceneDepth` | 【确认】 |
| 2 | MV 的生产者 | **不是 MotionOnly pass**（`MotionOnly` 是 frame component 标记）。MV 目标无独立资源名/材质，最可能是**延迟几何/着色阶段的 color 附件** | 否定【确认】/ 肯定【推断】 |
| 3 | 描述符 → `ID3D12Resource` | 解析产物是 **bgfx 纹理句柄**，不是 `ID3D12Resource`；到 D3D12 资源必须经 bgfx d3d12 后端 | 【确认】前半 /【待确认】后端入口 |
| 4 | Windows 26.51 走 Taau 还是 Fsr1 | **PC 只有 Taau / Bilinear，没有 Fsr1**（13 个平台配置 + 素材实测）；走哪个看 tier/预设（tier1-3 Performance=bilinear，其余 taau） | 【确认】无 Fsr1 /【部分确认】当前档位 |
| 5 | present 路径 | **bgfx `flip()`**（Mac 实测 `bgfx::mtl::…`）；Windows = bgfx **D3D12** 后端，DXGI Present 在 bgfx 内部 | 【确认】 |
| 6 | `UpscalingCameraData` 字段 | prev/cur ViewProjection、亚像素抖动+valid history、世界原点偏移、标志位 | 【部分确认】 |

### 7.1 taau 不采样深度【确认】

证据（`taau` = `0x10b772140`，反编译见 `.ida/taau.json`；渲染回调 = `0x10b774160`，用
`mcp__ida__decompile` 可复现）：

1. **依赖表**：`visitEachDependency<Declarator&, BindFramebufferColor const, BindMaterialTexture×3, GroupedInPhase>`
   —— 1 个 color 依赖 + **恰好 3 个材质纹理**；参数表里 4 个 `BindFramebufferColor` 的用途是
   `a10→InputFinalColor`(读)、`a9→InputTAAHistory`(读)、`a12→InputBufferMotionVectors`(读)、`a11`=输出目标。
2. **没有任何 depth 依赖**：`BindFramebufferDepth<RenderContext>` 在引擎里是独立类型（`drawPersistentUI`、
   `extractAndDraw<Opaque>`、`drawDeferredShadingPass` 都用它），但 taau 的签名与函数体里都没有。
3. **不引用 `SceneDepth`**（`0x10dd4edef`，帧图里真实存在的深度资源名）。
4. **Windows 1.26.51 独立验证**：`Minecraft.Windows.exe` 里保留了 MSVC 修饰的 RTTI 类型名，其中
   `.?AV<lambda_1>@?0???$custom@UTAACallable@?1??taau@modules@framerenderer@dragon@@YAX…` 完整内嵌了
   taau 的签名：`Declarator&, CpuRingBufferAllocator&, MaterialResourceManager&,
   CheckedResourceService<ResolvedMaterialResource>&, Rect<u16> const&, UpscalingData const&,
   UpscalingCameraData const&, MeshFilter const&, BindFramebufferColor& ×4`，模板参数仍为
   `BindMaterialTexture ×3` —— **与 Mac 1.26.32 完全一致，Windows 侧同样没有 depth 依赖**。

⇒ TAAU 的去遮挡/历史有效性走的是 **MV + `SubPixelJitterAndValidHistory`**，不是深度。
**对 P1 的影响：NR/DLSS 所需的深度在 taau 这里拿不到**；要深度只能另钩带 `BindFramebufferDepth` 的通道
（如 `drawDeferredShadingPass` / `drawScene`）或复用 `SceneDepth` 资源。

### 7.2 MV 生产者：不是 MotionOnly pass【确认否定】，GBuffer 附件【推断】

- `MotionOnly` 的真实身份是 **`mce::framebuilder::gamecomponents::MotionOnly`**——一个 *frame component*
  （和 `ItemInHandObject`/`ParticleObject`/`NameTag` 同级），是"对象标记"，**不是 pass**。
- frameImpl 的 187 个被调里没有任何 `extractAndDraw<…MotionOnly…>`；引擎里也没有名字含
  `MotionVector`/`VelocityBuffer` 的 pass、材质或帧图资源（`Velocity` 只是天气材质的 uniform）。
- 因此 MV 是**几何绘制阶段一并写出的一张 color 附件**（`drawDeferredShadingPass` 一族带 4 个 color 绑定 +
  1 个 depth，G-buffer 资源名为 `ColorMetalnessSubsurface` / `EmissiveAmbientLinearRoughness` /
  `NormalsAndDepthLighting`），而不是额外的一遍 `MotionOnly` 渲染。

> **Windows 上 5 分钟定案**：P1 钩子里打印 3 个 `BindMaterialTexture` 对应依赖对象的 `+0x10` 句柄，同时在
> `bgfx::createTexture2D`/`setImage` 处按尺寸（= 渲染分辨率、RG16F 左右）反查创建者；创建时刻紧跟在
> `drawDeferredShadingPass` 之后即证实"GBuffer 附件"。

### 7.3 描述符 → 真实资源：中间是 bgfx 句柄【确认】，ID3D12Resource 不可直达

链路（Mac 侧实测）：

```
framegraph::dependency::BindFramebufferColor / BindMaterialTexture   (声明期依赖)
   └─ 执行期解析 → 纹理句柄写入依赖对象 +0x10            (taau 直接 *(aN+0x10) 取用)
        └─ 材质槽绑定 + bgfx 绘制                             ← 渲染回调里直接操作 bgfx::s_ctx
             └─ bgfx 后端纹理：Metal=MTLTexture / D3D12=ID3D12Resource
```

证据：taau 的渲染回调（`0x10b774160`）里出现 `bgfx::s_ctx`、`bgfx::View::setTransform`，并直接改写 view
状态块 —— **RenderDragon 自己就是 bgfx 的调用方**，纹理标识天然是 bgfx 句柄（`bgfx::TextureHandle` 系），
不是 `ID3D12Resource`。

⇒ **P1 能直接拿纹理吗？**
- 给 **bgfx 侧**用（自定义 shader / bgfx 资源）：✅ 句柄即可。
- 给 **D3D12 原生**用（NR/DLSS NGX 要 `ID3D12Resource*`）：❌ 需要自己做桥接表——钩 bgfx d3d12 后端的
  纹理创建（或 `ID3D12Device::CreateCommittedResource/CreatePlacedResource`）维护 `handle → ID3D12Resource*`。
  这是 Windows 侧 C 阶段（工程骨架）必须先落的一个小模块。

### 7.4 Windows 26.51 走 Taau 还是 Fsr1（机制已定，实际值待数据）

**机制【确认】**（SDK 头 + IDB 一致）：

```cpp
// mc/external/render_dragon/frame_renderer/modules/UpscalingParameters.h  (52 B)
enum class Mode : int { Taau = 0, Bilinear = 1, Fsr1 = 2, Fsr1DebugBilinear = 3 };
Mode  mUpscalingMode;  float mUpscalingFactor;  bool mEnableDynamicResolution; ...
bool  mEnableColorClipping;  bool mEnableMotionVectorFiltering;  glm::vec2 mVarianceClippingBiasMinMax;
```

- 选择来源：`upscaling_configuration.shared.json`（`0x10db3f12b`，由
  `SharedConfigurationLoader::loadSharedConfigurationSync` 载入），键为 `taau` / `upscaling_factor` /
  `upscaling_mode`；factor 取值为 `one_third|one_half|two_thirds|three_quarters|native`。
- 另有 **平台画质配置** `PlatformConfiguration::GraphicsQualityPresetLODs::bindType` 同样引用
  `taau` + `upscaling_mode` ⇒ **按平台/画质预设（而非硬编码平台判断）决定**。
- 代码默认值：`UpscalingConfiguration::Configs` 构造（`0x103496f20`）把 **8 组**预设都初始化成
  `Mode::Taau`、`factor=1.0`（= 原生分辨率不放大），`mVarianceClippingBiasMinMax={0.75,1.25}`。

**实际值：Fsr1 在 PC 上不会被选中【确认】；taau 还是 bilinear 取决于档位【部分确认】**

用本机 Windows 1.26.5101.0 客户端实测（当时在 `D:\Program Files\LeviLauncher\versions\1.26.51.01`；
2026-10-21 已整体搬到 `E:\LeviLauncher\versions\1.26.51.01`，内容不变）：

- `data/renderer/platform_config/pc/platform_configuration.pc.json`：`pc.tiers` 共 **5 档**，每档两个
  预设 `{name: Performance|Quality, lods: {upscaling_mode, ...}}`：

  | tier | Performance 档 | Quality 档 |
  |---|---|---|
  | 1–3 | `bilinear` | `taau` |
  | 4–5 | `taau` | `taau` |

- **13 个平台的 platform_config 里 `upscaling_mode` 只出现 `bilinear` / `taau`，一次 `fsr1` 都没有**；
  `data/renderer/materials/FSR1.material.bin` 仍随包发布（死资源）⇒ **Windows 26.51 不会走 Fsr1 路径**。
- 用户当前档：`Minecraft Bedrock/Users/Shared/games/com.mojang/minecraftpe/options.txt` 里有
  `gfx_upscaling:1`、`upscaling_mode:1`、`graphics_mode:2`、`deferred_viewdistance:12`（Vibrant Visuals /
  延迟渲染在跑）。
- ✅ **`upscaling_mode` 的 0/1 已实测钉死：`0=Taau`、`1=Bilinear`**（与 `UpscalingParameters::Mode`
  枚举同序）。证据：用户在游戏内把 Upscaling 切到 TAAU 后，活动档案
  `Minecraft Bedrock/Users/9587534086357524579/.../options.txt`（23:37 写入）里 `upscaling_mode` 由 `1` 变成 `0`；
  `Users/Shared` 那份是 10-11 的旧档，仍为 `1`。**这直接解释了 23:26/23:36 两次「taau 钩子 0 命中」——
  那两次实际跑的是 Bilinear，引擎压根不进 taau 分支。**
- ✅ **后果已实测**：`bilinear`（值 1）时 taau 一次都不会被调用（23:26/23:36 两次运行 = 0 命中），
  说明 bilinear 走的是另一条路径，**不进 taau 分支**；切到 `taau`（值 0）后才有资格命中 P1。
- `getUpscalingFactor()` 返回的是当前 `mUpscalingFactor`（1.0=原生，0.5=半分辨率），来自
  `UpscalingData.mUpscalingParameters`；档位里给的是 `target_resolution`（如 `720p`）而非 factor，
  具体换算在动态分辨率模块里【推断】。

### 7.5 present：bgfx `flip()`，不是引擎直连 DXGI【Mac 确认】

- 实测符号：`bgfx::mtl::RendererContextMtl::flip()`（`0x10b850b20`），内部含字符串
  `"Swapchain mismatches initialized resolution."` —— 也就是 **swapchain 尺寸/呈现都归 bgfx 的
  `RendererContextI::flip()` 管**。
- 由此，Windows 上是 bgfx 的 **d3d12 后端 flip**（DXGI `Present/Present1` 在 bgfx 内部），
  **不是** RenderDragon 自己调 Present1。
- Windows 后端已实测：exe 里有 bgfx D3D12 后端的 RTTI 类型名
  `. ?AV<lambda_1>@?0???$?RI@<lambda_7>@?0??submit@RendererContextD3D12@d3d12@bgfx@@UEAAX…`
  （≈ file offset 298 762 159），且**找不到** `RendererContextD3D11` / `RendererContextVulkan` /
  `RendererContextMtl` ⇐ **Windows 26.51 = bgfx D3D12 后端**（import 里仍有 `d3d11.dll` 属兜底，未使用）。
- **P6 的正确落点 = `RendererContextI::flip()` 虚函数**（平台无关、每帧一次、在 UI 合成之后）；
  直接钩 DXGI 会连带截到别的窗口，且 Present1 还是 Present 取决于 bgfx 版本。
- 额外合成层：Mac 上未见（bgfx 直接 presentDrawable）；Windows 的 Game Bar / HDR 覆盖属于
  DWM/bgfx swapchain 之外，**不会**改变 flip 的位置，但会影响我们插入 FG 后是否需要自行做
  HDR 重打包【待 Windows 确认】。

### 7.6 `UpscalingCameraData` 字段【部分确认】

SDK 头里该类是**空定义**（LeviLamina 的 dump 没有成员），只能从 taau 的取用反推：

| 偏移（推断） | 内容 | 依据 |
|---|---|---|
| `+0x00` / `+0x40` | `Current` / `Previous` ViewProjection（各 `float4x4`；**哪个在前待确认**——回调里 Previous 取自 TAAContext+584、Current 取自 +520，按此推 +0x40 更像 Previous） | taau 起 128 B 原样进 uniform 块 |
| `+0x80` | `SubPixelJitter`(xy) + `ValidHistory`(zw) 的 `vec4` | uniform 名 `SubPixelJitterAndValidHistory`（`0x10dd52606`） |
| `+0x90` | 打包参数（8 B，原样进 uniform 块） | taau 里 `*((QWORD*)a7+18)` |
| `+0xE0` | 1 字节标志位（疑似 valid-history / MV 有效） | taau 里 `*((BYTE*)a7+224)` |

TAAU 材质（名字 `TAAUpscaling`，FNV-1a 64）**完整 uniform 集**（= cameraData 的语义字段）：

`ResolutionRatiosAndFPEpsilon` / `PreviousViewProjectionMatrixUniform` / `CurrentViewProjectionMatrixUniform` /
`RenderResolution` / `DisplayResolution` / `RecipDisplayResolution` / `SubPixelJitterAndValidHistory` /
`CurrentWorldOrigin` / `PreviousWorldOrigin` / `TAAUpscalingParameters`

⇒ cameraData 至少还含 **当前/上一帧世界原点偏移**（大坐标重定位用，未在上面偏移表里定位到，标【待确认】）。
总大小 ≥ `0xE1`；**没有 MV scale**（MV 缩放不在 cameraData 里，`mEnableMotionVectorFiltering` 在
`UpscalingData.mUpscalingParameters` 中）。

### 7.7 P1 可用性结论（给阶段 D/E 的输入）

| 需要 | P1（包住 taau）能否拿到 | 说明 |
|---|---|---|
| 渲染分辨率颜色 | ✅ | `InputFinalColor` |
| 运动矢量 | ✅ | `InputBufferMotionVectors`（但来源是 G-buffer 附件，见 7.2） |
| 历史色 | ✅ | `InputTAAHistory` |
| 抖动相位 | ✅ | `SubPixelJitterAndValidHistory` + `UpscalingData::mCurrentJitterIdx` |
| 深度 | ❌ | taau 不绑深度（7.1）→ NR 要么改钩 `drawDeferredShadingPass`，要么自建深度拷贝 |
| `ID3D12Resource` | ❌ 直接 / 🔧 可桥接 | 只有 bgfx 句柄（7.3），需 handle→resource 表 |
| factor / 模式 | ✅ | `UpscalingData`（60 B POD）随 taau 参数进来 |

### 7.8 无 PDB 时的符号找回（B 阶段的可用锚点）

本机 Windows 客户端**没有 PDB**，但 exe 里保留了 **MSVC 修饰的 RTTI 类型名**（clang-cl 为 lambda/局部类
生成的 type_info name），可直接当"符号锚点"：

1. 搜字符串 `.?AV<lambda_1>@?0???$custom@UTAACallable@?1??taau@modules@framerenderer@dragon@@YAX…`
   （file offset ≈ `298242268`）—— 里面**完整内嵌了 taau 的签名**（见 §7.1 第 4 条）；
2. 该字符串是 MSVC `TypeDescriptor::name` 字段（descriptor 本体在它前面 16 字节），在 `.rdata` 里反查指向
   descriptor 的 `CompleteObjectLocator` → 再找 vtable 中指向该 COL 的表项 → 得到
   `__func<…>::operator()`，它就是 **taau 的绘制回调**（已解析好纹理、设置 uniform 的那一层，最适合 P1）；
3. `ConcretePass<…taau…>` / `TAAContext` 的 type_info 可同样锚定 pass 对象与 vtable；
4. taau 本体（frameImpl 内的调用点）用"回调地址 → 反查唯一调用者"定位。

> ⚠️ 这些是 **type_info 名字，不是导出符号**：`ll::memory::Symbol()` / `bedrock_runtime_data` 都解析不到
> （LL 的符号数据只覆盖服务端 bedrock_runtime）。要在客户端用，只能走上面的 vtable 链，或运行时扫一次
> 字符串 + 计算交叉引用（~300 MB 冷启动扫描可接受）。

---

## 8. 证据文件

- `.ida/frameImpl_callees.json` — frameImpl 的 187 个直接被调
- `.ida/passes_xrefs.json` — 各通道在 frameImpl 内的调用点
- `.ida/taau.json` — taau 反编译 + 引用表（含输入名）
- `.ida/README.md` — 导出说明

**W1 用到的非 IDB 证据（本机源码副本）**：

- `E:\LeviLauncher\versions\1.26.51.01\Minecraft.Windows.exe`
  （原在 `D:\Program Files\LeviLauncher\…`，2026-10-21 搬盘后路径如上；311 MB，`MicrosoftGame.Config` 里 `Version="1.26.5101.0"`，**无 PDB**；RTTI 名字里带 taau 签名与 bgfx d3d12 类名）
- `…\data\renderer\platform_config\pc\platform_configuration.pc.json` —— PC 五档 × {Performance, Quality} 的
  `upscaling_mode`（只有 bilinear/taau）
- `…\Minecraft Bedrock\Users\Shared\games\com.mojang\minecraftpe\options.txt` —— 用户当前
  `gfx_upscaling:1 / upscaling_mode:1 / graphics_mode:2 / deferred_viewdistance:12`

- `D:\Documents\LeviLamina\src-client\mc\external\render_dragon\frame_renderer\modules\UpscalingParameters.h`
  —— `Mode{Taau,Bilinear,Fsr1,Fsr1DebugBilinear}` 与 52 B 字段布局（§7.4）
- `D:\Documents\LeviLamina\src\mc\external\render_dragon\rendering\UpscalingData.h`
  —— `mUpscalingParameters(52) + mCurrentJitterIdx + mDynamicResolutionScale`
- `…\rendering\UpscalingCameraData.h` —— **空定义**（LL 的 header dump 无成员，§7.6 的偏移只能反推）
- `…\frame_graph\BindFramebufferColor.h / BindMaterialTexture.h / BindFramebufferDepth.h` —— 依赖类型确认

> 复现方式：`mcp__ida__*`，`database` = `idb_list` 的 session id；大函数用 `callees`/`xrefs_to` 定位，不要整函数 `decompile`。
