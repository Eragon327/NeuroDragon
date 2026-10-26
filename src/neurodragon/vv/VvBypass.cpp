#include "neurodragon/vv/VvBypass.h"

#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/vv/VvTone.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "neurodragon/Macro.h"

#include "neurodragon/utils/Utils.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace neurodragon::vv::bypass {
namespace {

using SerializeRootFn = HRESULT (*)(D3D12_ROOT_SIGNATURE_DESC const*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using CompileFn = HRESULT (*)(LPCVOID, SIZE_T, LPCSTR, D3D_SHADER_MACRO const*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                             UINT, ID3DBlob**, ID3DBlob**);

char const* kShaderHead = R"hlsl(
cbuffer Params : register(b0)
{
    uint SrcW;
    uint SrcH;
    uint DstW;
    uint DstH;
    uint Mode; // 0 = measured encode table, 1 = gamma only, 2 = ACES + gamma
};

Texture2D<float4>   Src  : register(t0);
SamplerState        Samp : register(s0);
RWTexture2D<float4> Dst  : register(u0);

float3 Aces(float3 x)
{
    // Kept for the A/B switch: B0 fitted this curve against a screenshot and got
    // 26/255; B1c re-measured the pair properly and the engine does not do this.
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 Gamma(float3 x)
{
    return pow(max(x, 0.0), 1.0 / 2.2);
}
)hlsl";

char const* kShaderBody = R"hlsl(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH)
    {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / float2(DstW, DstH);
    float3 c  = Src.SampleLevel(Samp, uv, 0).rgb;
    if (Mode == 0)
    {
        c = vvEncode(c);
    }
    else if (Mode == 1)
    {
        c = Gamma(c);
    }
    else
    {
        c = Gamma(Aces(c));
    }
    Dst[id.xy] = float4(c, 1.0);
}
)hlsl";

struct Pipeline {
    ID3D12Device*               device{};
    ID3D12RootSignature*        root{};
    ID3D12PipelineState*        pso{};
    ID3D12DescriptorHeap*       heap{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu{};
    ID3D12Resource*             output{};
    D3D12_RESOURCE_STATES       outputState{D3D12_RESOURCE_STATE_COMMON};
    unsigned                    outputWidth{};
    unsigned                    outputHeight{};
};

std::mutex gMutex;
Pipeline   gPipeline{};
Stats      gStats{};

// The measured encode table, uploaded once into a small constant buffer (a CBV is
// the only way to hand 99 floats to a shader: the root signature is capped at 64
// DWORDs in total, so root constants cannot hold it).
ID3D12Resource* gToneCb{};
float           gToneValues[tone::kValues]{};

// The colour picked once and kept by reference. The probe's RTV ring is bounded
// and can evict the render graph under a flood of UI textures (the first B1b run
// lost it after ~40 s: "0 target(s) visible"). Keeping this one reference makes
// the bypass stable; renderGeneration invalidates it across a rebuild.
ID3D12Resource* gColorResource{};
unsigned        gColorWidth{};
unsigned        gColorHeight{};
unsigned        gColorGeneration{};
std::atomic<int>  gColorChoice{0};
std::atomic<int>  gTonemapMode{0};
std::atomic<bool> gColorDirty{false};
unsigned          gColorCandidates{};

void setError(HRESULT hr, char const* where) {
    std::lock_guard lock(gMutex);
    gStats.lastError = static_cast<int>(hr);
    if (FAILED(hr)) {
        utils::logger().warn("[vvsr] bypass {} failed hr=0x{:08X}", where, static_cast<unsigned>(hr));
    }
}

void setStats(bool recorded, void const* color, unsigned srcW, unsigned srcH, unsigned dstW, unsigned dstH) {
    std::lock_guard lock(gMutex);
    gStats.recorded     = recorded;
    gStats.colorFormat  = color ? 26u : 0u;
    gStats.srcWidth     = srcW;
    gStats.srcHeight    = srcH;
    gStats.dstWidth     = dstW;
    gStats.dstHeight    = dstH;
    gStats.colorCandidates = gColorCandidates;
    gStats.colorChoice  = gColorChoice.load(std::memory_order_relaxed);
    gStats.tonemap      = gTonemapMode.load(std::memory_order_relaxed);
    gStats.colorResource = const_cast<void*>(color);
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
    if (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = resource;
        barriers[1]               = barrier;
        list->ResourceBarrier(2, barriers);
        return;
    }
    list->ResourceBarrier(1, &barrier);
}

bool ensureOutput(ID3D12Device* device, unsigned width, unsigned height) {
    if (gPipeline.output && gPipeline.outputWidth == width && gPipeline.outputHeight == height) {
        return true;
    }
    if (gPipeline.output) {
        gPipeline.output->Release();
        gPipeline.output = nullptr;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT const hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&gPipeline.output)
    );
    if (FAILED(hr)) {
        setError(hr, "CreateCommittedResource(output)");
        return false;
    }
    gPipeline.outputState  = D3D12_RESOURCE_STATE_COMMON;
    gPipeline.outputWidth  = width;
    gPipeline.outputHeight = height;
    return true;
}

bool ensurePipeline(ID3D12Device* device) {
    if (gPipeline.pso && gPipeline.root && gPipeline.heap) {
        return true;
    }
    if (gPipeline.device && gPipeline.device != device) {
        utils::logger().warn("[vvsr] bypass device changed; pipeline rebuild is not supported in-session");
        return false;
    }
    gPipeline.device = device;

    HMODULE const d3d12 = ::LoadLibraryW(L"d3d12.dll");
    HMODULE const compiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3d12 || !compiler) {
        setError(HRESULT_FROM_WIN32(::GetLastError()), "LoadLibrary");
        return false;
    }
    auto const serialize = reinterpret_cast<SerializeRootFn>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    auto const compile = reinterpret_cast<CompileFn>(::GetProcAddress(compiler, "D3DCompile"));
    if (!serialize || !compile) {
        setError(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "D3D12SerializeRootSignature/D3DCompile");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors                    = 1;
    uavRange.BaseShaderRegister                = 0;
    uavRange.RegisterSpace                     = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[0].Constants.ShaderRegister  = 0;
    params[0].Constants.RegisterSpace   = 0;
    params[0].Constants.Num32BitValues  = 5;
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
    // The measured display encode (VvTone.h): a CBV at b1, one DWORD of root
    // signature instead of 99.
    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].Descriptor.RegisterSpace  = 0;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters     = 4;
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers   = &sampler;
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* signature = nullptr;
    ID3DBlob* error     = nullptr;
    HRESULT   hr        = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr) || !signature) {
        setError(hr, "SerializeRootSignature");
        if (error) error->Release();
        return false;
    }
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&gPipeline.root));
    signature->Release();
    if (error) error->Release();
    if (FAILED(hr)) {
        setError(hr, "CreateRootSignature");
        return false;
    }

    ID3DBlob* code = nullptr;
    error          = nullptr;
    // The tone table's HLSL lives next to its numbers (VvTone.h); the shader is the
    // three pieces in order, so the table can be regenerated without touching this file.
    std::string const source = std::string(kShaderHead) + tone::kHlsl + kShaderBody;
    hr = compile(
        source.data(),
        source.size(),
        "VvBypass",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &code,
        &error
    );
    if (FAILED(hr) || !code) {
        setError(hr, "D3DCompile");
        if (error) error->Release();
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = gPipeline.root;
    pso.CS.pShaderBytecode = code->GetBufferPointer();
    pso.CS.BytecodeLength  = code->GetBufferSize();
    hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&gPipeline.pso));
    code->Release();
    if (error) error->Release();
    if (FAILED(hr)) {
        setError(hr, "CreateComputePipelineState");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gPipeline.heap));
    if (FAILED(hr)) {
        setError(hr, "CreateDescriptorHeap");
        return false;
    }
    gPipeline.srvCpu = gPipeline.heap->GetCPUDescriptorHandleForHeapStart();
    gPipeline.srvGpu = gPipeline.heap->GetGPUDescriptorHandleForHeapStart();
    gPipeline.uavCpu = gPipeline.srvCpu;
    gPipeline.uavCpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gPipeline.uavGpu = gPipeline.srvGpu;
    gPipeline.uavGpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Constant buffer for the encode table (upload heap, 256-byte aligned).
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
        IID_PPV_ARGS(&gToneCb)
    );
    if (FAILED(hr) || !gToneCb) {
        setError(hr, "CreateCommittedResource(tone)");
        return false;
    }
    tone::pack(gToneValues);
    void* mapped = nullptr;
    if (FAILED(gToneCb->Map(0, nullptr, &mapped)) || !mapped) {
        setError(E_FAIL, "Map(tone)");
        return false;
    }
    std::memcpy(mapped, gToneValues, sizeof(gToneValues));
    gToneCb->Unmap(0, nullptr);

    std::lock_guard lock(gMutex);
    gStats.pipelineReady = true;
    utils::logger().info("[vvsr] bypass pipeline ready (encode table, CBV)");
    return true;
}

} // namespace

bool record(void* commandList, void* backBuffer) {
    auto* const list = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* const bb   = static_cast<ID3D12Resource*>(backBuffer);
    auto* const dev  = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    if (!list || !bb || !dev) {
        setError(E_POINTER, "record arguments");
        return false;
    }
    if (!ensurePipeline(dev)) {
        return false;
    }

    unsigned const rw = probe::vv::renderWidth();
    unsigned const rh = probe::vv::renderHeight();
    unsigned const gen = probe::vv::renderGeneration();
    if (gColorDirty.exchange(false, std::memory_order_relaxed) && gColorResource) {
        gColorResource->Release();
        gColorResource = nullptr;
    }
    if (gColorResource && (gColorGeneration != gen || gColorWidth != rw || gColorHeight != rh)) {
        gColorResource->Release();
        gColorResource = nullptr;
    }
    if (!gColorResource) {
        probe::vv::RenderTargetInfo targets[32]{};
        unsigned const              count = probe::vv::renderTargets(targets, 32);
        probe::vv::RenderTargetInfo candidates[16]{};
        unsigned                    candidateCount = 0;
        for (unsigned i = 0; i < count && candidateCount < 16; ++i) {
            if (targets[i].format == 26) {
                candidates[candidateCount++] = targets[i];
            }
        }
        gColorCandidates = candidateCount;
        if (candidateCount) {
            int const choice = gColorChoice.load(std::memory_order_relaxed) % static_cast<int>(candidateCount);
            for (unsigned i = 0; i < candidateCount; ++i) {
                utils::logger().info(
                    "[vvsr] bypass colour[{}]={:#x} {}x{}",
                    i,
                    reinterpret_cast<std::uintptr_t>(candidates[i].resource),
                    candidates[i].width,
                    candidates[i].height
                );
            }
            gColorResource   = static_cast<ID3D12Resource*>(candidates[choice].resource);
            gColorResource->AddRef();
            gColorWidth      = candidates[choice].width;
            gColorHeight     = candidates[choice].height;
            gColorGeneration = gen;
            utils::logger().info(
                "[vvsr] bypass colour choice={} ptr={:#x} tonemap={}",
                choice,
                reinterpret_cast<std::uintptr_t>(gColorResource),
                gTonemapMode.load(std::memory_order_relaxed)
            );
        }
    }
    if (!gColorResource) {
        utils::logger().warn("[vvsr] bypass: no render-size fmt26 colour available");
        setStats(false, nullptr, 0, 0, 0, 0);
        return false;
    }
    auto* const colorResource = gColorResource;
    unsigned const colorW = gColorWidth;
    unsigned const colorH = gColorHeight;

    D3D12_RESOURCE_DESC const bbDesc = bb->GetDesc();
    unsigned const            dstW   = static_cast<unsigned>(bbDesc.Width);
    unsigned const            dstH   = bbDesc.Height;
    if (!dstW || !dstH || !ensureOutput(dev, dstW, dstH)) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                        = DXGI_FORMAT_R11G11B10_FLOAT;
    srv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip     = 0;
    srv.Texture2D.MipLevels           = 1;
    dev->CreateShaderResourceView(colorResource, &srv, gPipeline.srvCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension       = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice  = 0;
    dev->CreateUnorderedAccessView(gPipeline.output, nullptr, &uav, gPipeline.uavCpu);

    // The colour texture is read as an SRV; the engine leaves it in
    // ALL_SHADER_RESOURCE after TAAU on this path. If that assumption is wrong the
    // compute reads stale data, which is visible as a frozen/wrong picture - the
    // same class of failure the B0 dump state test would expose.
    transition(list, gPipeline.output, gPipeline.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    list->SetDescriptorHeaps(1, &gPipeline.heap);
    list->SetComputeRootSignature(gPipeline.root);
    list->SetPipelineState(gPipeline.pso);
    unsigned const params[5] = {colorW, colorH, dstW, dstH,
                                static_cast<unsigned>(gTonemapMode.load(std::memory_order_relaxed))};
    list->SetComputeRoot32BitConstants(0, 5, params, 0);
    list->SetComputeRootDescriptorTable(1, gPipeline.srvGpu);
    list->SetComputeRootDescriptorTable(2, gPipeline.uavGpu);
    list->SetComputeRootConstantBufferView(3, gToneCb->GetGPUVirtualAddress());
    list->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

    transition(list, gPipeline.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gPipeline.outputState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    transition(list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource       = gPipeline.output;
    src.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = bb;
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    transition(list, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(list, gPipeline.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gPipeline.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    setStats(true, colorResource, colorW, colorH, dstW, dstH);
    return true;
}

Stats stats() {
    std::lock_guard lock(gMutex);
    return gStats;
}

void cycleColor() {
    int const next = gColorChoice.fetch_add(1, std::memory_order_relaxed) + 1;
    gColorDirty.store(true, std::memory_order_relaxed);
    utils::logger().info("[vvsr] bypass colour choice -> {}", next);
}

void cycleTonemap() {
    int const next = (gTonemapMode.load(std::memory_order_relaxed) + 1) % 3;
    gTonemapMode.store(next, std::memory_order_relaxed);
    utils::logger().info("[vvsr] bypass tonemap -> {} (0=ACES+g, 1=g, 2=ACES)", next);
}

void release() {
    std::lock_guard lock(gMutex);
    if (gColorResource) gColorResource->Release();
    gColorResource   = nullptr;
    gColorWidth      = 0;
    gColorHeight     = 0;
    gColorGeneration = 0;
    if (gToneCb) gToneCb->Release();
    gToneCb = nullptr;
    if (gPipeline.output) gPipeline.output->Release();
    if (gPipeline.heap) gPipeline.heap->Release();
    if (gPipeline.pso) gPipeline.pso->Release();
    if (gPipeline.root) gPipeline.root->Release();
    gPipeline = Pipeline{};
    gStats    = Stats{};
}

} // namespace neurodragon::vv::bypass
