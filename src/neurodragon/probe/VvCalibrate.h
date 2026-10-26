#pragma once

namespace neurodragon::probe::vv {

// ---------------------------------------------------------------------------
// Slot calibration - TESTED, CRASHED, DO NOT CALL
// ---------------------------------------------------------------------------
// Driving 32 record-and-forward detours over vtable slots 9..40 killed the client
// inside nvwgf2umx.dll (AV @0x2b88, 2026-10-24 22:0x): forwarding a method whose real
// signature is unknown corrupts the call. The implementation is kept in
// VvCalibrate.cpp as reference; if slot numbers are ever needed again, hook ONE slot
// at a time and call it once, on a device of our own first.
using SlotReporter = void (*)(char const*, int);
[[deprecated("crashes the client - see VvCalibrate.h")]] bool calibrateCommandListSlots(void* device, SlotReporter report);
//
// The ID3D12Device behind a command list the engine submitted. This is the only way
// to reach the engine's own device: it is created before mods load, and it lives in
// D3D12Core.dll (the Agility runtime), so hooking d3d12.dll's D3D12CreateDevice
// never sees it. The reference returned is owned by the caller.
void* engineDeviceFromCommandList(void* list);

// Size of one RTV descriptor in a descriptor heap, straight from the device. The
// content boundary compares bound render-target handles against the handles the
// back buffer's views were created with; when the engine passes a single handle for
// a contiguous range, the i-th handle is first + i * increment.
unsigned rtvDescriptorIncrement(void* device);

// One-shot readback of a texture into a 24-bit BMP (bottom-up rows). Only the
// formats VV actually uses are converted - R8G8B8A8 (typeless/unorm/sRGB) and
// R16G16B16A16_FLOAT - anything else is reported as unsupported instead of being
// written as noise. The copy is recorded on a command list of our own, executed on
// `queue` and waited on with a fence, so the file on disk is the frame that was
// submitted, not a half-written texture.
bool dumpTextureBmp(void* device, void* queue, void* resource, wchar_t const* path);

// Same copy, but the file keeps the resource's own bytes: a small "NDRW" header
// (width / height / DXGI_FORMAT / pitch / bytes per pixel) followed by the mapped
// rows. That is what makes the motion vector decidable offline - the same uint16
// pairs can be read as half floats or as int16 fixed point, and only one of the two
// produces the numbers DLSS wants. Tools: NeuroDragonWorkSpace\tools\vv_mv_decode.py.
bool dumpTextureRaw(void* device, void* queue, void* resource, wchar_t const* path);

// The state a resource is declared to be in when the copy starts. D3D12 never checks
// StateBefore, but the driver uses it: a wrong value can read stale bytes (the VV
// textures came back as all zeros with the obvious guess). The probe dumps the same
// resource once per plausible state and the one that yields real data wins.
enum class DumpState : unsigned {
    Common                 = 0,    // D3D12_RESOURCE_STATE_COMMON / PRESENT
    RenderTarget           = 4,    // D3D12_RESOURCE_STATE_RENDER_TARGET
    NonPixelShaderResource = 0x40, // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    ShaderResource         = 0x80, // D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
};

bool dumpTextureRawAs(void* device, void* queue, void* resource, wchar_t const* path, DumpState state);

// Writes a 24-bit BMP from an RGBA8 image that is already in CPU memory (the mapped
// readback of a copy we recorded ourselves). Used by the B1c cut capture: it copies
// the back buffer at the injection boundary, so there is no engine texture to copy
// and no device/queue work left to do - only the file.
bool writeBmpRgba8(
    wchar_t const*       path,
    unsigned char const* rgba,
    unsigned             width,
    unsigned             height,
    unsigned             rowPitch
);

// Back-buffer resources of a swap chain (2 for a flip-model chain). Used only to keep
// the BMP dump away from resources that live in PRESENT.
unsigned swapChainBuffers(void* swapChain, void** out, unsigned max);

// Releases one reference taken by swapChainBuffers(). Holding a back-buffer reference
// makes the engine's ResizeBuffers fail (DXGI_ERROR_INVALID_CALL), and the engine
// answers that with abort() - which is exactly the 0xC0000409 the client showed when
// the window was toggled. Every buffer handed out must go through this.
void releaseComObject(void* object);

// Takes a reference so a recorded render target stays alive after the engine rebuilds
// its render graph. Without it the inventory only holds raw pointers: a resize frees the
// old textures and the next allocation reuses the address, so a dump then reads a
// different resource (observed 2026-10-25: requested fmt=10, file header said fmt=28).
void addRefComObject(void* object);

} // namespace neurodragon::probe::vv
