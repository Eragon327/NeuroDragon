#include "neurodragon/vv/VvSr.h"

#include "neurodragon/Config.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/sr/SrSharpening.h"
#include "neurodragon/vv/VvDlssSr.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "neurodragon/Macro.h"

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>

namespace neurodragon::vv::sr {
namespace {

using SerializeRootFn = HRESULT (*)(D3D12_ROOT_SIGNATURE_DESC const*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using CompileFn = HRESULT (*)(LPCVOID, SIZE_T, LPCSTR, D3D_SHADER_MACRO const*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                             UINT, ID3DBlob**, ID3DBlob**);

char const* kPrepShader = R"hlsl(
cbuffer P : register(b0)
{
    uint W;
    uint H;
    uint Mode;   // bit 2: the engine texture carries the vector in .zw; bit 4: force zero motion
    uint Pad;
};
Texture2D<float4>   Color    : register(t0);
Texture2D<float>    Depth    : register(t1);
// R3 stage C: the engine's own motion vectors (arg11 / InputBufferMotionVectors) - a
// composite G-buffer whose .zw is the render-resolution-normalized displacement.
Texture2D<float4>   EngineMv : register(t7);
RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float>  OutDepth : register(u1);
RWTexture2D<float2> OutMv    : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H)
    {
        return;
    }
    OutColor[id.xy] = float4(Color.Load(int3(id.xy, 0)).rgb, 1.0);
    const float d = Depth.Load(int3(id.xy, 0));
    OutDepth[id.xy] = d;

    float4 engineMv = EngineMv.Load(int3(id.xy, 0));
    float2 stored = ((Mode & 2u) != 0u) ? engineMv.zw : engineMv.xy;
    // {z, -w} is the velocity (current - previous). DLSS wants the map current ->
    // previous in render pixels: negate x, keep y.
    OutMv[id.xy] = ((Mode & 4u) != 0u)
        ? float2(0.0, 0.0)
        : float2(-stored.x, stored.y) * float2(float(W), float(H));
}
)hlsl";

// R3 stage B: the DLSS output (RGBA16F, display size) is already in the same domain
// as TAAU's output, so this pass is a straight copy - no exposure, no encode. The
// fullscreen triangle needs no vertex buffer; the PS loads the source by pixel.
char const* kArg10VsShader = R"hlsl(
void main(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 p = float2((id << 1) & 2, id & 2);
    pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)hlsl";

char const* kArg10PsShader = R"hlsl(
Texture2D<float4> Src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    return Src.Load(int3(int2(pos.xy), 0));
}
)hlsl";

// Shader-visible descriptor slots. One SRV table (7) and one UAV table (5) keep the
// root signature to four parameters - a per-pass split layout was rejected by the
// runtime ("overlaps with another shader register range", 2026-10-25).
enum : unsigned {
    kSlotEngineColor = 0,
    kSlotEngineDepth = 1,
    kSlotColorSrv    = 2,
    kSlotDepthSrv    = 3,
    kSlotMvSrv       = 4,
    kSlotDlssSrv     = 5,
    kSlotExposureSrv = 6,
    kSlotEngineMv    = 7, ///< R3 stage C: arg11, the engine's own motion vectors
    kSlotColorUav    = 8,
    kSlotDepthUav    = 9,
    kSlotMvUav       = 10,
    kSlotFinalUav    = 11,
    kSlotExposureUav = 12,
    kSlotCount       = 13,
};

struct Pipeline {
    ID3D12Device*               device{};
    ID3D12RootSignature*        root{};
    ID3D12PipelineState*        prep{};
    ID3D12PipelineState*        arg10Ps{};
    ID3D12DescriptorHeap*       heap{};
    ID3D12DescriptorHeap*       rtvHeap{};
    D3D12_CPU_DESCRIPTOR_HANDLE arg10Rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    unsigned                    increment{};
    ID3D12Resource*             colorHalf{};
    ID3D12Resource*             depthR32{};
    ID3D12Resource*             mvRg32{}; ///< R16G16_FLOAT: DLSS accepts this, not RG32F
    ID3D12Resource*             dlssOut{};
    ID3D12Resource*             sharpOut{}; ///< RCAS result, sampled by the arg10 PS
    D3D12_RESOURCE_STATES       colorState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       depthState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       mvState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       dlssState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       sharpState{D3D12_RESOURCE_STATE_COMMON};
    unsigned                    renderWidth{};
    unsigned                    renderHeight{};
    unsigned                    displayWidth{};
    unsigned                    displayHeight{};
};

std::mutex       gMutex;
Pipeline         gPipeline{};
Stats            gStats{};
// Frames of forced Reset requested from the panel. Two, not one: the first frame after
// a rebuild may still be the one the engine had already recorded before the request.
std::atomic<unsigned> gManualReset{0};
// What DLSS is told as Jitter.Offset. Default is zero: EXP-0098's A/B showed the engine
// value makes the picture wobble and zero is stable, and the value itself is already in
// DLSS's space (Halton(2,3)-0.5, render pixels, y-down - see
// docs/research/2026-10-29-taau-jitter-dlss-contract.md). The three non-default modes
// are the discriminating experiment of that document §7: which of "the frame is not
// rendered with it", "the sign is flipped in our hand-off" or "the context value is one
// frame ahead of the render" is true.
std::atomic<int> gJitterSource{static_cast<int>(JitterSource::Zero)};
// Last frame's context jitter, so the Previous mode can feed j(n-1). Zero until the
// first recorded frame saves one, which is the safe fallback the panel's "previous"
// entry needs on a fresh session (and after `release()`, since these are file-scope).
float gPrevJitterX = 0.0f;
float gPrevJitterY = 0.0f;
// The product switch's current state (config `sr.vvEnabled`), so syncConfig() only acts
// on a change instead of re-arming every frame.
std::atomic<bool> gEnabled{false};
// Latched by the first successful arg10 write and cleared only on disable/release.
// Once native 100% starts, transient records with the previous size's motion vectors
// must not switch the camera back: that oscillates the render size every frame and
// prevents the native-size binding from ever arriving (EXP-0094).
std::atomic<bool> gLive{false};
// Serialises recordInternal() against release(). The render thread may still be
// recording an injected list when the mod is disabled; freeing the pipeline textures
// under it is a use-after-free (EXP-0085 exit crash). release() takes this lock after
// setting gReleased, so an in-flight record finishes before anything is released and
// a later frame refuses to start.
std::mutex       gRecordMutex;
std::atomic<bool> gReleased{false};
unsigned         gColorCandidates = 0;
unsigned         gDepthCandidates = 0;

// ---- feature-level fallback (docs/specs/vv.md §38) -------------------------
// Three consecutive creation failures are not something the optional passes can
// fix, so they jump straight to EngineTaau; evaluation failures walk one rung at a
// time (drop NR first - it is the invasive input - then RCAS, then give up).
constexpr unsigned kCreateFailLimit = 3;
constexpr unsigned kEvalFailLimit   = 120;  // ~2 s at 60 fps
constexpr unsigned kRetryFrames     = 600;  // ~10 s on TAAU, then try Full again

std::atomic<int>      gRung{static_cast<int>(Rung::Full)};
std::atomic<int>      gRungReason{static_cast<int>(RungReason::None)};
std::atomic<unsigned> gCreateFails{0};
std::atomic<unsigned> gEvalFails{0};
std::atomic<unsigned> gRungSince{0};
std::atomic<unsigned> gSuccessStreak{0};
#ifdef TEST
std::atomic<int>      gFault{static_cast<int>(Fault::None)};
#endif

// The bypass has to follow the rung even on frames nobody records, so it is applied
// when the rung changes rather than read per frame.
void setRung(Rung rung, RungReason reason, unsigned frame) {
    gRung.store(static_cast<int>(rung), std::memory_order_relaxed);
    gRungReason.store(static_cast<int>(reason), std::memory_order_relaxed);
    gRungSince.store(frame, std::memory_order_relaxed);
    gCreateFails.store(0, std::memory_order_relaxed);
    gEvalFails.store(0, std::memory_order_relaxed);
    gSuccessStreak.store(0, std::memory_order_relaxed);
    nr::nrSetBypass(rung >= Rung::NoNr);
    if (reason == RungReason::Retry) {
        utils::logger().info(
            "[vvsr] fallback: {} ({}) nr={} rcas={}",
            rungName(rung),
            rungReasonName(reason),
            rung < Rung::NoNr ? 1 : 0,
            rung < Rung::NoSharp ? 1 : 0
        );
    } else {
        utils::logger().warn(
            "[vvsr] fallback: {} ({}) nr={} rcas={}",
            rungName(rung),
            rungReasonName(reason),
            rung < Rung::NoNr ? 1 : 0,
            rung < Rung::NoSharp ? 1 : 0
        );
    }
}

void noteCreateFailure(unsigned frame) {
    unsigned const n = gCreateFails.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < kCreateFailLimit) {
        return;
    }
    if (static_cast<Rung>(gRung.load(std::memory_order_relaxed)) != Rung::EngineTaau) {
        setRung(Rung::EngineTaau, RungReason::Create, frame);
    } else {
        gCreateFails.store(0, std::memory_order_relaxed);
    }
}

void noteEvalFailure(unsigned frame) {
    unsigned const n = gEvalFails.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < kEvalFailLimit) {
        return;
    }
    Rung       next = Rung::EngineTaau;
    Rung const now  = static_cast<Rung>(gRung.load(std::memory_order_relaxed));
    if (now == Rung::Full) {
        next = Rung::NoNr;
    } else if (now == Rung::NoNr) {
        next = Rung::NoSharp;
    }
    setRung(next, RungReason::Eval, frame);
}

void noteSuccess(unsigned frame) {
    gCreateFails.store(0, std::memory_order_relaxed);
    gEvalFails.store(0, std::memory_order_relaxed);
    // A degraded rung that keeps succeeding is a transient failure, not a broken
    // feature: step back up one rung after the same cooldown the TAAU retry uses.
    unsigned const n = gSuccessStreak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < kRetryFrames) {
        return;
    }
    gSuccessStreak.store(0, std::memory_order_relaxed);
    Rung const now = static_cast<Rung>(gRung.load(std::memory_order_relaxed));
    if (now == Rung::NoNr) {
        setRung(Rung::Full, RungReason::Retry, frame);
    } else if (now == Rung::NoSharp) {
        setRung(Rung::NoNr, RungReason::Retry, frame);
    }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    list->ResourceBarrier(1, &barrier);
}

void setError(HRESULT hr, char const* where) {
    std::lock_guard lock(gMutex);
    gStats.lastError = static_cast<int>(hr);
    if (FAILED(hr)) {
        // A failing step is retried on the next injection attempt, so the same HRESULT
        // would otherwise be one line per frame. The first few name the step; the count
        // in `Stats::lastError` keeps answering after that.
        static std::atomic<unsigned> failures{0};
        unsigned const               n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            utils::logger().warn("[vvsr] sr {} failed hr=0x{:08X}", where, static_cast<unsigned>(hr));
        }
    }
}

ID3D12Resource* createTexture(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&resource)
        ))) {
        return nullptr;
    }
    return resource;
}

// R3 stage C: arg11 is bound as an SRV, and DXGI forbids an SRV format outside the
// resource's own family. The concrete format is mapped once instead of inferring the
// `.zw` layout from the material: `fourChannel` decides which pair the prep shader
// reads, `srvFormat` is what the descriptor may legally use.
bool engineMotionVectorSrvFormat(DXGI_FORMAT format, DXGI_FORMAT* srvFormat, bool* fourChannel) {
    switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        *srvFormat   = DXGI_FORMAT_R16G16_FLOAT;
        *fourChannel = false;
        return true;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        *srvFormat   = DXGI_FORMAT_R32G32_FLOAT;
        *fourChannel = false;
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *srvFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        *fourChannel = true;
        return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        *srvFormat   = DXGI_FORMAT_R32G32B32A32_FLOAT;
        *fourChannel = true;
        return true;
    default:
        return false;
    }
}

bool ensureResources(ID3D12Device* device, unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight) {
    if (gPipeline.colorHalf && gPipeline.renderWidth == renderWidth && gPipeline.renderHeight == renderHeight
        && gPipeline.displayWidth == displayWidth && gPipeline.displayHeight == displayHeight) {
        return true;
    }
    for (ID3D12Resource** resource :
         {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.dlssOut, &gPipeline.sharpOut}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    gPipeline.colorHalf   = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    gPipeline.depthR32    = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
    gPipeline.mvRg32      = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT);
    gPipeline.dlssOut     = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // RCAS lands here when sr.sharpening is on; the arg10 write then samples this
    // instead of dlssOut. Same format as the DLSS output so the compute pass reads
    // and writes one domain (docs/specs/sr.md §8).
    gPipeline.sharpOut    = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!gPipeline.colorHalf || !gPipeline.depthR32 || !gPipeline.mvRg32 || !gPipeline.dlssOut
        || !gPipeline.sharpOut) {
        setError(E_OUTOFMEMORY, "CreateCommittedResource");
        return false;
    }
    gPipeline.colorState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.depthState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.mvState       = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.dlssState     = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.sharpState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.renderWidth   = renderWidth;
    gPipeline.renderHeight  = renderHeight;
    gPipeline.displayWidth  = displayWidth;
    gPipeline.displayHeight = displayHeight;
    ND_DEBUG("[vvsr] sr buffers {}x{} -> {}x{}", renderWidth, renderHeight, displayWidth, displayHeight);
    return true;
}

bool ensurePipeline(ID3D12Device* device) {
    if (gPipeline.prep && gPipeline.arg10Ps && gPipeline.root && gPipeline.heap && gPipeline.rtvHeap) {
        return true;
    }
    if (gPipeline.device && gPipeline.device != device) {
        utils::logger().warn("[vvsr] sr device changed; rebuild is not supported in-session");
        return false;
    }
    gPipeline.device = device;

    HMODULE const d3d12    = ::LoadLibraryW(L"d3d12.dll");
    HMODULE const compiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3d12 || !compiler) {
        setError(HRESULT_FROM_WIN32(::GetLastError()), "LoadLibrary");
        return false;
    }
    auto const serialize = reinterpret_cast<SerializeRootFn>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    auto const compile   = reinterpret_cast<CompileFn>(::GetProcAddress(compiler, "D3DCompile"));
    if (!serialize || !compile) {
        setError(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "GetProcAddress");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 8;
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 6;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[3].Descriptor.ShaderRegister = 1;
    params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[4].Descriptor.ShaderRegister = 2;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = 5;
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;

    ID3DBlob* signature = nullptr;
    ID3DBlob* error     = nullptr;
    HRESULT   hr        = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (error) {
        utils::logger().warn("[vvsr] sr root signature: {}", static_cast<char const*>(error->GetBufferPointer()));
        error->Release();
    }
    if (FAILED(hr) || !signature) {
        setError(hr, "SerializeRootSignature");
        return false;
    }
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&gPipeline.root));
    signature->Release();
    if (FAILED(hr)) {
        setError(hr, "CreateRootSignature");
        return false;
    }

    auto compileShader = [&](std::string const& source, ID3D12PipelineState** out) {
        ID3DBlob* code   = nullptr;
        ID3DBlob* err    = nullptr;
        HRESULT   result = compile(source.data(), source.size(), "main", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err);
        if (err) {
            utils::logger().warn("[vvsr] sr shader: {}", static_cast<char const*>(err->GetBufferPointer()));
            err->Release();
        }
        if (FAILED(result) || !code) {
            setError(result, "D3DCompile");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature    = gPipeline.root;
        pso.CS.pShaderBytecode = code->GetBufferPointer();
        pso.CS.BytecodeLength  = code->GetBufferSize();
        result = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(out));
        code->Release();
        if (FAILED(result)) {
            setError(result, "CreateComputePipelineState");
            return false;
        }
        return true;
    };

    if (!compileShader(kPrepShader, &gPipeline.prep)) {
        return false;
    }

    // R3 stage B: DLSS output -> arg10. A graphics PSO with a fullscreen triangle
    // reads the DLSS texture (t0) and writes the display-sized target; the RTV format
    // is arg10's (R11G11B10_FLOAT). It shares the compute root signature: the PS uses
    // only the SRV table at parameter 1, and the static sampler stays unused.
    auto compileStage = [&](char const* source, char const* target) -> ID3DBlob* {
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT   result =
            compile(source, std::strlen(source), "main", nullptr, nullptr, "main", target, 0, 0, &code, &err);
        if (err) {
            utils::logger().warn("[vvsr] sr {} shader: {}", target, static_cast<char const*>(err->GetBufferPointer()));
            err->Release();
        }
        if (FAILED(result) || !code) {
            setError(result, "D3DCompile");
            return nullptr;
        }
        return code;
    };
    ID3DBlob* vsCode = compileStage(kArg10VsShader, "vs_5_0");
    ID3DBlob* psCode = compileStage(kArg10PsShader, "ps_5_0");
    if (!vsCode || !psCode) {
        if (vsCode) {
            vsCode->Release();
        }
        if (psCode) {
            psCode->Release();
        }
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature                                  = gPipeline.root;
    graphics.VS.pShaderBytecode                              = vsCode->GetBufferPointer();
    graphics.VS.BytecodeLength                               = vsCode->GetBufferSize();
    graphics.PS.pShaderBytecode                              = psCode->GetBufferPointer();
    graphics.PS.BytecodeLength                               = psCode->GetBufferSize();
    graphics.RasterizerState.FillMode                        = D3D12_FILL_MODE_SOLID;
    graphics.RasterizerState.CullMode                        = D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable                 = TRUE;
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.DepthStencilState.DepthEnable                   = FALSE;
    graphics.DepthStencilState.StencilEnable                 = FALSE;
    graphics.SampleMask                                      = UINT_MAX;
    graphics.PrimitiveTopologyType                           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets                                = 1;
    graphics.RTVFormats[0]                                   = DXGI_FORMAT_R11G11B10_FLOAT;
    graphics.SampleDesc                                      = {1, 0};
    hr = device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&gPipeline.arg10Ps));
    vsCode->Release();
    psCode->Release();
    if (FAILED(hr)) {
        setError(hr, "CreateGraphicsPipelineState");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&gPipeline.rtvHeap));
    if (FAILED(hr)) {
        setError(hr, "CreateDescriptorHeap(rtv)");
        return false;
    }
    gPipeline.arg10Rtv = gPipeline.rtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kSlotCount;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gPipeline.heap));
    if (FAILED(hr)) {
        setError(hr, "CreateDescriptorHeap");
        return false;
    }
    gPipeline.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gPipeline.cpu       = gPipeline.heap->GetCPUDescriptorHandleForHeapStart();
    gPipeline.gpu       = gPipeline.heap->GetGPUDescriptorHandleForHeapStart();

    std::lock_guard lock(gMutex);
    gStats.pipelineReady = true;
    ND_DEBUG("[vvsr] sr pipeline ready (prep + DLSS -> arg10)");
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpuAt(unsigned slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gPipeline.cpu;
    handle.ptr += static_cast<SIZE_T>(slot) * gPipeline.increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpuAt(unsigned slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = gPipeline.gpu;
    handle.ptr += static_cast<UINT64>(slot) * gPipeline.increment;
    return handle;
}

// A render-size arg9 InputFinalColor is preferred; the RTV pick is the fallback.
// EXP-0034 used arg10, which is actually the display-size Upscaling output. The DSV inventory
// has depth (2026-10-25: the RTV list had 1689x903 fmt=26 but no depth at all, which is
// why the first SR run failed with "no colour/depth").
bool pickInputs(void** colorOut, unsigned* colorFormat, void** depthOut, unsigned* depthFormat) {
    probe::vv::RenderTargetInfo targets[48]{};
    probe::vv::RenderTargetInfo colors[16]{};
    probe::vv::RenderTargetInfo depths[16]{};
    *colorOut    = nullptr;
    *depthOut    = nullptr;
    *colorFormat = 0;
    *depthFormat = 0;

    unsigned const colorCount = probe::vv::renderTargets(targets, 48, 64);
    unsigned       colorsCount = 0;
    for (unsigned i = 0; i < colorCount && colorsCount < 16; ++i) {
        if (targets[i].format == 26) {
            colors[colorsCount++] = targets[i];
        }
    }
    gColorCandidates = colorsCount;
    if (colorsCount) {
        *colorOut    = colors[0].resource;
        *colorFormat = colors[0].format;
    }

    auto const semantic = probe::vv::inputFinalColorTarget();
    if (semantic.resource && semantic.format == 26 && semantic.width == probe::vv::renderWidth()
        && semantic.height == probe::vv::renderHeight()) {
        *colorOut    = semantic.resource;
        *colorFormat = semantic.format;
    }

    unsigned const depthCount = probe::vv::depthTargets(targets, 48, 64);
    {
        // One line that says which side is empty when the pick fails - the panel only
        // shows zeros, and "0 of 0" cannot distinguish "inventory empty" from "size
        // mismatch" (the render size follows the engine's factor, so after a resize it
        // can be 1689x903 while the views are still 1689x950).
        static std::atomic<unsigned> diag{0};
        if (diag.fetch_add(1, std::memory_order_relaxed) < 5) {
            ND_DEBUG(
                "[vvsr] sr pick: render={}x{} colourTargets={} fmt26={} depthTargets={}",
                probe::vv::renderWidth(),
                probe::vv::renderHeight(),
                colorCount,
                colorsCount,
                depthCount
            );
        }
    }
    unsigned       depthsCount = 0;
    for (unsigned i = 0; i < depthCount && depthsCount < 16; ++i) {
        unsigned const format = targets[i].format;
        if (format == 43 || format == 44 || format == 45 || format == 40 || format == 39 || format == 54 || format == 41) {
            depths[depthsCount++] = targets[i];
        }
    }
    gDepthCandidates = depthsCount;
    if (depthsCount) {
        *depthOut    = depths[0].resource;
        *depthFormat = depths[0].format;
    }
    return *colorOut && *depthOut;
}

} // namespace

void requestReset() {
    gManualReset.store(2, std::memory_order_relaxed);
    ND_DEBUG("[vvsr] sr reset requested (two frames)");
}

void setJitterSource(JitterSource source) {
    // The panel's four entries; anything outside the enum folds to Zero so a stale
    // draft (or a future edit) cannot leave the pipeline on a value with no branch.
    int const raw   = static_cast<int>(source);
    int const value = raw >= static_cast<int>(JitterSource::Zero) && raw <= static_cast<int>(JitterSource::Previous)
                        ? raw
                        : static_cast<int>(JitterSource::Zero);
    int const previous = gJitterSource.exchange(value, std::memory_order_relaxed);
    if (previous == value) {
        return;
    }
    // One history wipe on both edges: the modes accumulate differently, and carrying
    // samples across the switch would show up as a smear on the frame after.
    gManualReset.store(2, std::memory_order_relaxed);
    ND_DEBUG("[vvsr] sr jitter source={}", value);
}

JitterSource jitterSource() {
    return static_cast<JitterSource>(gJitterSource.load(std::memory_order_relaxed));
}

void setEnabled(bool wantEnabled) {
    bool const was = gEnabled.exchange(wantEnabled, std::memory_order_acq_rel);
    if (was == wantEnabled) {
        return;
    }
    if (wantEnabled) {
        // A fresh enable starts from Full: the user's action is the cheapest possible
        // "retry now", and a rung latched from a previous failure would otherwise
        // hide the feature behind a stale decision.
        if (static_cast<Rung>(gRung.load(std::memory_order_relaxed)) != Rung::Full) {
            setRung(Rung::Full, RungReason::Retry, 0);
        }
        // Arm the injection first, then the draw skip: the suppress gate only fires on
        // a frame that really injected, so this order leaves TAAU running for the one
        // frame that has no replacement yet.
        probe::vv::setProductArmed(true);
        probe::vv::inject::setUseArg10(true);
        probe::vv::inject::setAction(probe::vv::inject::Action::DlssArg10);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Fixed);
        probe::vv::setDrawSuppress(true);
    } else {
        // The reverse order: TAAU comes back first, then our writer stops touching
        // arg10, so no frame is drawn by neither path.
        gLive.store(false, std::memory_order_release);
        probe::vv::setDrawSuppress(false);
        probe::vv::setProductArmed(false);
        probe::vv::inject::setUseArg10(false);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
    }
    // The one line the product prints about itself: a user-visible switch flipping.
    utils::logger().info(
        "[vvsr] sr {} (TAAU draw {})",
        wantEnabled ? "enabled" : "disabled",
        wantEnabled ? "skipped" : "restored"
    );
}

bool enabled() {
    return gEnabled.load(std::memory_order_relaxed);
}

bool live() {
    return gLive.load(std::memory_order_acquire);
}

char const* rungName(Rung rung) noexcept {
    switch (rung) {
    case Rung::Full:
        return "Full";
    case Rung::NoNr:
        return "NoNr";
    case Rung::NoSharp:
        return "NoSharp";
    case Rung::EngineTaau:
        return "TAAU";
    }
    return "?";
}

char const* rungReasonName(RungReason reason) noexcept {
    switch (reason) {
    case RungReason::None:
        return "none";
    case RungReason::Create:
        return "create";
    case RungReason::Eval:
        return "eval";
    case RungReason::Retry:
        return "retry";
    }
    return "?";
}

Fallback fallback() {
    Fallback state{};
    state.rung        = static_cast<Rung>(gRung.load(std::memory_order_relaxed));
    state.reason      = static_cast<RungReason>(gRungReason.load(std::memory_order_relaxed));
    state.createFails = gCreateFails.load(std::memory_order_relaxed);
    state.evalFails   = gEvalFails.load(std::memory_order_relaxed);
    state.sinceFrame  = gRungSince.load(std::memory_order_relaxed);
    return state;
}

#ifdef TEST
void setFault(Fault fault) {
    gFault.store(static_cast<int>(fault), std::memory_order_relaxed);
    utils::logger().info("[vvsr] fault injection -> {}", static_cast<int>(fault));
}

Fault fault() {
    return static_cast<Fault>(gFault.load(std::memory_order_relaxed));
}
#endif

void syncConfig() {
    bool const wanted = config::get().sr.vvEnabled;
    if (wanted != gEnabled.load(std::memory_order_relaxed)) {
        setEnabled(wanted);
    }
}

// The per-frame TAAContext parameters that are still consumed: the sub-pixel jitter fed
// to DLSS and, for the evidence lines, the camera-origin delta. The motion vectors
// themselves are the engine's arg11 (stage C) - the struct no longer carries a
// view-projection packing and the prep shader has no MV constant buffer.
struct MvFrame {
    float jitterX{};
    float jitterY{};
    float delta[3]{};    ///< camera-origin delta (previous -> current); evidence only
    bool  reset{true};
    bool  valid{false};
    bool  prevSame{true}; ///< the two 16-float halves are identical (no camera motion,
                          ///< or the snapshot was not taken before the engine's draw)
};

MvFrame decodeMvFrame();

// The per-frame parameters that still come from the TAAContext: the sub-pixel jitter
// fed to DLSS, plus the camera-origin delta kept for the evidence lines. The motion
// vectors themselves are the engine's arg11 (stage C) - no view-projection decode and
// no near-plane reconstruction remains here.
MvFrame decodeMvFrame() {
    MvFrame frame{};
    float   cur[16]{};
    float   prev[16]{};
    float   cam[6]{};
    float   jitter[2]{};
    if (!probe::vv::taaMatrices(cur, prev, cam, jitter)) {
        return frame;
    }
    frame.prevSame = std::memcmp(cur, prev, sizeof(cur)) == 0;
    frame.delta[0] = cam[0] - cam[3];
    frame.delta[1] = cam[1] - cam[4];
    frame.delta[2] = cam[2] - cam[5];
    frame.jitterX  = jitter[0];
    frame.jitterY  = jitter[1];
    frame.valid    = true;
    return frame;
}

bool recordInternal(void* commandListVoid, void* arg10Void) {
    if (gReleased.load(std::memory_order_acquire)) {
        return false;
    }
    // Held for the whole record: release() waits here before freeing the pipeline.
    std::lock_guard recordLock(gRecordMutex);
    if (gReleased.load(std::memory_order_acquire)) {
        return false;
    }
    auto* const list   = static_cast<ID3D12GraphicsCommandList*>(commandListVoid);
    auto* const arg10  = static_cast<ID3D12Resource*>(arg10Void);
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    if (!list || !arg10 || !device) {
        return false;
    }
    // World presence is deliberately not a gate: since the user's 2026-10-28 decision the
    // pipeline keeps running after leaving a save, so the menu background is processed by
    // the same DLSS -> arg10 path instead of falling back to TAAU.
    static std::atomic<unsigned> srFrames{0};
    unsigned const frameNo = srFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    // Feature-level ladder: EngineTaau means "give the frame back to TAAU". It is not
    // a permanent state - after the cooldown one frame tries Full again, and a real
    // success resets the counters (the rung itself only moves back on that retry, so
    // a single good frame does not oscillate between TAAU and DLSS).
    if (static_cast<Rung>(gRung.load(std::memory_order_relaxed)) == Rung::EngineTaau) {
        unsigned const since = gRungSince.load(std::memory_order_relaxed);
        if (frameNo - since < kRetryFrames) {
            return false;
        }
        setRung(Rung::Full, RungReason::Retry, frameNo);
    }
    // TEST: `vv_sr_spatial.txt` in the game directory forces a purely spatial
    // evaluate (zero jitter/motion, Reset every frame). The user's 2 px seam appears
    // only with SR+DLSS and only in motion, while every single dumped frame (engine
    // colour, DLSS output, final write-back) is clean - so it lives in the temporal
    // path. This toggles that path off at runtime, from the agent side.
    bool spatial = false;
#ifdef TEST
    static std::atomic<bool> spatialMode{false};
    if (frameNo % 30 == 1) {
        std::error_code error;
        spatialMode.store(
            std::filesystem::exists(
                std::filesystem::path(utils::applicationDirectory()) / "vv_sr_spatial.txt",
                error
            ),
            std::memory_order_relaxed
        );
    }
    spatial = spatialMode.load(std::memory_order_relaxed);
#endif
    unsigned       renderWidth  = probe::vv::renderWidth();
    unsigned       renderHeight = probe::vv::renderHeight();
    D3D12_RESOURCE_DESC const sizeDesc = arg10->GetDesc();
    unsigned const displayWidth  = static_cast<unsigned>(sizeDesc.Width);
    unsigned const displayHeight = sizeDesc.Height;
    if ((!renderWidth || !renderHeight) && displayWidth && displayHeight) {
        // Bilinear never runs the taau declarator, which is the only thing that used
        // to fill the render size. The engine still creates its render-resolution
        // colour target, so take the newest fmt=26 that is smaller than the display
        // and matches its aspect.
        if (probe::vv::renderSizeHint(displayWidth, displayHeight, &renderWidth, &renderHeight)) {
            probe::vv::latchRenderSize(renderWidth, renderHeight);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                ND_DEBUG(
                    "[vvsr] sr: TAAU is not running; render size from the inventory = {}x{}",
                    renderWidth,
                    renderHeight
                );
            }
        }
    }
    if (!renderWidth || !renderHeight || !displayWidth || !displayHeight) {
        return false;
    }
    if (!ensurePipeline(device) || !ensureResources(device, renderWidth, renderHeight, displayWidth, displayHeight)) {
        return false;
    }
    void*    color       = nullptr;
    void*    depth       = nullptr;
    unsigned colorFormat = 0;
    unsigned depthFormat = 0;
    if (!pickInputs(&color, &colorFormat, &depth, &depthFormat)) {
        // The engine creates its render graph before the mod loads, so the RTV
        // inventory is empty until it rebuilds once (F11 / fullscreen toggle). Say it
        // instead of failing mute.
        static std::atomic<unsigned> warned{0};
        if (warned.fetch_add(1) < 3) {
            utils::logger().warn(
                "[vvsr] sr: no render-resolution colour/depth in the inventory yet"
                " - press F11 once (fullscreen toggle) so the engine rebuilds its render graph"
            );
        }
        return false;
    }
    auto* const colorTexture = static_cast<ID3D12Resource*>(color);
    auto* const depthTexture = static_cast<ID3D12Resource*>(depth);

    // --- descriptors ------------------------------------------------------
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    srv.Format                  = DXGI_FORMAT_R11G11B10_FLOAT;
    device->CreateShaderResourceView(colorTexture, &srv, cpuAt(kSlotEngineColor));

    // The depth resource may be typeless (43) or already D24S8 (44); the SRV format
    // has to stay in the family, and the read is .r = depth either way.
    D3D12_RESOURCE_DESC const depthDesc = depthTexture->GetDesc();
    switch (depthDesc.Format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // the read is .r = depth
        break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        break;
    default:
        srv.Format = depthDesc.Format;
        break;
    }
    device->CreateShaderResourceView(depthTexture, &srv, cpuAt(kSlotEngineDepth));

    // R3 stage C: arg11 is the declarator's role for the engine motion vectors. Its own
    // desc decides the SRV family. No barrier is issued for it, exactly like the engine
    // colour above: the TAAU draw already bound it as a shader resource this frame, and
    // a guessed StateBefore is worse than the state the engine left behind.
    void* const    mvResource = probe::vv::motionVectorResource();
    unsigned       prepMode   = 0;
    unsigned       mvFormat   = 0;
    if (mvResource) {
        auto* const               mvTexture = static_cast<ID3D12Resource*>(mvResource);
        D3D12_RESOURCE_DESC const mvDesc    = mvTexture->GetDesc();
        mvFormat                            = static_cast<unsigned>(mvDesc.Format);
        DXGI_FORMAT srvFormat               = DXGI_FORMAT_UNKNOWN;
        bool        fourChannel             = false;
        if (mvDesc.Width == renderWidth && mvDesc.Height == renderHeight
            && engineMotionVectorSrvFormat(mvDesc.Format, &srvFormat, &fourChannel)) {
            srv.Format = srvFormat;
            device->CreateShaderResourceView(mvTexture, &srv, cpuAt(kSlotEngineMv));
            prepMode = 1u | (fourChannel ? 2u : 0u);
        }
    }
    {
        static std::atomic<unsigned> mvLogs{0};
        unsigned const               n = mvLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            if (prepMode) {
                ND_DEBUG(
                    "[vvsr] sr mv: engine arg11={:#x} fmt={} layout={} ({}x{}, pixels current->previous)",
                    reinterpret_cast<std::uintptr_t>(mvResource),
                    mvFormat,
                    (prepMode & 2u) ? "zw" : "xy",
                    renderWidth,
                    renderHeight
                );
            } else {
                ND_DEBUG(
                    "[vvsr] sr mv: arg11={:#x} fmt={} unusable at {}x{}",
                    reinterpret_cast<std::uintptr_t>(mvResource),
                    mvFormat,
                    renderWidth,
                    renderHeight
                );
            }
        }
    }
    // R3 stage C: both render paths must carry the engine's own motion vectors. After a
    // resize the captured arg11 can still be the old size while the record already runs
    // at the new one; feeding zero motion (or the deleted reconstructed pair) is exactly
    // the failure this stage exists to remove. Skip the frame instead - the injector
    // already treats a failed record as "do nothing", and the engine's TAAU output
    // stays on screen.
    if (prepMode == 0) {
        static std::atomic<unsigned> skippedMv{0};
        unsigned const               n = skippedMv.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            utils::logger().warn(
                "[vvsr] sr mv: no usable arg11 for {}x{} (native={:#x} fmt={}); frame skipped",
                renderWidth,
                renderHeight,
                reinterpret_cast<std::uintptr_t>(mvResource),
                mvFormat
            );
        }
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC texSrv{};
    texSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    texSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texSrv.Texture2D.MipLevels     = 1;
    texSrv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(gPipeline.colorHalf, &texSrv, cpuAt(kSlotColorSrv));
    texSrv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(gPipeline.depthR32, &texSrv, cpuAt(kSlotDepthSrv));
    texSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateShaderResourceView(gPipeline.mvRg32, &texSrv, cpuAt(kSlotMvSrv));
    // Slot 5 is reserved for the DLSS output; the colour goes there for now so the
    // table is complete before that exists.
    texSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(gPipeline.colorHalf, &texSrv, cpuAt(kSlotDlssSrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(gPipeline.colorHalf, nullptr, &uav, cpuAt(kSlotColorUav));
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(gPipeline.depthR32, nullptr, &uav, cpuAt(kSlotDepthUav));
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateUnorderedAccessView(gPipeline.mvRg32, nullptr, &uav, cpuAt(kSlotMvUav));

    // --- prep: engine colour + depth -> RGBA16F / R32F / RG16F -------------
    transition(list, gPipeline.colorHalf, gPipeline.colorState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, gPipeline.depthR32, gPipeline.depthState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.depthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, gPipeline.mvRg32, gPipeline.mvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.mvState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[1] = {gPipeline.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(gPipeline.root);
    list->SetComputeRootDescriptorTable(1, gpuAt(kSlotEngineColor));
    list->SetComputeRootDescriptorTable(2, gpuAt(kSlotColorUav));
    list->SetPipelineState(gPipeline.prep);
    unsigned const prepParams[4] = {renderWidth, renderHeight, prepMode | (spatial ? 4u : 0u), 0};
    list->SetComputeRoot32BitConstants(0, 4, prepParams, 0);
    MvFrame mv = decodeMvFrame();
    list->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

    // The DLSS evaluate reads the prepared buffers, so they become SRVs here. The
    // NR detour's encode/composite assume ALL_SHADER_RESOURCE and restore it
    // themselves, so the inputs are published in that state unconditionally: it is
    // valid for DLSS too, and it removes the toggle-frame disagreement between this
    // transition and the detour's own `nr.enabled` read.
    D3D12_RESOURCE_STATES const srInputState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    transition(list, gPipeline.colorHalf, gPipeline.colorState, srInputState);
    gPipeline.colorState = srInputState;
    transition(list, gPipeline.depthR32, gPipeline.depthState, srInputState);
    gPipeline.depthState = srInputState;
    transition(list, gPipeline.mvRg32, gPipeline.mvState, srInputState);
    gPipeline.mvState = srInputState;

    bool dlssEvaluated = false;
    bool featureReady  = false;
    bool sharpened     = false;
    int  dlssCreateRet = 0;
    int  dlssEvalRet   = 0;
    {
        // DLSS-SR: our prepared colour/depth/motion are already in a shader-readable
        // state above, which is what NGX expects of its inputs.
        featureReady = dlss::ensureSrFeature(device, list, renderWidth, renderHeight, displayWidth, displayHeight);
        if (featureReady) {
            transition(list, gPipeline.dlssOut, gPipeline.dlssState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gPipeline.dlssState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            // V1: no jitter, zero motion, Reset every frame -> spatially correct, no
            // ghosting, and it proves the evaluate path before temporal accumulation.
            // Reset only where history cannot be trusted: the first frames of a
            // session, after a world change and after a resolution change.
            static std::atomic<unsigned> resetFrames{2};
            static std::atomic<bool>     lastWorld{false};
            static std::atomic<unsigned> lastGeneration{0};
            bool const                   world      = probe::vv::worldLoaded();
            unsigned const               generation = probe::vv::renderGeneration();
            if (frameNo <= 2 || world != lastWorld.load(std::memory_order_relaxed)
                || generation != lastGeneration.load(std::memory_order_relaxed)) {
                resetFrames.store(2, std::memory_order_relaxed);
                lastWorld.store(world, std::memory_order_relaxed);
                lastGeneration.store(generation, std::memory_order_relaxed);
            }
            unsigned const pending = resetFrames.load(std::memory_order_relaxed);
            if (pending > 0) {
                resetFrames.store(pending - 1, std::memory_order_relaxed);
            }
            bool const gap = probe::vv::takeArg10Gap();
            // A panel reset request ("clear history") uses the same two-frame window as
            // a world or size change.
            unsigned const manual = gManualReset.load(std::memory_order_relaxed);
            if (manual > 0) {
                gManualReset.store(manual - 1, std::memory_order_relaxed);
            }
            // Outside a world (menu / title screen) the panorama is not rendered with
            // the world TAAU jitter, so feeding it shifts the image every frame - that
            // was the up-down judder (EXP-0073). History is allowed to accumulate again
            // (EXP-0074); if the panorama's MV does not describe its rotation this
            // smears, which is what the next run checks.
            bool const reset = spatial || pending > 0 || manual > 0 || !mv.valid || gap;
            // NR is driven by the RT probe's EvaluateFeature detour: it sees this
            // call's Color/Depth/MotionVectors from the parameter object, records
            // feature 0x12 on this same command list, and points Color at its
            // composite before the original evaluate runs. The VV side only has to
            // leave the prepared inputs in ALL_SHADER_RESOURCE (above).
            // Jitter handed to DLSS. Default 0 (see gJitterSource); the other three modes
            // are the §7 discrimination the panel still offers. Only applied inside a
            // world: outside it the panorama was not rendered with the world jitter and
            // feeding it back shifts the image every frame (EXP-0073).
            float jitterX = 0.0f;
            float jitterY = 0.0f;
            if (world) {
                switch (gJitterSource.load(std::memory_order_relaxed)) {
                case static_cast<int>(JitterSource::Engine):
                    jitterX = mv.jitterX;
                    jitterY = mv.jitterY;
                    break;
                case static_cast<int>(JitterSource::Negated):
                    jitterX = -mv.jitterX;
                    jitterY = -mv.jitterY;
                    break;
                case static_cast<int>(JitterSource::Previous):
                    // j(n-1): the value saved by the frame before this one. Zero until a
                    // frame has been saved, which is the intended fallback for the first
                    // recorded frame (and after a release/reload).
                    jitterX = gPrevJitterX;
                    jitterY = gPrevJitterY;
                    break;
                default: break;
                }
            }
            // Saved every recorded frame, whatever the mode: Previous needs it on the
            // next one, and saving it only while Previous is selected would feed a stale
            // value across a mode switch.
            gPrevJitterX = mv.jitterX;
            gPrevJitterY = mv.jitterY;
            dlssEvaluated = dlss::evaluateSr(
                list,
                gPipeline.colorHalf,
                gPipeline.depthR32,
                gPipeline.mvRg32,
                gPipeline.dlssOut,
                jitterX,
                jitterY,
                reset
            );
            dlss::SrState const state = dlss::srState();
            dlssCreateRet = state.createRet;
            dlssEvalRet   = state.evaluateRet;
            transition(list, gPipeline.dlssOut, gPipeline.dlssState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gPipeline.dlssState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (dlssEvaluated) {
                D3D12_SHADER_RESOURCE_VIEW_DESC dlssSrv{};
                dlssSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                dlssSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                dlssSrv.Texture2D.MipLevels     = 1;
                dlssSrv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateShaderResourceView(gPipeline.dlssOut, &dlssSrv, cpuAt(kSlotColorSrv));
            }
            // RCAS on the DLSS output, before it is written into arg10 (docs/specs/sr.md
            // §8): both textures are ours, so the states are the ones this function
            // already tracks, and the engine-path entry point is skipped for this call
            // (see OwnEvaluateScope). A refused pass leaves dlssOut as the source, i.e.
            // the frame is simply unsharpened. The ladder's NoSharp rung takes the same
            // path: the frame stays unsharpened without touching the config.
            if (dlssEvaluated
                && static_cast<Rung>(gRung.load(std::memory_order_relaxed)) < Rung::NoSharp) {
                transition(
                    list,
                    gPipeline.sharpOut,
                    gPipeline.sharpState,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                );
                gPipeline.sharpState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                sharpened            = neurodragon::sr::recordSharpeningInto(
                    list,
                    gPipeline.dlssOut,
                    gPipeline.sharpOut,
                    displayWidth,
                    displayHeight,
                    frameNo
                );
                if (sharpened) {
                    transition(
                        list,
                        gPipeline.sharpOut,
                        gPipeline.sharpState,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                    );
                    gPipeline.sharpState            = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    D3D12_SHADER_RESOURCE_VIEW_DESC sharpSrv{};
                    sharpSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sharpSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sharpSrv.Texture2D.MipLevels     = 1;
                    sharpSrv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    // Same slot the unsharpened path uses: the PS reads t0 either way.
                    device->CreateShaderResourceView(gPipeline.sharpOut, &sharpSrv, cpuAt(kSlotColorSrv));
                }
            }
        }
    }
    // Ladder accounting (docs/specs/vv.md §38). The TEST switch fakes the two NGX
    // outcomes on the same path, so the escalation is exercised by the real code
    // rather than by a parallel branch: a "failure" here also skips the arg10 write,
    // which is exactly what the user sees when the feature really fails.
#ifdef TEST
    {
        int const faultNow = gFault.load(std::memory_order_relaxed);
        if (faultNow == static_cast<int>(Fault::Create) && featureReady) {
            featureReady  = false;
            dlssCreateRet = -1;
        } else if (faultNow == static_cast<int>(Fault::Eval) && dlssEvaluated) {
            dlssEvaluated = false;
            dlssEvalRet   = 0;
        }
    }
#endif
    if (featureReady && dlssEvaluated) {
        noteSuccess(frameNo);
    } else if (!featureReady) {
        noteCreateFailure(frameNo);
    } else {
        noteEvalFailure(frameNo);
    }
    if (arg10) {
        if (!dlssEvaluated) {
            return false;
        }
        D3D12_RESOURCE_DESC const targetDesc = arg10->GetDesc();
        if (targetDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT) {
            static std::atomic<unsigned> warned{0};
            if (warned.fetch_add(1, std::memory_order_relaxed) < 3) {
                utils::logger().warn(
                    "[vvsr] sr arg10: unexpected target format {}",
                    static_cast<unsigned>(targetDesc.Format)
                );
            }
            return false;
        }
        // Stage A proved arg10 is left in RENDER_TARGET by its writer list, so this
        // pass only binds it and leaves the state alone. The source texture was last
        // in a compute-read state; a pixel shader needs its own - except when RCAS
        // ran, where sharpOut was already moved to PIXEL_SHADER_RESOURCE above.
        if (!sharpened) {
            transition(list, gPipeline.dlssOut, gPipeline.dlssState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            gPipeline.dlssState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        probe::vv::inject::setRecordingOwnView(true);
        device->CreateRenderTargetView(arg10, nullptr, gPipeline.arg10Rtv);
        probe::vv::inject::setRecordingOwnView(false);
        D3D12_VIEWPORT const viewport{
            0.0f,
            0.0f,
            static_cast<float>(displayWidth),
            static_cast<float>(displayHeight),
            0.0f,
            1.0f
        };
        D3D12_RECT const scissor{
            0,
            0,
            static_cast<LONG>(displayWidth),
            static_cast<LONG>(displayHeight)
        };
        ID3D12DescriptorHeap* heaps[1] = {gPipeline.heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(gPipeline.root);
        // The DLSS output was written to descriptor slot 2 by the evaluate branch;
        // the graphics table starts there, so the PS sees it as t0.
        list->SetGraphicsRootDescriptorTable(1, gpuAt(kSlotColorSrv));
        list->OMSetRenderTargets(1, &gPipeline.arg10Rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetPipelineState(gPipeline.arg10Ps);
        list->DrawInstanced(3, 1, 0, 0);

        std::lock_guard lock(gMutex);
        gStats.recorded        = true;
        gStats.frame           = frameNo;
        gStats.dlssReady       = true;
        gStats.dlssCreateRet   = dlssCreateRet;
        gStats.dlssEvaluateRet = dlssEvalRet;
        gStats.renderWidth     = renderWidth;
        gStats.renderHeight    = renderHeight;
        gStats.displayWidth    = displayWidth;
        gStats.displayHeight   = displayHeight;
        gStats.colorSource     = color;
        gStats.depthSource     = depth;
        gStats.colorFormat     = colorFormat;
        gStats.depthFormat     = depthFormat;
        gStats.mvSource        = prepMode ? mvResource : nullptr;
        gStats.mvFormat        = prepMode ? mvFormat : 0;
        gStats.colorCandidates = gColorCandidates;
        gStats.depthCandidates = gDepthCandidates;
        gStats.lastError       = 0;
        static std::atomic<unsigned> frames{0};
        unsigned const               n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            ND_DEBUG(
                "[vvsr] sr arg10 #{}: {}x{} fmt={} eval={} mv={} sharp={}",
                n,
                displayWidth,
                displayHeight,
                static_cast<unsigned>(targetDesc.Format),
                dlssEvalRet,
                prepMode ? "engine" : "vp",
                sharpened ? "on" : "off"
            );
        }
        gLive.store(true, std::memory_order_release);
        return true;
    }
    // R3 writes arg10 only; there is no back-buffer path left (EXP-0067).
    return false;
}

bool recordArg10(void* commandList, void* arg10) {
    return recordInternal(commandList, arg10);
}

Stats stats() {
    std::lock_guard lock(gMutex);
    return gStats;
}

void release() {
    // Stop new records first, then wait for the one that may already be running. The
    // detour/NR state is left to nrShutdown(); here only our own pipeline goes away.
    gLive.store(false, std::memory_order_release);
    gReleased.store(true, std::memory_order_release);
    std::lock_guard recordLock(gRecordMutex);
    std::lock_guard lock(gMutex);
    for (ID3D12Resource** resource :
         {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.dlssOut, &gPipeline.sharpOut}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    if (gPipeline.heap) gPipeline.heap->Release();
    if (gPipeline.prep) gPipeline.prep->Release();
    if (gPipeline.root) gPipeline.root->Release();
    if (gPipeline.arg10Ps) gPipeline.arg10Ps->Release();
    if (gPipeline.rtvHeap) gPipeline.rtvHeap->Release();
    gPipeline = Pipeline{};
    gStats    = Stats{};
    // Drop the saved frame so a reload starts the Previous mode from its documented
    // fallback (zero) instead of the last frame of the previous session.
    gPrevJitterX = 0.0f;
    gPrevJitterY = 0.0f;
}

} // namespace neurodragon::vv::sr
