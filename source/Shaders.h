#pragma once

/**
    The passes, as GLSL source.

        1. **copy**       picture size, RGBA16F. Resolves MaxUV once, so no
                          later pass has to think about it.
        2. **tone**       picture size, R16F, **mipmapped**. The one scalar
                          channel the plate is cut from. The mip chain is what
                          lets `Detail` choose the scale the gradient is taken
                          at, rather than always the pixel.
        3. **tensor**     picture size. The structure tensor (gxx, gyy, gxy) of
                          the tone at the Detail scale.
        4. **tensorBlur** picture size, run twice (H then V). Smooths the
                          *tensor*, which is the only correct way to average
                          orientations. Passes 2 to 4 are nib's, reused --
                          see ATTRIBUTIONS.md.
        5. **phase**      a REDUCED buffer, RG32F. The two line integrals that
                          make the hatch coordinate. This is the pass that is
                          this plugin's own, and the only expensive one.
        6. **engrave**    picture size, R16F. Lines, weights, cross-hatch,
                          bite, burr, plate tone -- out comes one number, the
                          ink coverage.
        7. **composite**  output size. Ink, paper, mix.

    ------------------------------------------------------- the hatch coordinate

    A hatch line is a curve that runs *along* the structure. To draw a whole
    family of them, evenly spaced, you need a scalar `h` whose level sets are
    those curves and whose gradient is one over the pitch -- and then the lines
    are `h = 0, 1, 2, ...` and the distance to the nearest one is
    `|h - round(h)| * pitch`.

    Such an `h` cannot be written down per pixel. `h = ( p . n ) / pitch`, with
    `n` the local normal to the flow, looks right and is not: rotate `n` by a
    degree and the phase at a point a thousand pixels from the origin moves by
    seventeen -- most of a pitch -- so the lines shatter wherever the flow
    turns. Measuring from the frame centre fixes the uniform case and the case
    where the picture's structure happens to be centred, and nothing else.

    So `h` is **integrated**, which is what the fifth pass is:

        A( p ) = integral of n . dq   along the straight ray from the centre to p
        B( p ) = integral of t . dq   along the same ray

    For a uniform field that gives exactly `( p - c ) . n` -- a perfect grating.
    For a field of concentric circles it gives exactly the radius, wherever the
    circles are centred, because along a ray `n . dq` is `d|r|`. Those are the
    two cases the harness measures, and the integral is exact in both.

    **One walk serves every hatch set.** A set at `phi` to the first has normal
    `cos( phi ) n + sin( phi ) t`, so its coordinate is
    `( cos( phi ) A + sin( phi ) B ) / pitch` -- no second integral.

    It is not exact in general, and it cannot be: an evenly spaced family of
    curves following an arbitrary direction field does not exist, which is why
    real engravings bifurcate. See AGENTS.md.

    ------------------------------------------------------------------ mirroring

    **The filtering is GPU-only.** There is no C++ mirror of the tensor
    decomposition, the tensor blur or the phase integral -- a mirrored
    per-pixel filter is a second implementation bought to restate the shader,
    and it would be tested against itself (orrery's precedent, followed across
    the fleet). What *is* mirrored, in `Controls.cpp` and marked
    `//= mirrored` on both sides, is the coverage arithmetic: the weight curve
    and the cross-hatch union. That is arithmetic, not filtering, and it is the
    thing `--weight` and `--limits` make an exact claim about.
*/

#include <string>

namespace intaglio
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kToneShader;
extern const char* const kTensorShader;
extern const char* const kTensorBlurShader;
extern const char* const kEngraveShader;
extern const char* const kCompositeShader;

/// The phase pass: the flow library plus its own main. Assembled rather than
/// written out twice, so there is exactly one place where "which way does the
/// plate run here" is answered.
std::string PhaseShaderSource();

/// The flow field written straight out as (dir.x, dir.y, anisotropy), built
/// from the same `flowAt` text the phase pass runs. Exists only for
/// `igtest --flow`, and assembled from the shared string rather than
/// reimplemented, so that what the measurement checks is the code that ships.
std::string FlowProbeShaderSource();

/// The largest number of taps the tensor blur will make to one side.
constexpr int kMaxTaps = 32;

/// The largest number of steps the phase walk will take. Past this the step
/// stretches rather than the loop growing: an unbounded loop driven straight
/// off a control is how a slider becomes a hang on somebody else's GPU.
constexpr int kMaxWalk = 256;

/// The longest side the phase buffer is allowed. It is sized from the ruling,
/// not from the raster -- two texels per pitch -- so the phase pass costs
/// roughly the same at 720p and at 4K. This cap is what stops a very fine
/// ruling at 4K from asking for a full-resolution walk.
constexpr int kMaxPhaseSide = 1024;

} // namespace intaglio
