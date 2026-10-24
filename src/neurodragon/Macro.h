#pragma once

// Preprocessor shims, in one place. Every block below says *when* it has to be included,
// because these macros only work if they run between two #includes - that is the one
// thing a header cannot arrange for itself.
//
// Why a header and not a few lines at the call site: the shims are per-translation-unit
// tricks that look like noise (a macro that expands to itself), and there will be more
// of them as soon as another engine header collides with an SDK one. Keeping them here
// means the explanation is written once and the call site reads as one line.

// ---------------------------------------------------------------------------
// mc/_HeaderOutputPredefine.h empty D3D12 shells
// ---------------------------------------------------------------------------
//
// Include AFTER <d3d12.h> and BEFORE any header that reaches mc/ (any LeviLamina header,
// because ll/api/Expected.h pulls mc/server/commands/CommandOutputMessageType.h; through
// ll/api/i18n/I18n.h that is the common case).
//
// What goes wrong without this: mc/_HeaderOutputPredefine.h forward-declares
//     struct D3D12_FEATURE_DATA_D3D12_OPTIONS {};
//     struct D3D12_FEATURE_DATA_ARCHITECTURE {};
//     struct D3D12_RAYTRACING_GEOMETRY_DESC {};
// (they are referenced by TypedStorage), each guarded with `#ifndef <type name>`. A type
// name is never a macro, so those guards are always true and the shells are declared
// unconditionally - and after <d3d12.h> they collide with the real structs:
//     error: redefinition of 'D3D12_FEATURE_DATA_D3D12_OPTIONS'
//
// The shim defines those names as macros that expand to themselves. The guards then fail,
// mc skips its shells, and every other occurrence of the token is left untouched (a macro
// is not re-expanded inside its own replacement - the "blue paint" rule). The macros stay
// defined for the rest of the translation unit on purpose: undefining them would re-open
// the collision for any later engine include, and a self-referential macro is inert.
//
// Guarded by __d3d12_h__ so including this file too early does nothing instead of
// half-applying the trick: the SDK header's own include guard is the only reliable way to
// ask "are the real structs already here?".
#ifdef __d3d12_h__
#    define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC
#    define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE
#    define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS
#endif
