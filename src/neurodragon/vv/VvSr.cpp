#include "neurodragon/vv/VvSr.h"

#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvProbe.h"
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
// Diagnostics: arg11's four channels, copied verbatim at prep time so a dump shows what
// the engine actually wrote, not what a transient target holds at present.
RWTexture2D<float4> OutMvRaw : register(u5);

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
    OutMvRaw[id.xy] = engineMv;
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
    kSlotMvRawUav    = 13, ///< R3 stage C: arg11's four channels, copied at prep time
    kSlotCount       = 14,
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
    ID3D12Resource*             mvRaw{};  ///< RGBA16F copy of arg11's four channels (diagnostic)
    ID3D12Resource*             dlssOut{};
    ID3D12Resource*             taauRef{}; ///< R11G11B10 copy of arg10 before our PS write (F2)
    D3D12_RESOURCE_STATES       colorState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       depthState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       mvState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       mvRawState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       dlssState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       taauRefState{D3D12_RESOURCE_STATE_COMMON};
    unsigned                    renderWidth{};
    unsigned                    renderHeight{};
    unsigned                    displayWidth{};
    unsigned                    displayHeight{};
};

std::mutex       gMutex;
Pipeline         gPipeline{};
Stats            gStats{};
std::atomic<int> gColorChoice{0};
std::atomic<int> gDepthChoice{0};
// One number per dump so a repeated trigger (panel button, file marker, F7 pair) keeps
// every frame instead of overwriting the previous one. File scope because the TEST
// hotkey path has to be able to name the run it just triggered.
std::atomic<unsigned> gDumpNo{0};
// F2: copy this frame's TAAU output (arg10, before our PS write) into taauRef, so the
// engine's result and the DLSS result of the *same* frame can be compared offline.
std::atomic<bool> gCaptureTaaRef{false};
std::atomic<bool> gTaaRefFresh{false};
unsigned         gColorCandidates = 0;
unsigned         gDepthCandidates = 0;

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
        utils::logger().warn("[vvsr] sr {} failed hr=0x{:08X}", where, static_cast<unsigned>(hr));
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
         {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.mvRaw, &gPipeline.dlssOut,
          &gPipeline.taauRef}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    gPipeline.colorHalf   = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    gPipeline.depthR32    = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
    gPipeline.mvRg32      = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT);
    gPipeline.mvRaw       = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    gPipeline.dlssOut     = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Same format as arg10 (fmt 26) so CopyResource is a straight texel copy; display
    // size, used only by the explicit F2 pair dump.
    gPipeline.taauRef     = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R11G11B10_FLOAT);
    if (!gPipeline.colorHalf || !gPipeline.depthR32 || !gPipeline.mvRg32 || !gPipeline.mvRaw
        || !gPipeline.dlssOut || !gPipeline.taauRef) {
        setError(E_OUTOFMEMORY, "CreateCommittedResource");
        return false;
    }
    gPipeline.colorState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.depthState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.mvState       = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.mvRawState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.dlssState     = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.taauRefState  = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.renderWidth   = renderWidth;
    gPipeline.renderHeight  = renderHeight;
    gPipeline.displayWidth  = displayWidth;
    gPipeline.displayHeight = displayHeight;
    utils::logger().info("[vvsr] sr buffers {}x{} -> {}x{}", renderWidth, renderHeight, displayWidth, displayHeight);
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
    utils::logger().info("[vvsr] sr pipeline ready (prep + DLSS -> arg10)");
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
        unsigned const choice = static_cast<unsigned>(gColorChoice.load(std::memory_order_relaxed)) % colorsCount;
        *colorOut    = colors[choice].resource;
        *colorFormat = colors[choice].format;
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 3) {
            for (unsigned i = 0; i < colorsCount; ++i) {
                utils::logger().info(
                    "[vvsr] sr colour[{}]={:#x} {}x{} fmt={}",
                    i,
                    reinterpret_cast<std::uintptr_t>(colors[i].resource),
                    colors[i].width,
                    colors[i].height,
                    colors[i].format
                );
            }
        }
    }

    auto const semantic = probe::vv::inputFinalColorTarget();
    if (semantic.resource && semantic.format == 26 && semantic.width == probe::vv::renderWidth()
        && semantic.height == probe::vv::renderHeight()) {
        *colorOut    = semantic.resource;
        *colorFormat = semantic.format;
        static std::atomic<unsigned> loggedSemantic{0};
        if (loggedSemantic.fetch_add(1) < 6) {
            utils::logger().info(
                "[vvsr] sr semantic colour={:#x} {}x{} fmt={} (arg9 InputFinalColor)",
                reinterpret_cast<std::uintptr_t>(semantic.resource),
                semantic.width,
                semantic.height,
                semantic.format
            );
        }
    }

    unsigned const depthCount = probe::vv::depthTargets(targets, 48, 64);
    {
        // One line that says which side is empty when the pick fails - the panel only
        // shows zeros, and "0 of 0" cannot distinguish "inventory empty" from "size
        // mismatch" (the render size follows the engine's factor, so after a resize it
        // can be 1689x903 while the views are still 1689x950).
        static std::atomic<unsigned> diag{0};
        if (diag.fetch_add(1, std::memory_order_relaxed) < 5) {
            utils::logger().info(
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
        unsigned const choice = static_cast<unsigned>(gDepthChoice.load(std::memory_order_relaxed)) % depthsCount;
        *depthOut    = depths[choice].resource;
        *depthFormat = depths[choice].format;
        static std::atomic<unsigned> loggedD{0};
        if (loggedD.fetch_add(1) < 3) {
            for (unsigned i = 0; i < depthsCount; ++i) {
                utils::logger().info(
                    "[vvsr] sr depth[{}]={:#x} {}x{} fmt={}",
                    i,
                    reinterpret_cast<std::uintptr_t>(depths[i].resource),
                    depths[i].width,
                    depths[i].height,
                    depths[i].format
                );
            }
        }
    }
    return *colorOut && *depthOut;
}

} // namespace

void cycleColor() {
    int const next = gColorChoice.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info("[vvsr] sr colour choice -> {}", next);
}
void cycleDepth() {
    int const next = gDepthChoice.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info("[vvsr] sr depth choice -> {}", next);
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

unsigned dumpRuns() { return gDumpNo.load(std::memory_order_relaxed); }

void captureTaaRef() {
    gCaptureTaaRef.store(true, std::memory_order_release);
    utils::logger().info("[vvsr] sr f2: TAAU reference capture armed (next injected frame)");
}

bool dumpInputs() {
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    auto* const queue  = static_cast<ID3D12CommandQueue*>(probe::vv::gameQueue());
    Stats const snapshot = stats();
    if (!device || !queue || !snapshot.colorSource || !snapshot.depthSource) {
        return false;
    }
    unsigned const run = gDumpNo.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info(
        "[vvsr] sr dump #{} frame={} dlssReady={} start", run, snapshot.frame, snapshot.dlssReady ? 1 : 0
    );
    // Per-batch motion state. A pair of dumps is only evidence if the two frames can be
    // told apart, and the camera delta is the part the user cannot fake by reporting
    // "I kept turning". No throttle: every batch gets its own line.
    MvFrame const mv = decodeMvFrame();
    utils::logger().info(
        "[vvsr] sr dump #{} frame={} mv valid={} prev_same={} delta=({:+.4f},{:+.4f},{:+.4f}) jitter=({:+.4f},{:+.4f})",
        run,
        snapshot.frame,
        mv.valid ? 1 : 0,
        mv.prevSame ? 1 : 0,
        mv.delta[0],
        mv.delta[1],
        mv.delta[2],
        mv.jitterX,
        mv.jitterY
    );
    std::wstring const directory = utils::applicationDirectory();
    wchar_t            name[192]{};
    // The run number is in every name, the picked pair included: the open question is
    // answered by comparing two dumps taken from two camera positions, and a name
    // without it lets the second batch overwrite the first.
    std::swprintf(
        name,
        std::size(name),
        L"sr_colour_%u_p%llX_%ux%u_fmt%u.bin",
        run,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(snapshot.colorSource)),
        snapshot.renderWidth,
        snapshot.renderHeight,
        snapshot.colorFormat
    );
    bool const colorOk = probe::vv::dumpTextureRaw(device, queue, snapshot.colorSource, (directory + L"\\" + name).c_str());
    std::swprintf(
        name,
        std::size(name),
        L"sr_depth_%u_p%llX_%ux%u_fmt%u.bin",
        run,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(snapshot.depthSource)),
        snapshot.renderWidth,
        snapshot.renderHeight,
        snapshot.depthFormat
    );
    bool const depthOk = probe::vv::dumpTextureRaw(device, queue, snapshot.depthSource, (directory + L"\\" + name).c_str());
    void* const semanticColor = probe::vv::inputFinalColorResource();
    bool        semanticOk    = false;
    if (semanticColor) {
        std::swprintf(
            name,
            std::size(name),
            L"sr_semantic_%u_p%llX.bin",
            run,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(semanticColor))
        );
        semanticOk = probe::vv::dumpTextureRaw(device, queue, semanticColor, (directory + L"\\" + name).c_str());
    }
    utils::logger().info(
        "[vvsr] sr dump #{} semantic colour={:#x} ok={}",
        run,
        reinterpret_cast<std::uintptr_t>(semanticColor),
        semanticOk ? 1 : 0
    );
    bool midOk      = false;
    bool mvOk       = false;
    bool depthR32Ok = false;
    bool dlssOk     = false;
    // Our own textures are left in NON_PIXEL_SHADER_RESOURCE by the record path; the
    // 2026-10-26 dump read the MV as all zeros and the depth as all 1.0 with the
    // Common hint, which is the stale-byte failure the DumpState comment describes.
    auto const state = probe::vv::DumpState::NonPixelShaderResource;
    if (gPipeline.colorHalf) {
        std::swprintf(name, std::size(name), L"sr_mid_colourhalf_%u.bin", run);
        midOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.colorHalf, (directory + L"\\" + name).c_str(), state);
    }
    // The three textures that decide the reprojection: the prepared motion vectors
    // (RG16F), the prepared linear depth (R32F) and DLSS's own output. A localised
    // seam is either a bad value in one of them or the frame is clean and the problem
    // is elsewhere - this dump is what tells those apart.
    if (gPipeline.mvRg32) {
        std::swprintf(name, std::size(name), L"sr_mv_rg32_%ux%u_%u.bin", snapshot.renderWidth, snapshot.renderHeight, run);
        mvOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.mvRg32, (directory + L"\\" + name).c_str(), state);
    }
    if (gPipeline.depthR32) {
        std::swprintf(name, std::size(name), L"sr_depth_r32_%ux%u_%u.bin", snapshot.renderWidth, snapshot.renderHeight, run);
        depthR32Ok = probe::vv::dumpTextureRawAs(device, queue, gPipeline.depthR32, (directory + L"\\" + name).c_str(), state);
    }
    if (gPipeline.dlssOut) {
        std::swprintf(
            name,
            std::size(name),
            L"sr_dlssout_%ux%u_%u.bin",
            snapshot.displayWidth,
            snapshot.displayHeight,
            run
        );
        dlssOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.dlssOut, (directory + L"\\" + name).c_str(), state);
    }
    // F2: the TAAU output of the frame the explicit capture was armed on (arg10 before
    // our PS write). Present only after that button was pressed; one dump per capture.
    bool taaurefOk = false;
    if (gPipeline.taauRef && gTaaRefFresh.exchange(false, std::memory_order_acq_rel)) {
        std::swprintf(
            name,
            std::size(name),
            L"sr_taauref_%ux%u_%u.bin",
            snapshot.displayWidth,
            snapshot.displayHeight,
            run
        );
        taaurefOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.taauRef, (directory + L"\\" + name).c_str(), state);
    }
    utils::logger().info(
        "[vvsr] sr dump #{} inputs colour={} depth={} mid={} mv={} depth32={} dlss={} taauref={}",
        run,
        colorOk ? 1 : 0,
        depthOk ? 1 : 0,
        midOk ? 1 : 0,
        mvOk ? 1 : 0,
        depthR32Ok ? 1 : 0,
        dlssOk ? 1 : 0,
        taaurefOk ? 1 : 0
    );
    // Every candidate the pick path can see, not just the picked one (EXP-0026). The
    // only open question is whether the picked colour stops changing once the engine's
    // TAAU draw is suppressed, and that is answered by comparing two batches taken from
    // two camera positions. The picked pointer alone cannot separate "this texture is
    // stale" from "pickInputs chose the wrong texture" - EXP-0025 had 15 live fmt26
    // candidates and kept one of them. tolerance=64 is pickInputs' own value, so the
    // picked texture is guaranteed to be inside the batch (and its index is logged);
    // the pick behaviour itself is untouched.
    probe::vv::RenderTargetInfo targets[48]{};
    probe::vv::RenderTargetInfo colours[16]{};
    probe::vv::RenderTargetInfo depths[16]{};
    unsigned const targetCount = probe::vv::renderTargets(targets, 48, 64);
    unsigned       colourCount = 0;
    for (unsigned i = 0; i < targetCount && colourCount < 16; ++i) {
        if (targets[i].format == 26) {
            colours[colourCount++] = targets[i];
        }
    }
    unsigned const targetDepthCount = probe::vv::depthTargets(targets, 48, 64);
    unsigned       depthCount       = 0;
    for (unsigned i = 0; i < targetDepthCount && depthCount < 16; ++i) {
        unsigned const format = targets[i].format;
        // The same format set pickInputs accepts, so index i is the candidate the
        // panel's depth choice would walk.
        if (format == 43 || format == 44 || format == 45 || format == 40 || format == 39 || format == 54
            || format == 41) {
            depths[depthCount++] = targets[i];
        }
    }
    // Which candidate the SR pass actually read. -1 means the pick is not in this
    // batch (capped at 16, or the inventory changed between the record and the dump) -
    // that has to be read as "cannot compare this batch", not as a stale colour.
    int pickedColourIndex = -1;
    for (unsigned i = 0; i < colourCount; ++i) {
        if (colours[i].resource == snapshot.colorSource) {
            pickedColourIndex = static_cast<int>(i);
            break;
        }
    }
    int pickedDepthIndex = -1;
    for (unsigned i = 0; i < depthCount; ++i) {
        if (depths[i].resource == snapshot.depthSource) {
            pickedDepthIndex = static_cast<int>(i);
            break;
        }
    }
    unsigned colourDumped = 0;
    for (unsigned i = 0; i < colourCount; ++i) {
        std::swprintf(
            name,
            std::size(name),
            L"sr_cand_%u_colour_%u_p%llX_%ux%u_fmt%u.bin",
            run,
            i,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(colours[i].resource)),
            colours[i].width,
            colours[i].height,
            colours[i].format
        );
        bool const ok = probe::vv::dumpTextureRaw(device, queue, colours[i].resource, (directory + L"\\" + name).c_str());
        colourDumped += ok ? 1U : 0U;
        utils::logger().info(
            "[vvsr] sr dump #{} cand colour[{}]={:#x} {}x{} fmt={} ok={}",
            run,
            i,
            reinterpret_cast<std::uintptr_t>(colours[i].resource),
            colours[i].width,
            colours[i].height,
            colours[i].format,
            ok ? 1 : 0
        );
    }
    unsigned depthDumped = 0;
    for (unsigned i = 0; i < depthCount; ++i) {
        std::swprintf(
            name,
            std::size(name),
            L"sr_cand_%u_depth_%u_p%llX_%ux%u_fmt%u.bin",
            run,
            i,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(depths[i].resource)),
            depths[i].width,
            depths[i].height,
            depths[i].format
        );
        bool const ok = probe::vv::dumpTextureRaw(device, queue, depths[i].resource, (directory + L"\\" + name).c_str());
        depthDumped += ok ? 1U : 0U;
        utils::logger().info(
            "[vvsr] sr dump #{} cand depth[{}]={:#x} {}x{} fmt={} ok={}",
            run,
            i,
            reinterpret_cast<std::uintptr_t>(depths[i].resource),
            depths[i].width,
            depths[i].height,
            depths[i].format,
            ok ? 1 : 0
        );
    }
    utils::logger().info(
        "[vvsr] sr dump #{} frame={} cand summary colour ok={}/{} depth ok={}/{}"
        " pickedColour={:#x} idx={} pickedDepth={:#x} idx={}",
        run,
        snapshot.frame,
        colourDumped,
        colourCount,
        depthDumped,
        depthCount,
        reinterpret_cast<std::uintptr_t>(snapshot.colorSource),
        pickedColourIndex,
        reinterpret_cast<std::uintptr_t>(snapshot.depthSource),
        pickedDepthIndex
    );
    return colorOk && depthOk;
}

std::atomic<int>      gLastLevel{-1};
std::atomic<unsigned> gLevelLoggedAt{0};

// World presence for the SR gate. Event-driven (see VvProbe::worldLoaded): a poll of
// ll::service::getLevel() looked right for the first world but stayed empty after
// leaving and entering another one, so a gate built on it never reopened.
bool inWorld() {
    bool const     present = probe::vv::worldLoaded();
    int const      now     = present ? 1 : 0;
    unsigned const tick    = ::GetTickCount();
    int const      last    = gLastLevel.load(std::memory_order_relaxed);
    unsigned const at      = gLevelLoggedAt.load(std::memory_order_relaxed);
    bool const     changed = last != now;
    if (changed || tick - at >= 1000) {
        gLevelLoggedAt.store(tick, std::memory_order_relaxed);
        gLastLevel.store(now, std::memory_order_relaxed);
        utils::logger().info("[vvsr] world: getLevel()={} changed={}", now, changed ? 1 : 0);
    }
    return present;
}

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
    auto* const list   = static_cast<ID3D12GraphicsCommandList*>(commandListVoid);
    auto* const arg10  = static_cast<ID3D12Resource*>(arg10Void);
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    if (!list || !arg10 || !device) {
        return false;
    }
    // World presence used to gate recording (an empty list outside the world): a failed
    // record could kill the SR action after the world came back (2026-10-25). The
    // Close fix (EXP-0065) removed that hazard, and the user asked for the original VV
    // behaviour back: keep running DLSS after leaving a save, so the menu background
    // is processed by the same pipeline instead of falling back to TAAU. The call is
    // kept for its once-per-second world log; its result no longer decides anything.
    (void)inWorld();
    // Step 2 groundwork: the motion-vector pass needs the engine's current/previous
    // view-projection pair, the camera origins and the sub-pixel jitter. The layout is
    // documented (docs/specs/vv.md §6) but the packing convention is not, so one frame
    // after the camera has settled logs the raw numbers; the reprojection is written
    // against what that frame shows.
    static std::atomic<unsigned> srFrames{0};
    unsigned const frameNo = srFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    // TEST: `vv_sr_spatial.txt` in the game directory forces a purely spatial
    // evaluate (zero jitter/motion, Reset every frame). The user's 2 px seam appears
    // only with SR+DLSS and only in motion, while every single dumped frame (engine
    // colour, DLSS output, final write-back) is clean - so it lives in the temporal
    // path. This toggles that path off at runtime, from the agent side.
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
    bool const spatial = spatialMode.load(std::memory_order_relaxed);
    // Three labelled in-world windows: a single still frame cannot separate a
    // row-vector layout from a column-vector one (cur == prev), so the camera has to
    // move between samples (walk / look around during the first ~15 s).
    if (frameNo == 300 || frameNo == 900 || frameNo == 1500) {
        utils::logger().info("[vvsr] mv sample at frame {}", frameNo);
        probe::vv::dumpTaaContextWindows();
    }
    if (frameNo == 60) {
        MvFrame const probeFrame = decodeMvFrame();
        utils::logger().info(
            "[vvsr] mv decode valid={} prev_same={} jitter=({:+.4f},{:+.4f}) delta=({:+.4f},{:+.4f},{:+.4f})",
            probeFrame.valid ? 1 : 0,
            probeFrame.prevSame ? 1 : 0,
            probeFrame.jitterX,
            probeFrame.jitterY,
            probeFrame.delta[0],
            probeFrame.delta[1],
            probeFrame.delta[2]
        );
    }
    if (frameNo == 30) {
        float cur[16]{};
        float prev[16]{};
        float cam[6]{};
        float jitter[2]{};
        if (probe::vv::taaMatrices(cur, prev, cam, jitter)) {
            auto const text = [](float const* m) {
                std::string out;
                char        one[32]{};
                for (int i = 0; i < 16; ++i) {
                    std::snprintf(one, sizeof(one), " %+.5f", m[i]);
                    out += one;
                }
                return out;
            };
            utils::logger().info(
                "[vvsr] mv cam=({:+.4f},{:+.4f},{:+.4f}) prevCam=({:+.4f},{:+.4f},{:+.4f}) jitter=({:+.4f},{:+.4f})",
                cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], jitter[0], jitter[1]
            );
            utils::logger().info("[vvsr] mv cur=[{}]", text(cur));
            utils::logger().info("[vvsr] mv prev=[{}]", text(prev));
        } else {
            utils::logger().warn("[vvsr] mv: no TAAContext captured yet");
        }
    }
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
                utils::logger().info(
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
        // inventory is empty until it rebuilds once (F11 / fullscreen toggle) - the
        // same reason the B0 dumps ask for F11 first. Say it instead of failing mute.
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
        if (n <= 3 || n % 600 == 0) {
            if (prepMode) {
                utils::logger().info(
                    "[vvsr] sr mv: engine arg11={:#x} fmt={} layout={} ({}x{}, pixels current->previous)",
                    reinterpret_cast<std::uintptr_t>(mvResource),
                    mvFormat,
                    (prepMode & 2u) ? "zw" : "xy",
                    renderWidth,
                    renderHeight
                );
            } else {
                utils::logger().warn(
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
        if (n <= 3 || n % 120 == 0) {
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
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(gPipeline.mvRaw, nullptr, &uav, cpuAt(kSlotMvRawUav));

    // --- prep: engine colour + depth -> RGBA16F / R32F / RG32F -------------
    transition(list, gPipeline.colorHalf, gPipeline.colorState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, gPipeline.depthR32, gPipeline.depthState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.depthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, gPipeline.mvRg32, gPipeline.mvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.mvState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, gPipeline.mvRaw, gPipeline.mvRawState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.mvRawState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[1] = {gPipeline.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(gPipeline.root);
    list->SetComputeRootDescriptorTable(1, gpuAt(kSlotEngineColor));
    list->SetComputeRootDescriptorTable(2, gpuAt(kSlotColorUav));
    list->SetPipelineState(gPipeline.prep);
    unsigned const prepParams[4] = {renderWidth, renderHeight, prepMode | (spatial ? 4u : 0u), 0};
    list->SetComputeRoot32BitConstants(0, 4, prepParams, 0);
    MvFrame mv = decodeMvFrame();
    // One-shot proof that the draw-time snapshot is doing its job: with a moving camera
    // the previous half differs from the current one. The post-frame read this replaced
    // could never log this line (prev==cur both halves), so it doubles as a regression.
    {
        static std::atomic<bool> logged{false};
        if (mv.valid && !mv.prevSame && !logged.exchange(true, std::memory_order_relaxed)) {
            utils::logger().info(
                "[vvsr] mv snapshot: previous VP differs from current at frame {} (delta=({:+.4f},{:+.4f},{:+.4f}))",
                frameNo,
                mv.delta[0],
                mv.delta[1],
                mv.delta[2]
            );
        }
    }
    list->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

    // The DLSS evaluate reads the prepared buffers, so they become SRVs here.
    transition(list, gPipeline.colorHalf, gPipeline.colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.depthR32, gPipeline.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.mvRg32, gPipeline.mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.mvRaw, gPipeline.mvRawState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.mvRawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool dlssEvaluated = false;
    int  dlssCreateRet = 0;
    int  dlssEvalRet   = 0;
    {
        // DLSS-SR: our prepared colour/depth/motion are already in a shader-readable
        // state above, which is what NGX expects of its inputs.
        if (dlss::ensureSrFeature(device, list, renderWidth, renderHeight, displayWidth, displayHeight)) {
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
            // Outside a world (menu / title screen) the panorama is not rendered with
            // the world TAAU jitter, so feeding it shifts the image every frame - that
            // was the up-down judder (EXP-0073). History is allowed to accumulate again
            // (EXP-0074); if the panorama's MV does not describe its rotation this
            // smears, which is what the next run checks.
            bool const reset = spatial || pending > 0 || !mv.valid || gap;
            dlssEvaluated    = dlss::evaluateSr(
                list,
                gPipeline.colorHalf,
                gPipeline.depthR32,
                gPipeline.mvRg32,
                gPipeline.dlssOut,
                world ? mv.jitterX : 0.0f,
                world ? mv.jitterY : 0.0f,
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
        }
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
        if (gCaptureTaaRef.exchange(false, std::memory_order_acq_rel) && gPipeline.taauRef) {
            // F2: arg10 still holds this frame's TAAU result (the writer list ran before
            // our injected list); copy it aside before the PS below overwrites it with
            // DLSS. Same frame, same inputs -> the two outputs are directly comparable.
            transition(list, arg10, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list, gPipeline.taauRef, gPipeline.taauRefState, D3D12_RESOURCE_STATE_COPY_DEST);
            gPipeline.taauRefState = D3D12_RESOURCE_STATE_COPY_DEST;
            list->CopyResource(gPipeline.taauRef, arg10);
            transition(list, gPipeline.taauRef, gPipeline.taauRefState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gPipeline.taauRefState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transition(list, arg10, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            gTaaRefFresh.store(true, std::memory_order_release);
            utils::logger().info(
                "[vvsr] sr f2: captured TAAU arg10 {}x{} before the DLSS write",
                static_cast<unsigned>(targetDesc.Width),
                targetDesc.Height
            );
        }
        // Stage A proved arg10 is left in RENDER_TARGET by its writer list, so this
        // pass only binds it and leaves the state alone. The DLSS output was last in
        // a compute-read state; a pixel shader needs its own.
        transition(list, gPipeline.dlssOut, gPipeline.dlssState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gPipeline.dlssState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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
        gStats.colorChoice     = gColorChoice.load(std::memory_order_relaxed);
        gStats.depthChoice     = gDepthChoice.load(std::memory_order_relaxed);
        gStats.lastError       = 0;
        static std::atomic<unsigned> frames{0};
        unsigned const               n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || n % 600 == 0) {
            utils::logger().info(
                "[vvsr] sr arg10 #{}: {}x{} fmt={} eval={} mv={}",
                n,
                displayWidth,
                displayHeight,
                static_cast<unsigned>(targetDesc.Format),
                dlssEvalRet,
                prepMode ? "engine" : "vp"
            );
        }
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
    std::lock_guard lock(gMutex);
    for (ID3D12Resource** resource :
         {&gPipeline.colorHalf,
          &gPipeline.depthR32,
          &gPipeline.mvRg32,
          &gPipeline.mvRaw,
          &gPipeline.dlssOut}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    if (gPipeline.heap) gPipeline.heap->Release();
    if (gPipeline.prep) gPipeline.prep->Release();
    if (gPipeline.root) gPipeline.root->Release();
    gPipeline = Pipeline{};
    gStats    = Stats{};
}

} // namespace neurodragon::vv::sr
