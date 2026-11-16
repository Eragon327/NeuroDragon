#include "neurodragon/fg/FrameInputs.h"

#include "neurodragon/fg/streamline-sdk/sl.h"
#include "neurodragon/fg/streamline-sdk/sl_dlss_g.h"
#include "neurodragon/fg/streamline-sdk/sl_pcl.h"
#include "neurodragon/fg/streamline-sdk/sl_reflex.h"

#include "neurodragon/fg/HudlessProbe.h"
#include "neurodragon/fg/Streamline.h"

#include <Windows.h>

// utils/Utils.h pulls the engine headers, which stub three d3d12 structs; rename the
// d3d12.h copies so both can live in this translation unit (same trick as
// fg/Streamline.cpp and ui/Overlay.cpp).
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d12.h>
#include <dxgi1_4.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include "neurodragon/Config.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvSr.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace neurodragon::fg::inputs {
namespace {

// ---------------------------------------------------------------------------
// 4x4 helpers, row-major with row vectors (v * M) - the layout sl::float4x4 uses
// ---------------------------------------------------------------------------
struct Mat4 {
    float m[16]{};
};

Mat4 load(float const* values) {
    Mat4 out{};
    std::memcpy(out.m, values, sizeof(out.m));
    return out;
}

Mat4 identity() {
    Mat4 out{};
    for (unsigned i = 0; i < 4; ++i) {
        out.m[i * 4 + i] = 1.0f;
    }
    return out;
}

// (v * a) * b == v * (a * b)
Mat4 multiply(Mat4 const& a, Mat4 const& b) {
    Mat4 out{};
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (unsigned k = 0; k < 4; ++k) {
                sum += a.m[row * 4 + k] * b.m[k * 4 + column];
            }
            out.m[row * 4 + column] = sum;
        }
    }
    return out;
}

// World-space translation by (dx,dy,dz) in row-vector form: (v+1) * T == v + d.
Mat4 translation(float dx, float dy, float dz) {
    Mat4 out = identity();
    out.m[3 * 4 + 0] = dx;
    out.m[3 * 4 + 1] = dy;
    out.m[3 * 4 + 2] = dz;
    return out;
}

bool invert(Mat4 const& a, Mat4* out) {
    double work[4][8]{};
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            work[row][column]     = a.m[row * 4 + column];
            work[row][column + 4] = row == column ? 1.0 : 0.0;
        }
    }
    for (unsigned column = 0; column < 4; ++column) {
        unsigned pivot = column;
        for (unsigned row = column + 1; row < 4; ++row) {
            if (std::fabs(work[row][column]) > std::fabs(work[pivot][column])) {
                pivot = row;
            }
        }
        if (std::fabs(work[pivot][column]) < 1.0e-9) {
            return false;
        }
        if (pivot != column) {
            for (unsigned k = 0; k < 8; ++k) {
                double const swap = work[column][k];
                work[column][k]   = work[pivot][k];
                work[pivot][k]    = swap;
            }
        }
        double const scale = 1.0 / work[column][column];
        for (unsigned k = 0; k < 8; ++k) {
            work[column][k] *= scale;
        }
        for (unsigned row = 0; row < 4; ++row) {
            if (row == column) {
                continue;
            }
            double const factor = work[row][column];
            if (factor == 0.0) {
                continue;
            }
            for (unsigned k = 0; k < 8; ++k) {
                work[row][k] -= factor * work[column][k];
            }
        }
    }
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            out->m[row * 4 + column] = static_cast<float>(work[row][column + 4]);
        }
    }
    return true;
}

bool finite(Mat4 const& a) {
    for (float const value : a.m) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

// Max |a - b|, for the identity checks below.
float maxDifference(Mat4 const& a, Mat4 const& b) {
    float worst = 0.0f;
    for (unsigned i = 0; i < 16; ++i) {
        float const delta = std::fabs(a.m[i] - b.m[i]);
        if (delta > worst) {
            worst = delta;
        }
    }
    return worst;
}

sl::float4x4 toSl(Mat4 const& a) {
    sl::float4x4 out{};
    for (unsigned row = 0; row < 4; ++row) {
        out.setRow(row, sl::float4(a.m[row * 4 + 0], a.m[row * 4 + 1], a.m[row * 4 + 2], a.m[row * 4 + 3]));
    }
    return out;
}

float length3(float const* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

float dot3(float const* a, float const* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool normalize3(float const* v, float* out) {
    float const length = length3(v);
    if (!(length > 1.0e-4f) || !std::isfinite(length)) {
        return false;
    }
    out[0] = v[0] / length;
    out[1] = v[1] / length;
    out[2] = v[2] / length;
    return true;
}

// ---------------------------------------------------------------------------
// Frame state
// ---------------------------------------------------------------------------
struct Api {
    PFun_slGetNewFrameToken* getToken{};
    PFun_slSetConstants*     setConstants{};
    PFun_slSetTagForFrame*   setTagForFrame{};
    PFun_slDLSSGGetState*    getState{};
    PFun_slPCLSetMarker*     pclSetMarker{};
    PFun_slReflexSleep*      reflexSleep{};
};

// Non-zero codes are what the failure lines print (the smoke asserts "non-zero code and
// a reason"); they are stable so a log can be read without asking the author.
enum class Skip : unsigned {
    None           = 0,
    NotReady       = 1,
    NoWorld        = 2,
    NoFreshSr      = 3,
    NoFreshHudless = 4,
    BadMatrices    = 5,
    BadResource    = 6,
    TokenFailed    = 7,
    ConstantsFailed = 8,
    TagFailed      = 9,
};

char const* skipReason(Skip skip) {
    switch (skip) {
    case Skip::None:
        return "ok";
    case Skip::NotReady:
        return "not-ready";
    case Skip::NoWorld:
        return "no-world";
    case Skip::NoFreshSr:
        return "no-fresh-sr";
    case Skip::NoFreshHudless:
        return "no-fresh-hudless";
    case Skip::BadMatrices:
        return "bad-matrices";
    case Skip::BadResource:
        return "bad-resource";
    case Skip::TokenFailed:
        return "token-failed";
    case Skip::ConstantsFailed:
        return "constants-failed";
    case Skip::TagFailed:
        return "tag-failed";
    }
    return "?";
}

struct State {
    std::atomic<unsigned>       presentSequence{0};
    std::atomic<void*>          fence{nullptr};
    std::atomic<uint64_t>       fenceValue{0};
    std::atomic<bool>           requestedEnabled{true};
    std::atomic<sl::FrameToken*> lastToken{nullptr}; ///< this flip's token, for the PCL markers
    int                         attemptedMode{0}; ///< 0=eOff, 1=eOn; one attempt per transition
    bool                        readyLogged{};
    // The fields below are written from two threads - the flip/present thread and the
    // resize/disable path (invalidate) - so they live behind gStateMutex. Nothing that
    // can re-enter or block on another module's lock (Streamline calls, SR snapshot,
    // hudless) is ever called with it held.
    std::mutex            stateMutex{};
    bool                  taggedLastFrame{};
    unsigned              validSequence{};
    // Freshness is "changed since this reader last looked", not "old enough": a frame
    // whose inputs were consumed (even into a failed submission) must not be reused by
    // the next Present.
    unsigned lastSrSerial{};
    unsigned lastHudlessSerial{};
    unsigned lastTaaSerial{};
    unsigned lastDlssgStatus{};
    bool     lastStatusKnown{};
    // Log gate: the first three frames, every 600th, and every state/error change.
    bool     gateSeen{};
    bool     gateValid{};
    unsigned gateCode{};
    int      gateConstants{};
    int      gateTags{};
    unsigned validFrames{};   ///< frames this session really marked (first 3 are logged)
    unsigned loggedFailures{};
    unsigned indexMisses{};   ///< GetCurrentBackBufferIndex unavailable (throttled)
    unsigned clearFailures{}; ///< tag-clear calls that did not return 0 (throttled)
};

State g;

Api refreshApi() {
    Api api{};
    streamline::FrameFunctions functions{};
    if (streamline::frameFunctions(&functions)) {
        api.getToken     = static_cast<PFun_slGetNewFrameToken*>(functions.newFrameToken);
        api.setConstants  = static_cast<PFun_slSetConstants*>(functions.setConstants);
        api.setTagForFrame = static_cast<PFun_slSetTagForFrame*>(functions.setTagForFrame);
    }
    api.getState = static_cast<PFun_slDLSSGGetState*>(functions.dlssgGetState);
    api.pclSetMarker = static_cast<PFun_slPCLSetMarker*>(functions.pclSetMarker);
    api.reflexSleep = static_cast<PFun_slReflexSleep*>(functions.reflexSleep);
    return api;
}

bool ready(Api const& api) { return api.getToken && api.setConstants && api.setTagForFrame; }

// The TAA snapshot is a camera-relative view-projection: it maps a position expressed
// relative to the camera at that frame into clip space, and it carries the camera rotation
// but no camera translation. Two things follow, and both are required for the constants:
//
//   * cameraViewToClip is the *projection* (view -> clip). The rotation is divided out
//     (see decodeFrame), because view space and camera-relative world space are not the
//     same space once the camera turns.
//   * the previous frame's transform is reached through the camera-origin delta:
//       v_cur  = p - camCur            (what M_cur was built for)
//       v_prev = p - camPrev = v_cur + (camCur - camPrev)
//       clip_prev = (clip_cur * M_cur^-1 * T(delta)) * M_prev
//     so clipToPrevClip = M_cur^-1 * T * M_prev. Dropping T is exactly the
//     "reprojection without the camera translation" bug this matrix exists to prevent.
struct Decoded {
    Mat4 cameraViewToClip;
    Mat4 clipToCameraView;
    Mat4 clipToPrevClip;
    Mat4 prevClipToClip;
    float nearPlane{};
    float farPlane{};
    float fov{};
    float aspect{};
    float scaleZ{}; ///< |column 2|: the clip-z scale, i.e. A in clip.z = A*depth + B
    float position[3]{};
    float up[3]{};
    float right[3]{};
    float forward[3]{};
};

// Axis/near decode taken from the verified decodeMvFrame of 9e0d27a: the 16 floats'
// columns 0..2 are right/up/forward scaled by the projection, and the row-3/column-2
// entry is -A*near. That commit's shader reconstructed the camera-relative point from
// these vectors and the depth, so the same reading is what the engine's own reprojection
// uses; a "plain" view-projection with translation would put the camera position in row
// 3 instead.
bool decodeFrame(vv::sr::FrameSnapshot const& snapshot, Decoded* out, char const** why) {
    if (!snapshot.matrices) {
        *why = "no-taa-snapshot";
        return false;
    }
    Mat4 const current  = load(snapshot.current);
    Mat4 const previous = load(snapshot.previous);
    if (!finite(current) || !finite(previous)) {
        *why = "matrix-not-finite";
        return false;
    }
    float const rightAxis[3] = {current.m[0], current.m[4], current.m[8]};
    float const upAxis[3]    = {current.m[1], current.m[5], current.m[9]};
    float const fwdAxis[3]   = {current.m[2], current.m[6], current.m[10]};
    float const scaleX       = length3(rightAxis);
    float const scaleY       = length3(upAxis);
    float const scaleZ       = length3(fwdAxis);
    if (!(scaleX > 1.0e-4f) || !(scaleY > 1.0e-4f) || !(scaleZ > 1.0e-4f)) {
        *why = "basis-degenerate";
        return false;
    }
    if (!normalize3(rightAxis, out->right) || !normalize3(upAxis, out->up) || !normalize3(fwdAxis, out->forward)) {
        *why = "basis-not-normalizable";
        return false;
    }
    // An orthonormal camera basis is the cheapest proof that the 16 floats really are a
    // camera transform and not, say, a projection-only matrix with axis-aligned columns
    // scaled per pixel.
    constexpr float kOrthogonal = 0.05f;
    if (std::fabs(dot3(out->right, out->up)) > kOrthogonal || std::fabs(dot3(out->right, out->forward)) > kOrthogonal
        || std::fabs(dot3(out->up, out->forward)) > kOrthogonal) {
        *why = "basis-not-orthogonal";
        return false;
    }

    // near = -B / A with B = m[3][2] and A = |column 2| (the clip-z scale on the view
    // depth). A == 1 is the infinite-far projection; that is a real projection, not a
    // decode failure, and cameraFar then reports infinity (see below) instead of a made-up
    // finite plane.
    float const nearPlane = -current.m[14] / scaleZ;
    if (!(nearPlane > 0.0f) || !std::isfinite(nearPlane)) {
        *why = "near-not-derivable";
        return false;
    }
    // OptiScaler's DLSS-G constants (framegen/dlssg/DLSSG_Common.cpp:135-136) treat
    // |A-1| <= 1e-3 as an infinite depth range and hand SL +inf. The earlier strict
    // `A <= 1` test also fired on float noise around A == 1 and skipped those frames.
    constexpr float kInfiniteDepthEpsilon = 1.0e-3f;
    bool const      infiniteFar           = std::fabs(scaleZ - 1.0f) <= kInfiniteDepthEpsilon;
    float           farPlane              = std::numeric_limits<float>::infinity();
    if (!infiniteFar) {
        farPlane = scaleZ * nearPlane / (scaleZ - 1.0f);
        if (!(farPlane > nearPlane) || !std::isfinite(farPlane)) {
            *why = "far-not-derivable";
            return false;
        }
    }
    // |column 1| is cot(vertical fov / 2) and |column 0| / |column 1| is the projection's
    // own aspect, so the constants report the matrix's geometry rather than a second,
    // possibly different source.
    float const fov = 2.0f * std::atan(1.0f / scaleY);
    if (!(fov > 0.01f) || !(fov < 3.1f) || !std::isfinite(fov)) {
        *why = "fov-not-derivable";
        return false;
    }
    float const aspect = scaleX > 0.0f ? scaleY / scaleX : 0.0f;
    if (!(aspect > 0.01f) || !std::isfinite(aspect)) {
        *why = "aspect-not-derivable";
        return false;
    }
    for (unsigned i = 0; i < 3; ++i) {
        out->position[i] = snapshot.camera[i];
    }
    if (!std::isfinite(out->position[0]) || !std::isfinite(out->position[1]) || !std::isfinite(out->position[2])) {
        *why = "camera-position-not-finite";
        return false;
    }

    Mat4 inverseCurrent{};
    Mat4 inversePrevious{};
    if (!invert(current, &inverseCurrent) || !invert(previous, &inversePrevious)) {
        *why = "matrix-not-invertible";
        return false;
    }
    // The decoded matrix is the camera-relative view-projection: it maps camera-relative
    // world positions to clip. sl::Constants wants the *projection* (view -> clip), so the
    // camera rotation is divided out here: rows of R are the basis, view = v * R^T, and
    // P = R * M. Handing M over as cameraViewToClip would tell SL that the world axes are
    // the camera axes whenever the camera is not axis-aligned.
    Mat4 rotation{};
    for (unsigned i = 0; i < 3; ++i) {
        rotation.m[0 * 4 + i] = out->right[i];
        rotation.m[1 * 4 + i] = out->up[i];
        rotation.m[2 * 4 + i] = out->forward[i];
    }
    rotation.m[15] = 1.0f;
    out->cameraViewToClip = multiply(rotation, current);
    if (!invert(out->cameraViewToClip, &out->clipToCameraView)) {
        *why = "projection-not-invertible";
        return false;
    }
    // The projection itself must be free of rotation/translation: a diagonal xy scale
    // and the clip-z/w pair are already checked above through the basis and near/far.
    if (std::fabs(out->cameraViewToClip.m[1]) > 1.0e-3f || std::fabs(out->cameraViewToClip.m[2]) > 1.0e-3f
        || std::fabs(out->cameraViewToClip.m[4]) > 1.0e-3f || std::fabs(out->cameraViewToClip.m[6]) > 1.0e-3f
        || std::fabs(out->cameraViewToClip.m[12]) > 1.0e-3f || std::fabs(out->cameraViewToClip.m[13]) > 1.0e-3f) {
        *why = "projection-not-axis-aligned";
        return false;
    }
    float const delta[3] = {
        snapshot.camera[0] - snapshot.camera[3],
        snapshot.camera[1] - snapshot.camera[4],
        snapshot.camera[2] - snapshot.camera[5],
    };
    if (!std::isfinite(delta[0]) || !std::isfinite(delta[1]) || !std::isfinite(delta[2])) {
        *why = "camera-delta-not-finite";
        return false;
    }

    out->clipToPrevClip = multiply(multiply(inverseCurrent, translation(delta[0], delta[1], delta[2])), previous);
    if (!finite(out->clipToPrevClip) || !invert(out->clipToPrevClip, &out->prevClipToClip)) {
        *why = "reprojection-not-invertible";
        return false;
    }
    // prevClipToClip must undo clipToPrevClip; a large residual means the inverse went
    // wrong (degenerate matrix) and the frame's reprojection cannot be trusted.
    if (maxDifference(multiply(out->clipToPrevClip, out->prevClipToClip), identity()) > 1.0e-2f) {
        *why = "reprojection-inconsistent";
        return false;
    }
    // A still frame with identical matrices and identical origins must reproject onto
    // itself. That is the one geometric claim this decode makes that can be checked
    // without a reference image.
    bool const still = std::memcmp(current.m, previous.m, sizeof(current.m)) == 0
                    && delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f;
    if (still && maxDifference(out->clipToPrevClip, identity()) > 1.0e-3f) {
        *why = "still-frame-not-identity";
        return false;
    }

    out->nearPlane = nearPlane;
    out->farPlane  = farPlane;
    out->fov       = fov;
    out->aspect    = aspect;
    out->scaleZ    = scaleZ;
    return true;
}

bool describeTexture(void* resource, unsigned width, unsigned height, unsigned format) {
    if (!resource) {
        return false;
    }
    D3D12_RESOURCE_DESC const desc = static_cast<ID3D12Resource*>(resource)->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width && desc.Height == height
        && static_cast<unsigned>(desc.Format) == format && desc.MipLevels == 1;
}

unsigned const kAllShaderResource = static_cast<unsigned>(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

struct Resources {
    vv::sr::FrameSnapshot sr{};
    hudless::Frame        hud{};
};

// Everything the input tags need to be truthful: the SR frame's resources and matrices,
// the hudless copy of the same frame, and the desc/state each of them has right now.
// `why` carries the specific reason for the log, so a skipped frame names the exact
// check that failed instead of only its category.
Skip collect(Resources* out, char const** why) {
    if (!probe::vv::worldLoaded()) {
        *why = "no-world";
        return Skip::NoWorld;
    }
    if (!vv::sr::frameSnapshot(&out->sr) || !out->sr.serial) {
        *why = "no-sr-snapshot";
        return Skip::NoFreshSr;
    }
    hudless::Frame hud{};
    if (!hudless::frame(&hud)) {
        *why = "no-hudless-copy";
        return Skip::NoFreshHudless;
    }
    out->hud = hud;
    // Freshness is consumed here, whatever the outcome below: one frame's inputs are read
    // once, and a rejected frame must not be offered again. The lock is only around these
    // three comparisons - the SR/hudless queries above and the resource checks below do
    // not run under it.
    bool srFresh      = false;
    bool hudlessFresh = false;
    bool taaFresh     = false;
    {
        std::lock_guard lock(g.stateMutex);
        srFresh            = out->sr.serial != g.lastSrSerial;
        hudlessFresh       = hud.serial != g.lastHudlessSerial;
        taaFresh           = out->sr.taaSerial != g.lastTaaSerial;
        g.lastSrSerial     = out->sr.serial;
        g.lastHudlessSerial = hud.serial;
        g.lastTaaSerial    = out->sr.taaSerial;
    }
    if (!srFresh) {
        *why = "sr-not-recorded-this-frame";
        return Skip::NoFreshSr;
    }
    if (!hudlessFresh) {
        *why = "hudless-not-copied-this-frame";
        return Skip::NoFreshHudless;
    }
    if (out->sr.generation != probe::vv::renderGeneration()) {
        *why = "stale-generation";
        return Skip::NoFreshSr;
    }
    if (out->sr.renderWidth != probe::vv::renderWidth() || out->sr.renderHeight != probe::vv::renderHeight()) {
        *why = "stale-render-size";
        return Skip::NoFreshSr;
    }
    if (!out->sr.matrices) {
        *why = "no-taa-snapshot";
        return Skip::BadMatrices;
    }
    // The matrices must be this frame's: a snapshot that is still "valid" but was taken
    // before this frame's TAAU draw would reproject with the previous camera.
    if (!taaFresh) {
        *why = "stale-taa-snapshot";
        return Skip::BadMatrices;
    }
    if (out->sr.displayWidth != hud.width || out->sr.displayHeight != hud.height) {
        // The copy and the SR output describe different frames (a resize between the two
        // hooks). Tagging either would misdescribe the frame.
        *why = "size-mismatch";
        return Skip::BadResource;
    }
    if (out->sr.depthState != kAllShaderResource || out->sr.motionState != kAllShaderResource
        || hud.state != kAllShaderResource) {
        *why = "not-shader-readable";
        return Skip::BadResource;
    }
    if (!describeTexture(out->sr.depth, out->sr.renderWidth, out->sr.renderHeight, out->sr.depthFormat)) {
        *why = "depth-desc";
        return Skip::BadResource;
    }
    if (!describeTexture(out->sr.motion, out->sr.renderWidth, out->sr.renderHeight, out->sr.motionFormat)) {
        *why = "motion-desc";
        return Skip::BadResource;
    }
    if (!describeTexture(hud.texture, hud.width, hud.height, hud.format)) {
        *why = "hudless-desc";
        return Skip::BadResource;
    }
    *why = "ok";
    return Skip::None;
}

sl::Resource makeResource(void* native, unsigned width, unsigned height, unsigned format, unsigned state) {
    sl::Resource resource{sl::ResourceType::eTex2d, native, nullptr, nullptr, state};
    resource.width        = width;
    resource.height       = height;
    resource.nativeFormat = format;
    resource.mipLevels    = 1;
    resource.arrayLayers  = 1;
    return resource;
}

// Every required field is filled from the frame itself. The motion vectors are the
// engine's own render-pixel vectors (so mvecScale normalises them by the render size),
// and depth is standard Z with the near plane decoded from the matrix.
sl::Constants buildConstants(Decoded const& decoded, Resources const& resources) {
    sl::Constants constants{};
    constants.cameraViewToClip = toSl(decoded.cameraViewToClip);
    constants.clipToCameraView = toSl(decoded.clipToCameraView);
    constants.clipToPrevClip   = toSl(decoded.clipToPrevClip);
    constants.prevClipToClip   = toSl(decoded.prevClipToClip);
    // No lens distortion is applied by the engine at this point of its chain.
    constants.clipToLensClip = toSl(identity());

    constants.jitterOffset = sl::float2(resources.sr.jitter[0], resources.sr.jitter[1]);
    constants.mvecScale    = sl::float2(
        1.0f / static_cast<float>(resources.sr.renderWidth),
        1.0f / static_cast<float>(resources.sr.renderHeight)
    );
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(decoded.position[0], decoded.position[1], decoded.position[2]);
    constants.cameraUp  = sl::float3(decoded.up[0], decoded.up[1], decoded.up[2]);
    constants.cameraRight = sl::float3(decoded.right[0], decoded.right[1], decoded.right[2]);
    constants.cameraFwd   = sl::float3(decoded.forward[0], decoded.forward[1], decoded.forward[2]);
    constants.cameraNear  = decoded.nearPlane;
    constants.cameraFar   = decoded.farPlane;
    constants.cameraFOV   = decoded.fov;
    constants.cameraAspectRatio = decoded.aspect;

    constants.depthInverted        = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D      = sl::Boolean::eFalse;
    constants.reset                = resources.sr.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    // The engine's vectors are per render pixel, not dilated by an upscaler.
    constants.motionVectorsDilated  = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    return constants;
}

// Drops the previous frame's tags when this frame cannot supply its own. The SDK's form is
// a ResourceTag with a *null resource* ("set to null to remove the specified tag"), not a
// Resource whose native pointer is null. Called outside gStateMutex - it is an SL call -
// and a failure is reported, throttled, instead of being swallowed by the debug line.
void clearTagsIfMarked(Api const& api, sl::FrameToken const& token, unsigned sequence) {
    bool marked = false;
    {
        std::lock_guard lock(g.stateMutex);
        marked            = g.taggedLastFrame;
        g.taggedLastFrame = false;
    }
    if (!marked) {
        return;
    }
    sl::ResourceTag clears[3] = {
        {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
    };
    sl::ViewportHandle viewport{0};
    sl::Result const   result = api.setTagForFrame(token, viewport, clears, 3, nullptr);
    if (result == sl::Result::eOk) {
        ND_DEBUG("[fg] inputs: cleared tags frame={} ret=0", sequence);
        return;
    }
    unsigned const failures = ++g.clearFailures;
    if (failures <= 3) {
        utils::logger().warn(
            "[fg] inputs: frame={} clear=failed ret={} reason=tag-clear-failed",
            sequence,
            static_cast<int>(result)
        );
    }
}

// Log gate: the first three frames, every 600th, and every state/error change.
bool shouldLog(unsigned sequence, bool valid, unsigned code, int constantsResult, int tagResult, bool force) {
    bool const changed = !g.gateSeen || g.gateValid != valid || g.gateCode != code
                      || g.gateConstants != constantsResult || g.gateTags != tagResult;
    g.gateSeen      = true;
    g.gateValid     = valid;
    g.gateCode      = code;
    g.gateConstants = constantsResult;
    g.gateTags      = tagResult;
    if (force || sequence <= 3 || sequence % 600 == 0) {
        return true;
    }
    return changed;
}

// One line per interesting frame. Everything the contract asks to be able to read back -
// frame, token, constants result, tag result, DLSS-G status, dimensions, fence - is here,
// and a failure adds its code and reason. Returns whether this frame was logged, so the
// per-frame detail line can ride along instead of printing every frame.
bool logFrame(
    unsigned          sequence,
    uint32_t          tokenIndex,
    bool              valid,
    Skip              skip,
    int               constantsResult,
    int               tagResult,
    Resources const&  resources,
    unsigned          dlssgStatus,
    char const*       detail = nullptr,
    bool              force  = false
) {
    if (!shouldLog(sequence, valid, static_cast<unsigned>(skip), constantsResult, tagResult, force)) {
        return false;
    }
    uint64_t const fenceValue = g.fenceValue.load(std::memory_order_relaxed);
    void* const    fence      = g.fence.load(std::memory_order_relaxed);
    if (valid) {
        utils::logger().info(
            "[fg] inputs: frame={} token={} size={}x{} depth={:#x} mv={:#x} hud={:#x} const={} tag={} status={:#x} fence={}/{}",
            sequence,
            tokenIndex,
            resources.sr.renderWidth,
            resources.sr.renderHeight,
            reinterpret_cast<std::uintptr_t>(resources.sr.depth),
            reinterpret_cast<std::uintptr_t>(resources.sr.motion),
            reinterpret_cast<std::uintptr_t>(resources.hud.texture),
            constantsResult,
            tagResult,
            dlssgStatus,
            fence ? 1 : 0,
            fenceValue
        );
        return true;
    }
    // A failing frame is reported with its code and reason; the count of the same
    // failure is available in the gate, not in a line per frame.
    ++g.loggedFailures;
    utils::logger().warn(
        "[fg] inputs: frame={} token={} skip={} reason={} const={} tag={} status={:#x} fence={}/{}",
        sequence,
        tokenIndex,
        static_cast<unsigned>(skip),
        detail ? detail : skipReason(skip),
        constantsResult,
        tagResult,
        dlssgStatus,
        fence ? 1 : 0,
        fenceValue
    );
    return true;
}

void updateMode(bool validFrame) {
    int const desired = g.requestedEnabled.load(std::memory_order_relaxed) ? 1 : 0;
    if (desired && !validFrame) {
        return;
    }
    {
        std::lock_guard lock(g.stateMutex);
        if (g.attemptedMode == desired) {
            return;
        }
        // One attempt per transition, success or failure. A failed option call is never
        // retried every frame (EXP-0143); it is retried only after another transition.
        g.attemptedMode = desired;
    }
    streamline::setDlssgEnabled(desired != 0);
}

struct ModeUpdate {
    bool valid{};
    ~ModeUpdate() { updateMode(valid); }
};

} // namespace

void install() {
    // presentSequence is deliberately NOT reset: it is the DLSS-G frame index and has to
    // stay monotonic for the life of the process, including across a mod reload.
    g.readyLogged = false;
    g.requestedEnabled.store(config::get().fg.enabled, std::memory_order_relaxed);
    {
        std::lock_guard lock(g.stateMutex);
        g.taggedLastFrame = false;
        g.validFrames     = 0;
    }
    g.gateSeen = false;
}

void uninstall() {
    g.requestedEnabled.store(false, std::memory_order_relaxed);
    // invalidate() already clears the shared flags under gStateMutex.
    invalidate("uninstall");
}

void presentFrame(void* swapChain) {
    g.requestedEnabled.store(config::get().fg.enabled, std::memory_order_relaxed);
    // A failed token this flip must not leave the previous frame's token for the
    // markers; presentMarker() is called after this function returns.
    g.lastToken.store(nullptr, std::memory_order_release);
    ModeUpdate   modeUpdate;
    unsigned const sequence = g.presentSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard lock(g.stateMutex);
        g.validSequence = 0;
    }
    Api const api = refreshApi();

    if (!ready(api)) {
        if (shouldLog(sequence, false, static_cast<unsigned>(Skip::NotReady), 0, 0, false)) {
            utils::logger().info(
                "[fg] inputs: ready=0 frame={} reason={}",
                sequence,
                "exports-missing"
            );
        }
        return;
    }
    if (!g.readyLogged) {
        g.readyLogged = true;
        utils::logger().info(
            "[fg] inputs: ready=1 token=1 const=1 tag=1 state={} off={}",
            api.getState ? 1 : 0,
            streamline::dlssgOff() ? 1 : 0
        );
    }

    // DLSS-G requires the integration to call SwapChain::GetCurrentBackBufferIndex every
    // frame (DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled). The chain handed in is
    // the proxy the game presents through; the query happens here, before SL's present.
    UINT backBufferIndex = 0;
    bool haveIndex       = false;
    if (swapChain) {
        IDXGISwapChain3* chain3 = nullptr;
        if (SUCCEEDED(static_cast<IDXGISwapChain*>(swapChain)->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3) {
            backBufferIndex = chain3->GetCurrentBackBufferIndex();
            chain3->Release();
            haveIndex = true;
        }
    }
    if (!haveIndex) {
        unsigned const misses = ++g.indexMisses;
        if (misses <= 3) {
            utils::logger().warn(
                "[fg] inputs: GetCurrentBackBufferIndex unavailable (frame={} chain={:#x})",
                sequence,
                reinterpret_cast<std::uintptr_t>(swapChain)
            );
        }
    }

    // EXP-0189: let SL own the frame index. The previous explicit Present sequence could
    // make constants/tags valid calls that the proxy never associates with its Present.
    sl::FrameToken*  token    = nullptr;
    sl::Result const tokenResult = api.getToken(token, nullptr);
    if (tokenResult != sl::Result::eOk || !token) {
        logFrame(sequence, 0, false, Skip::TokenFailed, static_cast<int>(tokenResult), 0, Resources{}, 0);
        return;
    }
    uint32_t const tokenIndex = static_cast<uint32_t>(*token);
    g.lastToken.store(token, std::memory_order_release);

    // DLSS-G's input fence, read once per present. Kept even at eOff: the plugin reports
    // the fence it would use, and the marker must not assume it is 0. A failed query
    // drops the cached fence instead of leaving a stale one in place.
    unsigned dlssgStatus = 0;
    if (api.getState) {
        sl::ViewportHandle viewport{0};
        sl::DLSSGState     state{};
        sl::Result const   stateResult = api.getState(viewport, state, nullptr);
        if (stateResult == sl::Result::eOk) {
            void* const fence = state.inputsProcessingCompletionFence;
            g.fence.store(fence, std::memory_order_release);
            g.fenceValue.store(
                fence ? state.lastPresentInputsProcessingCompletionFenceValue : 0,
                std::memory_order_release
            );
            dlssgStatus = static_cast<unsigned>(state.status);
        } else {
            g.fence.store(nullptr, std::memory_order_release);
            g.fenceValue.store(0, std::memory_order_release);
        }
        if (stateResult != sl::Result::eOk || !g.lastStatusKnown || dlssgStatus != g.lastDlssgStatus) {
            g.lastStatusKnown = stateResult == sl::Result::eOk;
            g.lastDlssgStatus = dlssgStatus;
            ND_DEBUG(
                "[fg] inputs: dlssg state frame={} result={} status={:#x} presented={}",
                sequence,
                static_cast<int>(stateResult),
                dlssgStatus,
                state.numFramesActuallyPresented
            );
        }
    }

    Resources   resources{};
    char const* collectReason = "ok";
    Skip const  skip          = collect(&resources, &collectReason);
    if (skip != Skip::None) {
        // Whatever failed, the previous frame's tags must not survive as this frame's
        // description: an explicit clear makes "no input" a fact instead of an absence.
        clearTagsIfMarked(api, *token, sequence);
        logFrame(sequence, tokenIndex, false, skip, 0, 0, resources, dlssgStatus, collectReason);
        return;
    }

    char const* reason = "ok";
    Decoded     decoded{};
    if (!decodeFrame(resources.sr, &decoded, &reason)) {
        clearTagsIfMarked(api, *token, sequence);
        logFrame(sequence, tokenIndex, false, Skip::BadMatrices, 0, 0, resources, dlssgStatus, reason);
        return;
    }

    sl::Constants const constants = buildConstants(decoded, resources);
    sl::ViewportHandle  viewport{0};
    sl::Result const    constantsResult = api.setConstants(constants, *token, viewport);

    sl::Resource depth = makeResource(
        resources.sr.depth,
        resources.sr.renderWidth,
        resources.sr.renderHeight,
        resources.sr.depthFormat,
        resources.sr.depthState
    );
    sl::Resource motion = makeResource(
        resources.sr.motion,
        resources.sr.renderWidth,
        resources.sr.renderHeight,
        resources.sr.motionFormat,
        resources.sr.motionState
    );
    // EXP-0191 removed the HUDLessColor tag: DLSS-G uses the intercepted final
    // backbuffer (world + UI) as its colour input. The hudless copy is still collected
    // for frame freshness and resource lifecycle checks.
    sl::ResourceTag tags[2] = {
        {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
    };
    sl::Result tagResult = sl::Result::eErrorMissingInputParameter;
    if (constantsResult == sl::Result::eOk) {
        tagResult = api.setTagForFrame(*token, viewport, tags, 2, nullptr);
    }

    bool const ok = constantsResult == sl::Result::eOk && tagResult == sl::Result::eOk;
    if (ok) {
        modeUpdate.valid = true;
        // The first three frames the marker really marks are always logged: a run that
        // spends a few seconds in the world must still produce the success lines a smoke
        // assertion needs, not only the 600th frame's.
        unsigned marked = 0;
        {
            std::lock_guard lock(g.stateMutex);
            g.taggedLastFrame = true;
            g.validSequence   = sequence;
            marked            = ++g.validFrames;
        }
        bool const logged = logFrame(
            sequence,
            tokenIndex,
            true,
            Skip::None,
            static_cast<int>(constantsResult),
            static_cast<int>(tagResult),
            resources,
            dlssgStatus,
            nullptr,
            marked <= 3
        );
        // The constants detail rides on the same gate: it is what makes the decoded
        // near/far/fov/aspect checkable in a log, and it must not become one line per
        // frame in a run that lasts minutes.
        if (logged) {
            ND_DEBUG(
                "[fg] inputs: constants frame={} near={:.4f} far={:.2f} fov={:.4f} aspect={:.4f} scaleZ={:.6f} jitter={:.4f},{:.4f} bbi={} reset={} gen={}",
                sequence,
                decoded.nearPlane,
                decoded.farPlane,
                decoded.fov,
                decoded.aspect,
                decoded.scaleZ,
                resources.sr.jitter[0],
                resources.sr.jitter[1],
                haveIndex ? backBufferIndex : 0u,
                resources.sr.reset ? 1 : 0,
                resources.sr.generation
            );
        }
        return;
    }
    logFrame(
        sequence,
        tokenIndex,
        false,
        constantsResult != sl::Result::eOk ? Skip::ConstantsFailed : Skip::TagFailed,
        static_cast<int>(constantsResult),
        static_cast<int>(tagResult),
        resources,
        dlssgStatus
    );
    clearTagsIfMarked(api, *token, sequence);
}

void presentMarker(bool start) {
    Api const api = refreshApi();
    if (!api.pclSetMarker) {
        return;
    }
    sl::FrameToken* const token = g.lastToken.load(std::memory_order_acquire);
    if (!token) {
        return;
    }
    sl::Result const result =
        api.pclSetMarker(start ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd, *token);
    if (result != sl::Result::eOk) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
            utils::logger().warn(
                "[fg] inputs: PCL {} marker failed result={}",
                start ? "present-start" : "present-end",
                static_cast<int>(result)
            );
        }
    }
}

void reflexSleep() {
    if (!streamline::dlssgOn()) {
        return;
    }
    Api const api = refreshApi();
    if (!api.reflexSleep) {
        return;
    }
    sl::FrameToken* const token = g.lastToken.load(std::memory_order_acquire);
    if (!token) {
        return;
    }
    sl::Result const result = api.reflexSleep(*token);
    if (result != sl::Result::eOk) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
            utils::logger().warn(
                "[fg] inputs: Reflex sleep failed result={}",
                static_cast<int>(result)
            );
        }
    }
}

void awaitInputsFence(void* queue) {
    auto* const commandQueue = static_cast<ID3D12CommandQueue*>(queue);
    void* const fence        = g.fence.load(std::memory_order_acquire);
    uint64_t const value     = g.fenceValue.load(std::memory_order_acquire);
    if (!commandQueue || !fence || value == 0) {
        return;
    }
    // The SR prep pass is about to rewrite the shared Depth/MV textures; DLSS-G has told
    // us it may still be reading the frame that was tagged with them. A fence that is
    // already complete needs no queue wait at all, and that is the common case.
    auto* const      d3dFence = static_cast<ID3D12Fence*>(fence);
    uint64_t const   completed = d3dFence->GetCompletedValue();
    if (completed != UINT64_MAX && completed >= value) {
        return;
    }
    HRESULT const result = commandQueue->Wait(d3dFence, value);
    if (FAILED(result)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            utils::logger().warn("[fg] inputs: queue Wait on the DLSS-G fence failed hr=0x{:08X}", static_cast<unsigned>(result));
        }
        return;
    }
    // One line the first time a real wait is decided: a fence the plugin reports but never
    // reaches would look exactly like "no fence" without it.
    static bool loggedFirstWait = false;
    if (!loggedFirstWait) {
        loggedFirstWait = true;
        utils::logger().info(
            "[fg] inputs: queue wait on the DLSS-G fence value={} completed={}",
            value,
            completed
        );
    }
}

void setEnabled(bool enabled) { g.requestedEnabled.store(enabled, std::memory_order_relaxed); }

bool enabled() { return g.requestedEnabled.load(std::memory_order_relaxed); }

bool frameReady() {
    std::lock_guard lock(g.stateMutex);
    return g.requestedEnabled.load(std::memory_order_relaxed) && streamline::dlssgOn()
        && g.validSequence == g.presentSequence.load(std::memory_order_relaxed);
}

void invalidate(char const* why) {
    bool disable = false;
    {
        std::lock_guard lock(g.stateMutex);
        disable          = g.attemptedMode != 0;
        g.attemptedMode  = 0;
    }
    if (disable) {
        streamline::setDlssgEnabled(false);
    }
    g.fence.store(nullptr, std::memory_order_release);
    g.fenceValue.store(0, std::memory_order_release);
    // Whatever is published right now belongs to the old graph; the next frame's record
    // has to be a new one before it may be tagged. The two module queries take their own
    // locks, so they run before gStateMutex is taken - never inside it.
    vv::sr::FrameSnapshot snapshot{};
    unsigned const        srSerial  = vv::sr::frameSnapshot(&snapshot) ? snapshot.serial : 0;
    hudless::Frame        hud{};
    unsigned const        hudSerial = hudless::frame(&hud) ? hud.serial : 0;
    unsigned const        taaSerial = probe::vv::taaSnapshotSerial();
    {
        std::lock_guard lock(g.stateMutex);
        if (srSerial) {
            g.lastSrSerial = srSerial;
        }
        if (hudSerial) {
            g.lastHudlessSerial = hudSerial;
        }
        g.lastTaaSerial   = taaSerial;
        g.taggedLastFrame = false;
        g.validSequence   = 0;
    }
    if (why) {
        ND_DEBUG("[fg] inputs: invalidated ({})", why);
    }
}

} // namespace neurodragon::fg::inputs
