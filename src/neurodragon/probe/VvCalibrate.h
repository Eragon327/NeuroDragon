#pragma once

namespace neurodragon::probe::vv {

// Reports one probe: which vtable slot the named ID3D12GraphicsCommandList method
// actually fired on (-1 when nothing fired).
using SlotReporter = void (*)(char const* method, int slot);

// Measures the ID3D12GraphicsCommandList vtable instead of trusting the published
// order. Slots 9..40 get a record-and-forward detour, a command list of our own is
// asked to call one known method at a time, and the slot that fires is the answer.
// The engine's lists are never touched and every recording detour is removed before
// this returns.
//
// `device` is an ID3D12Device. Returns false if the command list could not be
// created; the reporter is then never called.
//
// This lives in its own translation unit on purpose: <d3d12.h> does not coexist with
// the engine headers in one TU (docs/D §4 pitfall 5), the same reason
// nr/D3D12Bridge.cpp exists.
bool calibrateCommandListSlots(void* device, SlotReporter report);

// The ID3D12Device behind a command list the engine submitted. This is the only way
// to reach the engine's own device: it is created before mods load, and it lives in
// D3D12Core.dll (the Agility runtime), so hooking d3d12.dll's D3D12CreateDevice
// never sees it. The reference returned is owned by the caller.
void* engineDeviceFromCommandList(void* list);

} // namespace neurodragon::probe::vv
