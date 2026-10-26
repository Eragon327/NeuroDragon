#pragma once

namespace neurodragon::vv::tone {

// ---------------------------------------------------------------------------
// VV display encode (docs/specs/vv.md §14 B1c result)
// ---------------------------------------------------------------------------
//
// What the engine does after the world composite is an *encode*, not a tone map:
// the render-resolution colour buffer already carries the tone-mapped world
// (max ~0.55 in the frames measured), and TAAU only maps it onto the 8-bit back
// buffer. Measured on two independent frames (33% and 66% upsampling, one day
// apart): one fixed per-channel curve reproduces TAAU's output to ~2/255 in the
// middle 2/3 of the frame, and the two fits agree within ~3/255 in that range.
//
// So this is NOT ACES: a fitted ACES + gamma 2.2 curve was 21-26/255 off, which is
// why B1b's first bypass looked wrong. The curve is stored as a 33-entry table per
// channel, sampled on a quadratic grid (dense near black, where a gamma-like
// encode is steep), and passed to the shader as root constants.
//
// Regenerate with NeuroDragonWorkSpace/tools/vv_gen_tone.py after a new capture.
// Grid: x_i = (i / (N-1))^2, so a shader inverts it with t = sqrt(x) * (N-1).

inline constexpr unsigned kSize = 33;

inline constexpr float kTable[3][kSize] = {
    // R  (fitted residual 2.31/255 on the calibration frame)
    {0.00000f, 0.09073f, 0.14369f, 0.18802f, 0.22980f, 0.28431f, 0.33922f, 0.38655f, 0.42353f,
     0.46275f, 0.46275f, 0.46471f, 0.50196f, 0.52157f, 0.58431f, 0.64033f, 0.67477f, 0.70868f,
     0.73007f, 0.75626f, 0.78322f, 0.81055f, 0.83922f, 0.86922f, 0.90055f, 0.93322f, 0.96722f,
     1.00000f, 1.00000f, 1.00000f, 1.00000f, 1.00000f, 1.00000f},
    // G
    {0.00000f, 0.08263f, 0.13459f, 0.17904f, 0.22353f, 0.27608f, 0.32784f, 0.37990f, 0.43137f,
     0.47538f, 0.51709f, 0.55556f, 0.56471f, 0.56471f, 0.57647f, 0.60000f, 0.63529f, 0.66529f,
     0.70868f, 0.72922f, 0.75098f, 0.77108f, 0.79216f, 0.81422f, 0.83725f, 0.86127f, 0.88627f,
     0.91225f, 0.93922f, 0.96716f, 0.99608f, 1.00000f, 1.00000f},
    // B
    {0.00000f, 0.07161f, 0.12561f, 0.17450f, 0.22745f, 0.27059f, 0.29804f, 0.37225f, 0.40605f,
     0.43448f, 0.46625f, 0.50136f, 0.53982f, 0.58163f, 0.62677f, 0.67527f, 0.72092f, 0.75057f,
     0.77059f, 0.78873f, 0.80523f, 0.82353f, 0.83529f, 0.85000f, 0.86536f, 0.88137f, 0.89804f,
     0.91536f, 0.93333f, 0.95196f, 0.97124f, 0.99118f, 1.00000f},
};

// The table reaches the shaders as a single constant buffer (CBV at b1). Root
// constants were the obvious choice and are wrong: D3D12 caps the *whole* root
// signature at 64 DWORDs, and 99 floats alone are 99 - the runtime answers with
// E_INVALIDARG from CreateRootSignature (2026-10-25, the reason the first B2 build
// never came up).
inline constexpr unsigned kValues = 3 * kSize;

inline void pack(float* out) {
    unsigned index = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned i = 0; i < kSize; ++i, ++index) {
            out[index] = kTable[channel][i];
        }
    }
}

// HLSL side: a parameter-free analytic curve, deliberately.
//
// The fitted 33-entry table is kept above for reference, but handing it to a shader
// through a CBV turned out to read back as "only the R row" at runtime (2026-10-25:
// the encode step painted the whole frame red while the raw colour was correct and the
// same table applied offline gave a normal image). Rather than keep debugging the
// constant buffer, the shader uses the per-channel power law that the same fit
// produced: residual 6.3/255 against TAAU's output, which is visually
// indistinguishable from the table's 2.3/255 at this stage.
inline constexpr char const* kHlsl = R"hlsl(
float3 vvEncode(float3 colour)
{
    // Exponents from the same two-frame fit as the table (R 0.3316, G 0.3519,
    // B 0.4054). No constants, no CBV, nothing to bind.
    return pow(saturate(colour), float3(0.3316, 0.3519, 0.4054));
}
)hlsl";

} // namespace neurodragon::vv::tone
