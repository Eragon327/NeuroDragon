#include "neurodragon/nr/D3D12Bridge.h"

#include <Windows.h>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace neurodragon::nr {
namespace {
NrLogFn gNrLog = nullptr;

void nrLog(int level, char const* format, ...) {
    if (!gNrLog) {
        return;
    }
    char    buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gNrLog(level, buffer);
}



bool readable(void const* p, std::size_t bytes) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    auto const start = reinterpret_cast<std::uintptr_t>(p);
    auto const begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return start + bytes <= begin + mbi.RegionSize;
}

bool inImage(void const* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return false;
    }
    return mbi.Type == MEM_IMAGE;
}

} // namespace

ResourceInfo describe(void* resource) {
    ResourceInfo info{};
    if (!resource || !readable(resource, sizeof(void*))) {
        return info;
    }
    // A real COM object starts with a vtable pointer into a loaded image. This
    // rejects stack addresses, string pointers and stale heap blocks before we
    // ever issue a virtual call, so a wrong guess prints "unreadable" instead of
    // faulting the client.
    auto** vtable = *reinterpret_cast<void***>(resource);
    if (!vtable || !inImage(vtable) || !readable(vtable, 3 * sizeof(void*))) {
        return info;
    }

    auto* res = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_DESC const desc = res->GetDesc();
    info.dimension     = static_cast<std::uint32_t>(desc.Dimension);
    info.format        = static_cast<std::uint32_t>(desc.Format);
    info.width         = desc.Width;
    info.height        = desc.Height;
    info.depthOrArray  = desc.DepthOrArraySize;
    info.mips          = desc.MipLevels;
    info.sampleCount   = desc.SampleDesc.Count;
    info.sampleQuality = desc.SampleDesc.Quality;
    info.flags         = static_cast<std::uint32_t>(desc.Flags);

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(res->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device) {
        info.device = device;
        device->Release();
    }
    return info;
}

namespace {

struct ColorDump {
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    bool                                    recorded = false;
    std::uint32_t                           width    = 0;
    std::uint32_t                           height   = 0;
    std::uint32_t                           rowPitch = 0;
    std::string                             fileName;
    int                                     domain = 0; // 0 linear HDR, 1 encoded, 2 linear LDR
};

ColorDump gDump;

float halfToFloat(std::uint16_t h) {
    std::uint32_t const sign = (h & 0x8000u) << 16;
    std::uint32_t const exp  = (h >> 10) & 0x1Fu;
    std::uint32_t       mant = h & 0x3FFu;
    std::uint32_t       bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                ++shift;
            }
            mant &= 0x3FFu;
            bits = sign | ((127 - 15 - shift) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out = 0;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::uint8_t toSrgb8(float v) {
    if (!(v > 0)) {
        return 0;
    }
    if (v > 1) {
        v = 1;
    }
    float const s = std::pow(v, 1.0f / 2.2f) * 255.0f;
    return static_cast<std::uint8_t>(s);
}

} // namespace

namespace {

void queueDump(void* commandList, void* resource, char const* fileName, int domain) {
    if (gDump.recorded || !commandList || !resource) {
        return;
    }
    auto* res = static_cast<ID3D12Resource*>(resource);
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    D3D12_RESOURCE_DESC const desc = res->GetDesc();

    ID3D12Device* device = nullptr;
    if (FAILED(res->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return;
    }

    std::uint32_t rowPitch = static_cast<std::uint32_t>(desc.Width) * 8u; // RGBA16F
    rowPitch               = (rowPitch + 255u) & ~255u;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = static_cast<UINT64>(rowPitch) * desc.Height;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc       = {1, 0};
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&gDump.staging)
        ))) {
        device->Release();
        return;
    }
    device->Release();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = res;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource                             = gDump.staging.Get();
    dst.Type                                  = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format      = desc.Format;
    dst.PlacedFootprint.Footprint.Width       = static_cast<UINT>(desc.Width);
    dst.PlacedFootprint.Footprint.Height      = desc.Height;
    dst.PlacedFootprint.Footprint.Depth       = 1;
    dst.PlacedFootprint.Footprint.RowPitch    = rowPitch;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);

    gDump.width    = static_cast<std::uint32_t>(desc.Width);
    gDump.height   = static_cast<std::uint32_t>(desc.Height);
    gDump.rowPitch = rowPitch;
    gDump.fileName = fileName ? fileName : "color_dump.bmp";
    gDump.domain   = domain;
    gDump.recorded = true;
}

} // namespace

void queueColorDump(void* commandList, void* color, char const* fileName) {
    queueDump(commandList, color, fileName, 0);
}

void queueColorDumpEncoded(void* commandList, void* color, char const* fileName) {
    queueDump(commandList, color, fileName, 1);
}

void queueColorDumpLinear(void* commandList, void* color, char const* fileName) {
    queueDump(commandList, color, fileName, 2);
}

bool flushColorDump(float* maxValue, std::uint32_t* nonZeroWords) {
    if (!gDump.recorded || !gDump.staging) {
        return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(gDump.rowPitch) * gDump.height};
    if (FAILED(gDump.staging->Map(0, &range, &mapped)) || !mapped) {
        return false;
    }

    auto const*        src       = static_cast<std::uint16_t const*>(mapped);
    float              peak      = 0.0f;
    double             sum       = 0.0;
    std::uint64_t      values    = 0;
    std::uint64_t      aboveOne  = 0;
    std::uint32_t      nzWords   = 0;
    std::size_t const  words     = static_cast<std::size_t>(gDump.rowPitch) * gDump.height / 2;
    for (std::size_t i = 0; i < words; ++i) {
        if (src[i] != 0) {
            ++nzWords;
        }
        if ((i & 3u) != 3u) { // skip alpha
            float const v = halfToFloat(src[i]);
            if (v > 0.0f) {
                sum += v;
                ++values;
                if (v > 1.0f) {
                    ++aboveOne;
                }
                if (v > peak) {
                    peak = v;
                }
            }
        }
    }
    if (values) {
        nrLog(
            0,
            "[dump] %s domain=%s max=%.3f mean=%.4f above1=%.2f%%",
            gDump.fileName.c_str(),
            gDump.domain == 0 ? "linear" : (gDump.domain == 1 ? "encoded" : "linear-ldr"),
            peak,
            sum / static_cast<double>(values),
            100.0 * static_cast<double>(aboveOne) / static_cast<double>(values)
        );
    }
    std::uint32_t const w        = gDump.width;
    std::uint32_t const h        = gDump.height;
    std::uint32_t const dataSize = w * h * 3u;
    std::uint32_t const fileSize = 54u + dataSize;

    std::string out(54u, '\0');
    out[0] = 'B';
    out[1] = 'M';
    auto put32 = [&out](std::size_t off, std::uint32_t v) { std::memcpy(out.data() + off, &v, 4); };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    std::uint16_t const planes = 1, bpp = 24;
    std::memcpy(out.data() + 26, &planes, 2);
    std::memcpy(out.data() + 28, &bpp, 2);
    put32(34, dataSize);

    out.resize(fileSize);
    for (std::uint32_t y = 0; y < h; ++y) {
        auto const* row = reinterpret_cast<std::uint16_t const*>(
            reinterpret_cast<std::uint8_t const*>(src) + static_cast<std::size_t>(gDump.rowPitch) * (h - 1 - y)
        );
        std::uint8_t* dstRow = reinterpret_cast<std::uint8_t*>(out.data() + 54u + static_cast<std::size_t>(y) * w * 3u);
        for (std::uint32_t x = 0; x < w; ++x) {
            float r = halfToFloat(row[x * 4 + 0]);
            float g = halfToFloat(row[x * 4 + 1]);
            float b = halfToFloat(row[x * 4 + 2]);
            std::uint8_t outR = 0;
            std::uint8_t outG = 0;
            std::uint8_t outB = 0;
            if (gDump.domain == 0) {
                // Reinhard so highlights stop turning into flat white, then gamma.
                auto const tone = [](float v) { return v / (1.0f + (v > 0.0f ? v : 0.0f)); };
                outR = toSrgb8(tone(r));
                outG = toSrgb8(tone(g));
                outB = toSrgb8(tone(b));
            } else if (gDump.domain == 2) {
                outR = toSrgb8(r);
                outG = toSrgb8(g);
                outB = toSrgb8(b);
            } else {
                auto const store = [](float v) {
                    if (!(v > 0.0f)) {
                        return std::uint8_t{0};
                    }
                    if (v > 1.0f) {
                        v = 1.0f;
                    }
                    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
                };
                outR = store(r);
                outG = store(g);
                outB = store(b);
            }
            dstRow[x * 3 + 0] = outB;
            dstRow[x * 3 + 1] = outG;
            dstRow[x * 3 + 2] = outR;
        }
    }
    gDump.staging->Unmap(0, nullptr);

    HANDLE file = ::CreateFileA(
        gDump.fileName.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool  ok      = ::WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) != 0;
    ::CloseHandle(file);
    if (maxValue) {
        *maxValue = peak;
    }
    if (nonZeroWords) {
        *nonZeroWords = nzWords;
    }
    gDump.recorded = false;
    gDump.staging.Reset();
    return ok;
}

// ---------------------------------------------------------------------------
// Stage 2: NR (NGX feature 0x12) at the engine's render resolution.
//
// Everything below goes through measured vtable slots, never through the SDK
// header: this runtime's parameter object puts the resource overload at slot 1,
// unsigned at 3 and float at 6 (docs/D §4 pitfall 1), which is not the order the
// public header declares.
// ---------------------------------------------------------------------------
namespace {

constexpr int kSlotSetPtr = 1;   // void* / ID3D12Resource*
constexpr int kSlotSetU32 = 3;   // unsigned
constexpr int kSlotSetI32 = 4;   // int (the 310.x runtime reads its flags as int)
constexpr int kSlotSetF32 = 6;   // float
constexpr int kSlotGetU32 = 11;  // unsigned read-back

using SetPtrFn          = void (*)(void*, char const*, void*);
using SetU32Fn          = void (*)(void*, char const*, unsigned);
using SetI32Fn          = void (*)(void*, char const*, int);
using SetF32Fn          = void (*)(void*, char const*, float);
using GetU32Fn          = int (*)(void*, char const*, unsigned*);
using GetResFn          = int (*)(void*, char const*, void**);
using CreateFeatureFn   = int (*)(void*, int, void*, void**);
using EvaluateFeatureFn = int (*)(void*, void*, void const*, void*, void*);
using GetCapabilitiesFn = int (*)(void**);
using InitExtFn =
    int (*)(unsigned long long, wchar_t const*, void*, int, void const*);
using InitFn = int (*)(unsigned long long, wchar_t const*, void*, int);

// TRP's "custom engine" identity: the generic CMS id from NVIDIA's mapping table
// plus the newest feature API version the 310.x runtime understands.
constexpr unsigned long long kNrGenericCmsId      = 0x0876232Cull;
constexpr int                kNrFeatureApiVersion = 0x15;

struct RuntimeApi {
    char const*       name         = nullptr;
    HMODULE           module       = nullptr;
    CreateFeatureFn   create       = nullptr;
    EvaluateFeatureFn evaluate     = nullptr;
    GetCapabilitiesFn capabilities = nullptr;
};

using ReleaseFeatureFn = int (*)(void*);

// Only Init_with_ProjectID takes a FeatureCommonInfo; lay it out exactly as
// nvsdk_ngx_defs.h declares it (path list, internal data, logging info).
struct NgxPathList {
    wchar_t const* const* path;
    unsigned              length;
};

struct NgxLoggingInfo {
    void* callback;
    int   minimumLevel;
    bool  disableOtherSinks;
};

struct NgxFeatureCommonInfo {
    NgxPathList     pathList;
    void*           internalData;
    NgxLoggingInfo  logging;
};

using InitProjectIdFn = int (*)(
    char const*,
    int,
    char const*,
    wchar_t const*,
    void*,
    NgxFeatureCommonInfo const*,
    int
);

// Any project id works for a CUSTOM engine; the core maps it to the generic
// custom CMS id (0x0876232C) that the runtime's Init_Ext expects.
constexpr char const* kNrProjectId     = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
constexpr char const* kNrEngineVersion = "0.1.0-neural-rendering-source";

// Captured from the engine's own NVSDK_NGX_D3D12_Init* call (see RenderProbe.cpp).
unsigned long long gEngineAppId = 0;
std::wstring       gEngineDataPath;

// Directory of this mod's DLL, supplied by the probe from LeviLamina's
// Mod::getModDir(). The runtime lives there under its original file name.
constexpr wchar_t const* kNrRuntimeFileName = L"nvngx_dlssnr_310.9.16.dll";
std::wstring             gModDirectory;

std::wstring moduleDirectory() { return gModDirectory; }

// Fixed name by default. A one-line override file keeps experiments explicit:
// the file names exactly one DLL that must already sit next to the mod - this is
// not a search.
std::wstring runtimeFileName() {
    std::wstring const override = gModDirectory + L"nr_runtime.txt";
    std::ifstream      file(override);
    if (file) {
        std::string line;
        if (std::getline(file, line)) {
            while (!line.empty()
                   && (line.back() == char(13) || line.back() == ' ' || line.back() == char(9))) {
                line.pop_back();
            }
            std::size_t const first = line.find_first_not_of(" 	");
            if (first != std::string::npos) {
                line = line.substr(first);
            }
            if (!line.empty()) {
                return std::wstring(line.begin(), line.end());
            }
        }
    }
    return kNrRuntimeFileName;
}

HMODULE loadRuntimeModule() {
    std::wstring const dir = moduleDirectory();
    if (dir.empty()) {
        nrLog(2, "[nr] 取不到模组 DLL 目录");
        return nullptr;
    }
    std::wstring const name = runtimeFileName();
    if (HMODULE existing = GetModuleHandleW(name.c_str())) {
        return existing;
    }
    nrLog(0, "[nr] runtime 文件名=%ls", name.c_str());
    std::wstring const path = dir + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        nrLog(2, "[nr] 缺少运行时：%ls（必须与模组 DLL 同目录；名字由 nr_runtime.txt 指定）", path.c_str());
        return nullptr;
    }
    HMODULE const module = LoadLibraryExW(
        path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    );
    if (!module) {
        nrLog(2, "[nr] LoadLibrary 失败 %ls (err=%lu)", path.c_str(), GetLastError());
    }
    return module;
}

template <class Fn>
Fn ngxSlot(void* object, int index) {
    auto** table = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(table[index]);
}

void paramSetPtr(void* params, char const* name, void* value) {
    if (params && name) {
        ngxSlot<SetPtrFn>(params, kSlotSetPtr)(params, name, value);
    }
}

void paramSetU32(void* params, char const* name, unsigned value) {
    if (params && name) {
        ngxSlot<SetU32Fn>(params, kSlotSetU32)(params, name, value);
    }
}

void paramSetI32(void* params, char const* name, int value) {
    if (params && name) {
        ngxSlot<SetI32Fn>(params, kSlotSetI32)(params, name, value);
    }
}

void paramSetF32(void* params, char const* name, float value) {
    if (params && name) {
        ngxSlot<SetF32Fn>(params, kSlotSetF32)(params, name, value);
    }
}

unsigned paramGetU32(void* params, char const* name) {
    unsigned value = 0;
    if (params && name) {
        (void)ngxSlot<GetU32Fn>(params, kSlotGetU32)(params, name, &value);
    }
    return value;
}

void* paramGetResource(void* params, char const* name) {
    // Measured read order for this runtime: 9 and 8 both return the D3D12
    // resource, 14 is the older guess and stays as a last resort.
    static constexpr int kSlots[] = {9, 8, 14};
    for (int const index : kSlots) {
        void* value = nullptr;
        if (ngxSlot<GetResFn>(params, index)(params, name, &value) && value) {
            return value;
        }
    }
    return nullptr;
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    // Leaving a UAV state and reading the resource right away needs a UAV barrier;
    // a transition barrier alone does not order the UAV writes.
    if (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = resource;
        barriers[1]               = barrier;
        cmd->ResourceBarrier(2, barriers);
        return;
    }
    cmd->ResourceBarrier(1, &barrier);
}

// ---------------------------------------------------------------------------
// HDR -> LDR encode (item 2 of §6.14): the network expects display-referred
// input, but the engine's Color is linear HDR (peaks around 69). This pass writes
// a Reinhard + gamma 2.2 copy of the 1485x835 region into its own texture; the
// feature reads that instead of the engine's Color.
// ---------------------------------------------------------------------------
// Optional overrides, read once from "<game dir>/nr_params.txt":
//   Style=1 / Intensity=0.5 / DepthInverted=0 / Sweep=1 (Sweep cycles Style 0..2 per window)
struct NrOverrides {
    int   style         = -1;
    int   enabled       = -1;
    float intensity     = -1.0f;
    float localTone     = -1.0f;
    float localStructure = -1.0f;
    int   depthInverted = -1;
    int   sweep         = 0;      // 1 = Style sweep, 2 = Enabled freeze test
    int   replace       = 0;      // 1 = copy the NR result back into engine Color (route A)
    float gainBoost     = 1.0f;   // Replace=5: exaggerate the network's gain for judging
    // TRP's Reconstruction knobs (src/FrameGen/NeuralRenderingReconstruction.h):
    float whitePoint     = 1.0f;  // producer's paper white, in the engine's linear units
    float transferStrength = 1.0f;
    float colourStrength   = 1.0f;
    float maxRatio         = 2.0f;
    // Replace=7: our own DLSS SR.
    int   srPreset         = 0x8000000B; // engine's own value for most tiers
    float srSharpness      = 0.35f;
    int   control          = 0;      // Replace=8 + Control=1: engine DLSS -> private texture
    int   stamp            = 0;      // Stamp=1: red square in our own SR input (visible proof)
    float mvecScale        = -1.0f;  // <0 = 1.0; set 1485 to test normalised vectors
    int   resetEveryFrame  = 0;      // NRReset=1: kills temporal history (diagnostic)
    int   srResetEveryFrame = 0;     // SRReset=1: reset *our* DLSS SR history每帧
    int   paramLog          = 0;     // ParamLog=1: log every engine NGX parameter write
    bool  parsed        = false;
};
NrOverrides gOverrides;
int         gWindowIndex = -1;

std::string paramFilePath() {
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    char narrow[MAX_PATH * 2]{};
    ::WideCharToMultiByte(CP_UTF8, 0, exe, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::string path{narrow};
    auto const  slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        path.resize(slash + 1);
    }
    return path + "nr_params.txt";
}

void parseOverrides(bool verbose = true) {
    if (gOverrides.parsed) {
        return;
    }
    gOverrides.parsed = true;
    std::ifstream file(paramFilePath());
    if (!file) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        auto const eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        auto const  trim  = [](std::string& text) {
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
                text.pop_back();
            }
        };
        trim(key);
        trim(value);
        try {
            if (key == "Style") {
                gOverrides.style = std::stoi(value);
            } else if (key == "Intensity") {
                gOverrides.intensity = std::stof(value);
            } else if (key == "LocalToneStrength") {
                gOverrides.localTone = std::stof(value);
            } else if (key == "LocalStructureStrength") {
                gOverrides.localStructure = std::stof(value);
            } else if (key == "DepthInverted") {
                gOverrides.depthInverted = std::stoi(value);
            } else if (key == "Enabled") {
                gOverrides.enabled = std::stoi(value);
            } else if (key == "Sweep") {
                gOverrides.sweep = std::stoi(value);
            } else if (key == "Replace") {
                gOverrides.replace = std::stoi(value);
            } else if (key == "GainBoost") {
                gOverrides.gainBoost = std::stof(value);
            } else if (key == "WhitePoint") {
                gOverrides.whitePoint = std::stof(value);
            } else if (key == "TransferStrength") {
                gOverrides.transferStrength = std::stof(value);
            } else if (key == "ColourStrength" || key == "ColorStrength") {
                gOverrides.colourStrength = std::stof(value);
            } else if (key == "MaxRatio") {
                gOverrides.maxRatio = std::stof(value);
            } else if (key == "DLSSPreset") {
                gOverrides.srPreset = std::stoi(value, nullptr, 0); // accepts 0x8000000B
            } else if (key == "Sharpness") {
                gOverrides.srSharpness = std::stof(value);
            } else if (key == "Control") {
                gOverrides.control = std::stoi(value);
            } else if (key == "Stamp") {
                gOverrides.stamp = std::stoi(value);
            } else if (key == "MVecScale") {
                gOverrides.mvecScale = std::stof(value);
            } else if (key == "NRReset") {
                gOverrides.resetEveryFrame = std::stoi(value);
            } else if (key == "SRReset") {
                gOverrides.srResetEveryFrame = std::stoi(value);
            } else if (key == "ParamLog") {
                gOverrides.paramLog = std::stoi(value);
            }
        } catch (...) {
            nrLog(1, "[nr] nr_params.txt 解析失败: %s", line.c_str());
        }
    }
    if (!verbose) {
        return;
    }
    nrLog(
        0,
        "[nr] nr_params.txt: Style=%d Intensity=%.2f LocalTone=%.2f Sweep=%d DepthInverted=%d Replace=%d GainBoost=%.2f",
        gOverrides.style,
        static_cast<double>(gOverrides.intensity),
        static_cast<double>(gOverrides.localTone),
        gOverrides.sweep,
        gOverrides.depthInverted,
        gOverrides.replace,
        static_cast<double>(gOverrides.gainBoost)
    );
}

ID3D12RootSignature* gEncodeRoot  = nullptr;

// Live tuning: re-read nr_params.txt every ~2 seconds so GainBoost (and Replace)
// can be judged without restarting the game.
void reloadOverridesIfDue() {
    static unsigned frame = 0;
    if (++frame % 120u != 0u) {
        return;
    }
    int const   beforeReplace = gOverrides.replace;
    float const beforeBoost   = gOverrides.gainBoost;
    gOverrides.parsed         = false;
    parseOverrides(false);
    if (beforeReplace != gOverrides.replace || beforeBoost != gOverrides.gainBoost) {
        nrLog(
            0,
            "[nr] nr_params 已更新：Replace=%d GainBoost=%.2f",
            gOverrides.replace,
            static_cast<double>(gOverrides.gainBoost)
        );
    }
}

ID3D12PipelineState* gEncodePso   = nullptr;
ID3D12DescriptorHeap* gEncodeHeap = nullptr;
ID3D12Resource*      gEncodedColor = nullptr;
ID3D12Resource*      gDecodedColor = nullptr;
// Replace=3 borrows the engine's Color for one frame: pixels are saved here and
// put back as soon as the engine's DLSS has read them (nrRestoreColor).
ID3D12Resource*      gColorBackup  = nullptr;
ID3D12Resource*      gColorBorrowed = nullptr;
bool                 gColorBackupPending = false;
unsigned             gColorBackupWidth   = 0;
unsigned             gColorBackupHeight  = 0;
UINT                 gEncodeStride = 0;
bool                 gEncodedIsSrv = false;
bool                 gDecodedIsSrv = false;

// One shader, four jobs (docs/D §6.16). Modes 0/1 are the early experiments and
// stay for A/B; modes 3/4 are Theo's Render Pipeline's actual contract
// (src/FrameGen/SourceDLSSGNeuralResolveShader.h in TRP, MIT):
//   mode 3 = TRP encode: Colour / WhitePoint, RGB shoulder bound, then sRGB.
//   mode 4 = TRP "Ratio" resolve: rebase the network's *result* onto the original's
//            luminance (ratio = by<ay ? by/ay : (by-ay+ny)/ny), transfer chroma in
//            OkLab, then blend by TransferStrength / ColourStrength and clamp to
//            base * MaxRatio. Highlights come from the original, never from the
//            network, so nothing is flattened.
constexpr char const* kEncodeHlsl = R"hlsl(
Texture2D<float4> baseTex  : register(t0); // original linear HDR colour
Texture2D<float4> inputTex : register(t1); // what the network was fed (encoded)
Texture2D<float4> nrTex    : register(t2); // the network's result (encoded)
RWTexture2D<float4> target : register(u0);
cbuffer Params : register(b0) {
    uint  width;
    uint  height;
    uint  mode;
    float whitePoint;
    float transferStrength;
    float colourStrength;
    float maxRatio;
    float spare;
};
float3 Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float LumaF(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 ToSRGB(float3 c) {
    c = saturate(c);
    return lerp(c * 12.92, 1.055 * pow(max(c, 0.0), 0.416666657) - 0.055, step(0.0031308, c));
}
float3 ToLinear(float3 c) {
    c = saturate(c);
    return lerp(c * 0.0773993805, pow((c + 0.0549999997) * 0.947867334, 2.40000010), step(0.0404499993, c));
}
// Same shoulder TRP uses: bounds the brightest channel with one RGB scale so
// saturated highlights cannot exceed one channel-by-channel after ToSRGB.
float3 Shoulder(float3 c) {
    float peak = max(c.r, max(c.g, c.b));
    if (peak > 0.75) {
        c *= (0.75 + 0.25 * (1.0 - exp2(-5.77078009 * (peak - 0.75)))) / peak;
    }
    return c;
}
float3 ToLab(float3 c) {
    float3 lms = float3(dot(c, float3(0.412221462, 0.536332548, 0.0514459945)),
                        dot(c, float3(0.211903498, 0.680699527, 0.107396960)),
                        dot(c, float3(0.0883024633, 0.281718850, 0.629978716)));
    lms = sign(lms) * pow(abs(lms), 0.333333343);
    return float3(dot(lms, float3(0.210454255, 0.793617785, -0.00407204684)),
                  dot(lms, float3(1.97799850, -2.42859221, 0.450593710)),
                  dot(lms, float3(0.0259040371, 0.782771766, -0.808675766)));
}
float3 FromLab(float3 c) {
    float3 lms = float3(dot(c, float3(1, 0.396337777, 0.215803757)),
                        dot(c, float3(1, -0.105561346, -0.0638541728)),
                        dot(c, float3(1, -0.0894841775, -1.29148555)));
    lms = lms * lms * lms;
    return float3(dot(lms, float3(4.07674170, -3.30771160, 0.230969936)),
                  dot(lms, float3(-1.26843798, 2.60975742, -0.341319382)),
                  dot(lms, float3(-0.00419608643, -0.703418612, 1.70761466)));
}
float3 GamutClip(float3 c) {
    float3 wide = float3(dot(c, float3(0.613097012, 0.339522988, 0.0473789982)),
                         dot(c, float3(0.0701939985, 0.916354001, 0.0134520000)),
                         dot(c, float3(0.0206160005, 0.109569997, 0.869814992)));
    wide = max(wide, 0);
    return float3(dot(wide, float3(1.70505095, -0.621792018, -0.0832590014)),
                  dot(wide, float3(-0.130255997, 1.14080501, -0.0105480002)),
                  dot(wide, float3(-0.0240030009, -0.128968999, 1.15297198)));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float3 outC;
    if (mode == 0) {
        float3 c = max(baseTex.Load(int3(id.xy, 0)).rgb, 0.0);
        c = c / (1.0 + c);
        c = pow(c, 1.0 / 2.2);
        outC = c;
    } else if (mode == 1) {
        float3 c = max(baseTex.Load(int3(id.xy, 0)).rgb, 0.0);
        c = pow(saturate(c), 2.2);
        c = c / max(1.0 - c, 1.0 / 255.0);
        outC = c;
    } else if (mode == 2) {
        float3 base  = max(baseTex.Load(int3(id.xy, 0)).rgb, 0.0);
        float3 inp   = max(inputTex.Load(int3(id.xy, 0)).rgb, 1e-4);
        float3 nr    = max(nrTex.Load(int3(id.xy, 0)).rgb, 0.0);
        float3 ratio = clamp(nr / inp, 0.5, 2.0);
        outC = base * clamp(1.0 + (ratio - 1.0) * transferStrength, 0.25, 4.0);
    } else if (mode == 3) {
        // TRP encode: the producer's linear RGB normalised by its white point.
        float3 c = max(baseTex.Load(int3(id.xy, 0)).rgb, 0.0);
        outC = ToSRGB(Shoulder(c / max(whitePoint, 0.0001)));
    } else {
        // TRP Ratio resolve, 1:1 (no Lanczos: input and work extents match).
        float3 original = max(baseTex.Load(int3(id.xy, 0)).rgb, 0.0);
        float3 a = inputTex.Load(int3(id.xy, 0)).rgb; // what the network saw
        float3 n = nrTex.Load(int3(id.xy, 0)).rgb;    // what it produced
        if (transferStrength == 0.0) {
            outC = original;
        } else {
            a = ToLinear(a);
            n = ToLinear(n);
            float  white = max(whitePoint, 0.0001);
            float3 base  = original / white;
            float3 result = base;
            float  by = LumaF(base), ny = LumaF(n), ay = LumaF(a);
            if (ny > 0.00001) {
                float ratio = by < ay ? by / max(ay, 0.000001) : (max(by - ay, 0.0) + ny) / ny;
                float3 scaled = ToLab(n * ratio);
                float3 normal = ToLab(n);
                float  chroma = length(normal.yz);
                scaled.yz = normal.yz * (chroma == 0.0 ? 1.0 : length(scaled.yz) / chroma);
                result = lerp(base, GamutClip(FromLab(scaled)), transferStrength);
            }
            float lumaRatio = max((LumaF(result) + 0.001953125) / (max(by, 0.0) + 0.001953125), 0.0);
            result = max(lerp(base * lumaRatio, result, colourStrength), 0.0);
            float resultY = LumaF(result), maxY = max(by, 0.0) * maxRatio;
            if (resultY > maxY) {
                result *= maxY / max(resultY, 0.0000000001);
            }
            outC = min(result * white, 65504.0);
        }
    }
    if (any(outC != outC) || any(isinf(outC))) outC = 0.0;
    target[id.xy] = float4(outC, 1.0);
}
)hlsl";

using D3DCompileFn = HRESULT (*)(
    LPCVOID, SIZE_T, LPCSTR, void const*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
using SerializeRootFn =
    HRESULT (*)(D3D12_ROOT_SIGNATURE_DESC const*, int, void**, void**);

bool ensureEncoder(ID3D12Device* device, D3D12_RESOURCE_DESC const& colorDesc) {
    if (gEncodePso) {
        return true;
    }
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto const compile = compiler
        ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"))
        : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto const serialize = d3d12
        ? reinterpret_cast<SerializeRootFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        : nullptr;
    if (!compile || !serialize) {
        nrLog(1, "[nr] HDR→LDR encode 不可用（缺 D3DCompile / SerializeRootSignature）");
        return false;
    }

    void* code = nullptr;
    void* errors = nullptr;
    HRESULT const hr = compile(
        kEncodeHlsl, std::strlen(kEncodeHlsl), "nr_encode", nullptr, nullptr, "main", "cs_5_1",
        3u /* optimization level 3 */, 0, &code, &errors
    );
    if (errors) {
        auto* blob = static_cast<ID3DBlob*>(errors);
        nrLog(1, "[nr] encode shader 编译失败: %s", static_cast<char const*>(blob->GetBufferPointer()));
        blob->Release();
    }
    if (FAILED(hr) || !code) {
        return false;
    }

    // One descriptor table with four ranges (three SRVs, one UAV) plus four root
    // constants. A multi-table variant failed CreateComputePipelineState with
    // E_INVALIDARG, so everything stays in one table.
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    ranges[2].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors                    = 1;
    ranges[2].BaseShaderRegister                = 2;
    ranges[2].OffsetInDescriptorsFromTableStart = 2;
    ranges[3].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors                    = 1;
    ranges[3].BaseShaderRegister                = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = 3;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {4, ranges};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants        = {0, 0, 8}; // w, h, mode + four TRP knobs + spare
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters   = parameters;
    rootDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    void* signature  = nullptr;
    void* rootErrors = nullptr;
    HRESULT const rootHr = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &rootErrors);
    if (rootErrors) {
        static_cast<ID3DBlob*>(rootErrors)->Release();
    }
    if (FAILED(rootHr) || !signature) {
        static_cast<ID3DBlob*>(code)->Release();
        nrLog(1, "[nr] encode root signature 失败 0x%08X", static_cast<unsigned>(rootHr));
        return false;
    }
    auto* sigBlob = static_cast<ID3DBlob*>(signature);
    HRESULT const rsHr = device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&gEncodeRoot));
    sigBlob->Release();
    if (FAILED(rsHr)) {
        static_cast<ID3DBlob*>(code)->Release();
        nrLog(1, "[nr] encode CreateRootSignature 失败 0x%08X", static_cast<unsigned>(rsHr));
        return false;
    }

    auto* codeBlob = static_cast<ID3DBlob*>(code);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = gEncodeRoot;
    psoDesc.CS             = {codeBlob->GetBufferPointer(), codeBlob->GetBufferSize()};
    HRESULT const psoHr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&gEncodePso));
    nrLog(
        FAILED(psoHr) ? 1 : 0,
        "[nr] encode shader %llu 字节，PSO ret=0x%08X",
        static_cast<unsigned long long>(codeBlob->GetBufferSize()),
        static_cast<unsigned>(psoHr)
    );
    codeBlob->Release();
    if (FAILED(psoHr)) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 4;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gEncodeHeap)))) {
        nrLog(1, "[nr] encode 描述符堆失败");
        return false;
    }
    gEncodeStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_DESC encodedDesc = colorDesc;
    encodedDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &encodedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&gEncodedColor)))) {
        nrLog(1, "[nr] encoded color 分配失败");
        return false;
    }
    // Route A needs the way back: the NR result is display-referred, engine Color
    // is linear HDR, so the copy-back goes through the inverse of this pass.
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &encodedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&gDecodedColor)))) {
        nrLog(1, "[nr] decoded color 分配失败（Replace 将退化为直拷）");
        gDecodedColor = nullptr;
    }
    nrLog(0, "[nr] HDR↔LDR shader 就绪（Reinhard + gamma 2.2 / 反变换）");
    return true;
}

// Binds three SRVs (t0, t1, t2) and one UAV (u0) in the single table.
void writeDescriptors(ID3D12Device* device, ID3D12Resource* s0, ID3D12Resource* s1,
                      ID3D12Resource* s2, ID3D12Resource* target) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = gEncodeHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    ID3D12Resource* const sources[3] = {s0, s1, s2};
    for (int i = 0; i < 3; ++i) {
        if (sources[i]) {
            device->CreateShaderResourceView(sources[i], &srv, handle);
        }
        handle.ptr += gEncodeStride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(target, nullptr, &uav, handle);
}

// Root constants shared by every dispatch: dims, mode and the (bit-cast) gain
// boost so the gain mode can be exaggerated for judging without a rebuild.
void setPassConstants(ID3D12GraphicsCommandList* cmd, unsigned w, unsigned h, unsigned mode,
                      float whitePoint, float transferStrength, float colourStrength,
                      float maxRatio) {
    auto bits = [](float f) {
        std::uint32_t out = 0;
        std::memcpy(&out, &f, sizeof(out));
        return out;
    };
    std::uint32_t const v[8] = {
        w,
        h,
        mode,
        bits(whitePoint),
        bits(transferStrength),
        bits(colourStrength),
        bits(maxRatio),
        0u
    };
    cmd->SetComputeRoot32BitConstants(1, 8, v, 0);
}

struct NrState {
    void*              params   = nullptr;
    void*              feature  = nullptr;
    CreateFeatureFn    create   = nullptr;
    EvaluateFeatureFn  evaluate = nullptr;
    ReleaseFeatureFn   release  = nullptr;
    ID3D12Resource*    output   = nullptr;
    unsigned           width    = 0;
    unsigned           height   = 0;
    bool               created  = false;
    bool               attempted = false;
    bool               outputWasSrv = false;
    bool               shutdown    = false;
    bool               resourceRoundtripChecked = false;
    unsigned           evalCount   = 0;
};

NrState gNr;

// Timestamps: kSlots distinct frame slots, two brackets (NR / engine DLSS) per
// slot. A pair is harvested kHarvestDelay frames later, because nothing in this
// process can signal a fence on the game's queue.
constexpr int kStampSlots   = 8;
constexpr int kStampKinds   = 2;
constexpr int kStampPerSlot = kStampKinds * 2;
constexpr std::uint64_t kHarvestDelay = 3;

struct StampRing {
    ID3D12QueryHeap*     heap      = nullptr;
    ID3D12Resource*      readback  = nullptr;
    std::uint64_t*       mapped    = nullptr;
    std::uint64_t        frequency = 0;
    std::uint64_t        frameOf[kStampSlots]{};
    unsigned             pending[kStampSlots]{};
    bool                 active = false;
    std::vector<double>  samples[kStampKinds];
    unsigned             dropped[kStampKinds]{};
    unsigned             recorded[kStampKinds]{};
};

StampRing gStamps;

bool ensureStamps(ID3D12Device* device) {
    if (gStamps.active) {
        return true;
    }
    D3D12_QUERY_HEAP_DESC query{};
    query.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = static_cast<UINT>(kStampSlots * kStampPerSlot);
    if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&gStamps.heap)))) {
        nrLog(1, "[nr] timestamp heap 创建失败，计时关闭");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width            = sizeof(std::uint64_t) * kStampSlots * kStampPerSlot;
    buffer.Height           = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels        = 1;
    buffer.SampleDesc       = {1, 0};
    buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&gStamps.readback)))) {
        nrLog(1, "[nr] timestamp readback 分配失败，计时关闭");
        return false;
    }
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(buffer.Width)};
    if (FAILED(gStamps.readback->Map(0, &readRange, reinterpret_cast<void**>(&gStamps.mapped)))
        || !gStamps.mapped) {
        nrLog(1, "[nr] timestamp readback map 失败，计时关闭");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && queue) {
        queue->GetTimestampFrequency(&gStamps.frequency);
        queue->Release();
    }
    if (!gStamps.frequency) {
        nrLog(1, "[nr] 取不到 timestamp frequency，计时关闭");
        return false;
    }
    gStamps.active = true;
    nrLog(0, "[nr] timestamp 频率 %llu", static_cast<unsigned long long>(gStamps.frequency));
    return true;
}

void reportOne(int id, char const* name) {
    auto const& values = gStamps.samples[id];
    if (values.empty()) {
        nrLog(1, "[nr] gpu %s: 没有可用样本 (recorded=%u dropped=%u)", name, gStamps.recorded[id],
              gStamps.dropped[id]);
        return;
    }
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double       sum    = 0.0;
    for (double const v : sorted) {
        sum += v;
    }
    double const median = sorted[sorted.size() / 2];
    nrLog(0, "[nr] gpu %s ms median=%.3f mean=%.3f min=%.3f max=%.3f n=%zu dropped=%u", name,
          median, sum / static_cast<double>(sorted.size()), sorted.front(), sorted.back(),
          sorted.size(), gStamps.dropped[id]);
}

} // namespace

// ---------------------------------------------------------------------------
// Own-device probe: TRP initialises NGX with a D3D12 device *it* created, while
// this mod hands over the engine's device. If the runtime refuses the engine's
// device, initialising against a fresh device for the same adapter is the one
// remaining difference we can test without touching the frame path.
// ---------------------------------------------------------------------------
namespace {

using D3D12CreateDeviceFn = HRESULT (*)(IUnknown*, int, REFIID, void**);

IDXGIAdapter1* findAdapter(ID3D12Device* device) {
    LUID const luid = device->GetAdapterLuid();
    HMODULE    dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        return nullptr;
    }
    using CreateFactory1Fn = HRESULT (*)(REFIID, void**);
    auto const createFactory =
        reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!createFactory
        || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))
        || !factory) {
        FreeLibrary(dxgi);
        return nullptr;
    }
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(i, &candidate) != S_OK || !candidate) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
            chosen = candidate;
            break;
        }
        candidate->Release();
    }
    factory->Release();
    FreeLibrary(dxgi);
    return chosen;
}

// Returns true when the runtime accepted Init_Ext AND CreateFeature on its own
// device. Nothing is submitted: a successful CreateFeature already answers the
// question, and leaving recorded work unsent is harmless.
bool probeOwnDevice(
    ID3D12Device*    engineDevice,
    void*            params,
    CreateFeatureFn  create,
    wchar_t const*   dataPath,
    unsigned         width,
    unsigned         height
) {
    IDXGIAdapter1* adapter = findAdapter(engineDevice);
    if (!adapter) {
        nrLog(1, "[nr] 私有设备: 找不到同一 adapter");
        return false;
    }
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto const createDevice = d3d12
        ? reinterpret_cast<D3D12CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"))
        : nullptr;
    if (!createDevice) {
        nrLog(1, "[nr] 私有设备: 取不到 D3D12CreateDevice");
        adapter->Release();
        return false;
    }
    ID3D12Device* device = nullptr;
    HRESULT const hr = createDevice(
        adapter,
        D3D_FEATURE_LEVEL_12_0,
        __uuidof(ID3D12Device),
        reinterpret_cast<void**>(&device)
    );
    adapter->Release();
    if (FAILED(hr) || !device) {
        nrLog(1, "[nr] 私有设备: D3D12CreateDevice 失败 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    nrLog(0, "[nr] 私有设备: %p（引擎设备 %p）", static_cast<void*>(device),
          static_cast<void*>(engineDevice));

    HMODULE runtime = loadRuntimeModule();
    auto const initExt = runtime
        ? reinterpret_cast<InitExtFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_Init_Ext"))
        : nullptr;
    if (!initExt) {
        nrLog(1, "[nr] 私有设备: 模型没有 Init_Ext");
        device->Release();
        return false;
    }
    int const initRet = initExt(kNrGenericCmsId, dataPath, device, kNrFeatureApiVersion, nullptr);
    nrLog(
        initRet == 1 || static_cast<unsigned>(initRet) == 0xBAD00003u ? 0 : 1,
        "[nr] 私有设备 Init_Ext ret=0x%08X",
        static_cast<unsigned>(initRet)
    );
    bool ok = false;
    if (initRet == 1 || static_cast<unsigned>(initRet) == 0xBAD00003u) {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue*    queue     = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list  = nullptr;
        if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))
            && SUCCEEDED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
            && SUCCEEDED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
            void* handle = nullptr;
            int const ret = create(list, 0x12, params, &handle);
            nrLog(
                ret == 1 && handle ? 0 : 2,
                "[nr] 私有设备 CreateFeature(0x12) ret=0x%08X handle=%p %ux%u",
                static_cast<unsigned>(ret),
                handle,
                width,
                height
            );
            ok = ret == 1 && handle != nullptr;
        } else {
            nrLog(1, "[nr] 私有设备: 建队列/分配器/命令列表失败");
        }
        if (list) {
            list->Release();
        }
        if (allocator) {
            allocator->Release();
        }
        if (queue) {
            queue->Release();
        }
    }
    device->Release();
    if (d3d12) {
        FreeLibrary(d3d12);
    }
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// "Why is feature 0x12 rejected?" - NVSDK_NGX_D3D12_GetFeatureRequirements.
// The structs below mirror nvsdk_ngx_defs.h; the NGX headers themselves stay out
// of this translation unit (docs/D §4 pitfall 5).
// ---------------------------------------------------------------------------
namespace {

struct NgxProjectIdDesc {
    char const* projectId;
    int         engineType;
    char const* engineVersion;
};

struct NgxAppIdentifier {
    int identifierType; // 0 = application id, 1 = project id
    union {
        NgxProjectIdDesc  desc;
        unsigned long long appId;
    } v;
};

struct NgxDiscoveryInfo {
    int                       sdkVersion;
    int                       featureId;
    NgxAppIdentifier          identifier;
    wchar_t const*            appDataPath;
    void const*               featureInfo;
};

struct NgxFeatureRequirement {
    int      featureSupported; // 0 = supported; bitfield of NVSDK_NGX_FeatureSupportResult_*
    unsigned minHwArchitecture;
    char     minOsVersion[255];
};

using GetFeatureRequirementsFn = int (*)(void*, NgxDiscoveryInfo const*, NgxFeatureRequirement*);

void logFeatureRequirementsFrom(
    void*          function,
    ID3D12Device*  device,
    wchar_t const* dataPath,
    char const*    tag
) {
    auto const fn = reinterpret_cast<GetFeatureRequirementsFn>(function);
    if (!fn) {
        nrLog(1, "[nr] %s 没有 GetFeatureRequirements", tag);
        return;
    }

    IDXGIAdapter1* adapter = nullptr;
    LUID const     luid    = device->GetAdapterLuid();
    HMODULE        dxgi    = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        using CreateFactory1Fn = HRESULT (*)(REFIID, void**);
        auto const createFactory =
            reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        IDXGIFactory1* factory = nullptr;
        if (createFactory
            && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))
            && factory) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory->EnumAdapters1(i, &candidate) != S_OK || !candidate) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                candidate->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == luid.LowPart
                    && desc.AdapterLuid.HighPart == luid.HighPart) {
                    adapter = candidate;
                    break;
                }
                candidate->Release();
            }
            factory->Release();
        }
        FreeLibrary(dxgi);
    }

    static constexpr int kFeatures[] = {0x12, 0x01}; // NR, then DLSS SR as control
    for (int const feature : kFeatures) {
        NgxDiscoveryInfo info{};
        info.sdkVersion      = kNrFeatureApiVersion;
        info.featureId       = feature;
        info.identifier.identifierType        = 0;
        info.identifier.v.appId               = kNrGenericCmsId;
        info.appDataPath     = dataPath;
        info.featureInfo     = nullptr;
        NgxFeatureRequirement requirement{};
        int const             ret = fn(adapter, &info, &requirement);
        nrLog(
            ret == 1 && requirement.featureSupported == 0 ? 0 : 1,
            "[nr] %s GetFeatureRequirements(0x%02X) ret=0x%08X supported=%d (0=ok,1=check,2=driver,4=adapter,8=os,16=notimpl) minHW=0x%X minOS=%s",
            tag,
            feature,
            static_cast<unsigned>(ret),
            requirement.featureSupported,
            requirement.minHwArchitecture,
            requirement.minOsVersion
        );
    }
    if (adapter) {
        adapter->Release();
    }
}

} // namespace

void nrSetLogger(NrLogFn fn) { gNrLog = fn; }

void nrSetWindowIndex(int index) { gWindowIndex = index; }

void nrSetModDirectory(wchar_t const* directory) {
    gModDirectory = directory ? directory : L"";
    if (!gModDirectory.empty() && gModDirectory.back() != L'\\' && gModDirectory.back() != L'/') {
        gModDirectory.push_back(L'\\');
    }
}

void nrSetEngineInit(unsigned long long appId, wchar_t const* dataPath) {
    if (appId) {
        gEngineAppId = appId;
    }
    if (dataPath) {
        gEngineDataPath = dataPath;
    }
}

bool nrCreate(void* commandList, void* colorResource, unsigned width, unsigned height,
              void* engineParams, void* createFeature, void* evaluateFeature) {
    if (gNr.shutdown) {
        return false;
    }
    if (gNr.attempted) {
        return gNr.created;
    }
    gNr.attempted = true;
    if (!commandList || !colorResource || !width || !height) {
        nrLog(2, "[nr] create: 参数不完整 (cl=%p color=%p %ux%u)", commandList, colorResource, width,
              height);
        return false;
    }
    if (!createFeature || !evaluateFeature) {
        nrLog(2, "[nr] create: 缺 original NGX 导出指针");
        return false;
    }

    auto* color = static_cast<ID3D12Resource*>(colorResource);
    ID3D12Device* device = nullptr;
    if (FAILED(color->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
        || !device) {
        nrLog(2, "[nr] create: 从 Color 取不到 ID3D12Device");
        return false;
    }

    D3D12_RESOURCE_DESC outDesc = color->GetDesc();
    outDesc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    outDesc.MipLevels           = 1;
    outDesc.DepthOrArraySize    = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT const createHr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &outDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&gNr.output)
    );
    if (FAILED(createHr)) {
        nrLog(2, "[nr] 输出纹理创建失败 0x%08X (%llux%llu fmt=%u)", static_cast<unsigned>(createHr),
              static_cast<unsigned long long>(outDesc.Width),
              static_cast<unsigned long long>(outDesc.Height), static_cast<unsigned>(outDesc.Format));
        return false;
    }
    gNr.width  = width;
    gNr.height = height;

    // Replace=3 needs somewhere to park the engine's own pixels for one frame.
    D3D12_RESOURCE_DESC backupDesc = outDesc;
    backupDesc.Flags               = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &backupDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&gColorBackup)))) {
        nrLog(1, "[nr] Color 备份纹理分配失败（Replace=3 不可用）");
        gColorBackup = nullptr;
    }

    parseOverrides();
    ensureEncoder(device, outDesc);

    bool const stamps = ensureStamps(device);

    // --- NGX: the model module first (TRP order), the core as the fallback -----
    //
    // The engine already ran Init on the core, but the core's CreateFeature(0x12)
    // answered 0xBAD0000C ("OutOfDate") on 2026-10-21. TRP never goes through the
    // core for the feature: it loads the runtime module, calls the module's
    // Init_Ext (the plain Init in both model builds is a 0xBAD00001 stub) and
    // creates the feature with the module's own exports. Both orders are tried so
    // one run reports both return codes.
    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    if (!core) {
        core = GetModuleHandleW(L"nvngx.dll");
    }
    if (!core) {
        nrLog(2, "[nr] NGX 核心模块未加载");
        device->Release();
        return false;
    }
    nrLog(0, "[nr] core module=%p", static_cast<void*>(core));

    // Shared application data path (NGX wants a writable directory here).
    wchar_t dataPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dataPath);
    std::wstring dataDir = std::wstring(dataPath) + L"NeuroDragon";
    CreateDirectoryW(dataDir.c_str(), nullptr);
    dataDir += L"\\NGX";
    CreateDirectoryW(dataDir.c_str(), nullptr);

    logFeatureRequirementsFrom(
        reinterpret_cast<void*>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetFeatureRequirements")),
        device,
        dataDir.c_str(),
        "core"
    );

    // TRP owns the process' NGX session because it is the first (only) consumer,
    // so its runtime Init_Ext names the id the core was initialised with. Here the
    // engine owns the session; instead of re-initialising the core (which would
    // risk the engine's own DLSS feature), reuse the app id captured from the
    // engine's Init call - that is the id this core will accept.
    nrLog(
        gEngineAppId ? 0 : 1,
        "[nr] 引擎初始化上下文 appId=%#llx dataPath=%ls（0/空 表示没抓到）",
        static_cast<unsigned long long>(gEngineAppId),
        gEngineDataPath.empty() ? L"<none>" : gEngineDataPath.c_str()
    );

    // The core loads the NR model itself (2026-10-21, driver 617.42): the file
    // must be named nvngx_dlssnr.dll and sit in the application directory. Do not
    // preload it here - that only creates a second 266 MB copy.
    HMODULE runtime = GetModuleHandleW(L"nvngx_dlssnr.dll");
    nrLog(
        runtime ? 0 : 1,
        "[nr] runtime module=%p（由 core 自己从应用目录加载）",
        static_cast<void*>(runtime)
    );

    RuntimeApi runtimeApi{};
    RuntimeApi coreApi{};
    auto const coreCapabilities = reinterpret_cast<GetCapabilitiesFn>(
        GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto const runtimeInitExt = reinterpret_cast<InitExtFn>(
        GetProcAddress(runtime, "NVSDK_NGX_D3D12_Init_Ext"));
    // The runtime build exports no GetCapabilityParameters, so it borrows the
    // core's parameter object - the same mix TRP uses.
    runtimeApi.name         = "runtime";
    runtimeApi.module       = runtime;
    runtimeApi.create       = reinterpret_cast<CreateFeatureFn>(
        GetProcAddress(runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    runtimeApi.evaluate     = reinterpret_cast<EvaluateFeatureFn>(
        GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
    runtimeApi.capabilities = coreCapabilities;
    coreApi.name            = "core";
    coreApi.module          = core;
    coreApi.create          = reinterpret_cast<CreateFeatureFn>(createFeature);
    coreApi.evaluate        = reinterpret_cast<EvaluateFeatureFn>(evaluateFeature);
    coreApi.capabilities    = coreCapabilities;

    // Do NOT re-initialise the core: the engine owns the session, and calling
    // Init_ProjectID / Init_Ext on it crashes inside _nvngx.dll (2026-10-21,
    // read at 0x15). Everything below only *uses* the core.
    bool const sessionClaimed = false;

    void*     params = nullptr;
    int const capsRet = coreCapabilities ? coreCapabilities(&params) : 0xBAD00001;
    if (capsRet != 1 || !params) {
        nrLog(2, "[nr] GetCapabilityParameters ret=0x%08X params=%p", static_cast<unsigned>(capsRet),
              params);
        device->Release();
        return false;
    }
    if (engineParams) {
        bool const sameTable =
            *reinterpret_cast<void***>(params) == *reinterpret_cast<void***>(engineParams);
        nrLog(
            sameTable ? 0 : 1,
            "[nr] params vtable ours=%p engine=%p same=%d",
            *reinterpret_cast<void**>(params),
            engineParams ? *reinterpret_cast<void**>(engineParams) : nullptr,
            sameTable ? 1 : 0
        );
    }

    paramSetU32(params, "CreationNodeMask", 1);
    paramSetU32(params, "VisibilityNodeMask", 1);
    paramSetU32(params, "PerfQualityValue", 2);
    paramSetI32(params, "DLSSNR.Hint.Render.Preset", 0);
    paramSetF32(params, "DLSSNR.ScalingRatio", 1.0f);
    paramSetU32(params, "DLSSNR.Width", width);
    paramSetU32(params, "DLSSNR.Height", height);
    paramSetI32(params, "DLSSNR.Enabled", 1);
    nrLog(
        paramGetU32(params, "DLSSNR.Width") == width ? 0 : 1,
        "[nr] 参数往返 DLSSNR.Width=%u 期望=%u",
        paramGetU32(params, "DLSSNR.Width"),
        width
    );

    auto createWith = [&](RuntimeApi const& api, char const* tag) -> bool {
        if (!api.create || !api.evaluate) {
            nrLog(1, "[nr] %s 不可用（create=%d eval=%d）", tag, api.create ? 1 : 0,
                  api.evaluate ? 1 : 0);
            return false;
        }
        void*                    handle = nullptr;
        // The core loads the ~264 MB NR model inside this call, so the first
        // CreateFeature of a session can stall for seconds. Log both edges so a
        // stall is attributable instead of looking like a freeze.
        LARGE_INTEGER freq{};
        LARGE_INTEGER before{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&before);
        nrLog(
            0,
            "[nr] %s CreateFeature(0x12) 开始（核心在此加载 NR 模型，可能耗时数秒）",
            tag
        );
        int const ret = api.create(commandList, 0x12, params, &handle);
        LARGE_INTEGER after{};
        QueryPerformanceCounter(&after);
        double const ms = freq.QuadPart
                        ? static_cast<double>(after.QuadPart - before.QuadPart) * 1000.0 /
                              static_cast<double>(freq.QuadPart)
                        : 0.0;
        nrLog(
            ret == 1 && handle ? 0 : 2,
            "[nr] %s CreateFeature(0x12) ret=0x%08X handle=%p 尺寸=%ux%u 用时=%.0f ms",
            tag,
            static_cast<unsigned>(ret),
            handle,
            width,
            height,
            ms
        );
        if (ret == 1 && handle) {
            gNr.params   = params;
            gNr.feature  = handle;
            gNr.create   = api.create;
            gNr.evaluate = api.evaluate;
            gNr.release  = reinterpret_cast<ReleaseFeatureFn>(
                api.module ? GetProcAddress(api.module, "NVSDK_NGX_D3D12_ReleaseFeature") : nullptr);
            if (!gNr.release) {
                gNr.release = reinterpret_cast<ReleaseFeatureFn>(
                    GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
            }
            gNr.created  = true;
            if (!stamps) {
                nrLog(1, "[nr] feature 已建，但没有 GPU 计时");
            }
            return true;
        }
        switch (static_cast<unsigned>(ret)) {
        case 0xBAD00002u:
            nrLog(2, "[nr] 提示: 0xBAD00002 = PlatformError（平台/依赖错误，见 NGX 日志）");
            break;
        case 0xBAD0000Cu:
            nrLog(2, "[nr] 提示: 0xBAD0000C = OutOfDate（核心认为运行时版本不匹配）");
            break;
        case 0xBAD00005u:
            nrLog(2, "[nr] 提示: 0xBAD00005 = 参数无效（DLSSNR.* 缺失或类型不符）");
            break;
        case 0xBAD00007u:
            nrLog(2, "[nr] 提示: 0xBAD00007 = NotInitialized（核心/模型未初始化）");
            break;
        default:
            break;
        }
        return false;
    };

    if (runtimeApi.create) {
        probeOwnDevice(
            device,
            params,
            reinterpret_cast<CreateFeatureFn>(
                GetProcAddress(runtime, "NVSDK_NGX_D3D12_CreateFeature")),
            dataDir.c_str(),
            width,
            height
        );
    }

    // Ladder, all in one run: TRP works without a core-side Init because it owns
    // the process' NGX session. Here the engine got there first, so the model may
    // need its own Init_Ext before it will create the feature. Report every code.
    if (createWith(coreApi, "core")) {
        device->Release();
        return true;
    }
    if (runtimeApi.create) {
        logFeatureRequirementsFrom(
            reinterpret_cast<void*>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_GetFeatureRequirements")),
            device,
            dataDir.c_str(),
            "runtime"
        );
        if (createWith(runtimeApi, "runtime/未Init")) {
            device->Release();
            return true;
        }
        if (runtimeInitExt) {
            unsigned long long const appId = gEngineAppId ? gEngineAppId : kNrGenericCmsId;
            wchar_t const* const     appDataPath =
                gEngineDataPath.empty() ? dataDir.c_str() : gEngineDataPath.c_str();
            int const initRet =
                runtimeInitExt(appId, appDataPath, device, kNrFeatureApiVersion, nullptr);
            nrLog(
                initRet == 1 || static_cast<unsigned>(initRet) == 0xBAD00003u ? 0 : 1,
                "[nr] runtime Init_Ext(appId=%#llx) ret=0x%08X",
                static_cast<unsigned long long>(appId),
                static_cast<unsigned>(initRet)
            );
            if (createWith(runtimeApi, "runtime/Init_Ext后")) {
                device->Release();
                return true;
            }
        }
    }
    device->Release();
    return false;
}

bool nrIsCreated() { return gNr.created; }

void nrShutdown() {
    // Teardown on this path is what killed Windows (bugcheck 0x6
    // when the game exited with Replace=5 active): releasing the NGX feature (which
    // unloads the 264 MB model inside the driver) and our textures/PSO/heap while
    // the GPU may still be executing command lists that reference them is a
    // use-after-free on the GPU. Objects are deliberately leaked instead - the
    // process is either exiting or the mod stays PINned anyway, and leaking a few
    // hundred MB once is strictly better than taking the machine down.
    gNr.shutdown = true;
    gNr.created  = false; // hooks pass through from here on
    nrLog(0, "[nr] 退出：保留 GPU 对象与 feature（不 Release，避免驱动在退出时崩溃）");
}

void* nrOutputResource() { return gNr.output; }

void* nrEncodedColorResource() { return gEncodedColor; }

bool nrReplaceContinuous() { return gOverrides.replace != 0; }
namespace {

// Route A (docs/D §6.14): make the engine's own upscaler consume the NR result,
// so the picture on screen finally contains NR pixels.
//
// The first attempt () *copied into* the engine's Color and the screen
// went permanently black: Color belongs to the engine's temporal chain, and a
// single near-black frame during world load is never recovered from (all later
// frames read back zero). So nothing engine-owned is written any more. Instead the
// NR result is decoded into our own texture and the engine's *parameter object* is
// pointed at it, right after the engine filled it in and right before it calls
// EvaluateFeature. The engine rewrites Color every frame, so nothing leaks.
//
// NR stays 1:1 at the render resolution (1485x835); the engine's DLSS does the
// real upscaling to 2560x1440.
bool feedBackToColor(ID3D12GraphicsCommandList* cmd, ID3D12Resource* engineColor, void* engineParams,
                     unsigned w, unsigned h) {
    if (!gNr.output || !engineParams || !gDecodedColor) {
        return false;
    }
    auto*                  source      = static_cast<ID3D12Resource*>(gNr.output);
    D3D12_RESOURCE_STATES  sourceState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    int const              replace     = gOverrides.replace;
    bool                   decoded     = false;
    bool                   sourceSampled = false;

    if (replace == 4) {
        // Raw-domain pass-through: the network sees the engine's linear HDR Color
        // and writes linear HDR, so its output can be handed to DLSS unchanged -
        // no encode, no decode, no extra dispatch. The HDR->LDR round trip is what
        // destroys highlights (a 0.99 display value carries 65 linear; the network
        // saturates at ~0.95 and the inverse map turns that into 7.5).
        auto* nrOut = static_cast<ID3D12Resource*>(gNr.output);
        paramSetPtr(engineParams, "Color", nrOut);
        static bool logged4 = false;
        if (!logged4) {
            logged4 = true;
            nrLog(0, "[nr] Replace=4：NR 输出（原始线性域）直接交给引擎 DLSS，%ux%u", w, h);
        }
        return true;
    }

    if (replace == 2) {
        // Swap-mechanism test (docs/D §6.16): give the engine's DLSS a *copy* of
        // its own Color held in a foreign texture - identical pixels, different
        // resource identity. If the picture freezes here, NGX refuses foreign
        // resources and the whole parameter-swap approach is dead.
        if (!engineColor) {
            return false;
        }
        transition(cmd, engineColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, gDecodedColor,
                   gDecodedIsSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
                                 : D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = gDecodedColor;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = engineColor;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_BOX const region{0, 0, 0, w, h, 1};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &region);
        transition(cmd, gDecodedColor, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        transition(cmd, engineColor, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        gDecodedIsSrv = true;
        decoded       = true;
    } else if (gEncodePso) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(source->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            && device) {
            transition(cmd, source, sourceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            sourceSampled = true;
            transition(
                cmd,
                gDecodedColor,
                gDecodedIsSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
            writeDescriptors(device, source, source, source, gDecodedColor);
            device->Release();

            ID3D12DescriptorHeap* heaps[1] = {gEncodeHeap};
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(gEncodeRoot);
            cmd->SetPipelineState(gEncodePso);
            cmd->SetComputeRootDescriptorTable(0, gEncodeHeap->GetGPUDescriptorHandleForHeapStart());
            setPassConstants(cmd, w, h, 1 /* decode */, 1.0f, 1.0f, 1.0f, 2.0f);
            cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
            D3D12_RESOURCE_BARRIER uavBarrier{};
            uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.UAV.pResource = gDecodedColor;
            cmd->ResourceBarrier(1, &uavBarrier);
            transition(
                cmd,
                gDecodedColor,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
            );
            gDecodedIsSrv = true;
            source        = gDecodedColor;
            sourceState   = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            decoded       = true;
        }
    }

    if (!decoded) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            nrLog(1, "[nr] Replace 但编解码 shader 不可用，跳过替换（画面保持引擎原样）");
        }
        return false;
    }

    if (sourceSampled) {
        transition(
            cmd,
            static_cast<ID3D12Resource*>(gNr.output),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
        );
    }
    if (replace == 3) {
        // Borrow the engine's own Color for exactly one frame: DLSS then reads the
        // texture it was created with (no foreign resource, nothing for NGX to
        // reject), and nrRestoreColor() hands the engine its pixels back right
        // after its evaluate.
        if (!engineColor || !gColorBackup) {
            return false;
        }
        transition(cmd, engineColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, gColorBackup, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION bdst{};
        bdst.pResource        = gColorBackup;
        bdst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bdst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION bsrc{};
        bsrc.pResource        = engineColor;
        bsrc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bsrc.SubresourceIndex = 0;
        D3D12_BOX const region{0, 0, 0, w, h, 1};
        cmd->CopyTextureRegion(&bdst, 0, 0, 0, &bsrc, &region);
        transition(cmd, gColorBackup, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

        transition(cmd, gDecodedColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, engineColor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION wdst{};
        wdst.pResource        = engineColor;
        wdst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        wdst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION wsrc{};
        wsrc.pResource        = gDecodedColor;
        wsrc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        wsrc.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&wdst, 0, 0, 0, &wsrc, &region);
        transition(cmd, gDecodedColor, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        transition(cmd, engineColor, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        gColorBorrowed      = engineColor;
        gColorBackupWidth   = w;
        gColorBackupHeight  = h;
        gColorBackupPending = true;
        static bool borrowLogged = false;
        if (!borrowLogged) {
            borrowLogged = true;
            nrLog(0, "[nr] Replace=3：借出引擎 Color 的 %ux%u 区域，DLSS 读完后立即归还", w, h);
        }
        return true;
    }

    // Point the engine's own parameter object at our texture. It is filled in by
    // the engine right before EvaluateFeature and rewritten next frame, so the
    // swap lives exactly one frame and engine resources are never touched.
    paramSetPtr(engineParams, "Color", gDecodedColor);
    static bool logged = false;
    if (!logged) {
        logged = true;
        void* readBack = paramGetResource(engineParams, "Color");
        nrLog(
            readBack == static_cast<void*>(gDecodedColor) ? 0 : 1,
            "[nr] 已把引擎 DLSS 的 Color 指向 NR 结果 %p（读回 %p）%ux%u",
            static_cast<void*>(gDecodedColor),
            readBack,
            w,
            h
        );
    }
    return true;
}

// Route A / Replace=5 (docs/D §6.16): the network's *relative* change, applied to
// the engine's original linear HDR Colour. Feeding the network's output straight
// to DLSS flattens HDR (its output saturates near 1.0, a 0.99 display value is 65
// linear), which is what made the picture grey and soft. Here:
//     gain = NR_out / NR_in        (both display-referred, clamped)
//     out  = original_linear * gain
// Where the network agrees with its input the gain is 1, so highlights survive and
// the frame is otherwise untouched; where it changed something, the change carries
// over into linear space. Uses the *previous* frame's NR result so this pass runs
// before our own evaluate - the same window as the encode pass.
void compositeGain(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                   D3D12_RESOURCE_STATES colorState, void* engineParams, unsigned w, unsigned h) {
    ID3D12Device* device = nullptr;
    if (FAILED(color->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
        || !device) {
        return;
    }
    // Fade the effect in over the first frames of the process. Switching the
    // upscaler's input in one step leaves the previous frame burned into DLSS's
    // history ("第一帧轮廓残留", ); starting at gain = 1 lets the history
    // converge instead of being overwritten.
    static unsigned composed = 0;
    float const     ramp     = composed >= 24u ? 1.0f : static_cast<float>(composed) / 24.0f;
    if (composed < 24u) {
        ++composed;
    }
    // Replace=6 uses TRP's "Ratio" resolve: rebase the network's result onto the
    // original luminance instead of multiplying by a raw gain. Replace=5 keeps the
    // earlier bounded-gain experiment for comparison.
    unsigned const mode = gOverrides.replace == 6 ? 4u : 2u;
    float const    strength =
        mode == 4u ? gOverrides.transferStrength : (gOverrides.gainBoost > 0.0f ? gOverrides.gainBoost : 1.0f);
    float const    strengthRamped = strength * ramp;
    setPassConstants(
        cmd,
        w,
        h,
        mode,
        gOverrides.whitePoint,
        strengthRamped,
        gOverrides.colourStrength,
        gOverrides.maxRatio
    );
    D3D12_RESOURCE_STATES const nrState =
        gNr.outputWasSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
    auto*       nrTex = static_cast<ID3D12Resource*>(gNr.output);

    transition(cmd, color, colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, nrTex, nrState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(
        cmd,
        gDecodedColor,
        gDecodedIsSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    writeDescriptors(device, color, gEncodedColor, nrTex, gDecodedColor);
    device->Release();

    ID3D12DescriptorHeap* heaps[1] = {gEncodeHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(gEncodeRoot);
    cmd->SetPipelineState(gEncodePso);
    cmd->SetComputeRootDescriptorTable(0, gEncodeHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = gDecodedColor;
    cmd->ResourceBarrier(1, &uavBarrier);
    transition(
        cmd,
        gDecodedColor,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
    );
    gDecodedIsSrv = true;
    transition(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colorState);
    transition(cmd, nrTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nrState);

    // Same one-frame trick as the other Replace modes: the engine re-fills its
    // parameter object before every EvaluateFeature. Replace=7 keeps the engine's
    // DLSS untouched: our own SR consumes the composite instead.
    if (gOverrides.replace != 7) {
        paramSetPtr(engineParams, "Color", gDecodedColor);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        nrLog(
            0,
            "[nr] Replace=%d 生效：%s（用上一帧 NR 结果，strength=%.2f white=%.2f colour=%.2f maxRatio=%.2f）",
            gOverrides.replace,
            mode == 4u ? "TRP Ratio 合成" : "增益合成",
            static_cast<double>(strengthRamped),
            static_cast<double>(gOverrides.whitePoint),
            static_cast<double>(gOverrides.colourStrength),
            static_cast<double>(gOverrides.maxRatio)
        );
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Our own DLSS SR (Replace=7, docs/D §6.16). TRP's structure: the upscaler is a
// feature *we* create, fed our composited image plus the engine's Depth/MV, and
// writing into a texture we own. Nothing of the engine's DLSS is re-pointed, so
// the "NGX keeps reading the resource it was created with" freeze cannot happen.
// Step 1 only dumps the result - the engine's own DLSS still draws the picture.
// ---------------------------------------------------------------------------
namespace {

struct SrState {
    void*             params   = nullptr;
    void*             feature  = nullptr;
    CreateFeatureFn   create   = nullptr;
    EvaluateFeatureFn evaluate = nullptr;
    ID3D12Resource*   output   = nullptr;
    bool              attempted = false;
    bool              created   = false;
    bool              loggedShare = false;
    bool              lastOk      = false;
    bool              ownsOutput  = false;
    unsigned          warmupFrames = 0;
    void*             engineOutput = nullptr; // the engine's real present texture
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
};

SrState gSr;

// The *original* NGX exports, handed over by the probe's hook installer. Our own
// SR must never call GetProcAddress again: that address is the hooked entry
// point, so the call would come straight back into our detour
// (ngxEvaluateDetour -> nrRecord -> srStep -> ngxEvaluateDetour, ,
// stack overflow 0xC00000FD).
void* gOriginalCreateFeature   = nullptr;
void* gOriginalEvaluateFeature = nullptr;

// Pending "put the engine's Output parameter back" for control mode, done by
// nrRestoreColor() right after the engine's EvaluateFeature.
void* gControlParams = nullptr;

// Set when a level (re)loads: our SR feature is process-wide and keeps its
// temporal history, so without an explicit reset the previous world's content
// stays imprinted and follows the player into the next world.
std::atomic_bool gSrResetPending{false};

// ---------------------------------------------------------------------------
// Visible marker: a solid red square in the top-left corner of *our* texture.
// It must never touch the engine's Color (writing that is what caused the
// permanent black screen earlier); stamping our own SR input instead makes the
// marker travel through our DLSS SR to the screen, which proves the chain
// "our composite -> our SR -> presented" end to end.
// Copied from a 64x64 upload *buffer*, so no shader and no offline validation.
// (Upload heaps only accept buffers, not textures - that is why the first
// attempt failed with "标记纹理分配失败".)
ID3D12Resource* gStampBuffer = nullptr;
constexpr unsigned kStampSize = 64;
constexpr unsigned kStampRowPitch = kStampSize * 8; // RGBA16F

std::uint16_t packHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::uint32_t const sign     = (bits >> 16) & 0x8000u;
    int const           exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    std::uint32_t const mantissa = (bits >> 13) & 0x3FFu;
    if (exponent <= 0) {
        return static_cast<std::uint16_t>(sign);
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | mantissa);
}

bool ensureStampBuffer(ID3D12Device* device) {
    if (gStampBuffer) {
        return true;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = static_cast<std::uint64_t>(kStampRowPitch) * kStampSize;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc         = {1, 0};
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&gStampBuffer)))) {
        gStampBuffer = nullptr;
        nrLog(2, "[nr] 标记 buffer 分配失败");
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(gStampBuffer->Map(0, nullptr, &mapped)) || !mapped) {
        return false;
    }
    // Red, opaque: R=1 G=0 B=0 A=1, RGBA16F rows of 512 bytes.
    auto* texels = static_cast<std::uint16_t*>(mapped);
    for (unsigned y = 0; y < kStampSize; ++y) {
        for (unsigned x = 0; x < kStampSize; ++x) {
            std::uint16_t* t =
                texels + (static_cast<std::size_t>(y) * kStampRowPitch) / 2 + x * 4;
            t[0] = packHalf(1.0f);
            t[1] = packHalf(0.0f);
            t[2] = packHalf(0.0f);
            t[3] = packHalf(1.0f);
        }
    }
    gStampBuffer->Unmap(0, nullptr);
    nrLog(0, "[nr] 标记 buffer 就绪（64x64 红块）");
    return true;
}

void stampMarker(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target) {
    if (!cmd || !target) {
        return;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(target->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return;
    }
    bool const ready = ensureStampBuffer(device);
    device->Release();
    if (!ready) {
        return;
    }
    transition(cmd, target, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = target;
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource                        = gStampBuffer;
    src.Type                             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset           = 0;
    src.PlacedFootprint.Footprint.Format = target->GetDesc().Format;
    src.PlacedFootprint.Footprint.Width  = kStampSize;
    src.PlacedFootprint.Footprint.Height = kStampSize;
    src.PlacedFootprint.Footprint.Depth  = 1;
    src.PlacedFootprint.Footprint.RowPitch = kStampRowPitch;
    D3D12_BOX const box{0, 0, 0, kStampSize, kStampSize, 1};
    cmd->CopyTextureRegion(&dst, 8, 8, 0, &src, &box);
    transition(cmd, target, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

bool srStep(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* depth,
            void* motionVectors, void* engineParams, void* engineOutput, unsigned w, unsigned h) {
    if (!cmd || !color || !depth || !motionVectors || !engineParams) {
        return false;
    }
    if (!gSr.created) {
        if (gSr.attempted) {
            return false;
        }
        gSr.attempted = true;
        HMODULE core  = GetModuleHandleW(L"_nvngx.dll");
        if (!core) {
            nrLog(2, "[sr] 找不到 _nvngx.dll");
            return false;
        }
        auto capabilities = reinterpret_cast<GetCapabilitiesFn>(
            GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        auto create     = reinterpret_cast<CreateFeatureFn>(gOriginalCreateFeature);
        auto evaluate   = reinterpret_cast<EvaluateFeatureFn>(gOriginalEvaluateFeature);
        if (!capabilities || !create || !evaluate) {
            nrLog(2, "[sr] 缺 core 导出（caps=%d create=%d eval=%d）", capabilities ? 1 : 0,
                  create ? 1 : 0, evaluate ? 1 : 0);
            return false;
        }

        ID3D12Device* device = nullptr;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            || !device) {
            nrLog(2, "[sr] 取不到 ID3D12Device");
            return false;
        }
        D3D12_RESOURCE_DESC desc = color->GetDesc(); // 2560x1440 RGBA16F, like the engine's
        desc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.MipLevels           = 1;
        desc.DepthOrArraySize    = 1;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT const created = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&gSr.output)
        );
        device->Release();
        if (FAILED(created)) {
            nrLog(2, "[sr] 输出纹理创建失败 0x%08X", static_cast<unsigned>(created));
            return false;
        }

        void* params   = nullptr;
        int   capsRet = capabilities(&params);
        if (capsRet != 1 || !params) {
            nrLog(2, "[sr] GetCapabilityParameters ret=0x%08X params=%p",
                  static_cast<unsigned>(capsRet), params);
            return false;
        }
        gSr.params = params;
        paramSetU32(params, "CreationNodeMask", 1);
        paramSetU32(params, "VisibilityNodeMask", 1);
        paramSetU32(params, "PerfQualityValue", 1);
        paramSetU32(params, "Width", w);
        paramSetU32(params, "Height", h);
        paramSetU32(params, "OutWidth", static_cast<unsigned>(desc.Width));
        paramSetU32(params, "OutHeight", desc.Height);
        paramSetF32(params, "Sharpness", gOverrides.srSharpness);
        // Mirror the engine's own creation-time contract. These keys are what the
        // engine sets on its parameter object (logged from its own setup), and the
        // feature reads several of them at CreateFeature: without
        // DLSS.Feature.Create.Flags the subrect-aware input path is off, which is
        // exactly the streaky garbage seen when the SR wrote the screen.
        for (char const* name : {
                 "DLSS.Feature.Create.Flags",
                 "DLSS.Render.Subrect.Dimensions.Width",
                 "DLSS.Render.Subrect.Dimensions.Height",
                 "DLSS.Enable.Output.Subrects",
                 "DLSS.Input.Color.Subrect.Base.X",
                 "DLSS.Input.Color.Subrect.Base.Y",
                 "DLSS.Input.Depth.Subrect.Base.X",
                 "DLSS.Input.Depth.Subrect.Base.Y",
                 "DLSS.Input.MV.Subrect.Base.X",
                 "DLSS.Input.MV.Subrect.Base.Y",
                 "DLSS.Output.Subrect.Base.X",
                 "DLSS.Output.Subrect.Base.Y",
                 "AnimatedTextureMask",
                 "IsParticleMask",
                 "TransparencyMask",
                 "TonemapperType",
                 "RTXValue",
                 "Position.ViewSpace",
                 "FrameTimeDeltaInMsec",
             }) {
            unsigned const value = paramGetU32(engineParams, name);
            if (value != 0) {
                paramSetU32(params, name, value);
            }
        }
        // The two that carry real values even when zero is meaningful.
        paramSetU32(params, "DLSS.Render.Subrect.Dimensions.Width", w);
        paramSetU32(params, "DLSS.Render.Subrect.Dimensions.Height", h);
        paramSetU32(params, "DLSS.Feature.Create.Flags", paramGetU32(engineParams, "DLSS.Feature.Create.Flags"));
        paramSetU32(params, "MV.Scale.X", paramGetU32(engineParams, "MV.Scale.X"));
        paramSetU32(params, "MV.Scale.Y", paramGetU32(engineParams, "MV.Scale.Y"));
        paramSetF32(params, "DLSS.Pre.Exposure", 1.0f);
        paramSetF32(params, "DLSS.Exposure.Scale", 1.0f);
        // Preset hints: the engine sets one value per quality tier. All tiers get
        // the same value so DLSSPreset in nr_params.txt is what actually runs.
        for (char const* name : {
                 "DLSS.Hint.Render.Preset.DLAA",
                 "DLSS.Hint.Render.Preset.Quality",
                 "DLSS.Hint.Render.Preset.Balanced",
                 "DLSS.Hint.Render.Preset.Performance",
                 "DLSS.Hint.Render.Preset.UltraQuality",
                 "DLSS.Hint.Render.Preset.UltraPerformance",
             }) {
            paramSetU32(params, name, static_cast<unsigned>(gOverrides.srPreset));
        }

        void* handle = nullptr;
        nrLog(0, "[sr] 自建 SR CreateFeature(1) 开始…");
        int const ret = create(cmd, 1, params, &handle);
        nrLog(
            ret == 1 && handle ? 0 : 2,
            "[sr] CreateFeature(1) ret=0x%08X handle=%p %ux%u -> %llux%llu preset=%#x sharpness=%.2f",
            static_cast<unsigned>(ret),
            handle,
            w,
            h,
            static_cast<unsigned long long>(desc.Width),
            static_cast<unsigned long long>(desc.Height),
            static_cast<unsigned>(gOverrides.srPreset),
            static_cast<double>(gOverrides.srSharpness)
        );
        if (ret != 1 || !handle) {
            return false;
        }
        gSr.feature     = handle;
        gSr.create      = create;
        gSr.evaluate    = evaluate;
        gSr.created     = true;
        gSr.outputState = D3D12_RESOURCE_STATE_COMMON;
    }

    // NGX's capability parameter object is process-wide: the engine writes its
    // DLSS knobs into the same map we are about to edit. Swap only Color/Output,
    // then put the engine's values back before its own EvaluateFeature runs -
    // otherwise the engine's DLSS writes into *our* output texture and the screen
    // freezes (observed screen frozen with Replace=7 in shadow mode).
    // Warm-up: let our SR build its history in a private texture for the first
    // ~60 frames, then take over the present texture. Writing a fresh feature's
    // first frames straight onto the screen is what made the first two seconds in
    // the world look broken.
    constexpr unsigned kWarmup = 60;
    bool const takeover = (gOverrides.replace == 8 || gOverrides.replace == 9);
    bool const warmedUp = gSr.warmupFrames >= kWarmup;
    ID3D12Resource* output = gSr.output;
    if (takeover && warmedUp) {
        // The engine double-buffers its Output: the parameter names a different
        // texture every frame. Always follow it - caching one texture here made
        // every other frame show a never-written buffer (black screen).
        output = static_cast<ID3D12Resource*>(engineOutput);
        if (!output) {
            output = static_cast<ID3D12Resource*>(paramGetResource(engineParams, "Output"));
        }
        if (!output || output == gSr.output) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                nrLog(2, "[sr] Replace=8 取不到引擎 Output，跳过");
            }
            return false;
        }
    }

    void* savedColor  = paramGetResource(engineParams, "Color");
    void* savedOutput = paramGetResource(engineParams, "Output");
    bool const sharedParams = (gSr.params == engineParams);
    if (!gSr.loggedShare) {
        gSr.loggedShare = true;
        nrLog(0, "[sr] 参数对象 same=%d（1 = NGX 单例，需要保存/恢复）", sharedParams ? 1 : 0);
    }

    paramSetPtr(engineParams, "Color", color);
    paramSetPtr(engineParams, "Output", output);
    paramSetPtr(engineParams, "Depth", depth);
    paramSetPtr(engineParams, "MotionVectors", motionVectors);

    transition(cmd, color, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, static_cast<ID3D12Resource*>(motionVectors),
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bool const writeToEngineOutput = (output != gSr.output);
    if (!writeToEngineOutput) {
        transition(cmd, output, gSr.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // The engine's own Output texture is NOT transitioned here: NGX manages that
    // resource's states itself, and bouncing it (ALL -> UAV -> ALL) desynchronises
    // the engine feature's tracking of it. Once we had done that, the engine's own
    // DLSS produced garbage until our SR overwrote the texture again.

    bool resetIssued = false;
    if (gSrResetPending.exchange(false, std::memory_order_relaxed)) {
        // The engine's own contract dumps a bare "Reset" key (no DLSS. prefix).
        paramSetI32(engineParams, "Reset", 1);
        resetIssued = true;
        nrLog(0, "[sr] 进存档：重置我们 SR 的时序历史");
    }
    // Diagnostic: SRReset=1 wipes our DLSS SR history every frame. If the burned-in
    // outline disappears here but not with NRReset, the imprint lives in the SR's
    // temporal state, not in NR's.
    if (gOverrides.srResetEveryFrame != 0) {
        paramSetI32(engineParams, "DLSS.Reset", 1);
    }
    int const ret = gSr.evaluate(cmd, gSr.feature, engineParams, nullptr, nullptr);
    // Hand the engine its own parameters back before its EvaluateFeature. The
    // reset flag is one-shot: leaving Reset=1 behind would keep resetting the
    // history of whichever feature reads this object next.
    if (resetIssued) {
        paramSetI32(engineParams, "Reset", 0);
    }
    paramSetPtr(engineParams, "Color", savedColor);
    paramSetPtr(engineParams, "Output", savedOutput);

    if (!writeToEngineOutput) {
        transition(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        gSr.outputState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    transition(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    transition(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    transition(cmd, static_cast<ID3D12Resource*>(motionVectors),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    static unsigned calls = 0;
    if (ret != 1 && ++calls < 8) {
        nrLog(2, "[sr] EvaluateFeature ret=0x%08X (#%u)", static_cast<unsigned>(ret), calls);
    }
    gSr.lastOk = (ret == 1);
    if (ret == 1 && gSr.warmupFrames < 1000000u) {
        ++gSr.warmupFrames;
    }
    bool const ownsNow = (ret == 1 && gSr.warmupFrames >= 60 && takeover);
    if (ownsNow && !gSr.ownsOutput) {
        nrLog(0, "[sr] 预热完成（%u 帧），开始接管上屏", gSr.warmupFrames);
    }
    gSr.ownsOutput = ownsNow;
    return ret == 1;
}

} // namespace

void* nrSrOutputResource() { return gSr.output; }

bool nrSrIsCreated() { return gSr.created; }

int nrReplaceMode() { return gOverrides.replace; }

bool nrParamLogEnabled() { return gOverrides.paramLog != 0; }

bool nrSrOwnsOutput() {
    // In control mode the engine's own DLSS still has to run (it writes our
    // private texture), so the caller must not skip it. Before the warm-up ends
    // the engine's evaluate must also keep running: we are only writing our
    // private texture at that point.
    return gSr.ownsOutput && gOverrides.control == 0;
}

void nrNotifyLevelLoaded() { gSrResetPending.store(true, std::memory_order_relaxed); }

void nrSetNgxOriginals(void* createFeature, void* evaluateFeature) {
    gOriginalCreateFeature   = createFeature;
    gOriginalEvaluateFeature = evaluateFeature;
}

// Route A, second half: give the engine its own pixels back. Same command
// list, right after the engine's EvaluateFeature returned. No-op otherwise.
void nrRestoreColor(void* commandList) {
    // Control mode leaves the engine's Output parameter pointing at our private
    // texture (that is how its own DLSS gets redirected); put the real present
    // texture back so the next frame starts from the engine's own state.
    if (gControlParams && gSr.engineOutput) {
        paramSetPtr(gControlParams, "Output", gSr.engineOutput);
        gControlParams = nullptr;
    }
    if (!gColorBackupPending || !gColorBackup || !gColorBorrowed || !commandList) {
        return;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    unsigned const w = gColorBackupWidth;
    unsigned const h = gColorBackupHeight;

    transition(cmd, gColorBorrowed, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(cmd, gColorBackup, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = gColorBorrowed;
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = gColorBackup;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_BOX const region{0, 0, 0, w, h, 1};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &region);
    transition(cmd, gColorBackup, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(cmd, gColorBorrowed, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    gColorBackupPending = false;
    gColorBorrowed      = nullptr;
}

// Route A entry point for the probe: decode the last NR result into our own
// texture and point the engine's parameter object at it (docs/D §6.16). Must be
// called after nrRecord() and before the engine's own EvaluateFeature.
bool nrReplaceEngineColor(void* commandList, void* engineColor, void* engineParams) {
    // Only the "hijack the engine's parameter object" modes belong here: 1 (decode),
    // 2 (copy test), 3 (borrow/restore), 4 (raw). 5 and 6 are composited inside
    // nrRecord; 7 is shadow mode and must never touch the engine's picture.
    // Missing the 7 here is what kept the screen frozen in shadow mode on
    //  - the old swap path was still running.
    int const mode = gOverrides.replace;
    if (!gNr.created || !commandList || !engineParams || mode < 1 || mode > 4) {
        return false;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    return feedBackToColor(
        cmd,
        static_cast<ID3D12Resource*>(engineColor),
        engineParams,
        gNr.width,
        gNr.height
    );
}

bool nrRecord(void* commandList, void* color, void* depth, void* motionVectors, unsigned guideWidth,
              unsigned guideHeight, float motionVectorScaleX, float motionVectorScaleY,
              int depthInverted, bool reset, void* engineParams, void* engineOutput) {
    if (!gNr.created || !commandList || !color || !depth || !motionVectors) {
        return false;
    }
    auto* cmd         = static_cast<ID3D12GraphicsCommandList*>(commandList);
    void* params      = gNr.params;
    auto* output      = gNr.output;
    unsigned const w  = gNr.width;
    unsigned const h  = gNr.height;

    reloadOverridesIfDue();
    bool const useEncoded = gOverrides.replace != 4 && gEncodePso && gEncodedColor;
    paramSetPtr(params, "DLSSNR.Color", useEncoded ? static_cast<void*>(gEncodedColor) : color);
    paramSetPtr(params, "DLSSNR.Depth", depth);
    paramSetPtr(params, "DLSSNR.MVec", motionVectors);
    paramSetPtr(params, "DLSSNR.Output", output);
    // This runtime aliases Backbuffer to the output resource (TRP does the same
    // for the reconstruction contract): it reads existing pixels for UI
    // correction, so it must be a display-sized texture.
    paramSetPtr(params, "DLSSNR.Backbuffer", output);

    paramSetU32(params, "DLSSNR.ColorSubrectBaseX", 0);
    paramSetU32(params, "DLSSNR.ColorSubrectBaseY", 0);
    paramSetU32(params, "DLSSNR.ColorSubrectWidth", w);
    paramSetU32(params, "DLSSNR.ColorSubrectHeight", h);
    paramSetU32(params, "DLSSNR.OutputSubrectBaseX", 0);
    paramSetU32(params, "DLSSNR.OutputSubrectBaseY", 0);
    paramSetU32(params, "DLSSNR.OutputSubrectWidth", w);
    paramSetU32(params, "DLSSNR.OutputSubrectHeight", h);
    paramSetU32(params, "DLSSNR.BackbufferSubrectBaseX", 0);
    paramSetU32(params, "DLSSNR.BackbufferSubrectBaseY", 0);
    paramSetU32(params, "DLSSNR.BackbufferSubrectWidth", w);
    paramSetU32(params, "DLSSNR.BackbufferSubrectHeight", h);
    paramSetU32(params, "DLSSNR.DepthSubrectBaseX", 0);
    paramSetU32(params, "DLSSNR.DepthSubrectBaseY", 0);
    paramSetU32(params, "DLSSNR.DepthSubrectWidth", guideWidth);
    paramSetU32(params, "DLSSNR.DepthSubrectHeight", guideHeight);
    paramSetU32(params, "DLSSNR.MVecSubrectBaseX", 0);
    paramSetU32(params, "DLSSNR.MVecSubrectBaseY", 0);
    paramSetU32(params, "DLSSNR.MVecSubrectWidth", guideWidth);
    paramSetU32(params, "DLSSNR.MVecSubrectHeight", guideHeight);
    // Optional inputs we do not own: an empty subrect keeps them out of the pass.
    for (char const* name : {"DLSSNR.UI", "DLSSNR.UIAlpha", "DLSSNR.ControlMask"}) {
        paramSetPtr(params, name, nullptr);
    }
    for (char const* prefix : {"UI", "UIAlpha", "ControlMask"}) {
        char name[64];
        std::snprintf(name, sizeof(name), "DLSSNR.%sSubrectBaseX", prefix);
        paramSetU32(params, name, 0);
        std::snprintf(name, sizeof(name), "DLSSNR.%sSubrectBaseY", prefix);
        paramSetU32(params, name, 0);
        std::snprintf(name, sizeof(name), "DLSSNR.%sSubrectWidth", prefix);
        paramSetU32(params, name, 0);
        std::snprintf(name, sizeof(name), "DLSSNR.%sSubrectHeight", prefix);
        paramSetU32(params, name, 0);
    }

    // Motion-vector scale: 1.0 for pixel-space vectors, the render size for
    // normalised ones. Getting this wrong leaves the temporal history fixed to the
    // screen (ghost clouds, ), because nothing is reprojected.
    float const mvScaleX = gOverrides.mvecScale >= 0.0f ? gOverrides.mvecScale : 1.0f;
    float const mvScaleY = gOverrides.mvecScale >= 0.0f ? gOverrides.mvecScale : 1.0f;
    paramSetF32(params, "DLSSNR.MVecScaleX", mvScaleX);
    paramSetF32(params, "DLSSNR.MVecScaleY", mvScaleY);
    // The runtime's shared keys are unprefixed (measured from the engine's own
    // setup: MV.Scale.X=1485 / MV.Scale.Y=835, Width/Height/OutWidth/OutHeight).
    // Without them the feature sees a zero motion-vector scale, i.e. "the camera
    // never moves": history sticks to the screen (ghost clouds/silhouettes).
    unsigned const mvX = static_cast<unsigned>(gOverrides.mvecScale >= 0.0f
                                                   ? gOverrides.mvecScale
                                                   : static_cast<float>(guideWidth));
    unsigned const mvY = static_cast<unsigned>(gOverrides.mvecScale >= 0.0f
                                                   ? gOverrides.mvecScale
                                                   : static_cast<float>(guideHeight));
    paramSetU32(params, "MV.Scale.X", mvX);
    paramSetU32(params, "MV.Scale.Y", mvY);
    // Deliberately NOT touching Width/Height/OutWidth/OutHeight here: if this
    // parameter object is the one the engine's own SR also reads, writing our 1:1
    // guide size into OutWidth/OutHeight makes the engine's upscaler write a
    // 1485x835 patch into a 2560x1440 target - a permanently stale-looking picture.
    paramSetU32(params, "Width", guideWidth);
    paramSetU32(params, "Height", guideHeight);
    int style = gOverrides.style >= 0 ? gOverrides.style : 0;
    if (gOverrides.sweep == 1) {
        style = (gWindowIndex >= 0 ? gWindowIndex : 0) % 3;
    }
    paramSetU32(params, "DLSSNR.Style", static_cast<std::uint32_t>(style));

    // Freeze test: with Enabled=0 the feature must not touch the output, so the
    // dump keeps the previous window's content. No stateless "filter" can do that.
    int enabled = gOverrides.enabled >= 0 ? gOverrides.enabled : 1;
    if (gOverrides.sweep == 2) {
        enabled = ((gWindowIndex >= 0 ? gWindowIndex : 0) % 2 == 0) ? 1 : 0;
    }
    paramSetI32(params, "DLSSNR.Enabled", enabled);
    paramSetI32(params, "DLSSNR.UseAutoMask", 0);
    paramSetI32(params, "DLSSNR.UICorrection", 0);
    int const inverted = gOverrides.depthInverted >= 0 ? gOverrides.depthInverted : (depthInverted ? 1 : 0);
    paramSetI32(params, "DLSSNR.DepthInverted", inverted ? 1 : 0);
    paramSetI32(
        params,
        "DLSSNR.Reset",
        (reset || (gOverrides.resetEveryFrame != 0)) ? 1 : 0
    );
    // 310.x reads both of these as int as well; 0 = keep the axis unflipped.
    paramSetI32(params, "DLSS.Indicator.Invert.X.Axis", 0);
    paramSetI32(params, "DLSS.Indicator.Invert.Y.Axis", 0);
    paramSetF32(params, "DLSSNR.Intensity", gOverrides.intensity >= 0.0f ? gOverrides.intensity : 1.0f);
    paramSetF32(
        params,
        "DLSSNR.LocalToneStrength",
        gOverrides.localTone >= 0.0f ? gOverrides.localTone : 1.0f
    );
    paramSetF32(
        params,
        "DLSSNR.LocalStructureStrength",
        gOverrides.localStructure >= 0.0f ? gOverrides.localStructure : 1.0f
    );
    paramSetF32(params, "DLSSNR.SkinStructureStrength", 1.0f);

    static int loggedWindow = -2;
    if (loggedWindow != gWindowIndex) {
        loggedWindow = gWindowIndex;
        nrLog(
            0,
            "[nr] 本窗参数 Style=%d Enabled=%d Intensity=%.2f DepthInverted=%d Replace=%d（窗口 #%d）",
            style,
            enabled,
            static_cast<double>(gOverrides.intensity >= 0.0f ? gOverrides.intensity : 1.0f),
            inverted,
            gOverrides.replace,
            gWindowIndex
        );
    }

    if (!gNr.resourceRoundtripChecked) {
        gNr.resourceRoundtripChecked = true;
        void* const expected = useEncoded ? static_cast<void*>(gEncodedColor) : color;
        void* const readBack = paramGetResource(params, "DLSSNR.Color");
        nrLog(
            readBack == expected ? 0 : 1,
            "[nr] 资源往返校验 DLSSNR.Color=%p 写入=%p %s",
            readBack,
            expected,
            readBack == expected ? "ok" : "MISMATCH"
        );
    }

    D3D12_RESOURCE_STATES const outputBefore =
        gNr.outputWasSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    if (useEncoded) {
        // HDR → LDR encode into our own texture; the feature reads that instead.
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(static_cast<ID3D12Resource*>(color)->GetDevice(
                __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            && device) {
            transition(cmd, static_cast<ID3D12Resource*>(color),
                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cmd, gEncodedColor,
                       gEncodedIsSrv ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                     : D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            writeDescriptors(
                device,
                static_cast<ID3D12Resource*>(color),
                static_cast<ID3D12Resource*>(color),
                static_cast<ID3D12Resource*>(color),
                gEncodedColor
            );
            device->Release();

            ID3D12DescriptorHeap* heaps[1] = {gEncodeHeap};
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(gEncodeRoot);
            cmd->SetPipelineState(gEncodePso);
            cmd->SetComputeRootDescriptorTable(
                0, gEncodeHeap->GetGPUDescriptorHandleForHeapStart());
            // Replace=6 feeds the network TRP's encoded proxy (sRGB + shoulder);
            // the other modes keep the earlier Reinhard encode for A/B.
            setPassConstants(
                cmd,
                w,
                h,
                gOverrides.replace == 6 ? 3u : 0u,
                gOverrides.whitePoint,
                1.0f,
                1.0f,
                gOverrides.maxRatio
            );
            cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
            D3D12_RESOURCE_BARRIER uavBarrier{};
            uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.UAV.pResource = gEncodedColor;
            cmd->ResourceBarrier(1, &uavBarrier);
            transition(cmd, gEncodedColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gEncodedIsSrv = true;
            transition(cmd, static_cast<ID3D12Resource*>(color),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
    } else {
        transition(cmd, static_cast<ID3D12Resource*>(color),
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // The composite and our SR run *after* our NR evaluate (see below): taking
    // last frame's NR result made our SR input one frame stale, which the SR then
    // re-projected with this frame's motion vectors -> a screen-space-fixed ghost.

    transition(cmd, static_cast<ID3D12Resource*>(depth),
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, static_cast<ID3D12Resource*>(motionVectors),
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, output, outputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    int const evaluateRet = gNr.evaluate(commandList, gNr.feature, params, nullptr, nullptr);
    if (evaluateRet == 1) {
        ++gNr.evalCount;
    }

    transition(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (!useEncoded) {
        transition(cmd, static_cast<ID3D12Resource*>(color),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    transition(cmd, static_cast<ID3D12Resource*>(depth),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    transition(cmd, static_cast<ID3D12Resource*>(motionVectors),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    gNr.outputWasSrv = true;

    // Same frame now: composite this frame's NR result onto this frame's Color,
    // then let our own DLSS SR reconstruct it. Replace=7 only dumps that result;
    // Replace=8 writes it into the engine's present texture.
    if (engineParams && (gOverrides.replace == 5 || gOverrides.replace == 6
                         || gOverrides.replace == 7 || gOverrides.replace == 8)
        && gEncodePso && gEncodedColor && gDecodedColor && gNr.evalCount > 2) {
        compositeGain(
            cmd,
            static_cast<ID3D12Resource*>(color),
            useEncoded ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                       : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            engineParams,
            w,
            h
        );
    }
    // Replace=9 (bisect): our SR upscales the engine's own Color, with NR and the
    // composite out of the picture entirely. Clean result => NR is what corrupts
    // the picture; still dirty => our SR-to-screen path is.
    if (gOverrides.replace == 9) {
        srStep(
            cmd,
            static_cast<ID3D12Resource*>(color),
            static_cast<ID3D12Resource*>(depth),
            motionVectors,
            engineParams,
            engineOutput,
            w,
            h
        );
    }
    if ((gOverrides.replace == 7 || gOverrides.replace == 8) && gDecodedColor) {
        if (gOverrides.stamp != 0) {
            stampMarker(cmd, gDecodedColor);
        }
        srStep(
            cmd,
            gDecodedColor,
            static_cast<ID3D12Resource*>(depth),
            motionVectors,
            engineParams,
            engineOutput,
            w,
            h
        );
    }

    static unsigned calls = 0;
    ++calls;
    if (evaluateRet != 1 && calls < 8) {
        nrLog(2, "[nr] EvaluateFeature ret=0x%08X (#%u)", static_cast<unsigned>(evaluateRet), calls);
    }
    return evaluateRet == 1;
}

void nrStampBegin(void* commandList, std::uint64_t frame, int id) {
    if (!gStamps.active || !commandList || id < 0 || id >= kStampKinds) {
        return;
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    int const slot  = static_cast<int>(frame % kStampSlots);
    int const index = slot * kStampPerSlot + id * 2;
    cmd->EndQuery(gStamps.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index));
}

void nrStampEnd(void* commandList, std::uint64_t frame, int id) {
    if (!gStamps.active || !commandList || id < 0 || id >= kStampKinds) {
        return;
    }
    auto* cmd       = static_cast<ID3D12GraphicsCommandList*>(commandList);
    int const slot  = static_cast<int>(frame % kStampSlots);
    int const index = slot * kStampPerSlot + id * 2;
    cmd->EndQuery(gStamps.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index + 1));
    cmd->ResolveQueryData(
        gStamps.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index), 2, gStamps.readback,
        static_cast<UINT64>(index) * sizeof(std::uint64_t)
    );
    gStamps.pending[slot] |= 1u << static_cast<unsigned>(id);
    gStamps.frameOf[slot]  = frame;
    ++gStamps.recorded[id];
}

void nrStampTick(std::uint64_t frame) {
    if (!gStamps.active || !gStamps.mapped) {
        return;
    }
    for (int slot = 0; slot < kStampSlots; ++slot) {
        if (!gStamps.pending[slot]) {
            continue;
        }
        if (frame < gStamps.frameOf[slot] + kHarvestDelay) {
            continue;
        }
        for (int id = 0; id < kStampKinds; ++id) {
            if ((gStamps.pending[slot] & (1u << static_cast<unsigned>(id))) == 0) {
                continue;
            }
            int const              index = slot * kStampPerSlot + id * 2;
            std::uint64_t const    begin = gStamps.mapped[index];
            std::uint64_t const    end   = gStamps.mapped[index + 1];
            bool                   valid = end > begin && begin != 0 && end != 0;
            double                 ms    = 0.0;
            if (valid) {
                ms = static_cast<double>(end - begin) * 1000.0 /
                     static_cast<double>(gStamps.frequency);
                // A frame pair never legitimately exceeds a fifth of a second,
                // and never finishes in under 20 microseconds.
                valid = ms > 0.02 && ms < 200.0;
            }
            if (valid) {
                gStamps.samples[id].push_back(ms);
            } else {
                ++gStamps.dropped[id];
            }
        }
        gStamps.pending[slot] = 0;
    }
}

void nrStampReport() {
    if (!gStamps.active) {
        nrLog(1, "[nr] gpu: 计时未启用");
        return;
    }
    reportOne(0, "nr");
    reportOne(1, "engine-dlss");
}

void nrStampReset() {
    for (int id = 0; id < kStampKinds; ++id) {
        gStamps.samples[id].clear();
        gStamps.dropped[id]  = 0;
        gStamps.recorded[id] = 0;
    }
}

} // namespace neurodragon::nr

