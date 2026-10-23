#pragma once

// ---------------------------------------------------------------------------
// TEST build switch
// ---------------------------------------------------------------------------
//
// Everything in this mod that exists only to *look at the mod itself* is
// compiled in only when TEST is defined: the BMP colour dumps, the NGX
// parameter trace (nr.paramLog), the resource/parameter/eval diagnostics and
// every debug-level line. A release DLL therefore writes no dump files, does
// not hook the engine's NGX parameter setters and spends no time formatting
// diagnostics that only a ticket would ever read.
//
// A release build keeps the user-facing info/warn/error lines, the stage
// markers (Log.h) and every functional path: feature creation, the per-frame
// evaluate, the gain composite, the panel and the GPU timing it reads.
//
//   xmake f --test=y && xmake build -y   # diagnostics build (TEST defined)
//   xmake f --test=n && xmake build -y   # release build (default)
//
// `TEST` is deliberately a plain one-token macro, so the gate reads as
// `#ifdef TEST` at every call site. This header carries no includes on purpose:
// D3D12Bridge.cpp must stay free of LeviLamina/engine headers (docs/D §4
// pitfall 5) and includes this one just to make the switch discoverable.
//
// Beware: a test build writes <mod dir>/dump/*.bmp and can log several lines
// per frame - it is a developer tool, not something to ship.
