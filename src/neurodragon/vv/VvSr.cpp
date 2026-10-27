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
    bool   sky    = d >= 0.999999;
    float  depth  = sky ? 1000000.0 : CurNear / max(1.0 - d, 1.0e-6);
    float3 p      = (ndc.x * depth / CurSx) * CurRight + (ndc.y * depth / CurSy) * CurUp + depth * CurFwd;
    float3 pp     = p + (sky ? float3(0.0, 0.0, 0.0) : CamDelta);
    float  wp     = dot(PrevFwd, pp);
    float2 ndcPrev = float2(dot(PrevRight, pp) * PrevSx / max(wp, 1.0e-6),
                            dot(PrevUp,    pp) * PrevSy / max(wp, 1.0e-6));
    // DLSS wants the offset from the current pixel to where it was in the last frame.
    OutMv[id.xy] = (ndcPrev - ndc) * 0.5 * float2(float(W), float(H));
}
)hlsl";

char const* kExposureShader = R"hlsl(
cbuffer P : register(b0)
{
    uint W;
    uint H;
    uint Mode;
    uint Pad;
};
Texture2D<float4>   Color : register(t0);
RWTexture2D<float>  White : register(u0);
groupshared float   ExposureSum[64];
groupshared uint    FloorCount[64];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint index = id.y * 8 + id.x;
    uint textureWidth;
    uint textureHeight;
    Color.GetDimensions(textureWidth, textureHeight);
    float sum = 0.0;
    uint floorCount = 0;
    for (uint y = 0; y < 8; ++y)
    {
        for (uint x = 0; x < 8; ++x)
        {
            uint2 p = uint2((id.x * 8 + x) * W / 64, (id.y * 8 + y) * H / 64);
            float3 c = max(Color.Load(int3((int)p.x, (int)p.y, 0)).rgb, 0.0);
            float rawLuma = dot(c, float3(0.2126, 0.7152, 0.0722));
            floorCount += rawLuma <= 0.0001 ? 1 : 0;
            sum += log2(max(rawLuma, 0.0001));
        }
    }
    ExposureSum[index] = sum;
    FloorCount[index] = floorCount;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1)
    {
        if (index < s)
        {
            ExposureSum[index] += ExposureSum[index + s];
            FloorCount[index] += FloorCount[index + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0)
    {
        // EXP-0048's reference is kept unchanged for the reset A/B. Its original 28-byte
        // NDRW parse was later found to be four bytes short; recalibration is separate.
        // Dividing by (this frame's mean / reference) pulls the 380x dark / 260x bright
        // frames of EXP-0045/0046 back onto the same display brightness.
        float meanLog   = ExposureSum[0] / 4096.0;
        float target    = meanLog - log2(0.0671);
        float previous  = White[uint2(0, 0)];
        float current   = Mode != 0 || previous <= 0.0 || !isfinite(previous) ? target : lerp(log2(previous), target, 0.03);
        float writes    = White[uint2(0, 1)];
        White[uint2(0, 0)] = exp2(clamp(current, -9.0, 9.0));
        White[uint2(1, 0)] = target;
        White[uint2(2, 0)] = previous;
        White[uint2(3, 0)] = float(Mode);
        White[uint2(0, 1)] = Mode != 0 || writes <= 0.0 || !isfinite(writes) ? 1.0 : writes + 1.0;
        White[uint2(1, 1)] = float(W);
        White[uint2(2, 1)] = float(H);
        White[uint2(3, 1)] = float(FloorCount[0]);
        White[uint2(0, 2)] = float(textureWidth);
        White[uint2(1, 2)] = float(textureHeight);
        White[uint2(2, 2)] = meanLog;
        float mirrorPrevious = White[uint2(3, 2)];
        float mirrorCurrent  = Mode != 0 || mirrorPrevious <= 0.0 || !isfinite(mirrorPrevious)
            ? target
            : lerp(log2(mirrorPrevious), target, 0.03);
        White[uint2(3, 2)] = exp2(clamp(mirrorCurrent, -9.0, 9.0));
        uint ringIndex = writes > 0.0 && isfinite(writes) ? uint(writes) % 4 : 0;
        White[uint2(ringIndex, 3)] = target;
    }
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
Texture2D<float>    White : register(t6);
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
    else if (Mode == 4)
    {
        c = vvEncode(Color.SampleLevel(Samp, uv, 0).rgb); // Bypass: no exposure normalisation
    }
    else if (Mode >= 3)
    {
        c = Color.SampleLevel(Samp, uv, 0).rgb;
    }
    else
    {
        // White is the ratio between this frame's geometric-mean luma and the
        // reference (EXP-0048); it is only read in the DLSS mode, which always runs
        // the exposure pass first. The floor keeps a missing texture from turning
        // into a divide-by-zero rather than limiting the gain.
        float white = max(White.Load(int3(0, 0, 0)), 0.002);
        c = vvEncode(Color.SampleLevel(Samp, uv, 0).rgb / white);
    }
    Dst[id.xy] = float4(saturate(c), 1.0);
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
    kSlotColorUav    = 7,
    kSlotDepthUav    = 8,
    kSlotMvUav       = 9,
    kSlotFinalUav    = 10,
    kSlotExposureUav = 11,
    kSlotCount       = 12,
};

struct Pipeline {
    ID3D12Device*               device{};
    ID3D12RootSignature*        root{};
    ID3D12PipelineState*        prep{};
    ID3D12PipelineState*        exposure{};
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
    ID3D12Resource*             exposureR32{};
    ID3D12Resource*             toneCb{};
    ID3D12Resource*             mvCb{};  ///< per-frame camera-relative matrix rows for the MV pass
    void*                       mvMapped{};
    D3D12_RESOURCE_STATES       colorState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       depthState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       mvState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       finalState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       dlssState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES       exposureState{D3D12_RESOURCE_STATE_COMMON};
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
// One number per dump so a repeated trigger (panel button, file marker, F7 pair) keeps
// every frame instead of overwriting the previous one. File scope because the TEST
// hotkey path has to be able to name the run it just triggered.
std::atomic<unsigned> gDumpNo{0};
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
    for (ID3D12Resource** resource :
         {&gPipeline.colorHalf, &gPipeline.depthR32, &gPipeline.mvRg32, &gPipeline.finalRgba8, &gPipeline.dlssOut, &gPipeline.exposureR32}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    gPipeline.colorHalf   = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    gPipeline.depthR32    = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
    gPipeline.mvRg32      = createTexture(device, renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT);
    gPipeline.finalRgba8  = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    gPipeline.dlssOut     = createTexture(device, displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // 4x4, not 1x1: dumpTextureRawAs refuses anything smaller (desc.Width < 4), and
    // the F10 dump is the only way to check the white point after a run. Only [0,0]
    // is written and read.
    gPipeline.exposureR32 = createTexture(device, 4, 4, DXGI_FORMAT_R32_FLOAT);
    if (!gPipeline.colorHalf || !gPipeline.depthR32 || !gPipeline.mvRg32 || !gPipeline.finalRgba8 || !gPipeline.dlssOut
        || !gPipeline.exposureR32) {
        setError(E_OUTOFMEMORY, "CreateCommittedResource");
        return false;
    }
    gPipeline.colorState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.depthState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.mvState       = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.finalState    = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.dlssState     = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.exposureState = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.renderWidth   = renderWidth;
    gPipeline.renderHeight  = renderHeight;
    gPipeline.displayWidth  = displayWidth;
    gPipeline.displayHeight = displayHeight;
    utils::logger().info("[vvsr] sr buffers {}x{} -> {}x{}", renderWidth, renderHeight, displayWidth, displayHeight);
    return true;
}

bool ensurePipeline(ID3D12Device* device) {
    if (gPipeline.prep && gPipeline.exposure && gPipeline.final && gPipeline.root && gPipeline.heap && gPipeline.toneCb
        && gPipeline.mvCb) {
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
    srvRange.NumDescriptors     = 7;
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 5;
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
    if (!compileShader(kExposureShader, &gPipeline.exposure)) {
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

// One frame of motion-vector parameters, decoded from the TAAContext the TAAU draw
// handed us. Layout matches the b2 constant buffer declared in the prep shader.
// Declared here (not next to decodeMvFrame) because dumpInputs reports the MV state of
// every batch it writes; the definition is below, which is all a forward declaration
// needs.
struct MvFrame {
    float rows[7][4]{};
    float jitterX{};
    float jitterY{};
    bool  reset{true};
    bool  valid{false};
    bool  prevSame{true}; ///< the two 16-float halves are identical (no camera motion,
                          ///< or the snapshot was not taken before the engine's draw)
};

MvFrame decodeMvFrame();

unsigned dumpRuns() { return gDumpNo.load(std::memory_order_relaxed); }

bool dumpInputs() {
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    auto* const queue  = static_cast<ID3D12CommandQueue*>(probe::vv::gameQueue());
    Stats const snapshot = stats();
    if (!device || !queue || !snapshot.colorSource || !snapshot.depthSource) {
        return false;
    }
    unsigned const run = gDumpNo.fetch_add(1, std::memory_order_relaxed) + 1;
    utils::logger().info(
        "[vvsr] sr dump #{} frame={} view={} dlssReady={} start", run, snapshot.frame, snapshot.view, snapshot.dlssReady ? 1 : 0
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
        mv.rows[6][0],
        mv.rows[6][1],
        mv.rows[6][2],
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
    bool finalOk    = false;
    bool mvOk       = false;
    bool depthR32Ok = false;
    bool dlssOk     = false;
    bool exposureOk = false;
    // Our own textures are left in NON_PIXEL_SHADER_RESOURCE by the record path; the
    // 2026-10-26 dump read the MV as all zeros and the depth as all 1.0 with the
    // Common hint, which is the stale-byte failure the DumpState comment describes.
    auto const state = probe::vv::DumpState::NonPixelShaderResource;
    if (gPipeline.colorHalf && gPipeline.finalRgba8) {
        std::swprintf(name, std::size(name), L"sr_mid_colourhalf_%u.bin", run);
        midOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.colorHalf, (directory + L"\\" + name).c_str(), state);
        std::swprintf(name, std::size(name), L"sr_final_rgba8_%u.bin", run);
        finalOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.finalRgba8, (directory + L"\\" + name).c_str(), state);
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
    // The white point the final encode divides by (EXP-0048). 4x4 R32F, only [0,0] is
    // written by the exposure pass; reading it back is the only way to tell "the pass
    // never ran" from "the pass ran and the target is wrong".
    if (gPipeline.exposureR32 && gPipeline.exposureState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        std::swprintf(name, std::size(name), L"sr_exposure_%u.bin", run);
        exposureOk = probe::vv::dumpTextureRawAs(device, queue, gPipeline.exposureR32, (directory + L"\\" + name).c_str(), state);
    }
    utils::logger().info(
        "[vvsr] sr dump #{} inputs colour={} depth={} mid={} final={} mv={} depth32={} dlss={} exposure={}",
        run,
        colorOk ? 1 : 0,
        depthOk ? 1 : 0,
        midOk ? 1 : 0,
        finalOk ? 1 : 0,
        mvOk ? 1 : 0,
        depthR32Ok ? 1 : 0,
        dlssOk ? 1 : 0,
        exposureOk ? 1 : 0
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

// One frame of motion-vector parameters, decoded from the TAAContext the TAAU draw
// handed us; the struct lives above dumpInputs, which reports it per batch.
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
            "[vvsr] mv decode valid={} prev_same={} jitter=({:+.4f},{:+.4f}) delta=({:+.4f},{:+.4f},{:+.4f}) near={:+.4f} sx={:.4f} sy={:.4f}",
            probeFrame.valid ? 1 : 0,
            probeFrame.prevSame ? 1 : 0,
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
    unsigned       renderWidth  = probe::vv::renderWidth();
    unsigned       renderHeight = probe::vv::renderHeight();
    D3D12_RESOURCE_DESC const backDesc = back->GetDesc();
    unsigned const displayWidth  = static_cast<unsigned>(backDesc.Width);
    unsigned const displayHeight = backDesc.Height;
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
    texSrv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(gPipeline.exposureR32, &texSrv, cpuAt(kSlotExposureSrv));

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
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(gPipeline.exposureR32, nullptr, &uav, cpuAt(kSlotExposureUav));

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
    MvFrame mv = decodeMvFrame();
    if (spatial) {
        // Zero motion, zero jitter, invalid -> the DLSS branch also forces Reset.
        mv = MvFrame{};
    }
    // One-shot proof that the draw-time snapshot is doing its job: with a moving camera
    // the previous half differs from the current one. The post-frame read this replaced
    // could never log this line (prev==cur both halves), so it doubles as a regression.
    {
        static std::atomic<bool> logged{false};
        if (mv.valid && !mv.prevSame && !logged.exchange(true, std::memory_order_relaxed)) {
            utils::logger().info(
                "[vvsr] mv snapshot: previous VP differs from current at frame {} (delta=({:+.4f},{:+.4f},{:+.4f}))",
                frameNo,
                mv.rows[6][0],
                mv.rows[6][1],
                mv.rows[6][2]
            );
        }
    }
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
    // 4 = Bypass (encode only). Mode 0 reads the white-point texture, and only the
    // DLSS branch runs the exposure pass that fills it; leaving the default at 0
    // would divide every other view by a never-written texture (EXP-0048).
    unsigned  finalMode      = 4;
    bool      dlssEvaluated  = false;
    bool      exposureReset  = false;
    int       dlssCreateRet  = 0;
    int       dlssEvalRet    = 0;
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
            static std::atomic<unsigned> lastGeneration{0};
            static std::atomic<int>      lastView{-1};
            bool const                   world      = probe::vv::worldLoaded();
            unsigned const               generation = probe::vv::renderGeneration();
            if (frameNo <= 2 || world != lastWorld.load(std::memory_order_relaxed)
                || generation != lastGeneration.load(std::memory_order_relaxed)
                || view != lastView.load(std::memory_order_relaxed)) {
                resetFrames.store(2, std::memory_order_relaxed);
                lastWorld.store(world, std::memory_order_relaxed);
                lastGeneration.store(generation, std::memory_order_relaxed);
                lastView.store(view, std::memory_order_relaxed);
            }
            unsigned const pending = resetFrames.load(std::memory_order_relaxed);
            if (pending > 0) {
                resetFrames.store(pending - 1, std::memory_order_relaxed);
            }
            bool const reset = spatial || pending > 0 || !mv.valid;
            exposureReset  = reset;
            dlssEvaluated  = dlss::evaluateSr(
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
        // EXP-0048: the engine's HDR input drifts 380x darker / 260x brighter between
        // sessions and the DLSS output follows it one-for-one, so the encode divides by
        // a white point measured from the same signal the final pass reads: descriptor
        // 2 is colorHalf until evaluate succeeds and the DLSS output afterwards, so
        // pointing the exposure table at it keeps both passes on one source.
        finalMode = 0;
        if (gPipeline.exposure) {
            unsigned const sampleWidth  = dlssEvaluated ? displayWidth : renderWidth;
            unsigned const sampleHeight = dlssEvaluated ? displayHeight : renderHeight;
            transition(list, gPipeline.exposureR32, gPipeline.exposureState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gPipeline.exposureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            list->SetPipelineState(gPipeline.exposure);
            list->SetComputeRootDescriptorTable(1, gpuAt(kSlotColorSrv));
            list->SetComputeRootDescriptorTable(2, gpuAt(kSlotExposureUav));
            unsigned const exposureParams[4] = {sampleWidth, sampleHeight, exposureReset ? 1U : 0U, 0};
            list->SetComputeRoot32BitConstants(0, 4, exposureParams, 0);
            list->Dispatch(1, 1, 1);
            transition(list, gPipeline.exposureR32, gPipeline.exposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gPipeline.exposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            // Both tables go back to the layout the final pass was written against:
            // t2/t3/t4/t6 with the SRV base at 0 and u3 with the UAV base at 7.
            list->SetComputeRootDescriptorTable(1, gpuAt(kSlotEngineColor));
            list->SetComputeRootDescriptorTable(2, gpuAt(kSlotColorUav));
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
    gStats.frame         = frameNo;
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
    for (ID3D12Resource** resource :
         {&gPipeline.colorHalf,
          &gPipeline.depthR32,
          &gPipeline.mvRg32,
          &gPipeline.finalRgba8,
          &gPipeline.dlssOut,
          &gPipeline.exposureR32,
          &gPipeline.toneCb,
          &gPipeline.mvCb}) {
        if (*resource) {
            (*resource)->Release();
            *resource = nullptr;
        }
    }
    if (gPipeline.heap) gPipeline.heap->Release();
    if (gPipeline.prep) gPipeline.prep->Release();
    if (gPipeline.exposure) gPipeline.exposure->Release();
    if (gPipeline.final) gPipeline.final->Release();
    if (gPipeline.root) gPipeline.root->Release();
    gPipeline = Pipeline{};
    gStats    = Stats{};
}

} // namespace neurodragon::vv::sr
