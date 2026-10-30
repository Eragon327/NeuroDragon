#include "neurodragon/fg/HudlessProbe.h"

#include "neurodragon/probe/VvProbe.h"

#include <Windows.h>

#ifdef TEST

// utils/Utils.h pulls the engine headers, which stub three d3d12 structs. Rename the
// d3d12.h copies (the same trick ui/Overlay.cpp uses) so both can live in one TU.
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d12.h>
#include <dxgi1_4.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include "neurodragon/utils/Utils.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace neurodragon::fg::hudless {
namespace {

// Six frames: two rounds of the three candidate boundaries. The menu and the world
// share the display/UI tail, so the same offsets apply; only world frames are used.
constexpr unsigned kMaxCaptures = 6;
constexpr int      kOffsets[3]  = {-6, -3, -2};

struct State {
    ID3D12Device*              device{};
    ID3D12CommandAllocator*    allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence*               fence{};
    UINT64                     fenceValue{};
    ID3D12Resource*            texture{};   ///< R8G8B8A8 copy of the back buffer
    ID3D12Resource*            readback{};
    ID3D12Resource*            pendingBuffer{}; ///< held until the copy's fence completes
    ID3D12CommandQueue*        queue{};         ///< not owned
    UINT                       width{};
    UINT                       height{};
    UINT                       rowPitch{};
    unsigned                   captures{};
    int                        pendingSplit{};
    unsigned                   pendingLists{};
    std::wstring               pendingFile;
    bool                       pending{};
};

State g;

void releaseResources() {
    if (g.pendingBuffer) {
        g.pendingBuffer->Release();
        g.pendingBuffer = nullptr;
    }
    if (g.texture) {
        g.texture->Release();
        g.texture = nullptr;
    }
    if (g.readback) {
        g.readback->Release();
        g.readback = nullptr;
    }
    if (g.list) {
        g.list->Release();
        g.list = nullptr;
    }
    if (g.allocator) {
        g.allocator->Release();
        g.allocator = nullptr;
    }
    if (g.fence) {
        g.fence->Release();
        g.fence = nullptr;
    }
    if (g.device) {
        g.device->Release();
        g.device = nullptr;
    }
    g.width        = 0;
    g.height       = 0;
    g.rowPitch     = 0;
    g.pending      = false;
    g.pendingSplit = 0;
    g.captures     = 0;
}

bool ensureDevice(ID3D12CommandQueue* queue) {
    if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return false;
    }
    if (g.device) {
        return true;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    g.device = device;
    if (FAILED(g.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&g.allocator)
        ))
        || FAILED(g.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            g.allocator,
            nullptr,
            IID_PPV_ARGS(&g.list)
        ))) {
        releaseResources();
        return false;
    }
    g.list->Close();
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        releaseResources();
        return false;
    }
    return true;
}

bool ensureTargets(UINT width, UINT height) {
    if (g.texture && g.width == width && g.height == height) {
        return true;
    }
    if (g.texture) {
        g.texture->Release();
        g.texture = nullptr;
    }
    if (g.readback) {
        g.readback->Release();
        g.readback = nullptr;
    }
    if (!width || !height) {
        return false;
    }

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width            = width;
    texture.Height           = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels        = 1;
    texture.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc       = {1, 0};
    texture.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g.device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&g.texture)
        ))) {
        return false;
    }

    UINT const rowPitch = (width * 4u + 255u) & ~255u;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width            = static_cast<UINT64>(rowPitch) * height;
    buffer.Height           = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels        = 1;
    buffer.Format           = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc       = {1, 0};
    buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(g.device->CreateCommittedResource(
            &readback,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&g.readback)
        ))) {
        return false;
    }

    g.width    = width;
    g.height   = height;
    g.rowPitch = rowPitch;
    return true;
}

ID3D12Resource* currentBackBuffer() {
    auto* chain = static_cast<IDXGISwapChain*>(probe::vv::swapChain());
    if (!chain) {
        return nullptr;
    }
    UINT index = 0;
    if (IDXGISwapChain3* chain3 = nullptr;
        SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3) {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }
    ID3D12Resource* buffer = nullptr;
    if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

bool record(ID3D12Resource* backBuffer) {
    if (FAILED(g.allocator->Reset()) || FAILED(g.list->Reset(g.allocator, nullptr))) {
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &barrier);

    g.list->CopyResource(g.texture, backBuffer);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &barrier);

    barrier.Transition.pResource   = g.texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource        = g.texture;
    source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = g.readback;
    destination.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width    = g.width;
    destination.PlacedFootprint.Footprint.Height   = g.height;
    destination.PlacedFootprint.Footprint.Depth    = 1;
    destination.PlacedFootprint.Footprint.RowPitch = g.rowPitch;
    g.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->ResourceBarrier(1, &barrier);

    return SUCCEEDED(g.list->Close());
}

bool writeBmp(std::wstring const& path, void const* data, UINT width, UINT height, UINT rowPitch) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        return false;
    }
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize        = sizeof(info);
    info.biWidth       = static_cast<LONG>(width);
    info.biHeight      = static_cast<LONG>(height);
    info.biPlanes      = 1;
    info.biBitCount    = 32;
    info.biCompression = BI_RGB;
    file.bfType        = 0x4D42;
    file.bfOffBits     = sizeof(file) + sizeof(info);
    file.bfSize        = file.bfOffBits + width * height * 4u;
    out.write(reinterpret_cast<char const*>(&file), sizeof(file));
    out.write(reinterpret_cast<char const*>(&info), sizeof(info));

    auto const* base = static_cast<std::uint8_t const*>(data);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        auto const* row = base + static_cast<std::size_t>(rowPitch) * y;
        for (UINT x = 0; x < width; ++x) {
            out.put(static_cast<char>(row[x * 4 + 2]));
            out.put(static_cast<char>(row[x * 4 + 1]));
            out.put(static_cast<char>(row[x * 4 + 0]));
            out.put(static_cast<char>(255));
        }
    }
    return out.good();
}

} // namespace

void install() {
    g.captures = 0;
    g.pending  = false;
}

void uninstall() {
    // Deliberately does not release the D3D12 objects: the process is going away and
    // the project's exit path already learned that releasing in-flight GPU objects
    // hangs the driver (NR/SR keep theirs for the same reason). EXP-0121's exit hung
    // right after this call; the objects are small and die with the process.
    //
    // The one reference that must go is the swap-chain back buffer: a held one makes
    // the engine's ResizeBuffers fail.
    if (g.pendingBuffer) {
        g.pendingBuffer->Release();
        g.pendingBuffer = nullptr;
    }
    g.pending  = false;
    g.captures = 0;
}

unsigned prepare(void* queueVoid, unsigned listCount, void* const* lists) {
    (void)lists;
    if (g.captures >= kMaxCaptures || !queueVoid || listCount <= 8) {
        return 0;
    }
    // Write the previous capture before reusing the readback buffer: the same buffer
    // backs every capture, so recording a second one first throws the first away
    // (EXP-0121 wrote only #5).
    poll();
    if (g.pending) {
        return 0;
    }
    // A world frame only: the menu tail has the same shape, but only the world path
    // is the product. The copy is submitted between engine lists, so no engine list
    // is modified. The first 120 world submissions are the load (terrain, chunk
    // meshes); capturing those would photograph the loading screen.
    if (!probe::vv::worldLoaded()) {
        return 0;
    }
    static unsigned worldFrames = 0;
    if (++worldFrames < 120) {
        return 0;
    }
    auto* queue = static_cast<ID3D12CommandQueue*>(queueVoid);
    if (!ensureDevice(queue)) {
        return 0;
    }
    if (g.pending && g.fence && g.fence->GetCompletedValue() < g.fenceValue) {
        return 0;
    }

    ID3D12Resource* buffer = currentBackBuffer();
    if (!buffer) {
        return 0;
    }
    D3D12_RESOURCE_DESC const desc = buffer->GetDesc();
    if (!ensureTargets(static_cast<UINT>(desc.Width), desc.Height)) {
        buffer->Release();
        return 0;
    }

    int const offset = kOffsets[g.captures % 3];
    int const split  = static_cast<int>(listCount) + offset;
    if (split <= 0 || split > static_cast<int>(listCount)) {
        buffer->Release();
        return 0;
    }
    if (!record(buffer)) {
        buffer->Release();
        return 0;
    }

    unsigned const capture = g.captures++;
    std::wstring   name    = L"hudless_c" + std::to_wstring(capture) + L"_s" + std::to_wstring(split) + L".bmp";
    std::filesystem::path dir = std::filesystem::path(utils::applicationDirectory()) / "screenshots";
    std::error_code       error;
    std::filesystem::create_directories(dir, error);

    g.queue        = queue;
    g.pendingBuffer = buffer;
    g.pendingSplit  = split;
    g.pendingLists  = listCount;
    g.pendingFile   = (dir / name).wstring();
    g.pending       = true;

    ND_DEBUG(
        "[fg] hudless capture #{} split={}/{} size={}x{} file={}",
        capture,
        split,
        listCount,
        g.width,
        g.height,
        std::string(name.begin(), name.end())
    );
    return static_cast<unsigned>(split);
}

void* list() { return g.list; }

void submitted() {
    if (g.queue && g.fence) {
        g.queue->Signal(g.fence, ++g.fenceValue);
    }
}

void poll() {
    if (!g.pending || !g.fence) {
        return;
    }
    if (g.fence->GetCompletedValue() < g.fenceValue) {
        return;
    }
    D3D12_RANGE const range{0, static_cast<SIZE_T>(g.rowPitch) * g.height};
    void*             mapped = nullptr;
    if (SUCCEEDED(g.readback->Map(0, &range, &mapped)) && mapped) {
        bool const ok = writeBmp(g.pendingFile, mapped, g.width, g.height, g.rowPitch);
        g.readback->Unmap(0, nullptr);
        ND_DEBUG(
            "[fg] hudless wrote #{} split={}/{} ok={}",
            g.captures - 1,
            g.pendingSplit,
            g.pendingLists,
            ok ? 1 : 0
        );
    }
    if (g.pendingBuffer) {
        g.pendingBuffer->Release();
        g.pendingBuffer = nullptr;
    }
    g.pending = false;
}

} // namespace neurodragon::fg::hudless

#else

namespace neurodragon::fg::hudless {

void install() {}
void uninstall() {}
unsigned prepare(void*, unsigned, void* const*) { return 0; }
void*    list() { return nullptr; }
void     submitted() {}
void     poll() {}

} // namespace neurodragon::fg::hudless

#endif
