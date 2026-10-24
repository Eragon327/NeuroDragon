# RenderDragon 帧结构与接入点

> **状态**：现行设计。吸收 2026-10-20/22 的 macOS 符号化 IDB 取证（Minecraft Education 1.26.32，
> imagebase `0x100000000`）与 2026-10-24 的 Windows 1.26.5101.0 运行时实测。
>
> **本文回答**：RenderDragon 的帧是怎么组织起来的、NeuroDragon 能钩在哪、那个位置上能拿到什么。
> **不回答**：NR/SR 的具体接法（[nr](nr.md) / [sr](sr.md)）、VV 迁移路线（[vv](vv.md)）。
> 原始证据与逐条推理在 [research/2026-10-22-renderdragon-passmap.md](../research/2026-10-22-renderdragon-passmap.md)，
> 归档的旧 `A` 同文。

## 0. 一句话

```
接入点 = 包住 dragon::framerenderer::modules::taau（不是"在帧图里找缝"）
```

`taau` 在语义上就是「3D 渲染完成之后、色调映射 / UI 之前」的**时间上采样**：它拿到
`InputFinalColor`（渲染分辨率）、`InputBufferMotionVectors`、`InputTAAHistory`、`UpscalingData`、
`UpscalingCameraData`。这正是 DLSS SR / NR 需要的输入集合，而且 **MV 是引擎自己产出的**。

一条补充：taau **不绑深度**（§3.2），所以 NR/DLSS 要的深度在 taau 这里拿不到，得另钩带
`BindFramebufferDepth` 的通道或复用 `SceneDepth` 资源。

## 1. 帧结构

```
GameRenderer::renderCurrentFrame              (Mac 0x1040c74c0)
  ├─ makeScreenContext / ViewRenderObject
  ├─ FrameBuilder::startFrame()               (ServiceLocator<mce::framebuilder::FrameBuilder>)
  ├─ FrameRenderer::frameImpl(...)             ← 整个帧图在这里构建（VV / RTX / Fancy 三选一）
  │     └─ dragon::framegraph::Declarator<RenderContext>
  │           ├─ declareResource<BackBufferColor / BackBufferDepth / TransientImage / ExternalImage>
  │           ├─ modules::* 与 bgfxbridge::draw*（阴影、场景、延迟着色、水、SSR、后处理、TAAU/FSR1）
  │           └─ UI（drawColorCorrectedUI / drawPersistentUI）
  ├─ FrameBuilder::endFrame(FrameBuilderContext&&)
  └─ UI 合成 stage（DefaultUIRendererCompositorStage / DefaultGameRendererCompositorStage）→ 呈现
```

三条帧渲染器路径（同一 `frameImpl` 语义）：

| 渲染器 | Mac 地址 | 说明 |
|---|---|---|
| `bgfxbridge::VibrantVisualsFrameRenderer::frameImpl` | `0x10b673570` | 延迟渲染 + 光追可选（Vibrant Visuals），**主目标** |
| `bgfxbridge::RTXMinecraftFrameRenderer::frameImpl` | `0x10b5db420` | RTX 路径 |
| `bgfxbridge::(anonymous)::_drawFrame` | `0x10b5cf180` | Fancy（非延迟）路径 |

`FrameBuilder` 是纯虚接口（SDK 头 `FrameBuilder.h`）：`startFrame/endFrame`、
`queuePreFrameTask(std::function<void()>)`、`custom(insertCallback)`、
`isUpscalingAvailable/setUpscaling/isUpscalingEnabled/getUpscalingFactor/getRenderResolution`、
`createExternalTexture/wrapExternalTexture/readBackTexture`、`updateFramePacing`。
全局入口 `renderDragonFrameBuilder()`（`0x10b6e8ac0`）。

**帧图是声明式的**：声明顺序 ≠ GPU 执行顺序，执行顺序由依赖决定。下面的通道表用于定位，不用于定序。

## 2. VV 通道清单（按 `frameImpl` 内声明顺序）

Mac 1.26.32 / imagebase `0x100000000`；Windows 侧按语义名重新定位（§7）。

| # | 调用点 | 被调函数 | 作用 |
|---|---|---|---|
| 1 | `0x10b6739aa` | `bgfxbridge::createDeferredRenderingCameras` | 主相机/视口（吃 `shared_ptr<UpscalingData>`） |
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
| 16 | `0x10b6af49a` | `modules::fidelityfx::FSR1Upscale` | 空间上采样（备选 SR 路径，位于调色之后） |
| 17 | `0x10b6afadf` | `bgfxbridge::drawColorCorrectedUI` | UI（色彩校正后） |
| 18 | `0x10b6afd44` | `gammaScreenColorPostProcess` | 屏幕 gamma |
| 19 | `0x10b6b58f4` | `modules::_drawGameface` | 前景 UI（Gameface/网页层） |
| 20 | `0x10b6b598c` | `drawPersistentUI` | 常驻 UI |

两条上采样路线：

- **TAAU 路线**：`drawDeferredShadingPass → taau → bloom/调色 → gamma → UI`
- **FSR1 路线**：`drawDeferredShadingPass → bloom/调色 → FSR1Upscale → gamma → UI`

PC 平台上 Fsr1 **不会被选中**（13 个平台的 `platform_config` 里只有 `bilinear` / `taau`），
所以 Windows 现实里只有 TAAU 路线；Bilinear 档根本不进 taau 分支（[research/2026-10-22-windows-probe.md](../research/2026-10-22-windows-probe.md) §6–7）。

## 3. `taau` 契约

### 3.1 签名（由 Itanium mangled name 解析）

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
    framegraph::dependency::BindFramebufferColor<RenderContext>&,   // ×4
    ...);
```

Windows 1.26.51 的 RTTI 里内嵌了同构签名（`.?AV<lambda_1>@?0???$custom@UTAACallable@?1??taau@…`），
模板参数同样为 `BindMaterialTexture ×3` —— **两平台都没有 depth 依赖**。

### 3.2 输入与输出

函数内引用的输入名（字符串常量）【确认】：

| 字符串 | Mac 地址 | 含义 |
|---|---|---|
| `InputFinalColor` | `0x10dd5252f` | 当前帧颜色（渲染分辨率） |
| `InputBufferMotionVectors` | `0x10dd5253f` | **运动矢量**（引擎产出，非外部注入） |
| `InputTAAHistory` | `0x10dd52558` | 上一帧 TAAU 结果（历史色） |
| `Upscaling` | `0x10dd4d4ba` | 材质/通道命名 |

四个 `BindFramebufferColor` 的语义（Mac 参数顺序，Windows 依赖对象偏移不同，见 [research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md) §2.3）：

| 参数 | 材质 uniform | 语义 |
|---|---|---|
| `[0]` | `InputTAAHistory` | 上一帧 TAAU 结果 |
| `[1]` | `InputFinalColor` | 本帧颜色 |
| `[2]` | （无名字） | **输出目标（显示分辨率）** |
| `[3]` | `InputBufferMotionVectors` | 运动矢量 |

**taau 不采样深度**：依赖表只有 4 个 `BindFramebufferColor` + 3 个 `BindMaterialTexture`
（颜色 / 历史 / MV），没有 `BindFramebufferDepth`，也不引用 `SceneDepth`。TAAU 的去遮挡与历史有效性
走 **MV + `SubPixelJitterAndValidHistory`**，不是深度。

### 3.3 `UpscalingData` / `UpscalingCameraData`

`UpscalingData`（SDK 头 `mc/external/render_dragon/rendering/UpscalingData.h`）：

```
UpscalingParameters mUpscalingParameters;  // Mode{Taau=0, Bilinear=1, Fsr1=2, Fsr1DebugBilinear=3},
                                           // factor, dynamicRes, MV filtering, clipping...
uint   mCurrentJitterIdx;                  // 抖动相位（每帧递增回绕）
float  mDynamicResolutionScale;            // 动态分辨率缩放
```

`UpscalingParameters` 里明确有 `mEnableMotionVectorFiltering`、`mUpscalingFactor`、
`mEnableDynamicResolution` ⇒ 引擎内部有抖动与 MV，渲染分辨率可被 factor 驱动。

`UpscalingCameraData` 在 LeviLamina 的 dump 里是**空定义**，字段只能从 taau 的取用反推：

| 偏移（推断） | 内容 | 依据 |
|---|---|---|
| `+0x00` / `+0x40` | `Current` / `Previous` ViewProjection（各 `float4x4`，哪个在前待确认） | taau 起 128 B 原样进 uniform 块 |
| `+0x80` | `SubPixelJitter`(xy) + `ValidHistory`(zw) 的 `vec4` | uniform 名 `SubPixelJitterAndValidHistory` |
| `+0x90` | 打包参数（8 B） | taau 里按 QWORD 取 |
| `+0xE0` | 1 字节标志位（疑似 valid-history / MV 有效） | taau 里按 BYTE 取 |

TAAU 材质（`TAAUpscaling`，FNV-1a 64）的完整 uniform 集包含 `ResolutionRatiosAndFPEpsilon`、
`Previous/CurrentViewProjectionMatrixUniform`、`RenderResolution`、`DisplayResolution`、
`RecipDisplayResolution`、`SubPixelJitterAndValidHistory`、`Current/PreviousWorldOrigin`、
`TAAUpscalingParameters` ⇒ cameraData 至少还含**当前/上一帧世界原点偏移**；**没有 MV scale**
（`mEnableMotionVectorFiltering` 在 `UpscalingData.mUpscalingParameters` 里）。

## 4. 接入点（P1–P6）

| 编号 | 钩住谁 | 能得到什么 | 适合 | 风险 |
|---|---|---|---|---|
| **P1** | `modules::taau` | 渲染分辨率颜色 + MV + 历史 + 抖动 + factor | **NR 前置 / DLSS SR** | 帧图依赖描述符 → 真实纹理需再解析 |
| **P2** | `fidelityfx::FSR1Upscale` | 调色后的低分辨率颜色 | 若走 FSR1 时作为 SR 落点 | 无 MV（空间上采样）；PC 上不会走 |
| **P3** | `drawColorCorrectedUI` / `drawPersistentUI` | UI 通道输入前的 hudless 图 | **DLSS-G 的 HUDLess/UI 分层** | UI 可能多来源（Gameface/常驻） |
| **P4** | `FrameBuilder::endFrame` 返回后 | 整帧已提交、UI 将画的时刻 | **帧生成的"帧末"落点** | 需确认与 UI stage 的先后 |
| **P5** | `DefaultUIRendererCompositorStage::preRender` | UI 合成前 | 原生 UI 合成 / 替换 | vtable 索引跨版本 |
| **P6** | `RendererContextI::flip()`（bgfx）或 DXGI `Present1` | 最终呈现 | **帧生成 / 输出** | 平台层，Mac 无参考 |

**已定的主路径**：`P1`（包住 taau：先原函数、后我们的 NR/SR，或完全替换）→ `P3/P5`（UI 分层）→
`P6`（present/FG）。理由：P1 天然满足 TRP 对「pre-UI present」的要求，且**不必**去解 bgfx 的命令批处理问题。
当前实际落点见 [nr](nr.md)（RT 路线走引擎 NGX `EvaluateFeature`，不经过 taau）与 [vv](vv.md)（VV 路线走 taau）。

## 5. 资源与呈现

### 5.1 描述符 → 真实资源：中间是 bgfx 句柄

```
framegraph::dependency::BindFramebufferColor / BindMaterialTexture   (声明期依赖)
   └─ 执行期解析 → 纹理句柄写入依赖对象                        (taau 直接取用)
        └─ 材质槽绑定 + bgfx 绘制
             └─ bgfx 后端纹理：Metal=MTLTexture / D3D12=ID3D12Resource
```

RenderDragon 自己就是 bgfx 的调用方，帧图节点里存的是 bgfx 侧的标识，**不是 `ID3D12Resource`**。
2026-10-24 的探针进一步证实：把依赖对象、`+0x8` 节点、`+0x10` 那一跳的 qword 逐个当候选，
用「vtable 必须精确等于真实 `ID3D12Resource` 的 vtable」过滤后调 `GetDesc`，四个依赖 × 三个层次
**一次都没命中**。

⇒ 给 bgfx 侧用（自定义 shader）句柄就够；给 D3D12 原生用（NR/DLSS NGX）必须自建
`handle → ID3D12Resource*` 桥——这是 VV 路线（阶段 2）的地基，**RT 路线不需要**：
引擎的 NGX 参数对象直接给出 `ID3D12Resource*`（[nr §3](nr.md)）。

### 5.2 present = bgfx `flip()`

- Mac 实测符号 `bgfx::mtl::RendererContextMtl::flip()`，内部含 `"Swapchain mismatches initialized resolution."`
  ⇒ swapchain 尺寸与呈现都归 bgfx 的 `RendererContextI::flip()` 管。
- Windows 26.51 exe 里有 bgfx D3D12 后端的 RTTI 类型名，且**找不到** `RendererContextD3D11` /
  `RendererContextVulkan` / `RendererContextMtl` ⇒ **Windows = bgfx D3D12 后端**，DXGI `Present/Present1`
  在 bgfx 内部。
- **P6 的正确落点 = `RendererContextI::flip()` 虚函数**（平台无关、每帧一次、在 UI 合成之后）；
  直接钩 DXGI 会连带截到别的窗口，且 Present1 还是 Present 取决于 bgfx 版本。
- 额外合成层（Game Bar / HDR 覆盖）属于 DWM/bgfx swapchain 之外，不改变 flip 位置，但会影响 FG 插入后
  是否需要自行做 HDR 重打包【待 Windows 确认】。

### 5.3 引擎用 Agility 的 `D3D12Core.dll`

VV 侦察发现：引擎提交的 command list，其 vtable 全落在 **`D3D12Core.dll`**，不是系统 `d3d12.dll`。
所以钩 `d3d12.dll` 导出的 `D3D12CreateDevice` 只能抓到 overlay 自己的临时设备，引擎的设备和
`CreateRenderTargetView` 一个都看不到。正确入口是拿引擎提交的某个 command list 调
`ID3D12CommandList::GetDevice`，再钩那个设备（细节与坑见 [vv](vv.md) §5）。

## 6. W1 六个结论（2026-10-20，Mac 1.26.32 取证 + Windows 交叉验证）

| # | 问题 | 结论 | 置信度 |
|---|---|---|---|
| 1 | taau 是否采样深度 | **不采样**（无 `BindFramebufferDepth`，不引用 `SceneDepth`；Windows 签名同构已验证） | 【确认】 |
| 2 | MV 的生产者 | **不是 MotionOnly pass**（那是 frame component）；推断是延迟几何/着色阶段的 color 附件 | 否定【确认】/ 肯定【推断】 |
| 3 | 描述符 → `ID3D12Resource` | 中间只有 **bgfx 句柄**；D3D12 资源需自建 handle→resource 桥 | 【确认】 |
| 4 | Windows 26.51 走 Taau 还是 Fsr1 | **PC 只有 Taau / Bilinear，没有 Fsr1**；tier1-3 Performance=bilinear、Quality=taau，tier4-5 全 taau | 【确认】无 Fsr1 / 当前档位运行时打印 |
| 5 | present 路径 | **bgfx `flip()`**；Windows 后端已确认 bgfx **D3D12** | 【确认】 |
| 6 | `UpscalingCameraData` 字段 | prev/cur VP + 亚像素抖动/valid history + 世界原点；精确布局待 Windows | 【部分确认】 |

**对接入的影响**：

| 需要 | P1（包住 taau）能否拿到 | 说明 |
|---|---|---|
| 渲染分辨率颜色 | ✅ | `InputFinalColor` |
| 运动矢量 | ✅ | `InputBufferMotionVectors`（来源是 G-buffer 附件） |
| 历史色 | ✅ | `InputTAAHistory` |
| 抖动相位 | ✅ | `SubPixelJitterAndValidHistory` + `UpscalingData::mCurrentJitterIdx` |
| 深度 | ❌ | taau 不绑深度 → 另钩 `drawDeferredShadingPass`，或复用 `SceneDepth` 资源 |
| `ID3D12Resource` | ❌ 直接 / 🔧 可桥接 | 只有 bgfx 句柄，需 handle→resource 表 |
| factor / 模式 | ✅ | `UpscalingData`（60 B POD）随 taau 参数进来 |

## 7. Windows 侧定位（无 PDB）

Windows 客户端**没有 PDB**，但 exe 保留了 MSVC 修饰的 RTTI 类型名（clang-cl 为 lambda/局部类生成的
`type_info` name），可直接当符号锚点；`taau` 的完整签名就内嵌在
`.?AV<lambda_1>@?0???$custom@UTAACallable@?1??taau@modules@framerenderer@dragon@@YAX…` 里。

两条可用的定位链：

1. **字符串 → 代码交叉引用 → `.pdata` 定界**：3 个材质输入名在 `.rdata` 唯一 → 扫 `.text` 的
   `lea r64,[rip+disp32]` → 用 `.pdata` 反查包含该指令的函数 → 校验同一函数内 ≥2 个输入名引用。
   这是探针里实际用的方法，**不依赖固定 RVA**，改版只会让函数边界校验失败而不是挂错地方。
2. **RTTI → COL → vtable**：由 type_info name 反查 `CompleteObjectLocator` → vtable 表项 →
   `__func<…>::operator()`（taau 的绘制回调，纹理已解析，最适合 P1）。

**版本约束**：所有 RVA 只对 `1.26.5101.0` 成立，换版本先用离线脚本重新定位。具体 RVA、签名、
每次运行的记录在 [research/2026-10-22-windows-probe.md](../research/2026-10-22-windows-probe.md)；
VV 下 taau / 回调的 Windows RVA 与参数基址在 [research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)。

## 8. 未决

| # | 未知 | 影响 | 下一步 |
|---|---|---|---|
| 1 | bgfx 句柄 → `ID3D12Resource` 的桥 | VV 路线 M6 的地基 | 钩 bgfx D3D12 纹理创建，或从 command list 反推设备后挂 `CreateRenderTargetView`（[vv](vv.md)） |
| 2 | VV 的深度源 | NR/DLSS 在 VV 下要深度 | 渲染目标清单里两个 844×475 深度面（格式 41/54），读准格式并与延迟着色对上 |
| 3 | 写 taau 输出目标是否被后续 pass 依赖 | 写错就是花屏/崩溃 | 先「TAAU 照跑再覆盖」验收（[vv](vv.md) M5） |
| 4 | FSR1 路线 | 仅当平台配置切到 Fsr1 才相关 | PC 上不做 |
| 5 | `RayTracingResources` 能否拿到 | RR 的唯一决定性问题 | 见 [roadmap §5](../roadmap.md) |

## 9. 参考

- 证据全文：[research/2026-10-22-renderdragon-passmap.md](../research/2026-10-22-renderdragon-passmap.md)（旧 `A`）
- Windows 定位链：[research/2026-10-22-windows-probe.md](../research/2026-10-22-windows-probe.md)（旧 `B`）
- VV 路径实测：[research/2026-10-24-vv-path.md](../research/2026-10-24-vv-path.md)（旧 `O`）
- 取证产物：`.ida/frameImpl_callees.json`、`.ida/passes_xrefs.json`、`.ida/taau.json`（导出说明见 `.ida/README.md`）
- 旧 `A` 里引用的 Mac IDB：`D:\Documents\DecompileData\version\minecraft-edu\minecraft-edu.i64`
