#include "neurodragon/vv/VvSr.h"

#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/vv/VvDlssSr.h"
#include "neurodragon/vv/VvTone.h"

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
    uint Mode;
    uint Pad;
};
// Camera-relative view-projection, decoded from the TAAContext dump (vv.md §16):
// the four logged rows are the *columns* of (right, up, forward, w) scaled by the
// projection; the w row equals the forward row, so ndc.z = 1 - near/viewDepth
// (infinite far plane, near = -m14).
cbuffer MV : register(b2)
{
    float3 CurRight;  float CurSx;
    float3 CurUp;     float CurSy;
    float3 CurFwd;    float CurNear;
    float3 PrevRight; float PrevSx;
    float3 PrevUp;    float PrevSy;
    float3 PrevFwd;   float PrevNear;
    float3 CamDelta;  float MvPad;
};
Texture2D<float4>   Color    : register(t0);
Texture2D<float>    Depth    : register(t1);
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

    // Camera-relative world point of this pixel, then the same point in the previous
    // frame's space (the camera moved by CamDelta) projected with the previous matrix.
    float2 ndc    = float2(float(id.x + 0.5) / float(W) * 2.0 - 1.0,
                           1.0 - float(id.y + 0.5) / float(H) * 2.0);
    float  depth  = CurNear / max(1.0 - d, 1.0e-6);
    float3 p      = (ndc.x * depth / CurSx) * CurRight + (ndc.y * depth / CurSy) * CurUp + depth * CurFwd;
    float3 pp     = p + CamDelta;
    float  wp     = dot(PrevFwd, pp);
    float2 ndcPrev = float2(dot(PrevRight, pp) * PrevSx / max(wp, 1.0e-6),
                            dot(PrevUp,    pp) * PrevSy / max(wp, 1.0e-6));
    // DLSS wants the offset from the current pixel to where it was in the last frame.
    OutMv[id.xy] = (ndcPrev - ndc) * 0.5 * float2(float(W), float(H));
}
)hlsl";

char const* kFinalHead = R"hlsl(
cbuffer P : register(b0)
{
    uint W;
    uint H;
    uint Mode;
    uint Pad;
};
Texture2D<float4>   Color : register(t2);
Texture2D<float4>   Depth : register(t3);
Texture2D<float4>   Mv    : register(t4);
SamplerState        Samp  : register(s0);
RWTexture2D<float4> Dst   : register(u3);
)hlsl";

char const* kFinalBody = R"hlsl(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H)
    {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / float2(W, H);
    float3 c;
    if (Mode == 1)
    {
        c = saturate((1.0 - Depth.SampleLevel(Samp, uv, 0).r) * 20.0).rrr; // reversed-Z, stretched
    }
    else if (Mode == 2)
    {
        float2 mv = Mv.SampleLevel(Samp, uv, 0).rg;
        c = float3(mv.x * 40.0 + 0.5, mv.y * 40.0 + 0.5, 0.5); // exaggerated
    }
    else if (Mode >= 3)
    {
        // Raw colour: separates "the texture read is wrong" from "the encode table is
        // wrong" - the dump already showed the resource itself holds a normal world.
        c = Color.SampleLevel(Samp, uv, 0).rgb;
    }
    else
    {
        c = vvEncode(Color.SampleLevel(Samp, uv, 0).rgb); // Bypass / Dlss
    }
    Dst[id.xy] = float4(saturate(c), 1.0);
}
)hlsl";

// Shader-visible descriptor slots. One SRV table (6) and one UAV table (4) keep the
// root signature to four parameters - a per-pass split layout was rejected by the
// runtime ("overlaps with another shader register range", 2026-10-25).
enum : unsigned {
    kSlotEngineColor = 0,
    kSlotEngineDepth = 1,
    kSlotColorSrv    = 2,
    kSlotDepthSrv    = 3,
    kSlotMvSrv       = 4,
    kSlotDlssSrv     = 5,
    kSlotColorUav    = 6,
    kSlotDepthUav    = 7,
    kSlotMvUav       = 8,
    kSlotFinalUav    = 9,
    kSlotCount       = 10,
};

struct Pipeline {
    ID3D12Device*               device{};
    ID3D12RootSignature*        root{};
    ID3D12PipelineState*        prep{};
    ID3D12PipelineState*        final{};
    ID3D12DescriptorHeap*       heap{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    unsigned                    increment{};
    ID3D12Resource*             colorHalf{};
    ID3D12Resource*             depthR32{};
    ID3D12Resource*             mvRg32{}; ///< R16G16_FLOAT: DLSS accepts this, not RG32F
    ID3D12Resource*             finalRgba8{};
    ID3D12Resource*             dlssOut{};
    ID3D12Resource*             toneCb{};
    ID3D12Resource*             mvCb{};  ///< per-frame camera-relative matrix rows for the MV pass
    void*                       mvMapped{};
    D3D12_RESOURCE_STATES       colorState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       depthState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       mvState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       finalState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       dlssState{D3D12_RESOURCE_STATE_COMMON};
    unsigned                    renderWidth{};
    unsigned                    renderHeight{};
    unsigned                    displayWidth{};
    unsigned                    displayHeight{};
};

std::mutex       gMutex;
Pipeline         gPipeline{};
Stats            gStats{};
std::atomic<int> gView{static_cast<int>(View::Bypass)};
std::atomic<int> gColorChoice{0};
std::atomic<int> gDepthChoice{0};
unsigned         gColorCandidates = 0;
unsigned         gDepthCandidates = 0;
float            gToneValues[tone::kValues]{};

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

bool ensureResources(ID3D12Device* device, unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight) {
    if (gPipeline.colorHalf && gPipeline.renderWidth == renderWidth && gPipeline.renderHeight == renderHeight
        && gPipeline.displayWidth == displayWidth && gPipeline.displayHeight == displayHeight) {
        return true;
    }
    for (ID3D12Resource** resource : {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.finalRgba8, &gPipeline.dlssOut}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    gPipeline.colorHalf  = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    gPipeline.depthR32   = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
    gPipeline.mvRg32     = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT);
    gPipeline.finalRgba8 = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    gPipeline.dlssOut    = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!gPipeline.colorHalf || !gPipeline.depthR32 || !gPipeline.mvRg32 || !gPipeline.finalRgba8 || !gPipeline.dlssOut) {
        setError(E_OUTOFMEMORY, "CreateCommittedResource");
        return false;
    }
    gPipeline.colorState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.depthState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.mvState       = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.finalState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.dlssState     = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.renderWidth   = renderWidth;
    gPipeline.renderHeight  = renderHeight;
    gPipeline.displayWidth  = displayWidth;
    gPipeline.displayHeight = displayHeight;
    utils::logger().info("[vvsr] sr buffers {}x{} -> {}x{}", renderWidth, renderHeight, displayWidth, displayHeight);
    return true;
}

bool ensurePipeline(ID3D12Device* device) {
    if (gPipeline.prep && gPipeline.final && gPipeline.root && gPipeline.heap && gPipeline.toneCb && gPipeline.mvCb) {
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
    srvRange.NumDescriptors     = 6;
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 4;
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
    std::string const finalSource = std::string(kFinalHead) + tone::kHlsl + kFinalBody;
    if (!compileShader(finalSource, &gPipeline.final)) {
        return false;
    }

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

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width            = 512;
    buffer.Height           = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels        = 1;
    buffer.Format           = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&gPipeline.toneCb)
    );
    if (FAILED(hr) || !gPipeline.toneCb) {
        setError(hr, "CreateCommittedResource(tone)");
        return false;
    }
    tone::pack(gToneValues);
    void* mapped = nullptr;
    if (FAILED(gPipeline.toneCb->Map(0, nullptr, &mapped)) || !mapped) {
        setError(E_FAIL, "Map(tone)");
        return false;
    }
    std::memcpy(mapped, gToneValues, sizeof(gToneValues));
    gPipeline.toneCb->Unmap(0, nullptr);

    // Motion-vector rows: 7 x (float3 + float) = 112 bytes, mapped once and rewritten
    // every frame (a torn read costs one frame of odd vectors, not a crash).
    D3D12_RESOURCE_DESC mvBuffer = buffer;
    mvBuffer.Width               = 256;
    hr = device->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &mvBuffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&gPipeline.mvCb)
    );
    if (FAILED(hr) || !gPipeline.mvCb) {
        setError(hr, "CreateCommittedResource(mv)");
        return false;
    }
    if (FAILED(gPipeline.mvCb->Map(0, nullptr, &gPipeline.mvMapped)) || !gPipeline.mvMapped) {
        setError(E_FAIL, "Map(mv)");
        return false;
    }

    std::lock_guard lock(gMutex);
    gStats.pipelineReady = true;
    utils::logger().info("[vvsr] sr pipeline ready (CBV encode, 5 root parameters)");
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

// The engine's render-resolution colour and depth. The colour comes from the RTV
// inventory; the depth is a *depth-stencil view* resource and only the DSV inventory
// has it (2026-10-25: the RTV list had 1689x903 fmt=26 but no depth at all, which is
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

void setView(View view) { gView.store(static_cast<int>(view), std::memory_order_relaxed); }
View view() { return static_cast<View>(gView.load(std::memory_order_relaxed)); }

void cycleColor() {
    int const next = gColorChoice.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info("[vvsr] sr colour choice -> {}", next);
}
void cycleDepth() {
    int const next = gDepthChoice.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info("[vvsr] sr depth choice -> {}", next);
}

bool dumpInputs() {
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    auto* const queue  = static_cast<ID3D12CommandQueue*>(probe::vv::gameQueue());
    Stats const snapshot = stats();
    if (!device || !queue || !snapshot.colorSource || !snapshot.depthSource) {
        return false;
    }
    std::wstring const directory = utils::applicationDirectory();
    wchar_t            name[128]{};
    std::swprintf(
        name,
        std::size(name),
        L"sr_colour_%llX_fmt%u_%ux%u.bin",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(snapshot.colorSource)),
        snapshot.colorFormat,
        snapshot.renderWidth,
        snapshot.renderHeight
    );
    bool const colorOk = probe::vv::dumpTextureRaw(device, queue, snapshot.colorSource, (directory + L"\\" + name).c_str());
    std::swprintf(
        name,
        std::size(name),
        L"sr_depth_%llX_fmt%u_%ux%u.bin",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(snapshot.depthSource)),
        snapshot.depthFormat,
        snapshot.renderWidth,
        snapshot.renderHeight
    );
    bool const depthOk = probe::vv::dumpTextureRaw(device, queue, snapshot.depthSource, (directory + L"\\" + name).c_str());
    bool midOk   = false;
    bool finalOk = false;
    if (gPipeline.colorHalf && gPipeline.finalRgba8) {
        std::swprintf(name, std::size(name), L"sr_mid_colourhalf.bin");
        midOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.colorHalf, (directory + L"\\" + name).c_str(), probe::vv::DumpState::Common);
        std::swprintf(name, std::size(name), L"sr_final_rgba8.bin");
        finalOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.finalRgba8, (directory + L"\\" + name).c_str(), probe::vv::DumpState::Common);
    }
    utils::logger().info("[vvsr] sr dump inputs colour={} depth={} mid={} final={}", colorOk ? 1 : 0, depthOk ? 1 : 0, midOk ? 1 : 0, finalOk ? 1 : 0);
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

// One frame of motion-vector parameters, decoded from the TAAContext the TAAU draw
// handed us. Layout matches the b2 constant buffer declared in the prep shader.
struct MvFrame {
    float rows[7][4]{};
    float jitterX{};
    float jitterY{};
    bool  reset{true};
    bool  valid{false};
};

MvFrame decodeMvFrame() {
    MvFrame frame{};
    float   cur[16]{};
    float   prev[16]{};
    float   cam[6]{};
    float   jitter[2]{};
    if (!probe::vv::taaMatrices(cur, prev, cam, jitter)) {
        return frame;
    }
    // The four logged rows are the columns of (right, up, fwd, w); near = -m14.
    auto const axis = [](float const* m, int i, float* out) {
        out[0] = m[i];
        out[1] = m[4 + i];
        out[2] = m[8 + i];
    };
    float curRight[3]{};
    float curUp[3]{};
    float curFwd[3]{};
    float prevRight[3]{};
    float prevUp[3]{};
    float prevFwd[3]{};
    axis(cur, 0, curRight);
    axis(cur, 1, curUp);
    axis(cur, 2, curFwd);
    axis(prev, 0, prevRight);
    axis(prev, 1, prevUp);
    axis(prev, 2, prevFwd);
    float const curNear  = -cur[14];
    float const prevNear = -prev[14];
    auto const  length   = [](float const* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    float const curRightLen  = length(curRight);
    float const curUpLen     = length(curUp);
    float const curFwdLen    = length(curFwd);
    float const prevRightLen = length(prevRight);
    float const prevUpLen    = length(prevUp);
    float const prevFwdLen   = length(prevFwd);
    // A wrong decode is worse than no vectors: require a sane basis and near plane.
    if (curRightLen < 1.0e-3f || curUpLen < 1.0e-3f || curFwdLen < 1.0e-3f || prevRightLen < 1.0e-3f
        || prevUpLen < 1.0e-3f || prevFwdLen < 1.0e-3f || curNear <= 0.0f || prevNear <= 0.0f) {
        return frame;
    }
    auto const row = [](float* dst, float const* v, float len, float extra) {
        dst[0] = v[0] / len;
        dst[1] = v[1] / len;
        dst[2] = v[2] / len;
        dst[3] = extra;
    };
    row(frame.rows[0], curRight, curRightLen, curRightLen);
    row(frame.rows[1], curUp, curUpLen, curUpLen);
    row(frame.rows[2], curFwd, curFwdLen, curNear);
    row(frame.rows[3], prevRight, prevRightLen, prevRightLen);
    row(frame.rows[4], prevUp, prevUpLen, prevUpLen);
    row(frame.rows[5], prevFwd, prevFwdLen, prevNear);
    frame.rows[6][0] = cam[0] - cam[3];
    frame.rows[6][1] = cam[1] - cam[4];
    frame.rows[6][2] = cam[2] - cam[5];
    frame.jitterX = jitter[0];
    frame.jitterY = jitter[1];
    frame.valid   = true;
    return frame;
}

bool record(void* commandListVoid, void* backBufferVoid) {
    auto* const list   = static_cast<ID3D12GraphicsCommandList*>(commandListVoid);
    auto* const back   = static_cast<ID3D12Resource*>(backBufferVoid);
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    if (!list || !back || !device) {
        return false;
    }
    if (!inWorld()) {
        // No world: record nothing, but do not report failure. A failed record makes
        // the injector skip, and on 2026-10-25 that left the SR action dead after the
        // world came back. An empty list is a valid no-op.
        return true;
    }
    // Step 2 groundwork: the motion-vector pass needs the engine's current/previous
    // view-projection pair, the camera origins and the sub-pixel jitter. The layout is
    // documented (docs/specs/vv.md §6) but the packing convention is not, so one frame
    // after the camera has settled logs the raw numbers; the reprojection is written
    // against what that frame shows.
    static std::atomic<unsigned> srFrames{0};
    unsigned const frameNo = srFrames.fetch_add(1, std::memory_order_relaxed) + 1;
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
            "[vvsr] mv decode valid={} jitter=({:+.4f},{:+.4f}) delta=({:+.4f},{:+.4f},{:+.4f}) near={:+.4f} sx={:.4f} sy={:.4f}",
            probeFrame.valid ? 1 : 0,
            probeFrame.jitterX,
            probeFrame.jitterY,
            probeFrame.rows[6][0],
            probeFrame.rows[6][1],
            probeFrame.rows[6][2],
            probeFrame.rows[2][3],
            probeFrame.rows[0][3],
            probeFrame.rows[1][3]
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
    unsigned const renderWidth  = probe::vv::renderWidth();
    unsigned const renderHeight = probe::vv::renderHeight();
    D3D12_RESOURCE_DESC const backDesc = back->GetDesc();
    unsigned const displayWidth  = static_cast<unsigned>(backDesc.Width);
    unsigned const displayHeight = backDesc.Height;
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
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(gPipeline.finalRgba8, nullptr, &uav, cpuAt(kSlotFinalUav));

    // --- prep: engine colour + depth -> RGBA16F / R32F / RG32F -------------
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
    unsigned const prepParams[4] = {renderWidth, renderHeight, 0, 0};
    list->SetComputeRoot32BitConstants(0, 4, prepParams, 0);
    MvFrame const mv = decodeMvFrame();
    if (gPipeline.mvCb && gPipeline.mvMapped) {
        std::memcpy(gPipeline.mvMapped, mv.rows, sizeof(mv.rows));
        list->SetComputeRootConstantBufferView(4, gPipeline.mvCb->GetGPUVirtualAddress());
    }
    list->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

    // The final pass reads all three prepared buffers as SRVs.
    transition(list, gPipeline.colorHalf, gPipeline.colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.depthR32, gPipeline.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.mvRg32, gPipeline.mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gPipeline.mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(list, gPipeline.finalRgba8, gPipeline.finalState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.finalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    int const view = gView.load(std::memory_order_relaxed);
    unsigned  finalMode  = 0;
    bool      dlssEvaluated = false;
    int       dlssCreateRet = 0;
    int       dlssEvalRet   = 0;
    if (view == static_cast<int>(View::Depth)) {
        finalMode = 1;
    } else if (view == static_cast<int>(View::Motion)) {
        finalMode = 2;
    } else if (view == static_cast<int>(View::Raw)) {
        finalMode = 3;
    } else if (view == static_cast<int>(View::Dlss)) {
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
            bool const                   world = probe::vv::worldLoaded();
            if (frameNo <= 2 || world != lastWorld.load(std::memory_order_relaxed)) {
                resetFrames.store(2, std::memory_order_relaxed);
                lastWorld.store(world, std::memory_order_relaxed);
            }
            unsigned const pending = resetFrames.load(std::memory_order_relaxed);
            if (pending > 0) {
                resetFrames.store(pending - 1, std::memory_order_relaxed);
            }
            bool const reset = pending > 0 || !mv.valid;
            dlssEvaluated = dlss::evaluateSr(
                list,
                gPipeline.colorHalf,
                gPipeline.depthR32,
                gPipeline.mvRg32,
                gPipeline.dlssOut,
                mv.jitterX,
                mv.jitterY,
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
    list->SetPipelineState(gPipeline.final);
    unsigned const finalParams[4] = {displayWidth, displayHeight, finalMode, 0};
    list->SetComputeRoot32BitConstants(0, 4, finalParams, 0);
    list->SetComputeRootConstantBufferView(3, gPipeline.toneCb->GetGPUVirtualAddress());
    list->Dispatch((displayWidth + 7) / 8, (displayHeight + 7) / 8, 1);

    transition(list, gPipeline.finalRgba8, gPipeline.finalState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gPipeline.finalState = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = back;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = back;
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = gPipeline.finalRgba8;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // The user sees a black band at the bottom of the screen while the game's own
    // captures look complete (2026-10-25 23:16), so the geometry of what we copy is
    // logged once: if these two agree and the copy carries no region, our write
    // covers the whole back buffer and the band is added after this point.
    static std::atomic<unsigned> copyLogged{0};
    if (copyLogged.fetch_add(1, std::memory_order_relaxed) < 1) {
        D3D12_RESOURCE_DESC const backDesc  = back->GetDesc();
        D3D12_RESOURCE_DESC const finalDesc = gPipeline.finalRgba8->GetDesc();
        utils::logger().info(
            "[vvsr] sr copy back={}x{} fmt={} final={}x{} fmt={}",
            static_cast<unsigned>(backDesc.Width),
            backDesc.Height,
            static_cast<unsigned>(backDesc.Format),
            static_cast<unsigned>(finalDesc.Width),
            finalDesc.Height,
            static_cast<unsigned>(finalDesc.Format)
        );
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);

    std::lock_guard lock(gMutex);
    gStats.recorded      = true;
    gStats.view          = view;
    gStats.dlssReady     = dlssEvaluated;
    gStats.dlssCreateRet = dlssCreateRet;
    gStats.dlssEvaluateRet = dlssEvalRet;
    gStats.renderWidth   = renderWidth;
    gStats.renderHeight  = renderHeight;
    gStats.displayWidth  = displayWidth;
    gStats.displayHeight = displayHeight;
    gStats.colorSource   = color;
    gStats.depthSource   = depth;
    gStats.colorFormat   = colorFormat;
    gStats.depthFormat   = depthFormat;
    gStats.colorCandidates = gColorCandidates;
    gStats.depthCandidates = gDepthCandidates;
    gStats.colorChoice     = gColorChoice.load(std::memory_order_relaxed);
    gStats.depthChoice     = gDepthChoice.load(std::memory_order_relaxed);
    gStats.lastError     = 0;
    return true;
}

Stats stats() {
    std::lock_guard lock(gMutex);
    return gStats;
}

void release() {
    std::lock_guard lock(gMutex);
    for (ID3D12Resource** resource : {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.finalRgba8, &gPipeline.toneCb, &gPipeline.mvCb}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    if (gPipeline.heap) gPipeline.heap->Release();
    if (gPipeline.prep) gPipeline.prep->Release();
    if (gPipeline.final) gPipeline.final->Release();
    if (gPipeline.root) gPipeline.root->Release();
    gPipeline = Pipeline{};
    gStats    = Stats{};
}

} // namespace neurodragon::vv::sr
