# 0001. 接入点选「包住 `taau`」

- **状态**：已采纳（2026-10-20）
- **相关**：[specs/renderdragon.md](../specs/renderdragon.md)、
  [research/2026-10-22-renderdragon-passmap.md](../research/2026-10-22-renderdragon-passmap.md)

## 背景

要在 Minecraft Bedrock（RenderDragon）里插入 NR / DLSS SR，首先要决定「钩在哪」。
两条候选路线：

1. 在帧图（framegraph）里找一条「缝」——在某两个 pass 之间插自己的 GPU 工作；
2. 钩一个语义上已经带齐输入的函数，把我们的工作包在它前后。

Bedrock 的帧图是声明式的（声明顺序 ≠ 执行顺序），且真实纹理在帧图节点里只是 bgfx 句柄，
不是 `ID3D12Resource`（`specs/renderdragon.md` §5.1）——路线 1 要先解决「在哪、拿到什么」两个问题。

## 决策

**接入点 = 包住 `dragon::framerenderer::modules::taau`**（P1），不是在帧图里找缝。
NR/SR 的输入从 taau 的参数拿，输出按两条路线落地：

- **RT 路线**：不经过 taau，直接在引擎 NGX 的 `EvaluateFeature` 调用链上换 `Color` 指针
  （引擎 DLSS 负责放大）；
- **VV 路线（阶段 2）**：包住 taau，把 TAAU 换成我们的 feature（[specs/vv.md](../specs/vv.md)）。

## 理由（证据）

1. **语义正好**：taau 就是「3D 之后、色调映射/UI 之前」的时间上采样，它拿
   `InputFinalColor`（渲染分辨率）、`InputBufferMotionVectors`、`InputTAAHistory`、`UpscalingData`、
   `UpscalingCameraData`——DLSS SR / NR 需要的输入集合，而且 MV 与抖动是引擎自己产出的。
2. **每帧一次且稳定**：Windows 实测声明 1 次/帧、draw 1 次/帧（Bilinear 档根本不进 taau 分支，
   这也解释了早期「taau 0 命中」）。
3. **不需要先解 bgfx 的命令批处理**：这是路线 1 的主要成本；P1 只需要函数级 hook。
4. **天然满足 TRP 的「pre-UI present」**：taau 在 UI 合成与调色之前，UI 不会被 NR/DLSS 处理。
5. **不采样深度**（W1-1）：所以 NR 要的深度得另找——这是接受的代价，不是意外。

## 后果

**正面**

- 插入点、输入集合、UI 边界一次性确定；后续 SR/NR 的 RT 落地都建立在这个判断上。
- VV 路线只需要再解决「bgfx 桥 + 深度源」两件事，不需要重新找插入点。

**负面 / 代价**

- **深度拿不到**：taau 不绑 `BindFramebufferDepth`，NR 要深度必须另钩 `drawDeferredShadingPass`
  或复用 `SceneDepth` 资源。这是 VV 路线的一个未决项（未知 #3）。
- **纹理是 bgfx 句柄**：给 D3D12 用要自建 handle→resource 桥（VV 路线的地基）。
- **版本漂移**：函数签名/序言随版本变，必须按字符串/`.pdata`/RTTI 重新定位，不能硬编码 RVA。

## 备选（未采纳）

- **P2 `FSR1Upscale`**：PC 上 `Fsr1` 不会被选中（13 个平台的配置只有 `bilinear`/`taau`），且没有 MV。
- **P3/P5（UI 通道）**：留给 FG 的 UI 分层，不作为 NR/SR 的输入点。
- **P6（`flip`）**：最终呈现，留给帧生成。
