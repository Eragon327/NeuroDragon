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

// One-shot readback of a texture into a 24-bit BMP (bottom-up rows). Only the
// formats VV actually uses are converted - R8G8B8A8 (typeless/unorm/sRGB) and
// R16G16B16A16_FLOAT - anything else is reported as unsupported instead of being
// written as noise. The copy is recorded on a command list of our own, executed on
// `queue` and waited on with a fence, so the file on disk is the frame that was
// submitted, not a half-written texture.
bool dumpTextureBmp(void* device, void* queue, void* resource, wchar_t const* path);

// Back-buffer resources of a swap chain (2 for a flip-model chain). Used only to keep
// the BMP dump away from resources that live in PRESENT.
unsigned swapChainBuffers(void* swapChain, void** out, unsigned max);

// Releases one reference taken by swapChainBuffers(). Holding a back-buffer reference
// makes the engine's ResizeBuffers fail (DXGI_ERROR_INVALID_CALL), and the engine
// answers that with abort() - which is exactly the 0xC0000409 the client showed when
// the window was toggled. Every buffer handed out must go through this.
void releaseComObject(void* object);

} // namespace neurodragon::probe::vv
