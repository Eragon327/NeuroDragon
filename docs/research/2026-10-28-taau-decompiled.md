# TAAU 拆解：CPU 提交与像素级算法（Windows 26.51）

## 0. 范围与证据

本文回答的是 `dragon::framerenderer::modules::taau` **到底怎样生成一帧上采样结果**，不是只列输入输出。
分析对象与证据固定为：

- Windows `1.26.51.01`，exe build `0245c20c4377`；函数定位使用 canon 的 `taau_module` 与 `taau_draw_callback`。
- declarator：RE 快照 `docs/agent/re/builds/0245c20c4377/decomp/taau_module.c`。
- draw callback：对同一 build 的直接反汇编；另以 `RQ-0003` 排队保存 Hex-Rays 快照。
- 真正算法：`data/renderer/materials/Upscaling.material.bin`，SHA-256
  `f448feba496cfdd2a2444afffeb8ddadad31e3291090e1bbb96a5510c3d46acb`。
- TAAU 像素 shader 的 SM5.0/DXBC（SHA-256 `757cd3c5b236…`）与 SM6.3/DXIL
  （SHA-256 `b6e91d619ee2…`）已分别反汇编并逐段对照，算法一致。
- SM6.3 的资源绑定表没有剥离，直接给出 `t0/t1/t2` 的名称；以下资源身份不是按采样行为猜的。

结论分级：本文的 shader 数学与 CPU 字段来源是**静态已确认**；不在本文额外宣称运行时画质或性能结论。

## 1. 一句话

TAAU 是**一次全屏 pixel-shader temporal resolve**：

> 在当前帧 3×3 邻域上估计颜色均值/方差，用运动矢量把上一帧重投影到当前像素，
> 对历史做方差约束，再以通常不超过 10% 的当前帧权重混合，输出显示分辨率颜色。

它不是 compute 管线，不在 TAAU 内生成 MV，不读深度，不做色调映射，也没有一个独立的“锐化 pass”。

## 2. CPU 侧到底做了什么

### 2.1 declarator：建一个 pass，不执行 TAA 数学

`taau_module` 做四件事：

1. 解析 `Upscaling` 材质和 `TAAUpscaling` technique。
2. 声明四个 framebuffer 依赖，并准备三个材质纹理覆盖项。
3. 把 viewport、upscaling 参数、相机上下文和 mesh/technique 状态组进 pass。
4. 注册 draw callback。

Windows 26.51 的依赖顺序为：

| declarator 参数 | 角色 | 后续用途 |
|---|---|---|
| arg9 | `InputFinalColor` | 当前帧、渲染分辨率颜色 |
| arg10 | `Color0` framebuffer 颜色附件 | 显示分辨率离屏输出；`Upscaling` 是材质名，不是该依赖名 |
| arg11 | `InputBufferMotionVectors` | 渲染分辨率 MV |
| arg12 | `InputTAAHistory` | 上一帧 TAAU 结果 |

这里**有 MV 输入纹理，但没有深度依赖**。declarator 不计算 MV、不更新相机历史、不做历史混合。

### 2.2 draw callback：填 uniform、绑纹理、录一次 draw

callback 的热路径依次是：

1. 设置 bgfx view 的矩形/变换等状态。
2. 上传 10 个材质 uniform。
3. 遍历材质纹理覆盖项并绑定三张输入纹理。
4. 选择 `TAAU` technique，通过引擎的 mesh/draw 编码器录制全屏绘制。

它没有直接调用 D3D12 `DrawInstanced`，也没有在 callback 内做 resource barrier、提交 command list、交换历史纹理或推进 previous VP。
这些都是 bgfx/帧图和外层相机的职责。

vertex shader 也很简单：`position.xy * 2 - 1` 映射到 NDC，原样传递 UV。

## 3. TAAContext 与 uniform 的真实布局

callback 第三个参数（本文称 `ctx`）提供以下字段：

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0x1E0/+0x1E2` | `uint16 ×2` | 显示宽/高 |
| `+0x1E4/+0x1E8` | `float ×2` | 当前亚像素 jitter |
| `+0x1EC` | 字节标志 | 历史是否有效 |
| `+0x1F0` | `float` | `ratio = render / display` |
| `+0x1F4/+0x1F8` | `float ×2` | variance clipping bias min/max |
| `+0x1FC` | 字节标志 | 色域 clip 开关 |
| `+0x1FD` | 字节标志 | MV filter 开关 |
| `+0x200` | `float[16]` | 当前 VP |
| `+0x240` | `float[16]` | 上一帧 VP |
| `+0x280/+0x28C` | `float[3] ×2` | 当前/上一帧世界原点 |

虽然 callback 上传 VP 和世界原点，**当前 TAAU pixel shader 并不引用它们**。
shader 实际打包进常量缓冲的只有六个 `float4`（96 字节）：

| 常量槽 | 内容 |
|---|---|
| `cb1[0]` | `SubPixelJitterAndValidHistory = {jitter.xy, validHistory, 0}` |
| `cb1[1]` | `TAAUpscalingParameters = {biasMin, biasMax, clippingEnabled, mvFilterEnabled}` |
| `cb1[2]` | `ResolutionRatiosAndFPEpsilon = {ratio, 1/ratio, fpEpsilon, 0}` |
| `cb1[3]` | `DisplayResolution` |
| `cb1[4]` | `RecipDisplayResolution` |
| `cb1[5]` | `RenderResolution` |

`validHistory=1` 走正常时间混合；为 0 时当前帧权重强制为 1，即丢弃历史。

## 4. 像素 shader 的资源与坐标契约

SM6.3 的绑定表明确为：

| 寄存器 | 资源 | 实际读取 |
|---|---|---|
| `t0` | `InputBufferMotionVectors` | **`.zw`** |
| `t1` | `InputFinalColor` | `.rgb` |
| `t2` | `InputTAAHistory` | `.rgb`，经 `s2` 采样 |

MV 的存储与重投影公式是：

```text
mv          = InputBufferMotionVectors[renderPixel].zw
mv.y        = -mv.y
mvRenderPix = mv * RenderResolution
historyPos  = displayPixelCenter - mvRenderPix / ratio
```

因此这份 MV 是**以渲染分辨率归一化的位移**，Y 方向在 shader 中翻转；不是直接以像素存储的 XY 纹理。

当前帧采样坐标为：

```text
displayCenter = floor(uv * DisplayResolution) + 0.5
srcPos        = displayCenter * ratio - jitter - 0.5
renderPixel   = round_ne(srcPos)
```

`srcPos` 同时保留到末尾，用来衡量它离最近整数渲染像素有多远。

## 5. 逐步拆解算法

### 5.1 当前色与 3×3 邻域

shader 先点采样中心 `C = current[renderPixel].rgb`，然后遍历中心周围的 3×3：

```text
sum   = Σ neighborhood.rgb
sumSq = Σ neighborhood.rgb²
mean  = sum / 9
sigma = sqrt(max(sumSq / 9 - mean², 0))
```

这是逐 RGB 通道统计，不是亮度方差，也没有 YCoCg 变换。

### 5.2 可选 MV 过滤：选 3×3 中最长者

遍历同一邻域时同时读 MV：

```text
if mvFilterEnabled && dot(candidate, candidate) > dot(selected, selected):
    selected = candidate
```

开关关闭时保留中心 MV。它只选“长度最大”的 MV，不做均值、中值或深度一致性过滤。

### 5.3 运动自适应的方差范围

```text
m      = saturate(length(mvRenderPix))
s      = m² * (3 - 2m)                 // smoothstep
bias   = lerp(biasMax, biasMin, s)
boxMin = mean - sigma * bias
boxMax = mean + sigma * bias
```

运动越大，smoothstep 越接近 1。若使用默认 `biasMin=0.75 / biasMax=1.25`，
静止时邻域盒较宽，运动时收紧到 0.75σ，从而更积极地拒绝旧历史。

### 5.4 Catmull–Rom 重采样历史

按 §4 的 `historyPos` 重投影后，坐标被限制在 `[0, DisplayResolution-1]`。
shader 计算小数权重并用 **9 次带线性 sampler 的 SampleLevel(0)** 完成 3×3 Catmull–Rom 历史重建，随后 `max(history, 0)`。

这不是单次双线性 history fetch；Catmull–Rom 的负瓣也提供了一定锐化感，但没有额外 sharpening 参数或独立锐化阶段。

### 5.5 历史色域约束

若 `clippingEnabled=0`，直接逐通道：

```text
history = clamp(history, boxMin, boxMax)
```

若开关为 1 且历史在盒外，则沿线段 `history → C` 与 AABB 求交，取合法的裁剪点。
三轴 slab 求交使用 `ResolutionRatiosAndFPEpsilon.z` 作为平行方向 epsilon。
所以“关闭 clipping”关闭的是**线段裁剪版本**，并不是完全取消邻域范围约束。

TAAU 没有深度测试或基于深度的 disocclusion 分支；历史有效性主要依靠 MV、这个颜色盒和全局 `validHistory`。

### 5.6 最终混合

令 `delta = srcPos - round_ne(srcPos)`：

```text
spatialWeight = clamp(1 - (1/ratio) * dot(delta, delta), 0.05, 1)
currentWeight = 0.1 * max(spatialWeight, s)

if !validHistory:
    currentWeight = 1

output.rgb = history + currentWeight * (C - history)
output.a   = 0
```

正常时间累积时，当前中心色的权重上限是 **0.1**，即历史至少保留 90%。
亚像素位置不利时权重可降到 0.005；运动较大时回到 0.1；历史无效时才完全使用当前帧。

## 6. 压缩后的伪代码

```hlsl
float3 TAAU(float2 uv) {
    float2 displayCenter = floor(uv * DisplayResolution) + 0.5;
    float2 srcPos = displayCenter * ratio - jitter - 0.5;
    int2 p = round_ne(srcPos);

    float3 C = FinalColor[p].rgb;
    float2 mv = MotionVectors[p].zw;
    mv.y = -mv.y;

    float3 sum = 0, sumSq = 0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float3 n = FinalColor[p + int2(x, y)].rgb;
            sum += n;
            sumSq += n * n;

            float2 candidate = MotionVectors[p + int2(x, y)].zw;
            candidate.y = -candidate.y;
            if (mvFilterEnabled && dot(candidate, candidate) > dot(mv, mv))
                mv = candidate;
        }
    }

    float3 mean = sum / 9;
    float3 sigma = sqrt(max(sumSq / 9 - mean * mean, 0));
    float2 mvRenderPix = mv * RenderResolution;

    float m = saturate(length(mvRenderPix));
    float s = m * m * (3 - 2 * m);
    float bias = lerp(biasMax, biasMin, s);
    float3 lo = mean - sigma * bias;
    float3 hi = mean + sigma * bias;

    float2 historyPos = displayCenter - mvRenderPix / ratio;
    historyPos = clamp(historyPos, 0, DisplayResolution - 1);
    float3 H = max(SampleCatmullRom3x3(History, historyPos), 0);

    H = clippingEnabled
        ? ClipSegmentTowardPointToAABB(H, C, lo, hi, fpEpsilon)
        : clamp(H, lo, hi);

    float2 d = srcPos - round_ne(srcPos);
    float spatialWeight = clamp(1 - dot(d, d) / ratio, 0.05, 1);
    float w = validHistory ? 0.1 * max(spatialWeight, s) : 1;
    return H + w * (C - H); // alpha = 0
}
```

`SampleCatmullRom3x3` 和 AABB 求交在上文展开；伪代码省略的只是这两段标准几何/滤波样板，不是额外算法阶段。

## 7. 明确否定旧结论

1. **“TAAU 没有 MV 纹理，靠深度 + 前后 VP 重建”是错的。**
   声明期已有 `InputBufferMotionVectors`，shader `t0` 的资源名也直接是它，并实际读取 `.zw`。
2. **“TAAU 通过深度做重投影/去遮挡”是错的。**
   没有深度资源绑定；前后 VP 虽被 CPU 上传，却不在当前像素 shader 的 96 字节常量缓冲中。
3. **“当前/上一帧 VP 是 TAAU 算法的核心输入”不成立。**
   它们对当前 TAAU shader 是未使用 uniform。相机矩阵可能对其它路径或上游 MV 生产有用，但不能据此描述这份 TAAU。
4. **“TAAU 内部负责曝光或色调映射”不成立。**
   shader 只做颜色统计、历史重采样/约束与混合，没有曝光、ACES、gamma 或 LUT。
5. **“MV 是 XY 像素位移”不适用于这份输入。**
   当前 shader 读 `.zw`，先翻 Y，再乘渲染分辨率并除以 ratio 映射到显示像素。

## 8. 对替换实现最直接的约束

- 替换 TAAU 时不能把这份 MV 当成“由 TAAU 自己算出的中间结果”；它是上游已经提供的输入。
- 跳过 TAAU 只会跳过重投影、历史约束与混合，不会从逻辑上停掉上游 MV/颜色生产。
- 若复用引擎 MV，必须遵守 `.zw + Y 翻转 + 渲染分辨率归一化` 的契约，或在自己的预处理阶段显式转换。
- 若自算 MV/历史，则不再需要伪装成引擎的深度重投影；但也必须自己承担历史无效、颜色约束与 reset 语义。

## 9. arg10 与 list44：同帧 RTV 指针对齐（EXP-0055）

### 已证实

静态反汇编中，arg10 的依赖名是 **`Color0`**，并走单独的 framebuffer 输出登记路径；
`Upscaling` 是材质名，不是这个 framebuffer 依赖的名字。

EXP-0055 在 declarator 返回后沿 arg10 的包装对象解析 bgfx handle 和 native，
再把每个 command list 首次 `OMSetRenderTargets` 的 RTV 反查成 `ID3D12Resource*`，
最后在同一次原始 `lists[]` 提交中做指针相等比较。

有效采样的 7 个连续帧中：

- arg10 native 在两个地址间交替，解析原因均为 `ok`；候选 RTV 的 `unresolved=0`。
- 命中始终是**显示分辨率、无 DSV、单次 6-index draw** 的前置 list。
- 命中下标随帧形变化：72-list 帧为 49，70-list 帧为 47，107-list 帧为 84。
- 后续带 DSV 的合成 list（68/66/103）使用另一张 RTV，从未与 arg10 相等。

因此在常见的 48-list 帧里，真实关系是：

```text
前置无 DSV list（此帧形为 25）：写 arg10 / Color0 离屏输出
    → bloom / 后处理等中间阶段
带 DSV 的 list44：后续世界合成，写的是另一张 RTV
```

**list44 不是 arg10 的写入者，arg10 也不是 list44 的 RTV。**
二者处于同一显示管线的上游/下游关系，但不能据此进一步声称 list44 直接采样 arg10；
中间还隔着后处理，本文没有把“直接 SRV 消费”当成已证实事实。

原始证据：`docs/agent/evidence/arg10-list-binding-EXP-0055.log`（含逐帧 native、RTV、下标和 DSV 形状）。

### 对旧锚点的影响

- “显示分辨率 + DSV + 单次 draw”是后续合成槽位的形状，不是 TAAU 输出写入者的身份证。
- `list44` 只是特定帧形下的下标，不能当跨帧/跨场景语义锚点。
- TAAU 的 `Color0` 是显示尺寸离屏目标，不是直接写入交换链后缓冲。

## 10. arg10 出 TAAU 后的引用与消费（EXP-0056）

### 帧图：共享节点引用，并有显式等待节点

Windows frameImpl 在 TAAU 返回后把 arg10 的 **24 字节宿主描述符**覆盖进“当前颜色源”槽。
未走 TAAU 时，这个槽保存的是 arg9 `InputFinalColor`；走 TAAU 后则改存 arg10 `Color0`。
因此后续 pass 不需要知道本次是否超分，它们都读统一的当前颜色源。

同一描述符还被复制进 `WaitOnUpscalingOutput` 的登记记录。这里复制的是 CPU 侧小结构，
其中的 `+8` 仍是同一个帧图节点指针；这不是 GPU 纹理复制。

对 frameImpl 调用点逐指令复核后，这个当前颜色源描述符的地址被传给：

| 下游声明函数 | 消费语义 |
|---|---|
| `LuminanceHistogram` | 以 `GameColor` 引用当前颜色，建立亮度直方图/平均亮度 |
| `Bloom`（第一次） | 以场景色引用当前颜色，进行 Bloom 下级处理 |
| `LocalExposure` | 以 `SceneColor` 引用当前颜色，建立局部曝光/双边网格 |
| `Bloom`（第二次） | 再次以同一当前颜色源进行 Bloom 上级/合成处理 |

这些调用都共享描述符及其 `+8` 节点指针。静态调用序列中，TAAU 与上述声明点之间没有
`CopyResource` / `CopyTextureRegion` 复制 pass。list44 类最终合成位于这些派生处理之后，
不能把它说成 arg10 的直接写入者或直接读者。

### native：后续材质引用存在，未观察到 GPU 复制

EXP-0056 在完整帧窗口内观察材质 tag-3 绑定，并同时纯转发观察
`CopyTextureRegion`（slot 16）与 `CopyResource`（slot 17）：

- 每张 arg10 除 TAAU 自身绑定外，均出现 **3 次后续材质引用**，数量与顺序对应上述三个下游声明点。
- 后续引用与 TAAU 自身使用相同的 bgfx handle/native，没有换成另一张纹理。
- 两个 copy hook 均安装成功；所有有效采样帧的 `copyMask` 都为空，未观察到 arg10 作为 copy src/dst。

材质 hook 的返回地址位于公共材质编码器，所以消费者名称来自 frameImpl 的静态数据流，
不是从该公共返回地址猜测。材质绑定证明的是**高层资源引用**；本文不把它夸大成像素级采样证明。

原始证据：`docs/agent/evidence/arg10-downstream-EXP-0056.log`。

### 最终管线口径

```text
TAAU 写 Color0 / arg10（同一 native，双缓冲）
  → WaitOnUpscalingOutput（帧图依赖登记）
  → LuminanceHistogram（共享引用）
  → Bloom（共享引用）
  → LocalExposure（共享引用）
  → Bloom 第二级/合成（再次共享引用）
  → 后续世界合成写另一张 RTV
```

结论：arg10 的下游是**引用 + shader 后处理**，不是先复制一张再由副本消费。
