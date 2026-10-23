#include "neurodragon/sr/SrSharpening.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/nr/D3D12Bridge.h"

#include <Windows.h>

#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace neurodragon::sr {
namespace {

// Levels follow nr's convention: 0 = info (the user has to know), 1 = warn
// (degraded but working), 2 = error, 3 = debug (TEST only).
void srLog(int level, char const* format, ...) {
    char    buffer[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    neurodragon::log::write(level, buffer);
}

#ifdef TEST
#define SR_DEBUG(...) srLog(3, __VA_ARGS__)
#else
#define SR_DEBUG(...) ((void)0)
#endif

// RCAS after DLSS, in the Reinhard domain. The MATH is TRP's RCAS.hlsl, which in
// turn is AMD's MIT-licensed FidelityFX RCAS (permission notice retained in the
// header below). The two adaptations are documented in SrSharpening.h.
char const* const kRcasHlsl = R"hlsl(
// RCAS (FSR1) for RenderDragon's DLSS output.
// Based on RCAS for ReShade by RdenBlaauwen and AMD's FidelityFX RCAS
// (sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h), MIT licensed.
//
// Sharpens in the Reinhard-encoded domain because the engine's DLSS output is
// linear HDR (peak measured in the tens) while RCAS only solves for [0,1]: with
// values above 1 the hitMax term (1 - maxRGB) / (4 * minRGB - 4) becomes positive
// and the lobe weight is clamped to zero, which would leave every pixel that
// straddles a highlight unsharpened.
//
// RCAS itself stays conservative (its lobe is capped at -0.1875), which on an
// already-sharp DLSS transformer output is barely visible. The strength therefore
// scales the RCAS *delta* by 0..4x in this same encoded (perceptual) domain,
// instead of only scaling the lobe the way TRP's Sharpness does.
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Dest : register(u0);

cbuffer Params : register(b0)
{
    float Sharpness;
    uint  Width;
    uint  Height;
    // 0 = off; otherwise the fraction of the width that stays unsharpened (left
    // side), with a 2px black divider on the line. A split on one frame makes the
    // difference visible on any screen size, which the plain full-frame view does
    // not (measured: mean change 0.38/255 at the old strength).
    float Split;
};

float3 EncodeReinhard(float3 c)
{
    c = max(c, 0.0);
    return c / (1.0 + c);
}

float3 DecodeReinhard(float3 y)
{
    // 0.995 caps the inverse at 199x the Reinhard unit: RCAS overshoot on a
    // near-white encoded pixel would otherwise explode when divided by (1 - y).
    y = clamp(y, 0.0, 0.995);
    return y / (1.0 - y);
}

// RCAS's own luma weighting (twice green), same as TRP's shader.
float Luma2(float3 c)
{
    return dot(c, float3(0.5, 1.0, 0.5));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Width || tid.y >= Height)
    {
        return;
    }
    int2 p  = int2(tid.xy);
    int2 hi = int2(int(Width) - 1, int(Height) - 1);

    float4 centerRaw = Source.Load(int3(p, 0));

    // Split compare: left of the line is the untouched frame, right is sharpened,
    // and the line itself is two black pixels. Diagnostic only; the panel exposes
    // it because a side-by-side on one frame is the only way to judge a subtle
    // sharpening on a small or remote screen.
    if (Split > 0.0)
    {
        float splitX = Split * (float)Width;
        float dx     = (float)tid.x - splitX;
        if (dx < 0.0)
        {
            Dest[tid.xy] = float4(centerRaw.rgb, centerRaw.a);
            return;
        }
        if (dx < 2.0)
        {
            Dest[tid.xy] = float4(0.0, 0.0, 0.0, centerRaw.a);
            return;
        }
    }

    float3 e = EncodeReinhard(centerRaw.rgb);

    // Edge taps repeat the border pixel instead of reading zeros outside the image
    // (HLSL Load returns 0 there, which would darken the screen border).
    float3 b = EncodeReinhard(Source.Load(int3(p.x, max(p.y - 1, 0), 0)).rgb);
    float3 d = EncodeReinhard(Source.Load(int3(max(p.x - 1, 0), p.y, 0)).rgb);
    float3 f = EncodeReinhard(Source.Load(int3(min(p.x + 1, hi.x), p.y, 0)).rgb);
    float3 h = EncodeReinhard(Source.Load(int3(p.x, min(p.y + 1, hi.y), 0)).rgb);

    float bL = Luma2(b);
    float dL = Luma2(d);
    float eL = Luma2(e);
    float fL = Luma2(f);
    float hL = Luma2(h);

    // Noise detection.
    float nz    = (bL + dL + fL + hL) * 0.25 - eL;
    float range = max(max(max(bL, dL), max(hL, fL)), eL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz          = saturate(abs(nz) * rcp(max(range, 1e-6)));
    nz          = -0.5 * nz + 1.0;

    float3 minRGB = min(min(b, d), min(f, h));
    float3 maxRGB = max(max(b, d), max(f, h));
    float  peak   = max(maxRGB.r, max(maxRGB.g, maxRGB.b));
    if (peak < 1e-6)
    {
        Dest[tid.xy] = float4(DecodeReinhard(e), centerRaw.a);
        return;
    }

    float2 peakC   = float2(1.0, -4.0);
    // The floor on the reciprocal keeps a black channel (minRGB = maxRGB = 0 on
    // that channel) from producing 0 * inf = NaN, which would otherwise spread
    // through max() as undefined.
    float3 hitMin  = minRGB * rcp(max(4.0 * maxRGB, 1e-6));
    float3 hitMax  = (peakC.xxx - maxRGB) * rcp(4.0 * minRGB + peakC.yyy);
    float3 lobeRGB = max(-hitMin, hitMax);
    // Full-strength RCAS lobe; the user's strength scales the resulting delta
    // below. Scaling the lobe alone (TRP's "Sharpness") was measurably too subtle
    // on an already-sharp DLSS transformer output: 0.672 produced a mean change of
    // 0.38/255, invisible at a glance.
    float  lobe    = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0));
    lobe *= nz;

    float  rcpL   = rcp(4.0 * lobe + 1.0);
    float3 rcas   = ((b + d + f + h) * lobe + e) * rcpL;
    // Strength 0..1 -> 0..4x the RCAS delta, in the encoded (perceptual) domain.
    // Default 0.672 is ~2.7x full RCAS, i.e. about 4x what the first version did.
    float  amount = 4.0 * saturate(Sharpness);
    float3 result = e + (rcas - e) * amount;
    Dest[tid.xy]  = float4(DecodeReinhard(result), centerRaw.a);
}
)hlsl";

using D3DCompileFn    = HRESULT (*)(LPCVOID, SIZE_T, LPCSTR, void const*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
using SerializeRootFn = HRESULT (*)(D3D12_ROOT_SIGNATURE_DESC const*, int, void**, void**);

// How long a new output size has to hold before the scratch pair is rebuilt. A
// window drag changes the size every few frames; rebuilding every frame would
// allocate (and only later release) two display-sized RGBA16F textures per frame
// (docs/H §5: windowed play is the normal case here).
constexpr unsigned kStableFrames = 12;
// A replaced pair was last used at least kStableFrames ago; the GPU is at most a
// couple of frames behind, so this is generous.
constexpr unsigned kRetireDelay = 120;

#ifdef TEST
// TEST builds take one before/after pair 300 sharpened frames in, so the diff can
// be checked offline without another game session.
constexpr unsigned kDumpAfterFrames = 300;
#endif

struct RetiredTexture {
    ID3D12Resource* resource  = nullptr;
    std::uint64_t   releaseAt = 0;
};

struct SharpState {
    ID3D12Device*         device = nullptr;
    ID3D12RootSignature*  root   = nullptr;
    ID3D12PipelineState*  pso    = nullptr;
    ID3D12DescriptorHeap* heap   = nullptr;
    UINT                  stride = 0;

    ID3D12Resource*       source      = nullptr; // pre-sharpen copy of the engine output
    ID3D12Resource*       target      = nullptr; // sharpened result
    D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_COMMON;

    unsigned width  = 0;
    unsigned height = 0;
    unsigned format = 0;

    unsigned pendingWidth  = 0;
    unsigned pendingHeight = 0;
    unsigned pendingFormat = 0;
    unsigned pendingTicks  = 0;

    std::uint64_t lastFrame  = ~std::uint64_t{0};
    bool          loggedOn   = false;
    bool          loggedOff  = true;
    // Pipeline creation is expensive (shader compile); a failure must not be retried
    // every frame.
    bool          pipelineFailed  = false;
    std::uint64_t pipelineRetryAt = 0;
    std::uint64_t dumpFlushAt = 0;
    bool          dumpPending = false;

    std::vector<RetiredTexture> retired;
};

SharpState g;

// Mirrors published for the panel (the render thread writes, the panel reads).
std::atomic<bool>     gActive{false};
std::atomic<bool>     gReady{false};
std::atomic<unsigned> gFrames{0};
std::atomic<unsigned> gFailures{0};
std::atomic<unsigned> gStrengthBits{0};

std::uint32_t floatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void transition(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource*            resource,
    D3D12_RESOURCE_STATES      before,
    D3D12_RESOURCE_STATES      after
) {
    if (!resource || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    cmd->ResourceBarrier(1, &barrier);
}

void releaseRetired(std::uint64_t frame) {
    for (auto it = g.retired.begin(); it != g.retired.end();) {
        if (frame >= it->releaseAt) {
            if (it->resource) {
                it->resource->Release();
            }
            it = g.retired.erase(it);
        } else {
            ++it;
        }
    }
}

void retireTexture(ID3D12Resource* resource, std::uint64_t frame) {
    if (!resource) {
        return;
    }
    g.retired.push_back({resource, frame + kRetireDelay});
}

// Shader, root signature, PSO and the two-descriptor heap. Built once per device;
// ~40 ms for the compile (the NR encoder measured the same order of magnitude and
// it happens on the first sharpened frame, not at start-up).
bool ensurePipeline(ID3D12Resource* output) {
    if (g.pso && g.root && g.heap) {
        return true;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(output->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return false;
    }
    if (g.device && g.device != device) {
        // A different device means every object below belongs to the old one.
        if (g.heap) g.heap->Release();
        if (g.pso) g.pso->Release();
        if (g.root) g.root->Release();
        g.heap = nullptr;
        g.pso  = nullptr;
        g.root = nullptr;
        g.device->Release();
        g.device = nullptr;
    }
    g.device = device;
    std::uint64_t const started = ::GetTickCount64();

    HMODULE    compiler   = ::LoadLibraryW(L"d3dcompiler_47.dll");
    auto const compile    = compiler ? reinterpret_cast<D3DCompileFn>(::GetProcAddress(compiler, "D3DCompile")) : nullptr;
    HMODULE    d3d12      = ::GetModuleHandleW(L"d3d12.dll");
    auto const serialize  =
        d3d12 ? reinterpret_cast<SerializeRootFn>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) {
        srLog(1, "[sr] RCAS 锐化不可用：找不到 D3DCompile / D3D12SerializeRootSignature");
        return false;
    }

    void*         code    = nullptr;
    void*         errors  = nullptr;
    HRESULT const compileHr = compile(
        kRcasHlsl,
        std::strlen(kRcasHlsl),
        "sr_rcas",
        nullptr,
        nullptr,
        "main",
        "cs_5_1", // same target the offline shader check compiles (check_shader.py)
        3u /* optimization level 3 */,
        0,
        &code,
        &errors
    );
    if (errors) {
        auto* blob = static_cast<ID3DBlob*>(errors);
        srLog(1, "[sr] RCAS 编译失败：%s", static_cast<char const*>(blob->GetBufferPointer()));
        blob->Release();
    }
    if (FAILED(compileHr) || !code) {
        return false;
    }

    // One table: SRV at t0 (descriptor 0) and UAV at u0 (descriptor 1). Root
    // constants carry strength + size (four 32-bit values).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable  = {2, ranges};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants        = {0, 0, 4}; // strength, width, height, pad
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters   = parameters;
    rootDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    void*         signature  = nullptr;
    void*         rootErrors = nullptr;
    HRESULT const rootHr     = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &rootErrors);
    if (rootErrors) {
        static_cast<ID3DBlob*>(rootErrors)->Release();
    }
    if (FAILED(rootHr) || !signature) {
        static_cast<ID3DBlob*>(code)->Release();
        srLog(1, "[sr] RCAS root signature 失败 0x%08X", static_cast<unsigned>(rootHr));
        return false;
    }
    auto*         sigBlob = static_cast<ID3DBlob*>(signature);
    HRESULT const rsHr    =
        g.device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g.root));
    sigBlob->Release();
    if (FAILED(rsHr)) {
        static_cast<ID3DBlob*>(code)->Release();
        srLog(1, "[sr] RCAS CreateRootSignature 失败 0x%08X", static_cast<unsigned>(rsHr));
        return false;
    }

    auto*                             codeBlob = static_cast<ID3DBlob*>(code);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g.root;
    psoDesc.CS             = {codeBlob->GetBufferPointer(), codeBlob->GetBufferSize()};
    HRESULT const psoHr    = g.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g.pso));
    codeBlob->Release();
    if (FAILED(psoHr)) {
        srLog(1, "[sr] RCAS PSO 失败 0x%08X", static_cast<unsigned>(psoHr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g.heap)))) {
        srLog(1, "[sr] RCAS descriptor heap 失败");
        return false;
    }
    g.stride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gReady.store(true, std::memory_order_relaxed);
    srLog(
        0,
        "[sr] RCAS 就绪：cs_5_1 编译 + 根签名 + PSO 用时 %llu ms（只在第一次启用时发生）",
        static_cast<unsigned long long>(::GetTickCount64() - started)
    );
    return true;
}

// Scratch pair, same size and format as the engine's output. Both are created with
// ALLOW_UNORDERED_ACCESS because the sharpened one is written through a UAV; the
// copy-in source only needs COPY_DEST/SRV but sharing the desc keeps one path.
bool ensureTextures(D3D12_RESOURCE_DESC const& outputDesc, std::uint64_t frame) {
    unsigned const width  = static_cast<unsigned>(outputDesc.Width);
    unsigned const height = outputDesc.Height;
    unsigned const format = static_cast<unsigned>(outputDesc.Format);

    if (g.source && g.target && width == g.width && height == g.height && format == g.format) {
        return true;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return false;
    }

    D3D12_RESOURCE_DESC desc = outputDesc;
    desc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.MipLevels           = 1;
    desc.DepthOrArraySize    = 1;
    desc.SampleDesc          = {1, 0};

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* source = nullptr;
    ID3D12Resource* target = nullptr;
    HRESULT const   sourceHr =
        g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source));
    if (FAILED(sourceHr) || !source) {
        srLog(1, "[sr] RCAS 源纹理分配失败 0x%08X (%ux%u)", static_cast<unsigned>(sourceHr), width, height);
        return false;
    }
    HRESULT const targetHr =
        g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target));
    if (FAILED(targetHr) || !target) {
        srLog(1, "[sr] RCAS 目标纹理分配失败 0x%08X (%ux%u)", static_cast<unsigned>(targetHr), width, height);
        source->Release();
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = desc.Format;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    g.device->CreateShaderResourceView(source, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = desc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{cpu.ptr + g.stride};
    g.device->CreateUnorderedAccessView(target, nullptr, &uav, uavHandle);

    // The old pair may still be referenced by command lists the GPU has not
    // finished; releasing it now would be a use-after-free.
    retireTexture(g.source, frame);
    retireTexture(g.target, frame);
    g.source      = source;
    g.target      = target;
    g.sourceState = D3D12_RESOURCE_STATE_COMMON;
    g.targetState = D3D12_RESOURCE_STATE_COMMON;
    g.width       = width;
    g.height      = height;
    g.format      = format;
    srLog(0, "[sr] RCAS 锐化目标 %ux%u（格式 %u）", width, height, format);
    return true;
}

// Follows the engine output's size: returns false while a resize is settling, so
// the caller skips this frame instead of allocating for a size that is about to
// change again.
bool followOutputSize(D3D12_RESOURCE_DESC const& outputDesc, std::uint64_t frame) {
    unsigned const width  = static_cast<unsigned>(outputDesc.Width);
    unsigned const height = outputDesc.Height;
    unsigned const format = static_cast<unsigned>(outputDesc.Format);

    if (g.source && width == g.width && height == g.height && format == g.format) {
        g.pendingTicks = 0;
        return true;
    }
    if (width != g.pendingWidth || height != g.pendingHeight || format != g.pendingFormat) {
        g.pendingWidth  = width;
        g.pendingHeight = height;
        g.pendingFormat = format;
        g.pendingTicks  = 0;
    }
    if (++g.pendingTicks < kStableFrames) {
        SR_DEBUG("[sr] RCAS 等待尺寸稳定（%ux%u → %ux%u）", g.width, g.height, width, height);
        return false;
    }
    g.pendingTicks = 0;
    return ensureTextures(outputDesc, frame);
}

void recordPass(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource*            output,
    unsigned                   width,
    unsigned                   height,
    float                      strength,
    float                      split
) {
    // 1. Copy the finished DLSS frame into our own texture. The engine output goes
    //    ALL_SHADER_RESOURCE -> COPY_SOURCE -> (later) COPY_DEST -> ALL_SHADER_RESOURCE:
    //    the same round trip the TEST colour dumps already perform on it.
    transition(cmd, g.source, g.sourceState, D3D12_RESOURCE_STATE_COPY_DEST);
    g.sourceState = D3D12_RESOURCE_STATE_COPY_DEST;
    transition(cmd, output, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(g.source, output);
    transition(cmd, g.source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.sourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // 2. RCAS source -> target (compute).
    transition(cmd, g.target, g.targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.targetState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heap = g.heap;
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetComputeRootSignature(g.root);
    cmd->SetPipelineState(g.pso);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g.heap->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(0, table);
    std::uint32_t const constants[4] = {floatBits(strength), width, height, floatBits(split)};
    cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);
    cmd->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = g.target;
    cmd->ResourceBarrier(1, &uavBarrier);

    // 3. Copy the sharpened frame back over the engine's output.
    transition(cmd, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cmd, g.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.targetState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->CopyResource(output, g.target);
    transition(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

} // namespace

void recordSharpening(void* commandList, void* engineOutput, std::uint64_t frame) {
    auto const& config = neurodragon::config::get().sr;
    float const strength = std::clamp(config.sharpness, 0.0f, 1.0f);
    bool const  wanted   = config.sharpening && strength > 0.001f;

    if (!wanted) {
        gActive.store(false, std::memory_order_relaxed);
        gStrengthBits.store(floatBits(strength), std::memory_order_relaxed);
        if (!g.loggedOff) {
            g.loggedOff = true;
            g.loggedOn  = false;
            srLog(0, "[sr] RCAS 锐化已关闭（不再记录任何 pass）");
        }
        return;
    }
    if (!commandList || !engineOutput || frame == g.lastFrame) {
        return;
    }
    g.lastFrame = frame;

    auto*                      cmd     = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto*                      output  = static_cast<ID3D12Resource*>(engineOutput);
    D3D12_RESOURCE_DESC const  outDesc = output->GetDesc();
    if (outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || outDesc.Width == 0 || outDesc.Height == 0) {
        return;
    }

    releaseRetired(frame);

    if (g.pipelineFailed && frame < g.pipelineRetryAt) {
        gActive.store(false, std::memory_order_relaxed);
        return;
    }
    if (!ensurePipeline(output)) {
        ++gFailures;
        g.pipelineFailed  = true;
        g.pipelineRetryAt = frame + 600;
        gActive.store(false, std::memory_order_relaxed);
        return;
    }
    g.pipelineFailed = false;
    if (!followOutputSize(outDesc, frame)) {
        gActive.store(false, std::memory_order_relaxed);
        return;
    }

    unsigned const width  = g.width;
    unsigned const height = g.height;
    float const    split  = config.compare ? std::clamp(config.compareSplit, 0.0f, 1.0f) : 0.0f;

#ifdef TEST
    // Same-frame before/after pair, queued where the engine output is in
    // ALL_SHADER_RESOURCE (the dump helper's contract). The BMPs differ only where
    // RCAS touched a pixel; the dump's own log line carries max/mean/above1 of the
    // raw half-float data, which is what confirms the linear-HDR domain.
    if (gFrames.load(std::memory_order_relaxed) == kDumpAfterFrames) {
        nr::queueColorDumpN(2, cmd, output, "sharp_before.bmp", 0);
        g.dumpPending   = true;
        g.dumpFlushAt   = frame + 200;
    }
#endif

    nr::nrStampBegin(cmd, frame, 2);
    recordPass(cmd, output, width, height, strength, split);
    nr::nrStampEnd(cmd, frame, 2);

#ifdef TEST
    if (g.dumpPending) {
        nr::queueColorDumpN(3, cmd, output, "sharp_after.bmp", 0);
    }
    if (g.dumpPending && frame >= g.dumpFlushAt) {
        g.dumpPending = false;
        float         maxBefore = -1.0f;
        float         maxAfter  = -1.0f;
        std::uint32_t nzBefore  = 0;
        std::uint32_t nzAfter   = 0;
        bool const    okBefore  = nr::flushColorDumpN(2, &maxBefore, &nzBefore);
        bool const    okAfter   = nr::flushColorDumpN(3, &maxAfter, &nzAfter);
        srLog(
            0,
            "[sr] 锐化前后 dump（强度 %.3f，增量倍率 %.2f）：before=%d max=%.2f nz=%u / after=%d max=%.2f nz=%u",
            static_cast<double>(strength),
            static_cast<double>(4.0f * strength),
            okBefore ? 1 : 0,
            static_cast<double>(maxBefore),
            nzBefore,
            okAfter ? 1 : 0,
            static_cast<double>(maxAfter),
            nzAfter
        );
    }
#endif

    unsigned const frames = gFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    gActive.store(true, std::memory_order_relaxed);
    gStrengthBits.store(floatBits(strength), std::memory_order_relaxed);
    if (!g.loggedOn) {
        g.loggedOn  = true;
        g.loggedOff = false;
        srLog(
            0,
            "[sr] RCAS 锐化已启用：强度 %.3f（增量 %.2f 倍），%ux%u%s（HDR→Reinhard 域，在引擎调色与 UI 之前）",
            static_cast<double>(strength),
            static_cast<double>(4.0f * strength),
            width,
            height,
            split > 0.0f ? "，分屏对比开" : ""
        );
    }
    if (frames == 1 || frames % 600 == 0) {
        SR_DEBUG("[sr] RCAS 第 %u 帧（%ux%u，强度 %.3f）", frames, width, height, static_cast<double>(strength));
    }
}

SharpStats sharpStats() {
    SharpStats stats;
    auto const& config = neurodragon::config::get().sr;
    stats.enabled  = config.sharpening;
    stats.active   = gActive.load(std::memory_order_relaxed);
    stats.ready    = gReady.load(std::memory_order_relaxed);
    stats.frames   = gFrames.load(std::memory_order_relaxed);
    stats.failures = gFailures.load(std::memory_order_relaxed);
    stats.strength = bitsFloat(gStrengthBits.load(std::memory_order_relaxed));
    return stats;
}

void shutdown() {
    // Pipeline objects are safe to release once no new pass will be recorded; the
    // textures are left allocated on purpose (see the header / docs/D §6.16). Their
    // pointers are dropped so a later enable builds a fresh pair whose descriptors
    // live in the new heap.
    if (g.heap) g.heap->Release();
    if (g.pso) g.pso->Release();
    if (g.root) g.root->Release();
    if (g.device) g.device->Release();
    g.heap   = nullptr;
    g.pso    = nullptr;
    g.root   = nullptr;
    g.device = nullptr;
    g.source = nullptr;
    g.target = nullptr;
    g.width  = 0;
    g.height = 0;
    g.format = 0;
    g.pendingTicks = 0;
    gReady.store(false, std::memory_order_relaxed);
    gActive.store(false, std::memory_order_relaxed);
    srLog(0, "[sr] RCAS 已停止；纹理等 GPU 对象随进程释放（避免卸载时释放正在使用的资源）");
}

} // namespace neurodragon::sr
