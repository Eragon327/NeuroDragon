#include "neurodragon/probe/VvCalibrate.h"

#include <MinHook.h>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <cstring>

namespace neurodragon::probe::vv {
namespace {

constexpr unsigned kFirstSlot = 9;
constexpr unsigned kSlotCount = 32; // slots 9..40

std::atomic<int> gLastSlot{-1};

// One recording detour per slot: remember which slot fired, then forward exactly the
// same arguments. Eight value slots cover every method in the range (the widest,
// CopyTiles, takes seven), and the return value is preserved in RAX as well, so a
// caller that checks an HRESULT still sees the real one.
template <unsigned Slot>
struct Probe {
    static inline void* original = nullptr;

    static std::uintptr_t detour(
        void*          self,
        std::uintptr_t a1,
        std::uintptr_t a2,
        std::uintptr_t a3,
        std::uintptr_t a4,
        std::uintptr_t a5,
        std::uintptr_t a6,
        std::uintptr_t a7,
        std::uintptr_t a8
    ) {
        if (gLastSlot.load(std::memory_order_relaxed) < 0) {
            gLastSlot.store(static_cast<int>(Slot), std::memory_order_relaxed);
        }
        using Fn = std::uintptr_t (*)(
            void*,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t,
            std::uintptr_t
        );
        return reinterpret_cast<Fn>(original)(self, a1, a2, a3, a4, a5, a6, a7, a8);
    }
};

template <std::size_t... I>
void fillTables(std::index_sequence<I...>, std::array<void*, kSlotCount>& detours, std::array<void**, kSlotCount>& originals) {
    ((detours[I] = reinterpret_cast<void*>(&Probe<kFirstSlot + I>::detour),
      originals[I] = reinterpret_cast<void**>(&Probe<kFirstSlot + I>::original)),
     ...);
}

} // namespace

unsigned rtvDescriptorIncrement(void* deviceVoid) {
    if (!deviceVoid) {
        return 0;
    }
    auto* const device = static_cast<ID3D12Device*>(deviceVoid);
    return device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

using SlotReporter = void (*)(char const*, int);

// Kept for reference only - see the note in VvCalibrate.h. Nothing calls it.
[[maybe_unused]] bool calibrateCommandListSlots(void* deviceVoid, SlotReporter report) {
    if (!deviceVoid || !report) {
        return false;
    }
    auto* const device = static_cast<ID3D12Device*>(deviceVoid);

    ID3D12CommandAllocator* allocator = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        || !allocator) {
        return false;
    }
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT const              created =
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    allocator->Release();
    if (FAILED(created) || !list) {
        return false;
    }

    std::array<void*, kSlotCount>  detours{};
    std::array<void**, kSlotCount> originals{};
    fillTables(std::make_index_sequence<kSlotCount>{}, detours, originals);

    auto** vtbl = *reinterpret_cast<void***>(list);
    for (unsigned i = 0; i < kSlotCount; ++i) {
        auto* const target = vtbl[kFirstSlot + i];
        if (target && MH_CreateHook(target, detours[i], originals[i]) == MH_OK) {
            MH_EnableHook(target);
        }
    }

    auto probe = [report](char const* name, auto&& call) {
        gLastSlot.store(-1, std::memory_order_relaxed);
        call();
        report(name, gLastSlot.load(std::memory_order_relaxed));
    };

    probe("ClearState", [&] { list->ClearState(nullptr); });
    probe("IASetPrimitiveTopology", [&] { list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); });
    probe("RSSetViewports(0,null)", [&] { list->RSSetViewports(0, nullptr); });
    probe("RSSetScissorRects(0,null)", [&] { list->RSSetScissorRects(0, nullptr); });
    probe("OMSetStencilRef", [&] { list->OMSetStencilRef(0); });
    probe("OMSetRenderTargets(0,null)", [&] { list->OMSetRenderTargets(0, nullptr, FALSE, nullptr); });
    probe("SetPredication(null,0)", [&] { list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO); });
    probe("SetDescriptorHeaps(0,null)", [&] { list->SetDescriptorHeaps(0, nullptr); });
    probe("DrawInstanced(3,1,0,0)", [&] { list->DrawInstanced(3, 1, 0, 0); });
    probe("DrawIndexedInstanced(3,1,0,0,0)", [&] { list->DrawIndexedInstanced(3, 1, 0, 0, 0); });
    probe("Dispatch(1,1,1)", [&] { list->Dispatch(1, 1, 1); });
    probe("SetGraphicsRoot32BitConstant", [&] { list->SetGraphicsRoot32BitConstant(0, 0, 0); });
    probe("Close", [&] { list->Close(); });

    for (unsigned i = 0; i < kSlotCount; ++i) {
        auto* const target = vtbl[kFirstSlot + i];
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }
    list->Release();
    return true;
}


namespace {

// 24-bit bottom-up BMP. 'src' is the mapped readback row range; 'rowPitch' is the
// D3D12 copyable footprint pitch, which is not width * bytesPerPixel.
bool writeBmp(
    wchar_t const*   path,
    unsigned char const* src,
    unsigned         width,
    unsigned         height,
    unsigned         rowPitch,
    DXGI_FORMAT      format
) {
    unsigned bytesPerPixel = 0;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        bytesPerPixel = 4;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        bytesPerPixel = 8;
        break;
    default:
        return false;
    }

    unsigned const rowBytes  = width * 3;
    unsigned const padBytes  = (4 - (rowBytes % 4)) % 4;
    unsigned const imageBytes = (rowBytes + padBytes) * height;
    unsigned const fileBytes  = 54 + imageBytes;

    std::vector<unsigned char> out(fileBytes, 0);
    out[0] = 'B';
    out[1] = 'M';
    std::memcpy(out.data() + 2, &fileBytes, 4);
    unsigned const offset = 54;
    std::memcpy(out.data() + 10, &offset, 4);
    unsigned const dib = 40;
    std::memcpy(out.data() + 14, &dib, 4);
    std::memcpy(out.data() + 18, &width, 4);
    std::memcpy(out.data() + 22, &height, 4);
    unsigned short const planes = 1;
    unsigned short const bits   = 24;
    std::memcpy(out.data() + 26, &planes, 2);
    std::memcpy(out.data() + 28, &bits, 2);
    std::memcpy(out.data() + 34, &imageBytes, 4);

    for (unsigned y = 0; y < height; ++y) {
        auto const* row = src + static_cast<std::size_t>(y) * rowPitch;
        auto* const dst = out.data() + 54 + static_cast<std::size_t>(height - 1 - y) * (rowBytes + padBytes);
        for (unsigned x = 0; x < width; ++x) {
            unsigned char r = 0, g = 0, b = 0;
            if (bytesPerPixel == 4) {
                r = row[x * 4 + 0];
                g = row[x * 4 + 1];
                b = row[x * 4 + 2];
            } else {
                auto const* px = reinterpret_cast<unsigned short const*>(row + x * 8);
                auto clamp     = [](float v) {
                    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    return static_cast<unsigned char>(v * 255.0f + 0.5f);
                };
                r = clamp(px[0] / 65535.0f);
                g = clamp(px[1] / 65535.0f);
                b = clamp(px[2] / 65535.0f);
            }
            dst[x * 3 + 0] = b;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = r;
        }
    }

    HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool const ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) != 0;
    CloseHandle(file);
    return ok && written == out.size();
}

} // namespace

bool dumpTextureBmp(void* deviceVoid, void* queueVoid, void* resourceVoid, wchar_t const* path) {
    auto* const device = static_cast<ID3D12Device*>(deviceVoid);
    auto* const queue  = static_cast<ID3D12CommandQueue*>(queueVoid);
    auto* const res    = static_cast<ID3D12Resource*>(resourceVoid);
    if (!device || !queue || !res || !path) {
        return false;
    }
    D3D12_RESOURCE_DESC const desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width < 16 || desc.Height < 16) {
        return false;
    }

    bool ok = false;
    ID3D12CommandAllocator*    allocator = nullptr;
    ID3D12GraphicsCommandList* list      = nullptr;
    ID3D12Fence*               fence     = nullptr;
    ID3D12Resource*            readback  = nullptr;
    HANDLE                     event     = nullptr;

    do {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) break;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) break;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) break;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) break;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT                              rows = 0;
        UINT64                            rowSize = 0;
        UINT64                            total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = total;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readback)
            )))
            break;

        list->Reset(allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = res;
        barrier.Transition.Subresource = 0;
        // COMMON is the only state that can be entered without knowing what the
        // engine left behind; the copy is a diagnostic, so a wrong guess costs a
        // frame, not correctness.
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = readback;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint  = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = res;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);

        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        UINT64 const value = 1;
        queue->Signal(fence, value);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 4000);
        }

        void*             data = nullptr;
        D3D12_RANGE       range{0, static_cast<SIZE_T>(total)};
        if (FAILED(readback->Map(0, &range, &data)) || !data) break;
        ok = writeBmp(
            path,
            static_cast<unsigned char const*>(data),
            static_cast<unsigned>(desc.Width),
            static_cast<unsigned>(desc.Height),
            footprint.Footprint.RowPitch,
            desc.Format
        );
        readback->Unmap(0, nullptr);
    } while (false);

    if (readback) readback->Release();
    if (event) CloseHandle(event);
    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    return ok;
}

unsigned swapChainBuffers(void* swapChainVoid, void** out, unsigned max) {
    auto* const chain = static_cast<IDXGISwapChain*>(swapChainVoid);
    if (!chain || !out || max == 0) {
        return 0;
    }
    unsigned count = 0;
    for (UINT i = 0; i < max; ++i) {
        ID3D12Resource* buffer = nullptr;
        if (SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&buffer))) && buffer) {
            out[count++] = buffer;
        }
    }
    return count;
}

void releaseComObject(void* object) {
    if (object) {
        static_cast<IUnknown*>(object)->Release();
    }
}

void addRefComObject(void* object) {
    if (object) {
        static_cast<IUnknown*>(object)->AddRef();
    }
}

void* engineDeviceFromCommandList(void* listVoid) {
    if (!listVoid) {
        return nullptr;
    }
    auto* const          list   = static_cast<ID3D12CommandList*>(listVoid);
    ID3D12Device*        device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        return nullptr;
    }
    return device;
}

namespace {

// Raw dump header: the CPU side (tools/vv_mv_decode.py) needs the exact format and
// pitch, because the whole point is to decode the same bytes two ways and see which
// one is the motion vector. A BMP would have destroyed that information.
struct RawHeader {
    char     magic[4]; // "NDRW"
    unsigned version;
    unsigned width;
    unsigned height;
    unsigned format;     // DXGI_FORMAT
    unsigned rowPitch;   // from GetCopyableFootprints, not width * bpp
    unsigned bytesPerPixel;
    unsigned reserved;
};

// Bytes per pixel for the whole DXGI enum range, by NUMBER rather than by name: the
// named constants are easy to get wrong from memory (fmt=26 is R11G11B10_FLOAT, fmt=44
// is R24G8_TYPELESS, fmt=54 is D16_UNORM - none of them are what the first version
// assumed), and a wrong name silently sends the format to `default` and the dump is
// skipped. Depth formats are listed too, but D3D12 forbids CopyTextureRegion from a
// depth resource to a buffer, so those still fail at the copy (see depth handling).
unsigned bytesPerPixelOf(unsigned format) {
    switch (format) {
    case 1:
    case 2:
    case 3:
    case 4: // R32G32B32A32_*
        return 16;
    case 5:
    case 6:
    case 7:
    case 8: // R32G32B32_*
        return 12;
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14: // R16G16B16A16_*
    case 15:
    case 16:
    case 17:
    case 18: // R32G32_*
    case 19:
    case 20:
    case 21:
    case 22: // R32G8X24 / D32_FLOAT_S8X24
        return 8;
    case 23:
    case 24:
    case 25: // R10G10B10A2_*
    case 26: // R11G11B10_FLOAT
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32: // R8G8B8A8_*
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38: // R16G16_*
    case 39:
    case 40:
    case 41:
    case 42:
    case 43: // R32_* / D32_FLOAT
    case 44:
    case 45:
    case 46:
    case 47: // R24G8 / D24S8
        return 4;
    case 48:
    case 49:
    case 50:
    case 51:
    case 52: // R8G8_*
    case 53:
    case 54:
    case 55:
    case 56: // R16_* / D16_UNORM
        return 2;
    case 57:
    case 58:
    case 59:
    case 60:
    case 61: // R8_*
        return 1;
    default:
        return 0;
    }
}

} // namespace

bool dumpTextureRaw(void* deviceVoid, void* queueVoid, void* resourceVoid, wchar_t const* path) {
    return dumpTextureRawAs(deviceVoid, queueVoid, resourceVoid, path, DumpState::Common);
}

bool dumpTextureRawAs(
    void*          deviceVoid,
    void*          queueVoid,
    void*          resourceVoid,
    wchar_t const* path,
    DumpState      state
) {
    auto* const device = static_cast<ID3D12Device*>(deviceVoid);
    auto* const queue  = static_cast<ID3D12CommandQueue*>(queueVoid);
    auto* const res    = static_cast<ID3D12Resource*>(resourceVoid);
    if (!device || !queue || !res || !path) {
        return false;
    }
    D3D12_RESOURCE_DESC const desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return false;
    }
    unsigned const bpp = bytesPerPixelOf(desc.Format);
    if (bpp == 0 || desc.Width < 4 || desc.Height < 4) {
        return false;
    }

    bool ok = false;
    ID3D12CommandAllocator*    allocator = nullptr;
    ID3D12GraphicsCommandList* list      = nullptr;
    ID3D12Fence*               fence     = nullptr;
    ID3D12Resource*            readback  = nullptr;
    HANDLE                     event     = nullptr;

    do {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) break;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) break;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) break;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) break;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT                              rows    = 0;
        UINT64                            rowSize = 0;
        UINT64                            total   = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = total;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readback)
            )))
            break;

        list->Reset(allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = res;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // COMMON is the only state that can be entered without knowing what the engine
        // left behind (docs/specs/vv.md §12 A carries the same lesson from the early
        // boundary experiment).
        barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(state);
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback;
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = res;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = static_cast<D3D12_RESOURCE_STATES>(state);
        list->ResourceBarrier(1, &barrier);

        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        UINT64 const value = 1;
        queue->Signal(fence, value);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 4000);
        }

        void*       data  = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        if (FAILED(readback->Map(0, &range, &data)) || !data) break;

        RawHeader header{};
        std::memcpy(header.magic, "NDRW", 4);
        header.version      = 1;
        header.width        = static_cast<unsigned>(desc.Width);
        header.height       = static_cast<unsigned>(desc.Height);
        header.format       = static_cast<unsigned>(desc.Format);
        header.rowPitch     = footprint.Footprint.RowPitch;
        header.bytesPerPixel = bpp;

        HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            bool  good    = WriteFile(file, &header, sizeof(header), &written, nullptr) != 0;
            good          = good && WriteFile(file, data, static_cast<DWORD>(total), &written, nullptr) != 0;
            CloseHandle(file);
            ok = good;
        }
        readback->Unmap(0, nullptr);
    } while (false);

    if (readback) readback->Release();
    if (event) CloseHandle(event);
    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    return ok;
}

bool writeBmpRgba8(
    wchar_t const*       path,
    unsigned char const* rgba,
    unsigned             width,
    unsigned             height,
    unsigned             rowPitch
) {
    if (!path || !rgba || !width || !height || rowPitch < width * 4) {
        return false;
    }
    return writeBmp(path, rgba, width, height, rowPitch, DXGI_FORMAT_R8G8B8A8_UNORM);
}

} // namespace neurodragon::probe::vv
