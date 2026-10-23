#include "neurodragon/nr/D3D12Bridge.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/Test.h"

#include <Windows.h>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef TEST
#include <cmath>
#include <filesystem>
#endif

namespace neurodragon::nr {
namespace {

// Levels: 0 = info (the user has to know), 1 = warn (degraded but working),
// 2 = error (NR cannot run), 3 = debug (diagnostics, only useful in a ticket).
// Every line leaves through LeviLamina's logger (log::write -> utils::logger());
// this helper only keeps this translation unit's printf-style call sites.
void nrLog(int level, char const* format, ...) {
    char    buffer[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    neurodragon::log::write(level, buffer);
}

// Debug line, printf style: TEST build only (src/neurodragon/Test.h). A dropped
// macro evaluates none of its arguments, so a release build does not format
// diagnostics that only a ticket would ever read.
#ifdef TEST
#define NR_DEBUG(...) nrLog(3, __VA_ARGS__)
#else
#define NR_DEBUG(...) ((void)0)
#endif

// NGX result codes as user-visible text: the catalogue lives in
// lang/<locale>.json under "nr.result.*", so a release build speaks the user's
// language instead of mixing English enum names with Chinese prose.
std::string getReason(uint32_t exitCode) {
    switch (exitCode) {
    case 0x1u:
        return neurodragon::log::text("nr.result.success");
    case 0xBAD00000u:
        return neurodragon::log::text("nr.result.fail");
    case 0xBAD00001u:
        return neurodragon::log::text("nr.result.featureNotSupported");
    case 0xBAD00002u:
        return neurodragon::log::text("nr.result.platformError");
    case 0xBAD00003u:
        return neurodragon::log::text("nr.result.featureAlreadyExists");
    case 0xBAD00004u:
        return neurodragon::log::text("nr.result.featureNotFound");
    case 0xBAD00005u:
        return neurodragon::log::text("nr.result.invalidParameter");
    case 0xBAD00006u:
        return neurodragon::log::text("nr.result.scratchBufferTooSmall");
    case 0xBAD00007u:
        return neurodragon::log::text("nr.result.notInitialized");
    case 0xBAD00008u:
        return neurodragon::log::text("nr.result.unsupportedInputFormat");
    case 0xBAD00009u:
        return neurodragon::log::text("nr.result.rwFlagMissing");
    case 0xBAD0000Au:
        return neurodragon::log::text("nr.result.missingInput");
    case 0xBAD0000Bu:
        return neurodragon::log::text("nr.result.unableToInitFeature");
    case 0xBAD0000Cu:
        return neurodragon::log::text("nr.result.outOfDate");
    case 0xBAD0000Du:
        return neurodragon::log::text("nr.result.outOfGpuMemory");
    case 0xBAD0000Eu:
        return neurodragon::log::text("nr.result.unsupportedFormat");
    case 0xBAD0000Fu:
        return neurodragon::log::text("nr.result.appDataPath");
    case 0xBAD00010u:
        return neurodragon::log::text("nr.result.unsupportedParameter");
    case 0xBAD00011u:
        return neurodragon::log::text("nr.result.denied");
    case 0xBAD00012u:
        return neurodragon::log::text("nr.result.notImplemented");
    default:
        return neurodragon::log::text("nr.result.unknown");
    }
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

    auto*                     res  = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_DESC const desc = res->GetDesc();
    info.dimension                 = static_cast<std::uint32_t>(desc.Dimension);
    info.format                    = static_cast<std::uint32_t>(desc.Format);
    info.width                     = desc.Width;
    info.height                    = desc.Height;
    info.depthOrArray              = desc.DepthOrArraySize;
    info.mips                      = desc.MipLevels;
    info.sampleCount               = desc.SampleDesc.Count;
    info.sampleQuality             = desc.SampleDesc.Quality;
    info.flags                     = static_cast<std::uint32_t>(desc.Flags);

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(res->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device) {
        info.device = device;
        device->Release();
    }
    return info;
}

namespace {

#ifdef TEST
struct ColorDump {
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    bool                                   recorded = false;
    std::uint32_t                          width    = 0;
    std::uint32_t                          height   = 0;
    std::uint32_t                          rowPitch = 0;
    std::string                            fileName;
    int                                    domain = 0; // 0 linear HDR, 1 encoded, 2 linear LDR
};

ColorDump  gDumps[4]; // up to four resources captured on the SAME frame
ColorDump* gDumpTarget = &gDumps[0];

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
            bits  = sign | ((127 - 15 - shift) << 23) | (mant << 13);
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
#endif

} // namespace

namespace {

#ifdef TEST
// Directory of this mod's DLL, supplied by nrSetModDirectory() (LeviLamina's
// Mod::getModDir). Declared up here because the dump writer needs it before the
// NGX section below.
std::wstring gModDirectory;

std::wstring moduleDirectory() { return gModDirectory; }

// Dumps are diagnostics, not deliverables: <mod>/dump/<name>. Returns an empty
// path when the mod directory is unknown, which simply skips the dump.
std::wstring dumpFilePath(std::string const& name) {
    auto dir = std::filesystem::path(moduleDirectory());
    if (dir.empty()) {
        return {};
    }
    dir /= L"dump";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    dir /= name;
    return dir.wstring();
}

void queueDump(void* commandList, void* resource, char const* fileName, int domain) {
    if (gDumpTarget->recorded || !commandList || !resource) {
        return;
    }
    auto*                     res  = static_cast<ID3D12Resource*>(resource);
    auto*                     cmd  = static_cast<ID3D12GraphicsCommandList*>(commandList);
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
            IID_PPV_ARGS(&gDumpTarget->staging)
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
    dst.pResource                          = gDumpTarget->staging.Get();
    dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = desc.Format;
    dst.PlacedFootprint.Footprint.Width    = static_cast<UINT>(desc.Width);
    dst.PlacedFootprint.Footprint.Height   = desc.Height;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);

    gDumpTarget->width    = static_cast<std::uint32_t>(desc.Width);
    gDumpTarget->height   = static_cast<std::uint32_t>(desc.Height);
    gDumpTarget->rowPitch = rowPitch;
    gDumpTarget->fileName = fileName ? fileName : "color_dump.bmp";
    gDumpTarget->domain   = domain;
    gDumpTarget->recorded = true;
}
#endif

} // namespace

#ifdef TEST
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
    if (!gDumpTarget->recorded || !gDumpTarget->staging) {
        return false;
    }
    void*       mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(gDumpTarget->rowPitch) * gDumpTarget->height};
    if (FAILED(gDumpTarget->staging->Map(0, &range, &mapped)) || !mapped) {
        return false;
    }

    auto const*       src      = static_cast<std::uint16_t const*>(mapped);
    float             peak     = 0.0f;
    double            sum      = 0.0;
    std::uint64_t     values   = 0;
    std::uint64_t     aboveOne = 0;
    std::uint32_t     nzWords  = 0;
    std::size_t const words    = static_cast<std::size_t>(gDumpTarget->rowPitch) * gDumpTarget->height / 2;
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
            3,
            "[dump] %s domain=%s max=%.3f mean=%.4f above1=%.2f%%",
            gDumpTarget->fileName.c_str(),
            gDumpTarget->domain == 0 ? "linear" : (gDumpTarget->domain == 1 ? "encoded" : "linear-ldr"),
            peak,
            sum / static_cast<double>(values),
            100.0 * static_cast<double>(aboveOne) / static_cast<double>(values)
        );
    }
    std::uint32_t const w        = gDumpTarget->width;
    std::uint32_t const h        = gDumpTarget->height;
    std::uint32_t const dataSize = w * h * 3u;
    std::uint32_t const fileSize = 54u + dataSize;

    std::string out(54u, '\0');
    out[0]     = 'B';
    out[1]     = 'M';
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
            reinterpret_cast<std::uint8_t const*>(src) + static_cast<std::size_t>(gDumpTarget->rowPitch) * (h - 1 - y)
        );
        std::uint8_t* dstRow = reinterpret_cast<std::uint8_t*>(out.data() + 54u + static_cast<std::size_t>(y) * w * 3u);
        for (std::uint32_t x = 0; x < w; ++x) {
            float        r    = halfToFloat(row[x * 4 + 0]);
            float        g    = halfToFloat(row[x * 4 + 1]);
            float        b    = halfToFloat(row[x * 4 + 2]);
            std::uint8_t outR = 0;
            std::uint8_t outG = 0;
            std::uint8_t outB = 0;
            if (gDumpTarget->domain == 0) {
                // Reinhard so highlights stop turning into flat white, then gamma.
                auto const tone = [](float v) { return v / (1.0f + (v > 0.0f ? v : 0.0f)); };
                outR            = toSrgb8(tone(r));
                outG            = toSrgb8(tone(g));
                outB            = toSrgb8(tone(b));
            } else if (gDumpTarget->domain == 2) {
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
    gDumpTarget->staging->Unmap(0, nullptr);

    std::wstring const path = dumpFilePath(gDumpTarget->fileName);
    if (path.empty()) {
        return false;
    }
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    gDumpTarget->recorded = false;
    gDumpTarget->staging.Reset();
    return ok;
}

// Second slot, so a single frame can dump two resources at once (fed vs base).
// Comparing dumps from different frames is useless whenever the camera moves.
void queueColorDump2(void* commandList, void* resource, char const* fileName) {
    gDumpTarget = &gDumps[1];
    queueColorDump(commandList, resource, fileName);
    gDumpTarget = &gDumps[0];
}

void queueColorDumpEncoded2(void* commandList, void* resource, char const* fileName) {
    gDumpTarget = &gDumps[1];
    queueColorDumpEncoded(commandList, resource, fileName);
    gDumpTarget = &gDumps[0];
}

// Generic slot form, so every input of the upscaler can be captured on one frame.
void queueColorDumpN(int slot, void* commandList, void* resource, char const* fileName, int domain) {
    if (slot < 0 || slot > 3) {
        return;
    }
    gDumpTarget = &gDumps[slot];
    queueDump(commandList, resource, fileName, domain);
    gDumpTarget = &gDumps[0];
}

bool flushColorDump2(float* maxValue, std::uint32_t* nonZeroWords) {
    return flushColorDumpN(1, maxValue, nonZeroWords);
}

bool flushColorDumpN(int slot, float* maxValue, std::uint32_t* nonZeroWords) {
    if (slot < 0 || slot > 3) {
        return false;
    }
    gDumpTarget   = &gDumps[slot];
    bool const ok = flushColorDump(maxValue, nonZeroWords);
    gDumpTarget   = &gDumps[0];
    return ok;
}
#endif

// ---------------------------------------------------------------------------
// Stage 2: NR (NGX feature 0x12) at the engine's render resolution.
//
// Everything below goes through measured vtable slots, never through the SDK
// header: this runtime's parameter object puts the resource overload at slot 1,
// unsigned at 3 and float at 6 (docs/D §4 pitfall 1), which is not the order the
// public header declares.
// ---------------------------------------------------------------------------
namespace {

constexpr int kSlotSetPtr = 1;  // void* / ID3D12Resource*
constexpr int kSlotSetU32 = 3;  // unsigned
constexpr int kSlotSetI32 = 4;  // int (the 310.x runtime reads its flags as int)
constexpr int kSlotSetF32 = 6;  // float
constexpr int kSlotGetU32 = 11; // unsigned read-back
constexpr int kSlotGetF32 = 14; // float read-back (probe scan: Sharpness = 0.35)

using SetPtrFn          = void (*)(void*, char const*, void*);
using SetU32Fn          = void (*)(void*, char const*, unsigned);
using SetI32Fn          = void (*)(void*, char const*, int);
using SetF32Fn          = void (*)(void*, char const*, float);
using GetU32Fn          = int (*)(void*, char const*, unsigned*);
using GetF32Fn          = int (*)(void*, char const*, float*);
using GetResFn          = int (*)(void*, char const*, void**);
using CreateFeatureFn   = int (*)(void*, int, void*, void**);
using EvaluateFeatureFn = int (*)(void*, void*, void const*, void*, void*);
using GetCapabilitiesFn = int (*)(void**);
using InitExtFn         = int (*)(unsigned long long, wchar_t const*, void*, int, void const*);
using InitFn            = int (*)(unsigned long long, wchar_t const*, void*, int);

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
    NgxPathList    pathList;
    void*          internalData;
    NgxLoggingInfo logging;
};

using InitProjectIdFn = int (*)(char const*, int, char const*, wchar_t const*, void*, NgxFeatureCommonInfo const*, int);

// Any project id works for a CUSTOM engine; the core maps it to the generic
// custom CMS id (0x0876232C) that the runtime's Init_Ext expects.
constexpr char const* kNrProjectId     = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
constexpr char const* kNrEngineVersion = "0.1.0-neural-rendering-source";

// Captured from the engine's own NVSDK_NGX_D3D12_Init* call (see RenderProbe.cpp).
unsigned long long gEngineAppId = 0;
std::wstring       gEngineDataPath;

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

float paramGetF32(void* params, char const* name) {
    float value = 0.0f;
    if (params && name) {
        (void)ngxSlot<GetF32Fn>(params, kSlotGetF32)(params, name, &value);
    }
    return value;
}

void* paramGetResource(void* params, char const* name) {
    // Measured read order for this runtime: 9 and 8 both return the D3D12
    // resource. Slot 14 used to be a "last resort" guess; it is the float getter
    // (docs/I §10), so reading it as a pointer would hand out 0.35 as an address.
    static constexpr int kSlots[] = {9, 8};
    for (int const index : kSlots) {
        void* value = nullptr;
        if (ngxSlot<GetResFn>(params, index)(params, name, &value) && value) {
            return value;
        }
    }
    return nullptr;
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
int gWindowIndex = -1;

#ifdef TEST
// The resource we most recently handed the engine's own DLSS as "Color", and
// which domain its pixels are in. Dumped once per measurement window so a frozen
// picture can be attributed: stale here = our write stopped; fresh here but the
// engine's Output is stale = the upscaler is the one not updating.
void* gFedResource = nullptr;
int   gFedDomain   = 0; // 0 = linear HDR, 1 = display-encoded
#endif

// The engine writes "Color" into its parameter object ONCE (docs/D §6.16), so from
// the first swap onwards the object keeps handing back OUR texture. Compositing
// onto whatever the object currently says is therefore a feedback loop: every
// frame would blend into the previous frame's output, and any old content (a
// previous view, a held item) would stay on screen almost forever. Keep the
// engine's own resource here and always composite onto that one.
std::atomic_bool gEngineResetPending{false};
// Parameter object that currently carries our Color swap, so it can be handed back.
void* gActiveEngineParams = nullptr;

ID3D12RootSignature* gEncodeRoot = nullptr;

// There is deliberately no hot reload: config/config.json is imported once at
// load, and the only things that change these values afterwards are the panel's
// "Apply" (in memory) and "Save as default" (memory + file).

ID3D12PipelineState*  gEncodePso    = nullptr;
ID3D12DescriptorHeap* gEncodeHeap   = nullptr;
ID3D12Resource*       gEncodedColor = nullptr;
ID3D12Resource*       gDecodedColor = nullptr;
// Replace=3 borrows the engine's Color for one frame: pixels are saved here and
UINT gEncodeStride = 0;
bool gEncodedIsSrv = false;
bool gDecodedIsSrv = false;

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
        outC = min(base * clamp(1.0 + (ratio - 1.0) * transferStrength, 0.25, 4.0), 64.0);
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

using D3DCompileFn =
    HRESULT (*)(LPCVOID, SIZE_T, LPCSTR, void const*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
using SerializeRootFn = HRESULT (*)(D3D12_ROOT_SIGNATURE_DESC const*, int, void**, void**);

// One texture sized exactly like the engine's Color. The engine's DLSS reads its
// input through a subrect anyway (its own Color is display-sized while the scene
// lives in the top-left render rect), so an identically sized allocation is a
// drop-in replacement at every resolution.
HRESULT createColorSizedTexture(ID3D12Device* device, D3D12_RESOURCE_DESC const& colorDesc, ID3D12Resource** out) {
    D3D12_RESOURCE_DESC desc = colorDesc;
    desc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.MipLevels           = 1;
    desc.DepthOrArraySize    = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    return device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(out)
    );
}

// The two HDR<->LDR textures the encode/composite passes need. Separated from the
// pipeline below because NR can be unloaded and created again in one session (the
// feature switch): the PSO, heap and root signature survive an unload, the textures
// do not.
bool ensureEncodeTextures(ID3D12Device* device, D3D12_RESOURCE_DESC const& colorDesc) {
    if (!gEncodedColor && FAILED(createColorSizedTexture(device, colorDesc, &gEncodedColor))) {
        nrLog(1, "[nr] encoded colour allocation failed");
        return false;
    }
    // Route A needs the way back: the NR result is display-referred, engine Color
    // is linear HDR, so the copy-back goes through the inverse of this pass.
    if (!gDecodedColor && FAILED(createColorSizedTexture(device, colorDesc, &gDecodedColor))) {
        nrLog(1, "[nr] decoded colour allocation failed (straight-copy fallback)");
        gDecodedColor = nullptr; // tolerated: Replace falls back to the raw colour
    }
    return true;
}

bool ensureEncoder(ID3D12Device* device, D3D12_RESOURCE_DESC const& colorDesc) {
    if (gEncodePso) {
        // Pipeline from an earlier load (NR was switched off and on again): only the
        // two textures have to come back.
        return ensureEncodeTextures(device, colorDesc);
    }
    // D3DCompile plus the pipeline state measured 44 ms on 2026-10-23 - below the
    // stage-marker bar, and covered by the enclosing nrCreate pair anyway.
    HMODULE    compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto const compile  = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    HMODULE    d3d12    = GetModuleHandleW(L"d3d12.dll");
    auto const serialize =
        d3d12 ? reinterpret_cast<SerializeRootFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) {
        nrLog(1, "[nr] HDR->LDR encode unavailable (no D3DCompile / SerializeRootSignature)");
        return false;
    }

    void*         code   = nullptr;
    void*         errors = nullptr;
    HRESULT const hr     = compile(
        kEncodeHlsl,
        std::strlen(kEncodeHlsl),
        "nr_encode",
        nullptr,
        nullptr,
        "main",
        "cs_5_1",
        3u /* optimization level 3 */,
        0,
        &code,
        &errors
    );
    if (errors) {
        auto* blob = static_cast<ID3DBlob*>(errors);
        nrLog(1, "[nr] encode shader compile failed: %s", static_cast<char const*>(blob->GetBufferPointer()));
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
    parameters[0].DescriptorTable  = {4, ranges};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants        = {0, 0, 8}; // w, h, mode + four TRP knobs + spare
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters   = 2;
    rootDesc.pParameters     = parameters;
    rootDesc.Flags           = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    void*         signature  = nullptr;
    void*         rootErrors = nullptr;
    HRESULT const rootHr     = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &rootErrors);
    if (rootErrors) {
        static_cast<ID3DBlob*>(rootErrors)->Release();
    }
    if (FAILED(rootHr) || !signature) {
        static_cast<ID3DBlob*>(code)->Release();
        nrLog(1, "[nr] encode root signature failed 0x%08X", static_cast<unsigned>(rootHr));
        return false;
    }
    auto*         sigBlob = static_cast<ID3DBlob*>(signature);
    HRESULT const rsHr =
        device
            ->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&gEncodeRoot));
    sigBlob->Release();
    if (FAILED(rsHr)) {
        static_cast<ID3DBlob*>(code)->Release();
        nrLog(1, "[nr] encode CreateRootSignature failed 0x%08X", static_cast<unsigned>(rsHr));
        return false;
    }

    auto*                             codeBlob = static_cast<ID3DBlob*>(code);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = gEncodeRoot;
    psoDesc.CS             = {codeBlob->GetBufferPointer(), codeBlob->GetBufferSize()};
    HRESULT const psoHr    = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&gEncodePso));
    if (FAILED(psoHr)) {
        nrLog(1, "[nr] encode PSO failed 0x%08X", static_cast<unsigned>(psoHr));
    } else {
        NR_DEBUG("[nr] encode PSO ok (%llu bytes)", static_cast<unsigned long long>(codeBlob->GetBufferSize()));
    }
    codeBlob->Release();
    if (FAILED(psoHr)) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 8; // 0-3 encode pass, 4-7 composite/decode pass
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gEncodeHeap)))) {
        nrLog(1, "[nr] encode descriptor heap failed");
        return false;
    }
    gEncodeStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!ensureEncodeTextures(device, colorDesc)) {
        return false;
    }
    NR_DEBUG("[nr] HDR↔LDR shader 就绪（Reinhard + gamma 2.2 / 反变换）");
    return true;
}

// Binds three SRVs (t0, t1, t2) and one UAV (u0) in the single table.
void writeDescriptors(
    ID3D12Device*   device,
    ID3D12Resource* s0,
    ID3D12Resource* s1,
    ID3D12Resource* s2,
    ID3D12Resource* target,
    int             slotBase
) {
    // Each pass gets its OWN four descriptors: a single heap rewritten twice in one
    // frame is a race - the GPU executes both dispatches with the descriptors the
    // CPU wrote LAST, so the encode pass silently wrote into the composite's target
    // and gEncodedColor never got refreshed after the first frame.
    D3D12_CPU_DESCRIPTOR_HANDLE handle  = gEncodeHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr                         += static_cast<SIZE_T>(slotBase) * gEncodeStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                       = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension                = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping      = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels          = 1;
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

D3D12_GPU_DESCRIPTOR_HANDLE gpuTable(int slotBase) {
    D3D12_GPU_DESCRIPTOR_HANDLE handle  = gEncodeHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr                         += static_cast<UINT64>(slotBase) * gEncodeStride;
    return handle;
}

// Root constants shared by every dispatch: dims, mode and the (bit-cast) gain
// boost so the gain mode can be exaggerated for judging without a rebuild.
void setPassConstants(
    ID3D12GraphicsCommandList* cmd,
    unsigned                   w,
    unsigned                   h,
    unsigned                   mode,
    float                      whitePoint,
    float                      transferStrength,
    float                      colourStrength,
    float                      maxRatio
) {
    auto bits = [](float f) {
        std::uint32_t out = 0;
        std::memcpy(&out, &f, sizeof(out));
        return out;
    };
    std::uint32_t const v[8] =
        {w, h, mode, bits(whitePoint), bits(transferStrength), bits(colourStrength), bits(maxRatio), 0u};
    cmd->SetComputeRoot32BitConstants(1, 8, v, 0);
}

struct NrState {
    void*             params   = nullptr;
    void*             feature  = nullptr;
    CreateFeatureFn   create   = nullptr;
    EvaluateFeatureFn evaluate = nullptr;
    ReleaseFeatureFn  release  = nullptr;
    ID3D12Resource*   output   = nullptr;
    unsigned          width    = 0;
    unsigned          height   = 0;
    // Allocation size of output/encoded/decoded. Grow-only: a smaller render size
    // only narrows the subrects, so reallocating on every downscale would burn VRAM
    // for nothing.
    unsigned texWidth                 = 0;
    unsigned texHeight                = 0;
    unsigned rebuilds                 = 0; // resolution changes followed
    unsigned lastRebuildEval          = 0; // evalCount when the last one ran
    bool     rebuildLimitLogged       = false;
    unsigned failedWidth              = 0; // new size that failed once
    unsigned failedHeight             = 0;
    bool     created                  = false;
    bool     attempted                = false;
    bool     outputWasSrv             = false;
    bool     shutdown                 = false;
    bool     resourceRoundtripChecked = false;
    unsigned evalCount                = 0;
    int      lastError                = 0;
    // The engine's own DLSS output (the texture the panel calls "Output"). Purely
    // for display; 0 until the first frame that handed us one.
    unsigned engineOutputWidth  = 0;
    unsigned engineOutputHeight = 0;
};

NrState gNr;

// What the engine's own DLSS is doing this frame, independent of NR. The panel's
// monitor reads it, so switching NR off does not blank out the sizes.
struct EngineFrame {
    unsigned renderWidth  = 0;
    unsigned renderHeight = 0;
    unsigned outputWidth  = 0;
    unsigned outputHeight = 0;
    void*    output       = nullptr;
};

EngineFrame gEngineFrame;

// One resolution change is one rebuild. The cap bounds a session that flips sizes
// constantly (a dynamic-resolution setting): beyond it NR stays on the size it has
// instead of building features forever.
constexpr unsigned kMaxRebuilds = 64;
// Menu <-> world transitions can legitimately change the size twice in a row; a
// per-frame size change is thrash, and rebuilding on every frame of it would be
// worse than the stale size.
constexpr unsigned kMinEvalsBetweenRebuilds = 20;

// Objects a rebuild replaced. They may still be referenced by command lists the GPU
// has not finished, so they are released only after a fence says the queue is idle;
// when the queue is unknown they are kept, because a leak costs memory once while a
// release-while-busy costs the whole session (docs/D §6.16).
void*                        gGameQueue = nullptr;
std::vector<void*>           gRetiredFeatures;
std::vector<ID3D12Resource*> gRetiredResources;

// The composite's fade-in counter, reset by a rebuild so a feature without history
// is not multiplied in at full strength on its first frame.
unsigned gCompositeRamp = 0;

bool gpuIsIdle(ID3D12Device* device) {
    auto* queue = static_cast<ID3D12CommandQueue*>(gGameQueue);
    if (!device || !queue) {
        return false;
    }
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence) {
        return false;
    }
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool   idle  = false;
    // 3 s: the render thread is blocked here, so a removed device must not turn a
    // resolution change into a frozen client.
    if (event && SUCCEEDED(queue->Signal(fence, 1))) {
        if (fence->GetCompletedValue() >= 1) {
            idle = true;
        } else if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
            idle = ::WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        }
    }
    if (event) {
        ::CloseHandle(event);
    }
    fence->Release();
    return idle;
}

void releaseRetired(ID3D12Device* device) {
    if (gRetiredFeatures.empty() && gRetiredResources.empty()) {
        return;
    }
    if (!gpuIsIdle(device)) {
        NR_DEBUG(
            "[nr] 退役对象暂不释放（queue=%p，%zu feature / %zu 纹理仍持有）",
            gGameQueue,
            gRetiredFeatures.size(),
            gRetiredResources.size()
        );
        return;
    }
    for (void* feature : gRetiredFeatures) {
        if (gNr.release) {
            gNr.release(feature);
        }
    }
    for (ID3D12Resource* resource : gRetiredResources) {
        resource->Release();
    }
    gRetiredFeatures.clear();
    gRetiredResources.clear();
}

void retire(void* feature, ID3D12Resource* output, ID3D12Resource* encoded, ID3D12Resource* decoded) {
    if (feature) {
        gRetiredFeatures.push_back(feature);
    }
    for (ID3D12Resource* resource : {output, encoded, decoded}) {
        if (resource) {
            gRetiredResources.push_back(resource);
        }
    }
}

// Timestamps: kSlots distinct frame slots, two brackets (NR / engine DLSS) per
// slot. A pair is harvested kHarvestDelay frames later, because nothing in this
// process can signal a fence on the game's queue.
constexpr int           kStampSlots   = 8;
constexpr int           kStampKinds   = 2;
constexpr int           kStampPerSlot = kStampKinds * 2;
constexpr std::uint64_t kHarvestDelay = 3;

struct StampRing {
    ID3D12QueryHeap*    heap      = nullptr;
    ID3D12Resource*     readback  = nullptr;
    std::uint64_t*      mapped    = nullptr;
    std::uint64_t       frequency = 0;
    std::uint64_t       frameOf[kStampSlots]{};
    unsigned            pending[kStampSlots]{};
    bool                active = false;
    std::vector<double> samples[kStampKinds];
    unsigned            dropped[kStampKinds]{};
    unsigned            recorded[kStampKinds]{};
};

StampRing gStamps;

// The panel reads these; the render thread publishes them a few times per second
// (publishMedian), so a reader can never see a half-appended sample vector.
std::atomic<double>   gStampMedianMs[kStampKinds]{};
std::atomic<unsigned> gStampMedianCount[kStampKinds]{};

void publishMedian(int id) {
    if (id < 0 || id >= kStampKinds) return;
    auto const& values = gStamps.samples[id];
    if (values.empty()) return;

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    gStampMedianMs[id].store(sorted[sorted.size() / 2], std::memory_order_relaxed);
    gStampMedianCount[id].store(static_cast<unsigned>(sorted.size()), std::memory_order_relaxed);
}

bool ensureStamps(ID3D12Device* device) {
    if (gStamps.active) {
        return true;
    }
    D3D12_QUERY_HEAP_DESC query{};
    query.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = static_cast<UINT>(kStampSlots * kStampPerSlot);
    if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&gStamps.heap)))) {
        NR_DEBUG("[nr] timestamp heap 创建失败，计时关闭");
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
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&gStamps.readback)
        ))) {
        NR_DEBUG("[nr] timestamp readback 分配失败，计时关闭");
        return false;
    }
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(buffer.Width)};
    if (FAILED(gStamps.readback->Map(0, &readRange, reinterpret_cast<void**>(&gStamps.mapped))) || !gStamps.mapped) {
        NR_DEBUG("[nr] timestamp readback map 失败，计时关闭");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type            = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && queue) {
        queue->GetTimestampFrequency(&gStamps.frequency);
        queue->Release();
    }
    if (!gStamps.frequency) {
        NR_DEBUG("[nr] 取不到 timestamp frequency，计时关闭");
        return false;
    }
    gStamps.active = true;
    NR_DEBUG("[nr] timestamp 频率 %llu", static_cast<unsigned long long>(gStamps.frequency));
    return true;
}

void reportOne(int id, [[maybe_unused]] char const* name) {
    auto const& values = gStamps.samples[id];
    if (values.empty()) {
        NR_DEBUG("[nr] gpu %s: 没有可用样本 (recorded=%u dropped=%u)", name, gStamps.recorded[id], gStamps.dropped[id]);
        return;
    }
    publishMedian(id);
#ifdef TEST
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double const v : sorted) {
        sum += v;
    }
    double const median = sorted[sorted.size() / 2];
    NR_DEBUG(
        "[nr] gpu %s ms median=%.3f mean=%.3f min=%.3f max=%.3f n=%zu dropped=%u",
        name,
        median,
        sum / static_cast<double>(sorted.size()),
        sorted.front(),
        sorted.back(),
        sorted.size(),
        gStamps.dropped[id]
    );
#endif
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
    using CreateFactory1Fn       = HRESULT (*)(REFIID, void**);
    auto const     createFactory = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory       = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))
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


} // namespace

// ---------------------------------------------------------------------------
// "Why is feature 0x12 rejected?" - NVSDK_NGX_D3D12_GetFeatureRequirements.
// The structs below mirror nvsdk_ngx_defs.h; the NGX headers themselves stay out
// of this translation unit (docs/D §4 pitfall 5). Diagnostics only, TEST build.
// ---------------------------------------------------------------------------
#ifdef TEST
namespace {

struct NgxProjectIdDesc {
    char const* projectId;
    int         engineType;
    char const* engineVersion;
};

struct NgxAppIdentifier {
    int identifierType; // 0 = application id, 1 = project id
    union {
        NgxProjectIdDesc   desc;
        unsigned long long appId;
    } v;
};

struct NgxDiscoveryInfo {
    int              sdkVersion;
    int              featureId;
    NgxAppIdentifier identifier;
    wchar_t const*   appDataPath;
    void const*      featureInfo;
};

struct NgxFeatureRequirement {
    int      featureSupported; // 0 = supported; bitfield of NVSDK_NGX_FeatureSupportResult_*
    unsigned minHwArchitecture;
    char     minOsVersion[255];
};

using GetFeatureRequirementsFn = int (*)(void*, NgxDiscoveryInfo const*, NgxFeatureRequirement*);

void logFeatureRequirementsFrom(void* function, ID3D12Device* device, wchar_t const* dataPath, char const* tag) {
    auto const fn = reinterpret_cast<GetFeatureRequirementsFn>(function);
    if (!fn) {
        NR_DEBUG("[nr] %s 没有 GetFeatureRequirements", tag);
        return;
    }

    IDXGIAdapter1* adapter = nullptr;
    LUID const     luid    = device->GetAdapterLuid();
    HMODULE        dxgi    = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        using CreateFactory1Fn       = HRESULT (*)(REFIID, void**);
        auto const     createFactory = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        IDXGIFactory1* factory       = nullptr;
        if (createFactory && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))
            && factory) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory->EnumAdapters1(i, &candidate) != S_OK || !candidate) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                candidate->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
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
        info.sdkVersion                = kNrFeatureApiVersion;
        info.featureId                 = feature;
        info.identifier.identifierType = 0;
        info.identifier.v.appId        = kNrGenericCmsId;
        info.appDataPath               = dataPath;
        info.featureInfo               = nullptr;
        NgxFeatureRequirement requirement{};
        int const             ret = fn(adapter, &info, &requirement);
        nrLog(
            ret == 1 && requirement.featureSupported == 0 ? 3 : 1,
            "[nr] %s GetFeatureRequirements(0x%02X) ret=0x%08X supported=%d "
            "(0=ok,1=check,2=driver,4=adapter,8=os,16=notimpl) minHW=0x%X minOS=%s",
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
#endif


void nrSetWindowIndex(int index) { gWindowIndex = index; }

#ifdef TEST
void nrSetModDirectory(wchar_t const* directory) {
    gModDirectory = directory ? directory : L"";
    if (!gModDirectory.empty() && gModDirectory.back() != L'\\' && gModDirectory.back() != L'/') {
        gModDirectory.push_back(L'\\');
    }
}
#endif

void nrSetEngineInit(unsigned long long appId, wchar_t const* dataPath) {
    if (appId) {
        gEngineAppId = appId;
    }
    if (dataPath) {
        gEngineDataPath = dataPath;
    }
}

bool nrCreate(
    void*    commandList,
    void*    colorResource,
    unsigned width,
    unsigned height,
    void*    engineParams,
    void*    createFeature,
    void*    evaluateFeature
) {
    if (gNr.shutdown) {
        return false;
    }
    if (gNr.attempted) {
        return gNr.created;
    }
    gNr.attempted = true;
    if (!commandList || !colorResource || !width || !height) {
        nrLog(2, "[nr] create: incomplete arguments (cl=%p color=%p %ux%u)", commandList, colorResource, width, height);
        return false;
    }
    if (!createFeature || !evaluateFeature) {
        nrLog(2, "[nr] create: original NGX exports missing");
        return false;
    }

    // One stage for the whole creation: the output texture, the shader compile, the
    // timestamp heap and the NGX call that makes the core load the 264 MB model. Each
    // of those has been the slow part at some point, and this pair reports which.
    // The model itself gets its own nested stage below.
    log::Stage createStage{"log.stage.nrCreate"};

    auto*         color  = static_cast<ID3D12Resource*>(colorResource);
    ID3D12Device* device = nullptr;
    if (FAILED(color->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        nrLog(2, "[nr] create: no ID3D12Device from Color");
        createStage.markFailed();
        return false;
    }

    D3D12_RESOURCE_DESC outDesc  = color->GetDesc();
    HRESULT const       createHr = createColorSizedTexture(device, outDesc, &gNr.output);
    if (FAILED(createHr)) {
        nrLog(
            2,
            "[nr] output texture creation failed 0x%08X (%llux%llu fmt=%u)",
            static_cast<unsigned>(createHr),
            static_cast<unsigned long long>(outDesc.Width),
            static_cast<unsigned long long>(outDesc.Height),
            static_cast<unsigned>(outDesc.Format)
        );
        createStage.markFailed();
        return false;
    }
    gNr.width     = width;
    gNr.height    = height;
    gNr.texWidth  = static_cast<unsigned>(outDesc.Width);
    gNr.texHeight = static_cast<unsigned>(outDesc.Height);

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
        nrLog(2, "[nr] NGX core module is not loaded");
        device->Release();
        createStage.markFailed();
        return false;
    }
    NR_DEBUG("[nr] core module=%p", static_cast<void*>(core));

    // Shared application data path (NGX wants a writable directory here).
    wchar_t dataPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dataPath);
    std::wstring dataDir = std::wstring(dataPath) + L"NeuroDragon";
    CreateDirectoryW(dataDir.c_str(), nullptr);
    dataDir += L"\\NGX";
    CreateDirectoryW(dataDir.c_str(), nullptr);

#ifdef TEST
    logFeatureRequirementsFrom(
        reinterpret_cast<void*>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetFeatureRequirements")),
        device,
        dataDir.c_str(),
        "core"
    );
#endif

    // TRP owns the process' NGX session because it is the first (only) consumer,
    // so its runtime Init_Ext names the id the core was initialised with. Here the
    // engine owns the session; instead of re-initialising the core (which would
    // risk the engine's own DLSS feature), reuse the app id captured from the
    // engine's Init call - that is the id this core will accept.
    NR_DEBUG(
        "[nr] 引擎初始化上下文 appId=%#llx dataPath=%ls（0/空 表示没抓到）",
        static_cast<unsigned long long>(gEngineAppId),
        gEngineDataPath.empty() ? L"<none>" : gEngineDataPath.c_str()
    );

    // The core loads the NR model itself (2026-10-21, driver 617.42): the file
    // must be named nvngx_dlssnr.dll and sit in the application directory. Do not
    // preload it here - that only creates a second 266 MB copy.
    HMODULE runtime = GetModuleHandleW(L"nvngx_dlssnr.dll");
    NR_DEBUG("[nr] runtime module=%p（由 core 自己从应用目录加载）", static_cast<void*>(runtime));

    RuntimeApi runtimeApi{};
    RuntimeApi coreApi{};
    auto const coreCapabilities =
        reinterpret_cast<GetCapabilitiesFn>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto const runtimeInitExt = reinterpret_cast<InitExtFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_Init_Ext"));
    // The runtime build exports no GetCapabilityParameters, so it borrows the
    // core's parameter object - the same mix TRP uses.
    runtimeApi.name   = "runtime";
    runtimeApi.module = runtime;
    runtimeApi.create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    runtimeApi.evaluate =
        reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
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

    void*     params  = nullptr;
    int const capsRet = coreCapabilities ? coreCapabilities(&params) : 0xBAD00001;
    if (capsRet != 1 || !params) {
        nrLog(2, "[nr] GetCapabilityParameters ret=0x%08X params=%p", static_cast<unsigned>(capsRet), params);
        device->Release();
        createStage.markFailed();
        return false;
    }
    if (engineParams) {
        bool const sameTable = *reinterpret_cast<void***>(params) == *reinterpret_cast<void***>(engineParams);
        // A different vtable is a real warning (the object we write is not the one
        // the engine writes through); equality is only worth a debug line.
        if (sameTable) {
            NR_DEBUG(
                "[nr] params vtable ours=%p engine=%p same=1",
                *reinterpret_cast<void**>(params),
                *reinterpret_cast<void**>(engineParams)
            );
        } else {
            nrLog(
                1,
                "[nr] params vtable ours=%p engine=%p same=0",
                *reinterpret_cast<void**>(params),
                *reinterpret_cast<void**>(engineParams)
            );
        }
    }

    paramSetU32(params, "CreationNodeMask", 1);
    paramSetU32(params, "VisibilityNodeMask", 1);
    paramSetU32(params, "PerfQualityValue", 2);
    paramSetI32(params, "DLSSNR.Hint.Render.Preset", 0);
    paramSetF32(params, "DLSSNR.ScalingRatio", 1.0f);
    paramSetU32(params, "DLSSNR.Width", width);
    paramSetU32(params, "DLSSNR.Height", height);
    paramSetI32(params, "DLSSNR.Enabled", 1);
    unsigned const widthBack = paramGetU32(params, "DLSSNR.Width");
    if (widthBack == width) {
        NR_DEBUG("[nr] 参数往返 DLSSNR.Width=%u 期望=%u", widthBack, width);
    } else {
        nrLog(1, "[nr] 参数往返 DLSSNR.Width=%u 期望=%u", widthBack, width);
    }

    auto createWith = [&](RuntimeApi const& api, [[maybe_unused]] char const* tag) -> bool {
        if (!api.create || !api.evaluate) {
            NR_DEBUG("[nr] %s 不可用（create=%d eval=%d）", tag, api.create ? 1 : 0, api.evaluate ? 1 : 0);
            return false;
        }
        void* handle = nullptr;
        // The core loads the ~264 MB NR model inside this call, so the first
        // CreateFeature of a session can stall for seconds. The duration only feeds
        // the debug line: the creation step has one start/end pair and a nested pair
        // for the model load read like two separate steps (2026-10-23).
        auto const                    started = std::chrono::steady_clock::now();
        int const                     ret     = api.create(commandList, 0x12, params, &handle);
        bool const                    created = ret == 1 && handle != nullptr;
        [[maybe_unused]] double const ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (created) {
            gNr.params   = params;
            gNr.feature  = handle;
            gNr.create   = api.create;
            gNr.evaluate = api.evaluate;
            gNr.release  = reinterpret_cast<ReleaseFeatureFn>(
                api.module ? GetProcAddress(api.module, "NVSDK_NGX_D3D12_ReleaseFeature") : nullptr
            );
            if (!gNr.release) {
                gNr.release =
                    reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
            }
            gNr.created = true;
            return true;
        }
        gNr.lastError = ret;
        NR_DEBUG(
            "[nr] %s CreateFeature(0x12) ret=0x%08X %s (%.0f ms)",
            tag,
            static_cast<unsigned>(ret),
            getReason(static_cast<std::uint32_t>(ret)).c_str(),
            ms
        );
        return false;
    };

    // Ladder, all in one run: TRP works without a core-side Init because it owns
    // the process' NGX session. Here the engine got there first, so the model may
    // need its own Init_Ext before it will create the feature. Only the final
    // failure is worth telling the user about; the per-rung codes are debug.
    if (createWith(coreApi, "core")) {
        device->Release();
        return true;
    }
    if (runtimeApi.create) {
#ifdef TEST
        logFeatureRequirementsFrom(
            reinterpret_cast<void*>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_GetFeatureRequirements")),
            device,
            dataDir.c_str(),
            "runtime"
        );
#endif
        if (createWith(runtimeApi, "runtime/未Init")) {
            device->Release();
            return true;
        }
        if (runtimeInitExt) {
            unsigned long long const appId       = gEngineAppId ? gEngineAppId : kNrGenericCmsId;
            wchar_t const* const     appDataPath = gEngineDataPath.empty() ? dataDir.c_str() : gEngineDataPath.c_str();
            [[maybe_unused]] int const initRet =
                runtimeInitExt(appId, appDataPath, device, kNrFeatureApiVersion, nullptr);
            NR_DEBUG(
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
    neurodragon::log::writeKey(
        2,
        "nr.error.createFailed",
        static_cast<unsigned>(gNr.lastError),
        getReason(static_cast<std::uint32_t>(gNr.lastError))
    );
    return false;
}

bool nrIsCreated() { return gNr.created; }

void nrSetGameQueue(void* queue) { gGameQueue = queue; }

bool nrEnsureRenderSize(void* commandList, void* color, unsigned width, unsigned height) {
    if (!gNr.created || !commandList || !color || !width || !height) {
        return false;
    }
    if (width == gNr.width && height == gNr.height) {
        return true;
    }
    if (width == gNr.failedWidth && height == gNr.failedHeight) {
        return false; // this exact size already failed; do not retry it every frame
    }
    if (gNr.rebuilds >= kMaxRebuilds) {
        if (!gNr.rebuildLimitLogged) {
            gNr.rebuildLimitLogged = true;
            neurodragon::log::writeKey(1, "nr.warn.rebuildLimit", gNr.rebuilds);
        }
        return false;
    }
    if (gNr.rebuilds > 0 && gNr.evalCount - gNr.lastRebuildEval < kMinEvalsBetweenRebuilds) {
        return false; // the size is still moving; try again once it settles
    }

    auto*         colorRes = static_cast<ID3D12Resource*>(color);
    ID3D12Device* device   = nullptr;
    if (FAILED(colorRes->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return false;
    }

    D3D12_RESOURCE_DESC const colorDesc = colorRes->GetDesc();
    // Only grow: a smaller size is served by narrowing the subrects, and allocating
    // a full set of textures per downscale would cost ~90 MB at 1440p for nothing.
    bool const      needTextures = !gNr.output || colorDesc.Width > gNr.texWidth || colorDesc.Height > gNr.texHeight;
    ID3D12Resource* newOutput    = nullptr;
    ID3D12Resource* newEncoded   = nullptr;
    ID3D12Resource* newDecoded   = nullptr;
    if (needTextures) {
        bool ok = SUCCEEDED(createColorSizedTexture(device, colorDesc, &newOutput));
        if (ok) {
            ok = SUCCEEDED(createColorSizedTexture(device, colorDesc, &newEncoded));
        }
        if (ok) {
            ok = SUCCEEDED(createColorSizedTexture(device, colorDesc, &newDecoded));
        }
        if (!ok) {
            nrLog(2, "[nr] 重建 %ux%u：纹理创建失败，保持 %ux%u", width, height, gNr.width, gNr.height);
            for (ID3D12Resource* resource : {newOutput, newEncoded, newDecoded}) {
                if (resource) {
                    resource->Release(); // never bound to a command list, safe to drop
                }
            }
            device->Release();
            gNr.failedWidth  = width;
            gNr.failedHeight = height;
            return false;
        }
    }

    paramSetU32(gNr.params, "DLSSNR.Width", width);
    paramSetU32(gNr.params, "DLSSNR.Height", height);
    // Create before releasing. Releasing the last feature makes the driver unload the
    // 264 MB model, so the replacement has to exist first or every resolution change
    // costs a model reload.
    void*     handle = nullptr;
    int const ret    = gNr.create(commandList, 0x12, gNr.params, &handle);
    if (ret != 1 || !handle) {
        gNr.lastError    = ret;
        gNr.failedWidth  = width;
        gNr.failedHeight = height;
        neurodragon::log::writeKey(
            1,
            "nr.warn.rebuildFailed",
            static_cast<unsigned>(ret),
            getReason(static_cast<std::uint32_t>(ret)),
            width,
            height
        );
        for (ID3D12Resource* resource : {newOutput, newEncoded, newDecoded}) {
            if (resource) {
                resource->Release();
            }
        }
        device->Release();
        return false;
    }

    [[maybe_unused]] unsigned const oldWidth  = gNr.width;
    [[maybe_unused]] unsigned const oldHeight = gNr.height;
    retire(
        gNr.feature,
        needTextures ? gNr.output : nullptr,
        needTextures ? gEncodedColor : nullptr,
        needTextures ? gDecodedColor : nullptr
    );
    if (needTextures) {
        gNr.output    = newOutput;
        gEncodedColor = newEncoded;
        gDecodedColor = newDecoded;
        gNr.texWidth  = static_cast<unsigned>(colorDesc.Width);
        gNr.texHeight = static_cast<unsigned>(colorDesc.Height);
        // Fresh resources start in COMMON, so every "was a shader resource" flag
        // that the transition helpers rely on has to be cleared.
        gNr.outputWasSrv = false;
        gEncodedIsSrv    = false;
        gDecodedIsSrv    = false;
    }
    gNr.feature                  = handle;
    gNr.width                    = width;
    gNr.height                   = height;
    gNr.resourceRoundtripChecked = false;
    gNr.engineOutputWidth        = 0; // re-measured from the next frame's Output
    gNr.engineOutputHeight       = 0;
    gCompositeRamp               = 0; // no history yet: fade the new feature in
    ++gNr.rebuilds;
    gNr.lastRebuildEval = gNr.evalCount;
    // Diagnostic builds only: a resolution change is routine once NR follows the
    // engine's render size, so the release log stays quiet (the panel's rebuild
    // counter is the user-visible trace).
    NR_DEBUG("[nr] 分辨率变化 %ux%u → %ux%u（第 %u 次重建）", oldWidth, oldHeight, width, height, gNr.rebuilds);
    // Try to free the replaced objects now; if the queue is unknown they stay alive
    // and the next rebuild tries again.
    releaseRetired(device);
    device->Release();
    return true;
}

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
    NR_DEBUG("[nr] 退出：保留 GPU 对象与 feature（不 Release，避免驱动在退出时崩溃）");
}

void nrPublishEngineFrame(unsigned renderWidth, unsigned renderHeight, void* output) {
    if (renderWidth && renderHeight) {
        gEngineFrame.renderWidth  = renderWidth;
        gEngineFrame.renderHeight = renderHeight;
    }
    if (output && output != gEngineFrame.output) {
        // The engine's upscale target is stable per frame-graph generation, so the
        // description is only taken when the pointer changes (a resize).
        ResourceInfo const info = describe(output);
        if (info.width && info.height) {
            gEngineFrame.output       = output;
            gEngineFrame.outputWidth  = static_cast<unsigned>(info.width);
            gEngineFrame.outputHeight = static_cast<unsigned>(info.height);
        }
    }
}

void nrUnload() {
    if (!gNr.created && !gNr.attempted) {
        return;
    }
    ID3D12Device* device = nullptr;
    if (gNr.output && FAILED(gNr.output->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
        device = nullptr;
    }
    // Retire instead of releasing in place: the GPU may still be reading these, and
    // the retired list is released after a fence. If the queue is unknown they stay
    // in that list for a later attempt rather than risking a driver fault.
    retire(gNr.feature, gNr.output, gEncodedColor, gDecodedColor);
    if (device) {
        releaseRetired(device);
        device->Release();
    }

    // Logically off either way: nothing below evaluates or swaps again until the
    // feature is enabled and nrCreate runs afresh.
    gNr.feature                  = nullptr;
    gNr.output                   = nullptr;
    gNr.created                  = false;
    gNr.attempted                = false; // re-arm creation for the next enable
    gNr.width                    = 0;
    gNr.height                   = 0;
    gNr.texWidth                 = 0;
    gNr.texHeight                = 0;
    gNr.outputWasSrv             = false;
    gNr.resourceRoundtripChecked = false;
    gNr.rebuilds                 = 0;
    gNr.lastRebuildEval          = 0;
    gNr.failedWidth              = 0;
    gNr.failedHeight             = 0;
    gNr.rebuildLimitLogged       = false;
    gNr.engineOutputWidth        = 0;
    gNr.engineOutputHeight       = 0;
    gCompositeRamp               = 0;
    gEncodedColor                = nullptr;
    gDecodedColor                = nullptr;
    gEncodedIsSrv                = false;
    gDecodedIsSrv                = false;
    NR_DEBUG("[nr] 已卸载：feature 与纹理退役，模型随最后一个 feature 离开驱动");
    // The user asked for it explicitly ("保存为默认" with NR off): say that it happened.
    neurodragon::log::write(0, neurodragon::log::text("nr.info.unloaded"));
}

namespace {

// Written by the panel's "Save as default" (render thread), consumed by the
// evaluation hook. One shot on purpose: a request that arrives before NR ever
// created anything must not unload a feature created afterwards.
std::atomic_bool gUnloadRequested{false};

} // namespace

void nrRequestUnload() { gUnloadRequested.store(true, std::memory_order_release); }

bool nrTakeUnloadRequest() { return gUnloadRequested.exchange(false, std::memory_order_acq_rel); }

#ifdef TEST
void* nrOutputResource() { return gNr.output; }

void* nrFedResource() { return gFedResource; }

int nrFedDomain() { return gFedDomain; }

void* nrEncodedColorResource() { return gEncodedColor; }

bool nrParamLogEnabled() { return config::get().nr.paramLog != 0; }
#endif

bool nrReplaceContinuous() { return config::get().nr.replace != 0; }
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
void compositeGain(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource*            color,
    D3D12_RESOURCE_STATES      colorState,
    void*                      engineParams,
    unsigned                   w,
    unsigned                   h
) {
    ID3D12Device* device = nullptr;
    if (FAILED(color->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return;
    }
    // Fade the effect in over the first frames of the process, and again after every
    // rebuild. Switching the upscaler's input in one step leaves the previous frame
    // burned into DLSS's history ("第一帧轮廓残留", ); starting at gain = 1 lets the
    // history converge instead of being overwritten.
    float const ramp = gCompositeRamp >= 24u ? 1.0f : static_cast<float>(gCompositeRamp) / 24.0f;
    if (gCompositeRamp < 24u) {
        ++gCompositeRamp;
    }
    // Replace=6 uses TRP's "Ratio" resolve: rebase the network's result onto the
    // original luminance instead of multiplying by a raw gain. Replace=5 keeps the
    // earlier bounded-gain experiment for comparison.
    unsigned const mode           = config::get().nr.replace == 6 ? 4u : 2u;
    float const    strength       = mode == 4u ? config::get().nr.transferStrength
                                               : (config::get().nr.gainBoost > 0.0f ? config::get().nr.gainBoost : 1.0f);
    float const    strengthRamped = strength * ramp;
    setPassConstants(
        cmd,
        w,
        h,
        mode,
        config::get().nr.whitePoint,
        strengthRamped,
        config::get().nr.colourStrength,
        config::get().nr.maxRatio
    );
    D3D12_RESOURCE_STATES const nrState =
        gNr.outputWasSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
    auto* nrTex = static_cast<ID3D12Resource*>(gNr.output);

    transition(cmd, color, colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd, nrTex, nrState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(
        cmd,
        gDecodedColor,
        gDecodedIsSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    writeDescriptors(device, color, gEncodedColor, nrTex, gDecodedColor, 4);
    device->Release();

    ID3D12DescriptorHeap* heaps[1] = {gEncodeHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(gEncodeRoot);
    cmd->SetPipelineState(gEncodePso);
    cmd->SetComputeRootDescriptorTable(0, gpuTable(4));
    cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = gDecodedColor;
    cmd->ResourceBarrier(1, &uavBarrier);
    transition(cmd, gDecodedColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    gDecodedIsSrv = true;
    transition(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colorState);
    transition(cmd, nrTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nrState);

    // Same one-frame trick as the other Replace modes: the engine re-fills its
    // parameter object before every EvaluateFeature. Replace=7 keeps the engine's
    // DLSS untouched: our own SR consumes the composite instead.
    if (config::get().nr.replace != 7) {
        paramSetPtr(engineParams, "Color", gDecodedColor);
#ifdef TEST
        gFedResource = gDecodedColor;
        gFedDomain   = 0;
#endif
    }
#ifdef TEST
    static bool logged = false;
    if (!logged) {
        logged = true;
        NR_DEBUG(
            "[nr] Replace=%d 生效：%s（用上一帧 NR 结果，strength=%.2f white=%.2f colour=%.2f maxRatio=%.2f）",
            config::get().nr.replace,
            mode == 4u ? "TRP Ratio 合成" : "增益合成",
            static_cast<double>(strengthRamped),
            static_cast<double>(config::get().nr.whitePoint),
            static_cast<double>(config::get().nr.colourStrength),
            static_cast<double>(config::get().nr.maxRatio)
        );
    }
#endif
}

} // namespace

void nrNotifyLevelLoaded() { gEngineResetPending.store(true, std::memory_order_relaxed); }

bool nrRecord(
    void*    commandList,
    void*    color,
    void*    depth,
    void*    motionVectors,
    unsigned guideWidth,
    unsigned guideHeight,
    float    motionVectorScaleX,
    float    motionVectorScaleY,
    int      depthInverted,
    bool     reset,
    void*    engineParams,
    void*    engineOutput
) {
    if (!gNr.created || !commandList || !color || !depth || !motionVectors) {
        return false;
    }
    auto*          cmd    = static_cast<ID3D12GraphicsCommandList*>(commandList);
    void*          params = gNr.params;
    auto*          output = gNr.output;
    unsigned const w      = gNr.width;
    unsigned const h      = gNr.height;

    // Display only: remember what the engine's DLSS is upscaling into, so the panel
    // can show the real resolution instead of a guess. Described once; the resource
    // is stable for the lifetime of the frame graph.
    if (gNr.engineOutputWidth == 0 && engineOutput) {
        ResourceInfo const info = describe(engineOutput);
        if (info.width && info.height) {
            gNr.engineOutputWidth  = static_cast<unsigned>(info.width);
            gNr.engineOutputHeight = static_cast<unsigned>(info.height);
        }
    }

    bool const useEncoded =
        (config::get().nr.replace != 4 || config::get().nr.nrEncode >= 0) && gEncodePso && gEncodedColor;
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
    //
    // The engine's own DLSS is driven with MV.Scale = render width/height (see the
    // parameter table in docs/D §6.16), i.e. its motion vectors are normalised.
    // The probe passes exactly those numbers in; defaulting to 1.0 here was the
    // reason NR's history never reprojected: every halo stayed nailed to the same
    // screen pixels while the camera moved. MVecScale= still overrides both axes.
    float const fallbackX = motionVectorScaleX > 0.0f ? motionVectorScaleX : static_cast<float>(w);
    float const fallbackY = motionVectorScaleY > 0.0f ? motionVectorScaleY : static_cast<float>(h);
    float const mvScaleX  = config::get().nr.mvecScale >= 0.0f ? config::get().nr.mvecScale : fallbackX;
    float const mvScaleY  = config::get().nr.mvecScale >= 0.0f ? config::get().nr.mvecScale : fallbackY;
    paramSetF32(params, "DLSSNR.MVecScaleX", mvScaleX);
    paramSetF32(params, "DLSSNR.MVecScaleY", mvScaleY);
    // The runtime's shared keys are unprefixed (measured from the engine's own
    // setup: MV.Scale.X=1485 / MV.Scale.Y=835, Width/Height/OutWidth/OutHeight).
    // Without them the feature sees a zero motion-vector scale, i.e. "the camera
    // never moves": history sticks to the screen (ghost clouds/silhouettes).
    unsigned const mvX = static_cast<unsigned>(
        config::get().nr.mvecScale >= 0.0f ? config::get().nr.mvecScale : static_cast<float>(guideWidth)
    );
    unsigned const mvY = static_cast<unsigned>(
        config::get().nr.mvecScale >= 0.0f ? config::get().nr.mvecScale : static_cast<float>(guideHeight)
    );
    paramSetU32(params, "MV.Scale.X", mvX);
    paramSetU32(params, "MV.Scale.Y", mvY);
    // Deliberately NOT touching Width/Height/OutWidth/OutHeight here: if this
    // parameter object is the one the engine's own SR also reads, writing our 1:1
    // guide size into OutWidth/OutHeight makes the engine's upscaler write a
    // 1485x835 patch into a 2560x1440 target - a permanently stale-looking picture.
    paramSetU32(params, "Width", guideWidth);
    paramSetU32(params, "Height", guideHeight);
    int style = config::get().nr.style >= 0 ? config::get().nr.style : 0;
    if (config::get().nr.sweep == 1) {
        style = (gWindowIndex >= 0 ? gWindowIndex : 0) % 3;
    }
    paramSetU32(params, "DLSSNR.Style", static_cast<std::uint32_t>(style));

    // Freeze test: with Enabled=0 the feature must not touch the output, so the
    // dump keeps the previous window's content. No stateless "filter" can do that.
    int enabled = 1;
    if (config::get().nr.sweep == 2) {
        enabled = ((gWindowIndex >= 0 ? gWindowIndex : 0) % 2 == 0) ? 1 : 0;
    }
    paramSetI32(params, "DLSSNR.Enabled", enabled);
    paramSetI32(params, "DLSSNR.UseAutoMask", 0);
    paramSetI32(params, "DLSSNR.UICorrection", 0);
    int const inverted = config::get().nr.depthInverted >= 0 ? config::get().nr.depthInverted : (depthInverted ? 1 : 0);
    paramSetI32(params, "DLSSNR.DepthInverted", inverted ? 1 : 0);
    paramSetI32(params, "DLSSNR.Reset", (reset || (config::get().nr.resetEveryFrame != 0)) ? 1 : 0);
    // 310.x reads both of these as int as well; 0 = keep the axis unflipped.
    paramSetI32(params, "DLSS.Indicator.Invert.X.Axis", 0);
    paramSetI32(params, "DLSS.Indicator.Invert.Y.Axis", 0);
    paramSetF32(params, "DLSSNR.Intensity", config::get().nr.intensity >= 0.0f ? config::get().nr.intensity : 1.0f);
    paramSetF32(
        params,
        "DLSSNR.LocalToneStrength",
        config::get().nr.localToneStrength >= 0.0f ? config::get().nr.localToneStrength : 1.0f
    );
    paramSetF32(
        params,
        "DLSSNR.LocalStructureStrength",
        config::get().nr.localStructureStrength >= 0.0f ? config::get().nr.localStructureStrength : 1.0f
    );
    paramSetF32(params, "DLSSNR.SkinStructureStrength", 1.0f);

#ifdef TEST
    static int loggedWindow = -2;
    if (loggedWindow != gWindowIndex) {
        loggedWindow = gWindowIndex;
        NR_DEBUG(
            "[nr] 本窗参数 Style=%d Enabled=%d Intensity=%.2f DepthInverted=%d Replace=%d "
            "NrEncode=%d NRReset=%d（窗口 #%d）",
            style,
            enabled,
            static_cast<double>(config::get().nr.intensity >= 0.0f ? config::get().nr.intensity : 1.0f),
            inverted,
            config::get().nr.replace,
            useEncoded
                ? (config::get().nr.nrEncode >= 0 ? config::get().nr.nrEncode : (config::get().nr.replace == 6 ? 3 : 0))
                : -1,
            config::get().nr.resetEveryFrame,
            gWindowIndex
        );
    }
#endif

    if (!gNr.resourceRoundtripChecked) {
        gNr.resourceRoundtripChecked = true;
        void* const expected         = useEncoded ? static_cast<void*>(gEncodedColor) : color;
        void* const readBack         = paramGetResource(params, "DLSSNR.Color");
        if (readBack == expected) {
            NR_DEBUG("[nr] 资源往返校验 DLSSNR.Color=%p 写入=%p ok", readBack, expected);
        } else {
            nrLog(1, "[nr] 资源往返校验 DLSSNR.Color=%p 写入=%p MISMATCH", readBack, expected);
        }
    }

    D3D12_RESOURCE_STATES const outputBefore =
        gNr.outputWasSrv ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;

    if (useEncoded) {
        // HDR → LDR encode into our own texture; the feature reads that instead.
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(
                static_cast<ID3D12Resource*>(color)
                    ->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))
            )
            && device) {
            transition(
                cmd,
                static_cast<ID3D12Resource*>(color),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            transition(
                cmd,
                gEncodedColor,
                gEncodedIsSrv ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
            writeDescriptors(
                device,
                static_cast<ID3D12Resource*>(color),
                static_cast<ID3D12Resource*>(color),
                static_cast<ID3D12Resource*>(color),
                gEncodedColor,
                0
            );
            device->Release();

            ID3D12DescriptorHeap* heaps[1] = {gEncodeHeap};
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(gEncodeRoot);
            cmd->SetPipelineState(gEncodePso);
            cmd->SetComputeRootDescriptorTable(0, gEncodeHeap->GetGPUDescriptorHandleForHeapStart());
            // Replace=6 feeds the network TRP's encoded proxy (sRGB + shoulder);
            // the other modes keep the earlier Reinhard encode for A/B. NrEncode
            // overrides both so the input domain can be tested on any mode.
            unsigned const encMode = config::get().nr.nrEncode >= 0 ? static_cast<unsigned>(config::get().nr.nrEncode)
                                                                    : (config::get().nr.replace == 6 ? 3u : 0u);
            setPassConstants(cmd, w, h, encMode, config::get().nr.whitePoint, 1.0f, 1.0f, config::get().nr.maxRatio);
            cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
            D3D12_RESOURCE_BARRIER uavBarrier{};
            uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.UAV.pResource = gEncodedColor;
            cmd->ResourceBarrier(1, &uavBarrier);
            transition(
                cmd,
                gEncodedColor,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            gEncodedIsSrv = true;
            transition(
                cmd,
                static_cast<ID3D12Resource*>(color),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
            );
        }
    } else {
        transition(
            cmd,
            static_cast<ID3D12Resource*>(color),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        );
    }

    // The composite and our SR run *after* our NR evaluate (see below): taking
    // last frame's NR result made our SR input one frame stale, which the SR then
    // re-projected with this frame's motion vectors -> a screen-space-fixed ghost.

    transition(
        cmd,
        static_cast<ID3D12Resource*>(depth),
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    transition(
        cmd,
        static_cast<ID3D12Resource*>(motionVectors),
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    transition(cmd, output, outputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    int const evaluateRet = gNr.evaluate(commandList, gNr.feature, params, nullptr, nullptr);
    if (evaluateRet == 1) {
        ++gNr.evalCount;
    } else {
        // Never silently accept a failed network evaluation: the output texture then
        // keeps whatever it held (a previous view), and the composite would inject
        // that stale frame into the current one.
        static int failed = 0;
        if (failed < 10) {
            ++failed;
            neurodragon::log::writeKey(
                2,
                "nr.error.evalFailed",
                static_cast<unsigned>(evaluateRet),
                getReason(static_cast<std::uint32_t>(evaluateRet)),
                failed
            );
        }
    }

    transition(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (!useEncoded) {
        transition(
            cmd,
            static_cast<ID3D12Resource*>(color),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
        );
    }
    transition(
        cmd,
        static_cast<ID3D12Resource*>(depth),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
    );
    transition(
        cmd,
        static_cast<ID3D12Resource*>(motionVectors),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
    );
    gNr.outputWasSrv = true;

    // Same frame: composite this frame's NR result onto this frame's Color, then
    // let the engine's own DLSS upscale it.
    if (engineParams && config::get().nr.replace == 5) {
        // The engine never resets its own DLSS when a world loads, so its history
        // keeps whatever was on screen when the previous world was left - the
        // "ghost that survives a save reload and shows the first frame of the
        // level" symptom. Reset is a one-shot key on the engine's parameter object
        // (no DLSS. prefix); leaving it at 1 would wipe the history every frame.
        static bool clearEngineReset = false;
        if (gEngineResetPending.exchange(false, std::memory_order_relaxed)) {
            paramSetI32(static_cast<void*>(engineParams), "Reset", 1);
            clearEngineReset = true;
            NR_DEBUG("[nr] 进存档：给引擎 DLSS 发一次 Reset=1");
        } else if (clearEngineReset) {
            clearEngineReset = false;
            paramSetI32(static_cast<void*>(engineParams), "Reset", 0);
        }
        // SRReset=1: wipe the engine's history every frame (diagnostic only).
        // Must not run when the switch is off: it would overwrite the one-shot
        // Reset=1 above with 0 before the engine ever sees it.
        if (config::get().nr.srResetEveryFrame != 0) {
            paramSetI32(static_cast<void*>(engineParams), "Reset", 1);
        }
    }
    if (engineParams && config::get().nr.replace == 5 && gEncodePso && gEncodedColor && gDecodedColor
        && gNr.evalCount > 2) {
        compositeGain(
            cmd,
            static_cast<ID3D12Resource*>(color),
            useEncoded ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            engineParams,
            w,
            h
        );
    }
    static unsigned calls = 0;
    ++calls;
    if (evaluateRet != 1 && calls < 8) {
        neurodragon::log::writeKey(
            2,
            "nr.error.engineEvalFailed",
            static_cast<unsigned>(evaluateRet),
            getReason(static_cast<std::uint32_t>(evaluateRet)),
            calls
        );
    }
    return evaluateRet == 1;
}

void nrEnsureStamps(void* commandList) {
    if (gStamps.active || !commandList) {
        return;
    }
    auto*         cmd    = static_cast<ID3D12GraphicsCommandList*>(commandList);
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
        return;
    }
    ensureStamps(device);
    device->Release();
}

void nrStampBegin(void* commandList, std::uint64_t frame, int id) {
    if (!gStamps.active || !commandList || id < 0 || id >= kStampKinds) {
        return;
    }
    auto*     cmd   = static_cast<ID3D12GraphicsCommandList*>(commandList);
    int const slot  = static_cast<int>(frame % kStampSlots);
    int const index = slot * kStampPerSlot + id * 2;
    cmd->EndQuery(gStamps.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index));
}

void nrStampEnd(void* commandList, std::uint64_t frame, int id) {
    if (!gStamps.active || !commandList || id < 0 || id >= kStampKinds) {
        return;
    }
    auto*     cmd   = static_cast<ID3D12GraphicsCommandList*>(commandList);
    int const slot  = static_cast<int>(frame % kStampSlots);
    int const index = slot * kStampPerSlot + id * 2;
    cmd->EndQuery(gStamps.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index + 1));
    cmd->ResolveQueryData(
        gStamps.heap,
        D3D12_QUERY_TYPE_TIMESTAMP,
        static_cast<UINT>(index),
        2,
        gStamps.readback,
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
            int const           index = slot * kStampPerSlot + id * 2;
            std::uint64_t const begin = gStamps.mapped[index];
            std::uint64_t const end   = gStamps.mapped[index + 1];
            bool                valid = end > begin && begin != 0 && end != 0;
            double              ms    = 0.0;
            if (valid) {
                ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(gStamps.frequency);
                // A frame pair never legitimately exceeds a fifth of a second,
                // and never finishes in under 20 microseconds.
                valid = ms > 0.02 && ms < 200.0;
            }
            if (valid) {
                gStamps.samples[id].push_back(ms);
                // Rolling window: in Replace mode the panel wants live numbers, so
                // the vectors are no longer drained at every window boundary.
                constexpr size_t kMaxSamples = 240;
                if (gStamps.samples[id].size() > kMaxSamples) {
                    gStamps.samples[id].erase(gStamps.samples[id].begin());
                }
            } else {
                ++gStamps.dropped[id];
            }
        }
        gStamps.pending[slot] = 0;
    }

    // Publish a few times per second rather than once per measurement window, so
    // the panel's GPU row tracks the picture instead of a number from 30 s ago.
    if (frame % 30 == 0) {
        publishMedian(0);
        publishMedian(1);
    }
}

void nrStampReport() {
    if (!gStamps.active) {
        return;
    }
    reportOne(0, "nr");
    reportOne(1, "engine-dlss");
}

void nrStampReset() {
    // Only the raw samples: the published medians are what the panel shows between
    // windows, and clearing them here used to blank the panel right after every
    // measurement window ended.
    for (int id = 0; id < kStampKinds; ++id) {
        gStamps.samples[id].clear();
        gStamps.dropped[id]  = 0;
        gStamps.recorded[id] = 0;
    }
}

void nrStampClearReported() {
    for (int id = 0; id < kStampKinds; ++id) {
        gStampMedianMs[id].store(0.0, std::memory_order_relaxed);
        gStampMedianCount[id].store(0, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Engine SR (DLSS feature 1): a read-only view of the engine's own upscaling.
//
// The engine sizes its render targets by calling the callback stored under
// "DLSSOptimalSettingsCallback" (docs/I §10.3). Wrapping that pointer is the
// cheapest observation point - and, later, the lever for an arbitrary R_in.
// The wrapper below forwards the answer unchanged: it records it, and in a TEST
// build logs it. No value is modified, so the picture cannot change.
// ---------------------------------------------------------------------------
namespace {

using SrOptimalFn = int (*)(void*);

std::atomic<SrOptimalFn> gSrOriginal{nullptr};
std::atomic<unsigned>    gSrCallbackCount{0};

// Forced feature rebuild (Apply with a new model preset). Phase 1 asks the engine
// for a different quality slot, phase 2 restores the mapped one; while either is
// active the sizing callback keeps the render size at the current value, so the
// feature is recreated without moving the resolution (2026-10-24).
std::atomic<unsigned> gSrRebuildPhase{0};
std::atomic<unsigned> gSrRebuildTicks{0};
// Readout hold: after the rebuild phases end the engine can still be using the
// temporary slot for a few frames while it settles, which made the read-only
// section flash. Keep reporting the stable slot until the hold expires.
std::atomic<unsigned> gSrReadoutHold{0};
// Render size captured when the rebuild is requested; the sizing callback pins
// it while the feature is recreated, so the image does not resize.
std::atomic<unsigned> gSrRebuildPinW{0};
std::atomic<unsigned> gSrRebuildPinH{0};
std::atomic<unsigned>    gSrDisplayW{0};
std::atomic<unsigned>    gSrDisplayH{0};
std::atomic<unsigned>    gSrRenderW{0};
std::atomic<unsigned>    gSrRenderH{0};
std::atomic<unsigned>    gSrQuality{0};
std::atomic<unsigned>    gSrSharpBits{0};
std::atomic<unsigned>    gSrDynMinW{0};
std::atomic<unsigned>    gSrDynMinH{0};
std::atomic<unsigned>    gSrDynMaxW{0};
std::atomic<unsigned>    gSrDynMaxH{0};
std::atomic_bool         gSrResetPending{false};

std::atomic<unsigned> gEngineQuality{0};
std::atomic<unsigned> gEngineFlags{0};
std::atomic<unsigned> gEnginePreset{0};
std::atomic<unsigned> gEngineSharpBits{0};

// Engine upscaling configuration (see srDriveFrameContext). Every value is
// measured from the engine's own objects, never inferred.
std::atomic<bool>     gSrEngValid{false};
std::atomic<unsigned> gSrEngCalls{0};
std::atomic<unsigned> gSrEngRejects{0};
std::atomic<unsigned> gSrCtxMode{0};
std::atomic<unsigned> gSrCtxQuality{0};
std::atomic<unsigned> gSrCtxBools{0};
std::atomic<unsigned> gSrCtxFactorBits{0};
std::atomic<unsigned> gSrCtxExtraBits{0};
std::atomic<unsigned> gSrActiveQ{0};
std::atomic<unsigned> gSrRequestedQ{0};
std::atomic<unsigned> gSrActiveFBits{0};
std::atomic<unsigned> gSrRequestedFBits{0};
std::atomic<unsigned> gSrEngRenderW{0};
std::atomic<unsigned> gSrEngRenderH{0};
std::atomic<unsigned> gSrTailFactorBits{0};
// The renderer object the engine hands to the per-frame copy (base+0xdfc53c0).
// Captured on the first call so the per-evaluate request write can target the
// same object without having to re-enter the rebuild path.
std::atomic<void*> gSrEngineObject{nullptr};

#if defined(TEST)
std::atomic<int> gSrLogBudget{200};
#endif

unsigned floatBits(float value) {
    unsigned bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(unsigned bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

int srOptimalCallback(void* params) {
    SrOptimalFn const original = gSrOriginal.load(std::memory_order_relaxed);
    int const         ret      = original ? original(params) : 0;
    if (!params) {
        return ret;
    }
    unsigned const displayW = paramGetU32(params, "Width");
    unsigned const displayH = paramGetU32(params, "Height");
    unsigned const quality  = paramGetU32(params, "PerfQualityValue");
    unsigned       renderW  = paramGetU32(params, "OutWidth");
    unsigned       renderH  = paramGetU32(params, "OutHeight");
    unsigned const minW     = paramGetU32(params, "DLSS.Get.Dynamic.Min.Render.Width");
    unsigned const minH     = paramGetU32(params, "DLSS.Get.Dynamic.Min.Render.Height");
    unsigned const maxW     = paramGetU32(params, "DLSS.Get.Dynamic.Max.Render.Width");
    unsigned const maxH     = paramGetU32(params, "DLSS.Get.Dynamic.Max.Render.Height");
    float const    sharp    = paramGetF32(params, "Sharpness");

    // Forced rebuild: while one is in flight the engine is asked for a different
    // quality slot, so pin the size (and the dynamic range) to the size captured
    // at request time. Otherwise the temporary slot's range clamps it (q=3 pins
    // 853, q=5 clamps to 2534; 2026-10-24).
    if (gSrRebuildPhase.load(std::memory_order_relaxed) != 0) {
        unsigned const pinW = gSrRebuildPinW.load(std::memory_order_relaxed);
        unsigned const pinH = gSrRebuildPinH.load(std::memory_order_relaxed);
        if (pinW != 0 && pinH != 0) {
            paramSetU32(params, "DLSS.Get.Dynamic.Min.Render.Width", pinW);
            paramSetU32(params, "DLSS.Get.Dynamic.Min.Render.Height", pinH);
            paramSetU32(params, "DLSS.Get.Dynamic.Max.Render.Width", pinW);
            paramSetU32(params, "DLSS.Get.Dynamic.Max.Render.Height", pinH);
            paramSetU32(params, "OutWidth", pinW);
            paramSetU32(params, "OutHeight", pinH);
            renderW = pinW;
            renderH = pinH;
#if defined(TEST)
            nrLog(0, "[sr] rebuild size pinned to %ux%u", pinW, pinH);
#endif
        }
    }

    unsigned const n = gSrCallbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
    gSrDisplayW.store(displayW, std::memory_order_relaxed);
    gSrDisplayH.store(displayH, std::memory_order_relaxed);
    gSrRenderW.store(renderW, std::memory_order_relaxed);
    gSrRenderH.store(renderH, std::memory_order_relaxed);
    gSrQuality.store(quality, std::memory_order_relaxed);
    gSrSharpBits.store(floatBits(sharp), std::memory_order_relaxed);
    gSrDynMinW.store(minW, std::memory_order_relaxed);
    gSrDynMinH.store(minH, std::memory_order_relaxed);
    gSrDynMaxW.store(maxW, std::memory_order_relaxed);
    gSrDynMaxH.store(maxH, std::memory_order_relaxed);

#if defined(TEST)
    if (gSrLogBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        // Who calls the engine's sizing function? The backtrace answers it without
        // hooking an unknown-signature function.
        void*          frames[6]{};
        USHORT const   depth   = ::CaptureStackBackTrace(2, 6, frames, nullptr);
        std::uintptr_t exeBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        char           stack[256]{};
        int            used = 0;
        for (USHORT i = 0; i < depth && used < 200; ++i) {
            used += std::snprintf(
                stack + used,
                sizeof(stack) - static_cast<std::size_t>(used),
                " exe+%llx",
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(frames[i]) - exeBase
                )
            );
        }
        nrLog(0, "[sr] cb stack:%s", stack);
        nrLog(
            0,
            "[sr] cb #%u display=%ux%u q=%u -> render=%ux%u sharp=%.3f dyn[%ux%u..%ux%u] ret=0x%08X",
            n,
            displayW,
            displayH,
            quality,
            renderW,
            renderH,
            static_cast<double>(sharp),
            minW,
            minH,
            maxW,
            maxH,
            static_cast<unsigned>(ret)
        );
    }
#else
    (void)n;
#endif
    return ret;
}

} // namespace

float srFactorForPercent(int percent) {
    switch (percent) {
    case 33:
        return 0.3333f; // matches the engine's 853x480 at 2560x1440
    case 50:
        return 0.5f;
    case 58:
        return 0.58f;
    case 66:
        return 0.6667f; // matches the engine's 1707x960
    case 100:
        return 1.0f;
    default:
        return 0.0f;
    }
}

int srQualityForPercent(int percent) {
    switch (percent) {
    case 33:
        return 3; // UltraPerformance
    case 50:
        return 0; // Performance
    case 58:
        return 1; // Balanced
    case 66:
        return 2; // Quality
    case 100:
        return 5; // DLAA
    default:
        return -1;
    }
}

// The engine's CreateFeature parameter object is the last place the DLSS model
// preset can be set. TRP wrote all six per-quality hints the same way; the engine
// marks the value with 0x80000000, so that bit is preserved. 0 is written too:
// the object is reused across feature builds, so skipping it would leave a
// previously chosen preset in place instead of returning to the driver default.
void srApplyCreateOverrides(void* params, int feature) {
    if (!params || feature != 1) { // 1 = NVSDK_NGX_Feature_SuperSampling (DLSS)
        return;
    }
    static char const* const keys[] = {
        "DLSS.Hint.Render.Preset.DLAA",
        "DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance",
        "DLSS.Hint.Render.Preset.UltraQuality",
    };
    unsigned const preset = static_cast<unsigned>(config::get().sr.preset);
    unsigned const old    = paramGetU32(params, "DLSS.Hint.Render.Preset.Quality");
    for (char const* key : keys) {
        unsigned const current = paramGetU32(params, key);
        paramSetU32(params, key, (current & 0x80000000u) | preset);
    }
#if defined(TEST)
    nrLog(0, "[sr] create preset=%u (hint 0x%x -> 0x%x)", preset, old, (old & 0x80000000u) | preset);
#endif
}

void srInstallCallback(void* params) {
    if (!params) {
        return;
    }
    void* current = paramGetResource(params, "DLSSOptimalSettingsCallback");
    if (!current || current == reinterpret_cast<void*>(&srOptimalCallback)) {
        return;
    }
    // The first pointer seen is the runtime's own function (nvngx_dlss.dll). Any
    // later object carries either the same one or our wrapper, which is skipped
    // above; the saved copy is the only callable original.
    SrOptimalFn expected = nullptr;
    (void)gSrOriginal.compare_exchange_strong(expected, reinterpret_cast<SrOptimalFn>(current));
    paramSetPtr(params, "DLSSOptimalSettingsCallback", reinterpret_cast<void*>(&srOptimalCallback));
    NR_DEBUG("[sr] wrapped optimal-settings callback params=%p orig=%p", params, current);
}

void srReadEngineParams(void* params) {
    if (!params) {
        return;
    }
    gEngineQuality.store(paramGetU32(params, "PerfQualityValue"), std::memory_order_relaxed);
    gEngineFlags.store(paramGetU32(params, "DLSS.Feature.Create.Flags"), std::memory_order_relaxed);
    gEnginePreset.store(paramGetU32(params, "DLSS.Hint.Render.Preset.Quality"), std::memory_order_relaxed);
    gEngineSharpBits.store(floatBits(paramGetF32(params, "Sharpness")), std::memory_order_relaxed);

    // One-shot history wipe. The engine never clears this key itself, so leaving
    // it at 1 would reset every frame; writing 0 again on the next frame restores
    // the engine's own "no reset" state.
    static bool restoreReset = false;
    if (gSrResetPending.exchange(false, std::memory_order_relaxed)) {
        paramSetI32(params, "Reset", 1);
        restoreReset = true;
        NR_DEBUG("[sr] manual reset: Reset=1 for one frame");
    } else if (restoreReset) {
        restoreReset = false;
        paramSetI32(params, "Reset", 0);
    }
}

void srRequestReset() { gSrResetPending.store(true, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Engine upscaling configuration: measured, and optionally overridden.
//
// The engine copies the frame's upscaling parameters into the renderer once per
// frame (base+0xdfc53c0). The context layout is:
//   +0xe806d8 mode (int)   +0xe806dc quality (int)
//   +0xe806e0 flags (2+2 bytes)   +0xe806e4 factor (float)
//   +0xe8083c second factor (float)
// and the renderer mirrors them at +0x3c56424..30, with the render size at
// +0x3c56434/38. The quality slot is what the engine feeds both into the
// optimal-settings callback and into its own world render rect, so it is the
// only lever that can move the world above the 58% Balanced factor (docs/I §11).
//
// Writes happen only when config.sr.qualityOverride / factorOverride are set;
// with both 0 this function is a pure read-out.
// ---------------------------------------------------------------------------
namespace {

constexpr std::size_t kCtxMode      = 0xe806d8;
constexpr std::size_t kCtxQuality   = 0xe806dc;
constexpr std::size_t kCtxFlags     = 0xe806e0;
constexpr std::size_t kCtxFactor    = 0xe806e4;
constexpr std::size_t kCtxExtra     = 0xe8083c;

constexpr std::size_t kEngActiveQ   = 0x3c56418;
constexpr std::size_t kEngActiveF   = 0x3c56420;
constexpr std::size_t kEngRequestQ  = 0x3c56428;
constexpr std::size_t kEngRequestF  = 0x3c56430;
constexpr std::size_t kEngRenderW   = 0x3c56434;
constexpr std::size_t kEngRenderH   = 0x3c56438;
constexpr std::size_t kEngTailF     = 0x3c5644c;

unsigned loadU32(void* base, std::size_t offset) {
    unsigned value = 0;
    std::memcpy(&value, static_cast<std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

float loadF32(void* base, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, static_cast<std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

void storeU32(void* base, std::size_t offset, unsigned value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

void storeF32(void* base, std::size_t offset, float value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

// The engine uses -1 as the "no explicit override / Automatic" sentinel for
// these factors, so the check has to accept it: rejecting -1 made the first
// version skip every call, including the world load (2026-10-23 log).
bool plausibleFactor(float value) { return value > -2.0f && value < 4.0f; }

// The slot the engine should use for the chosen percentage. Every offered ratio
// now has its own native DLSS slot (33/50/58/66/100 -> 3/0/1/2/5), so this is a
// pure mapping: no slot has to be borrowed and no forced rebuild is needed. Both
// writers (the per-frame context copy and the per-evaluate request) use it.
unsigned chosenQuality() {
    int const pct = config::get().sr.scalePercent;
    if (pct <= 0) {
        return 0xFFFFFFFFu;
    }
    int const mapped = srQualityForPercent(pct);
    return mapped < 0 ? 0xFFFFFFFFu : static_cast<unsigned>(mapped);
}

// Quality to write during the current frame: a forced rebuild asks for a
// different slot in phase 1 and the mapped one in phase 2. The ctx writer is the
// one that reaches the frame builder; the per-evaluate writer only mirrors it.
unsigned currentQuality(unsigned wantedQ) {
    unsigned const phase = gSrRebuildPhase.load(std::memory_order_relaxed);
    if (phase == 0) {
        return wantedQ;
    }
    return (phase == 1) ? ((wantedQ == 2u) ? 1u : 2u) : wantedQ;
}

} // namespace

void srDriveFrameContext(void* engine, void* ctx) {
    if (!engine || !ctx) {
        return;
    }
    unsigned const calls = gSrEngCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    unsigned const ctxMode    = loadU32(ctx, kCtxMode);
    unsigned const ctxQuality = loadU32(ctx, kCtxQuality);
    unsigned const ctxFlags   = loadU32(ctx, kCtxFlags);
    float const    ctxFactor  = loadF32(ctx, kCtxFactor);
    float const    ctxExtra   = loadF32(ctx, kCtxExtra);

    unsigned const activeQ    = loadU32(engine, kEngActiveQ);
    unsigned const requestedQ = loadU32(engine, kEngRequestQ);
    unsigned const renderW    = loadU32(engine, kEngRenderW);
    unsigned const renderH    = loadU32(engine, kEngRenderH);
    float const    activeF    = loadF32(engine, kEngActiveF);
    float const    requestedF = loadF32(engine, kEngRequestF);
    float const    tailF      = loadF32(engine, kEngTailF);

    // Sanity gate: only the fields this hook writes (quality) and reads for the
    // decision (active factor) have to look like the layout it was built
    // against. The rest is published, not trusted: rejecting on them made the
    // first version silently do nothing (2026-10-23).
    bool const valid = ctxMode <= 8 && ctxQuality <= 8 && activeQ <= 8 && plausibleFactor(activeF) && renderW <= 16384
                    && renderH <= 16384;

    gSrEngValid.store(valid, std::memory_order_relaxed);
    gSrEngineObject.store(engine, std::memory_order_relaxed);
    if (calls <= 10) {
        nrLog(
            0,
            "[sr] ctx call #%u engine=%p ctx=%p mode=%u q=%u flags=0x%x f=%.3f extra=%.3f | aq=%u rq=%u af=%.3f "
            "rf=%.3f render=%ux%u tail=%.3f valid=%d",
            calls,
            engine,
            ctx,
            ctxMode,
            ctxQuality,
            ctxFlags,
            static_cast<double>(ctxFactor),
            static_cast<double>(ctxExtra),
            activeQ,
            requestedQ,
            static_cast<double>(activeF),
            static_cast<double>(requestedF),
            renderW,
            renderH,
            static_cast<double>(tailF),
            valid ? 1 : 0
        );
    }
    if (!valid) {
        if (gSrEngRejects.fetch_add(1, std::memory_order_relaxed) < 3) {
            nrLog(
                0,
                "[sr] ctx layout rejected (mode=%u q=%u aq=%u rq=%u f=%.3f af=%.3f rf=%.3f tail=%.3f rw=%u rh=%u)",
                ctxMode,
                ctxQuality,
                activeQ,
                requestedQ,
                static_cast<double>(ctxFactor),
                static_cast<double>(activeF),
                static_cast<double>(requestedF),
                static_cast<double>(tailF),
                renderW,
                renderH
            );
        }
        return;
    }


    // Only touch the quality slot once the engine has already created its DLSS
    // feature (the callback wrapper is installed by CreateFeature). Writing a
    // non-default q before that makes the engine skip DLSS entirely - observed
    // 2026-10-23: config q=5 at startup -> no CreateFeature, no EvaluateFeature,
    // so NR never ran. After creation the same write is what drives the rebuild.
    bool const engineHasDlss = gSrOriginal.load(std::memory_order_relaxed) != nullptr;

    unsigned newQuality = ctxQuality;
    unsigned const wantedQ = chosenQuality();
    unsigned const phase   = gSrRebuildPhase.load(std::memory_order_relaxed);
    bool const     holdReadout = phase != 0 || gSrReadoutHold.load(std::memory_order_relaxed) != 0;
    // A forced rebuild also has to work while "follow the engine" is selected:
    // there is no mapped slot then, so the engine's own ctx quality is the base.
    unsigned const baseQ   = (wantedQ != 0xFFFFFFFFu) ? wantedQ : ctxQuality;
    bool const     canWrite = engineHasDlss && (wantedQ != 0xFFFFFFFFu || phase != 0);
    if (canWrite) {
        newQuality = currentQuality(baseQ);
        // Advance the forced-rebuild state machine here: this hook runs once per
        // frame and its ctx write is what the shim copies into the renderer; the
        // per-evaluate writer must not advance it a second time.
        if (phase != 0) {
            unsigned const ticks = gSrRebuildTicks.fetch_add(1, std::memory_order_relaxed) + 1;
            if (phase == 1) {
                if (ticks >= 6) {
                    gSrRebuildPhase.store(2, std::memory_order_relaxed);
                    gSrRebuildTicks.store(0, std::memory_order_relaxed);
                }
            } else if (ticks >= 6) {
                gSrRebuildPhase.store(0, std::memory_order_relaxed);
            }
        }
        if (newQuality != ctxQuality) {
            storeU32(ctx, kCtxQuality, newQuality);
        }
    }

    // The engine really is using the temporary slot for a few frames; report the
    // stable one so the read-only section does not flash another slot. The size
    // and the model are unchanged, only the slot used to force the recreate.
    unsigned const readoutQ = (holdReadout && canWrite) ? baseQ : newQuality;
    if (gSrReadoutHold.load(std::memory_order_relaxed) != 0) {
        gSrReadoutHold.fetch_sub(1, std::memory_order_relaxed);
    }

#if defined(TEST)
    if (newQuality != ctxQuality) {
        static std::uint64_t lastLog = 0;
        auto const          now     = GetTickCount64();
        if (now - lastLog > 1000) {
            lastLog = now;
            nrLog(
                0,
                "[sr] ctx write q=%u->%u (engine aq=%u af=%.3f render=%ux%u)",
                ctxQuality,
                newQuality,
                activeQ,
                static_cast<double>(activeF),
                renderW,
                renderH
            );
        }
    }
    if (calls <= 5 || calls % 600 == 0) {
        nrLog(
            0,
            "[sr] ctx #%u q=%u f=%.3f extra=%.3f flags=0x%x | engine aq=%u rq=%u af=%.3f rf=%.3f render=%ux%u tail=%.3f",
            calls,
            newQuality,
            static_cast<double>(ctxFactor),
            static_cast<double>(ctxExtra),
            ctxFlags,
            activeQ,
            requestedQ,
            static_cast<double>(activeF),
            static_cast<double>(requestedF),
            renderW,
            renderH,
            static_cast<double>(tailF)
        );
    }
#else
    (void)calls;
    (void)ctxFlags;
    (void)ctxExtra;
#endif

    gSrCtxMode.store(ctxMode, std::memory_order_relaxed);
    gSrCtxQuality.store(readoutQ, std::memory_order_relaxed);
    gSrCtxBools.store(ctxFlags, std::memory_order_relaxed);
    gSrCtxFactorBits.store(floatBits(ctxFactor), std::memory_order_relaxed);
    gSrCtxExtraBits.store(floatBits(ctxExtra), std::memory_order_relaxed);
    gSrActiveQ.store(holdReadout ? baseQ : activeQ, std::memory_order_relaxed);
    gSrRequestedQ.store(holdReadout ? baseQ : requestedQ, std::memory_order_relaxed);
    gSrActiveFBits.store(floatBits(activeF), std::memory_order_relaxed);
    gSrRequestedFBits.store(floatBits(requestedF), std::memory_order_relaxed);
    gSrEngRenderW.store(renderW, std::memory_order_relaxed);
    gSrEngRenderH.store(renderH, std::memory_order_relaxed);
    gSrTailFactorBits.store(floatBits(tailF), std::memory_order_relaxed);
}

void srRequestFeatureRebuild() {
    // Capture the size the engine is rendering right now; the sizing callback
    // pins it while the feature is recreated (works for both a mapped slot and
    // "follow the engine").
    gSrRebuildPinW.store(gSrEngRenderW.load(std::memory_order_relaxed), std::memory_order_relaxed);
    gSrRebuildPinH.store(gSrEngRenderH.load(std::memory_order_relaxed), std::memory_order_relaxed);
    gSrRebuildTicks.store(0, std::memory_order_relaxed);
    gSrRebuildPhase.store(1, std::memory_order_relaxed);
    gSrReadoutHold.store(90, std::memory_order_relaxed);
    NR_DEBUG("[sr] feature rebuild requested");
}

void srDriveEngineRequest() {
    void* engine = gSrEngineObject.load(std::memory_order_relaxed);
    if (!engine) {
        return;
    }
    unsigned const wantedQ = chosenQuality();
    if (wantedQ == 0xFFFFFFFFu) {
        return;
    }

    // Mirror the ctx writer. The shim copies the ctx quality into the renderer
    // every frame, so the phase machine lives in srDriveFrameContext; writing the
    // renderer field directly here would be overwritten before the frame builder
    // reads it (this is why the first rebuild attempt did nothing, 2026-10-24).
    unsigned const q = currentQuality(wantedQ);

    unsigned const activeQ    = loadU32(engine, kEngActiveQ);
    unsigned const requestedQ = loadU32(engine, kEngRequestQ);
    float const    activeF    = loadF32(engine, kEngActiveF);
    float const    requestedF = loadF32(engine, kEngRequestF);
    if (activeQ > 8 || requestedQ > 8 || !plausibleFactor(activeF) || !plausibleFactor(requestedF)) {
        return;
    }

    bool     changed = false;
    unsigned newQ    = requestedQ;
    if (q != requestedQ) {
        newQ = q;
        storeU32(engine, kEngRequestQ, newQ);
        changed = true;
    }

    if (changed) {
        static std::uint64_t lastLog = 0;
        auto const          now     = GetTickCount64();
        if (now - lastLog > 1000) {
            lastLog = now;
            nrLog(
                0,
                "[sr] req q=%u->%u (active q=%u f=%.3f)",
                requestedQ,
                newQ,
                activeQ,
                static_cast<double>(activeF)
            );
        }
    }
}

SrEngineStats srEngineStats() {
    SrEngineStats stats;
    stats.valid      = gSrEngValid.load(std::memory_order_relaxed);
    stats.calls      = gSrEngCalls.load(std::memory_order_relaxed);
    stats.ctxMode    = gSrCtxMode.load(std::memory_order_relaxed);
    stats.ctxQuality = gSrCtxQuality.load(std::memory_order_relaxed);
    stats.ctxBools   = gSrCtxBools.load(std::memory_order_relaxed);
    stats.ctxFactor  = bitsFloat(gSrCtxFactorBits.load(std::memory_order_relaxed));
    stats.ctxExtra   = bitsFloat(gSrCtxExtraBits.load(std::memory_order_relaxed));
    stats.activeQ    = gSrActiveQ.load(std::memory_order_relaxed);
    stats.requestedQ = gSrRequestedQ.load(std::memory_order_relaxed);
    stats.activeF    = bitsFloat(gSrActiveFBits.load(std::memory_order_relaxed));
    stats.requestedF = bitsFloat(gSrRequestedFBits.load(std::memory_order_relaxed));
    stats.renderW    = gSrEngRenderW.load(std::memory_order_relaxed);
    stats.renderH    = gSrEngRenderH.load(std::memory_order_relaxed);
    stats.tailFactor = bitsFloat(gSrTailFactorBits.load(std::memory_order_relaxed));
    return stats;
}

SrStats srPanelStats() {
    SrStats stats;
    stats.wrapped       = gSrOriginal.load(std::memory_order_relaxed) != nullptr;
    stats.callbackCount = gSrCallbackCount.load(std::memory_order_relaxed);
    stats.cbDisplayW    = gSrDisplayW.load(std::memory_order_relaxed);
    stats.cbDisplayH    = gSrDisplayH.load(std::memory_order_relaxed);
    stats.cbRenderW     = gSrRenderW.load(std::memory_order_relaxed);
    stats.cbRenderH     = gSrRenderH.load(std::memory_order_relaxed);
    stats.cbQuality     = gSrQuality.load(std::memory_order_relaxed);
    stats.cbSharpness   = bitsFloat(gSrSharpBits.load(std::memory_order_relaxed));
    stats.dynMinW       = gSrDynMinW.load(std::memory_order_relaxed);
    stats.dynMinH       = gSrDynMinH.load(std::memory_order_relaxed);
    stats.dynMaxW       = gSrDynMaxW.load(std::memory_order_relaxed);
    stats.dynMaxH       = gSrDynMaxH.load(std::memory_order_relaxed);
    stats.quality       = gEngineQuality.load(std::memory_order_relaxed);
    stats.flags         = gEngineFlags.load(std::memory_order_relaxed);
    stats.preset        = gEnginePreset.load(std::memory_order_relaxed);
    stats.sharpness     = bitsFloat(gEngineSharpBits.load(std::memory_order_relaxed));
    return stats;
}

PanelStats nrPanelStats() {
    PanelStats stats;
    stats.featureCreated = gNr.created;
    stats.stampsActive   = gStamps.active;
    stats.evalCount      = gNr.evalCount;
    stats.lastError      = gNr.lastError;
    // Sizes come from the engine's own frame (published every evaluation), so the
    // monitor keeps showing them with NR off or unloaded; the feature's numbers are
    // only the fallback for the frames before the first publication.
    stats.renderWidth    = gEngineFrame.renderWidth ? gEngineFrame.renderWidth : gNr.width;
    stats.renderHeight   = gEngineFrame.renderHeight ? gEngineFrame.renderHeight : gNr.height;
    stats.outputWidth    = gEngineFrame.outputWidth ? gEngineFrame.outputWidth : gNr.engineOutputWidth;
    stats.outputHeight   = gEngineFrame.outputHeight ? gEngineFrame.outputHeight : gNr.engineOutputHeight;

    stats.nrSamples     = gStampMedianCount[0].load(std::memory_order_relaxed);
    stats.engineSamples = gStampMedianCount[1].load(std::memory_order_relaxed);
    if (stats.nrSamples != 0) {
        stats.nrMs  = gStampMedianMs[0].load(std::memory_order_relaxed);
        stats.hasNr = true;
    }
    if (stats.engineSamples != 0) {
        stats.engineMs  = gStampMedianMs[1].load(std::memory_order_relaxed);
        stats.hasEngine = true;
    }
    return stats;
}

} // namespace neurodragon::nr
