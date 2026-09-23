#pragma once

/**
    The host's parameters, and what they mean in physical units.

    Every numeric parameter the host sees is a plain 0..1 float, because
    `SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
    `SetParamRange` could widen it -- so a control that stands for a ruling in
    lines, or an angle in degrees, cannot declare one as its default. The
    conversions all live in Controls.cpp, one function per control, and the
    shader is handed the physical value.

    Option and boolean parameters are the exception: they hold the element
    value itself.

    ------------------------------------------------------------ lengths

    **Every length in this plugin is a fraction of the frame height, not a
    number of pixels.** That is the one decision the rest of the geometry hangs
    off. A copper plate has a ruling -- so many lines across the plate -- and
    the plate is the picture, not the monitor. Expressing the ruling in pixels
    would mean the same clip engraved at 720p and at 4K came back as two
    different drawings, one four times finer than the other, and an operator
    who set a look on a preview monitor would not get it on the wall.

    Because Line Pitch, Detail and Flow Smoothing all scale together, the whole
    model is **exactly scale-invariant**: the same input, rendered at any
    raster, produces the same picture up to sampling. `igtest --pitch` and
    `igtest --weight` both run at two rasters and fail if the two disagree,
    which is what keeps that true.
*/

namespace intaglio
{
/**
    Parameter ids.

    **Append only.** Two separate things depend on this order:
    `SetParamGroup` collapses runs of consecutive same-group ids, so inserting
    an id mid-enum silently splits a group in two; and every saved composition
    stores parameters by index, so a renumber rewrites what an operator's old
    project means.
*/
enum ParamId : unsigned int
{
	// -- Flow ---------------------------------------------------------------
	// Which way the burin runs. This group is nib's machinery, reused: the
	// structure tensor, smoothed as a tensor rather than as an angle. See
	// AGENTS.md and ATTRIBUTIONS.md.
	PT_SOURCE = 0,
	PT_DETAIL,
	PT_SMOOTHING,
	PT_COHERENCE,
	PT_ANGLE,

	// -- Line ---------------------------------------------------------------
	// The ruling, and how tone becomes width.
	PT_PITCH,
	PT_CURVE,
	PT_MIN_WEIGHT,
	PT_MAX_WEIGHT,
	PT_TAPER,

	// -- Cross-hatch --------------------------------------------------------
	PT_SETS,
	PT_CROSS_ANGLE,
	PT_ENGAGE,
	PT_ENGAGE_SOFT,

	// -- Plate --------------------------------------------------------------
	PT_INK_R,
	PT_INK_G,
	PT_INK_B,
	PT_PAPER_R,
	PT_PAPER_G,
	PT_PAPER_B,
	PT_PLATE_TONE,
	PT_BITE,
	PT_BURR,

	// -- Output -------------------------------------------------------------
	PT_MIX,
	PT_INVERT,

	// -- The Stoatworks About block -----------------------------------------
	//
	// One display-only text line, then one button per link the block carries.
	// How many buttons there are is decided by which URLs StoatworksAbout.h
	// actually holds, so Intaglio.cpp static_asserts this run against
	// `about::kParamCount` -- writing a user guide later adds one, and without
	// the assert that would silently shift PT_COUNT and leave the last button
	// undeclared.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,
	PT_COUNT
};

/// Which scalar the flow and the tone are read from.
enum class Source
{
	Luma = 0,   ///< Rec.709 luminance. The default, and right most of the time.
	Red,
	Green,
	Blue,
	Saturation, ///< For material whose subject is separated by colour, not tone.

	Count
};

/// How many hatch sets the plate may carry.
constexpr int kMaxSets = 3;

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape, because "geometric" and
// "linear" are the difference between a control that is usable across its
// whole travel and one that does everything in the last tenth.
//---------------------------------------------------------------------------

/// The ruling, in **lines across the frame height**: 8 at the coarse end,
/// 200 at the fine end, geometrically. Reversed, so that turning Line Pitch
/// up widens the spacing -- the control is named for the pitch, not for the
/// count.
float PitchLinesFromParam( float value );

/// The inverse, so a test can ask for a ruling by name. Not used by the
/// plugin; it exists so `igtest` can say "24 lines" instead of "0.63".
float ParamFromPitchLines( float lines );

/// The gradient operator's reach, as a fraction of the frame height:
/// 0.0005 to 0.012, geometrically. At 1080 that is half a pixel to thirteen.
/// Small finds every pore; large finds only the shapes.
float DetailFromParam( float value );

/// The structure tensor's smoothing sigma, as a fraction of the frame height:
/// 0.0005 to 0.03, geometrically. How far apart two pieces of structure have
/// to be before they are allowed to disagree about which way the plate runs.
float SmoothingFromParam( float value );

/// How much to believe the measured flow, as a gain on the tensor's coherent
/// strength `sqrt( l1 - l2 )` -- a tone gradient per pixel: 0 to 120, linear.
/// At 120 a gradient of one code value per pixel is already believed
/// completely; at 24 it takes about five.
///
/// **At 0 the flow is ignored entirely** and the plate is a plain global hatch
/// at Angle Offset -- which is what this effect would be without the idea in
/// it, and is the A/B.
float CoherenceFromParam( float value );

/// The plate's own angle, 0 to 180 degrees, linear. Where the flow has no
/// opinion, this is the angle the lines run at.
float AngleFromParam( float value );

/// The weight curve's exponent, 0.25 to 4.0 geometrically, so the midpoint of
/// the control is exactly 1.0 and the two halves are reciprocal. Coverage is
/// `Min + ( Max - Min ) * pow( darkness, gamma )`.
float CurveFromParam( float value );

/// The coverage floor and ceiling, 0 to 1, linear. **The two limits are the
/// whole claim**: at Max Weight 1 a black input makes the plate solid, and at
/// Min Weight 0 a white input leaves bare paper.
float MinWeightFromParam( float value );
float MaxWeightFromParam( float value );

/// The softness of a line's edge as a fraction of its own half-width, 0 to 1,
/// linear. 0 is a burin line with one pixel of antialiasing; 1 is a bitten
/// line with no hard edge left. It does not change the coverage: the ramp is
/// symmetric about the nominal half-width, so what it takes off one side it
/// puts back on the other -- which is why `--weight` holds at any Taper.
float TaperFromParam( float value );

/// The angle between one hatch set and the next, 0 to 180 degrees, linear.
/// Set k runs at `Angle Offset + k * Cross Angle`.
float CrossAngleFromParam( float value );

/// The darkness at which the second set starts to arrive, 0 to 1, linear, and
/// the width of that arrival, 0.01 to 0.5 geometrically. The third set arrives
/// at `Engage Threshold + kThirdSetGap` further into the shadows.
float EngageFromParam( float value );
float EngageSoftnessFromParam( float value );

/// How much further into the shadows the third set arrives than the second.
constexpr float kThirdSetGap = 0.22f;

/// The veil of ink a wiped plate keeps, as coverage: 0 to 0.35, linear.
float PlateToneFromParam( float value );

/// How irregular the bite is, as a fraction of a line's width: 0 to 0.75,
/// linear. Zero-mean, so it roughens a line without moving its coverage.
float BiteFromParam( float value );

/// The burr's density and reach. The burr is the curl of metal a drypoint
/// needle throws up on one side of the furrow; it holds extra ink, so one side
/// of every line prints heavier. 0 to 1, linear.
float BurrFromParam( float value );

/// The width of the burr's shoulder, as a fraction of the pitch.
constexpr float kBurrReach = 0.28f;

//---------------------------------------------------------------------------
// The model, mirrored on the CPU.
//
// Only the *coverage bookkeeping* is mirrored -- the part that is arithmetic
// rather than filtering. There is no C++ copy of the tensor decomposition, the
// tensor blur or the phase integral: a mirrored per-pixel filter is a second
// implementation bought to restate the shader, and it would be tested against
// itself (orrery's precedent, followed across the fleet). What is mirrored is
// what `--limits` and `--weight` need to state their claim about the *model*
// as well as about the render.
//---------------------------------------------------------------------------

/// Total ink coverage the plate is asked for at this darkness.
float TargetCoverage( float darkness, float gamma, float minWeight, float maxWeight );

/// How engaged set `set` (0-based) is at this darkness. Set 0 is always 1.
float SetEngagement( int set, int sets, float darkness, float engage, float softness );

/// Fold sets that rule at the same angle into one.
///
/// Set k runs at `k * Cross Angle`, so at a Cross Angle of ninety degrees the
/// third set lands exactly on the first -- same direction, same phase, same
/// lines. Asking each of them for its own share of the tone prints that share
/// twice in the same place, and the plate comes out eleven per cent short of
/// the tone it was asked for (measured, before this existed). Folding their
/// engagements together first is what keeps the union exact.
///
/// `engagement` is three entries, in place. Only two Cross Angles are
/// degenerate -- zero and ninety -- so this does nothing at all the rest of
/// the time.
void FoldCoincidentSets( float engagement[ 3 ], int sets, float crossAngleDegrees );

/// The coverage one set must carry so that `sets` sets, engaged as
/// `SetEngagement` says, union to exactly `target`.
///
/// The union of independent sets is `1 - prod( 1 - c_k )`, so asking each
/// engaged set for `1 - pow( 1 - target, e_k / sum( e ) )` makes the product
/// telescope to `1 - target` for *any* pattern of engagement -- including the
/// fractional engagements in the middle of a threshold, where a naive
/// `target / sets` would visibly step.
float SetCoverage( float target, float engagement, float engagementSum );

} // namespace intaglio
