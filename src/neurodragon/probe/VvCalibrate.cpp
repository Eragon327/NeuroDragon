#include "neurodragon/probe/VvCalibrate.h"

#include <MinHook.h>
#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

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

bool calibrateCommandListSlots(void* deviceVoid, SlotReporter report) {
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

} // namespace neurodragon::probe::vv
