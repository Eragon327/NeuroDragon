#pragma once

namespace neurodragon::probe::camera {

// ---------------------------------------------------------------------------
// The client's camera, as the renderer sees it - TEST only
// ---------------------------------------------------------------------------
// A2 needs a per-frame camera source that exists *without* the TAAU pass: the
// option switch (EXP-0020) puts the engine in Bilinear, so the TAAContext the MV
// pass used to read is never filled. `ClientInstance::getCamera()` returns the
// game's mce::Camera; EXP-0021 showed its mRight/mUp/mForward are exactly the
// vectors the taau context carries (normalised columns match), but the documented
// offsets for position / aspect / fov / near hold zeros in this build, and the
// projection stack read back garbage (an empty stack). So the next probe reads the
// object raw and only trusts a matrix stack when it is non-empty.
//
// This translation unit is deliberately engine-headers-only: like GameNotice.cpp it
// pulls in mc/_HeaderOutputPredefine.h, which redefines DXGI/D3D12 structures and
// must not meet VvProbe.cpp's <d3d12.h> in one TU.
struct Sample {
    float position[3]{};
    float right[3]{};
    float up[3]{};
    float forward[3]{};
    float aspect{};
    float fov{};
    float zNear{};
    float zFar{};
    float view[16]{};        ///< viewMatrixStack top, when non-empty
    float world[16]{};       ///< worldMatrixStack top, when non-empty
    float projection[16]{};  ///< projectionMatrixStack top, when non-empty
    float inverseView[16]{}; ///< mInverseViewMatrix
    bool  viewOk{};
    bool  worldOk{};
    bool  projectionOk{};
    // Raw floats of the object, 0x240 bytes (covers the three stacks' members, the
    // basis, the scalars and mFrustum). The offsets in the public headers are not
    // trusted after EXP-0021; the dump is what the offline pass anchors against the
    // basis values.
    float raw[144]{};
    bool  valid{};
};

// Returns valid=false when there is no client instance yet (before the main menu).
// Safe to call every frame; cheap.
Sample read();

} // namespace neurodragon::probe::camera
