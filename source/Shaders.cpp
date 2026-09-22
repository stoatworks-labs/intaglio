#include "Shaders.h"

namespace intaglio
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once in the copy pass instead, and
	//every pass after it works on a texture we allocated, where the picture
	//really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and that shows up as
	//a false edge running down the side of the frame -- which this plugin
	//would then dutifully cut a line along.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: tone. One scalar, which everything downstream is computed on.
// Mipmapped by the caller afterwards, because Detail reads it at a level.
//---------------------------------------------------------------------------
const char* const kToneShader = R"(#version 410 core

uniform sampler2D CopyTexture;
uniform float SourceMode;  //0 luma, 1 R, 2 G, 3 B, 4 saturation

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 c = texture( CopyTexture, uv );

	//Un-premultiply before looking at colour. The copy is premultiplied, so a
	//half-transparent red pixel arrives as a dark red one, and a channel or
	//saturation reading taken off it would be measuring the alpha instead.
	vec3 rgb = c.a > 0.0031 ? c.rgb / c.a : c.rgb;

	int mode = int( SourceMode + 0.5 );

	float value;
	if( mode == 1 )
		value = rgb.r;
	else if( mode == 2 )
		value = rgb.g;
	else if( mode == 3 )
		value = rgb.b;
	else if( mode == 4 )
	{
		float high = max( rgb.r, max( rgb.g, rgb.b ) );
		float low  = min( rgb.r, min( rgb.g, rgb.b ) );
		value = high > 0.0 ? ( high - low ) / high : 0.0;
	}
	else
		value = dot( rgb, vec3( 0.2126, 0.7152, 0.0722 ) );

	fragColor = vec4( value, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: the structure tensor, at the Detail scale.
//
// Adapted from nib, whose AGENTS.md is the long-form explanation. The short
// one: the thing that has to be averaged over a neighbourhood is an
// *orientation*, and orientations live on a half circle -- a line running at
// 179 degrees and one at 1 degree are very nearly parallel, but their vectors
// nearly cancel. The tensor J = g g' is quadratic in the gradient, so it takes
// the same value for g and -g, and averaging it averages orientation without
// the wrap.
//
// What is new here is Reach and Level. nib takes its gradient at the pixel and
// moves scale with the band-pass downstream; there is no band-pass downstream
// here, so Detail has to move the gradient itself. Widening the Sobel's arms
// without also reading a coarser mip would sample a fine texture at eight
// points a dozen pixels apart, which is not a coarse gradient -- it is an
// aliased fine one.
//---------------------------------------------------------------------------
const char* const kTensorShader = R"(#version 410 core

uniform sampler2D ToneTexture;
uniform vec2 TexelSize;
uniform float Reach;   //how far the Sobel's arms reach, in pixels
uniform float Level;   //the mip level that matches that reach

in vec2 uv;
out vec4 fragColor;

float toneAt( vec2 p )
{
	return textureLod( ToneTexture, p, Level ).r;
}

void main()
{
	vec2 t = TexelSize * Reach;

	float tl = toneAt( uv + vec2( -t.x,  t.y ) );
	float tc = toneAt( uv + vec2(  0.0,  t.y ) );
	float tr = toneAt( uv + vec2(  t.x,  t.y ) );
	float ml = toneAt( uv + vec2( -t.x,  0.0 ) );
	float mr = toneAt( uv + vec2(  t.x,  0.0 ) );
	float bl = toneAt( uv + vec2( -t.x, -t.y ) );
	float bc = toneAt( uv + vec2(  0.0, -t.y ) );
	float br = toneAt( uv + vec2(  t.x, -t.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );

	//Sobel's kernel sums to 8 over the ring; dividing here keeps the tensor's
	//eigenvalues in the same units as the tone, which is what lets the
	//anisotropy measure below be compared against a plain constant.
	gx *= 0.125;
	gy *= 0.125;

	fragColor = vec4( gx * gx, gy * gy, gx * gy, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 4: separable Gaussian on the tensor. Run twice, H then V.
//---------------------------------------------------------------------------
const char* const kTensorBlurShader = R"(#version 410 core

uniform sampler2D TensorTexture;
uniform vec2 Direction;   //one texel along the axis being blurred
uniform float Sigma;
uniform int Taps;         //half-width in taps, already clamped host-side

in vec2 uv;
out vec4 fragColor;

void main()
{
	float twoSigmaSq = 2.0 * Sigma * Sigma;

	vec3 sum = texture( TensorTexture, uv ).xyz;
	float norm = 1.0;

	for( int i = 1; i <= 32; ++i )
	{
		if( i > Taps )
			break;

		float d = float( i );
		float w = exp( -d * d / twoSigmaSq );

		sum += w * texture( TensorTexture, uv + Direction * d ).xyz;
		sum += w * texture( TensorTexture, uv - Direction * d ).xyz;
		norm += 2.0 * w;
	}

	fragColor = vec4( sum / norm, 1.0 );
}
)";

//---------------------------------------------------------------------------
// The flow, recovered from the smoothed tensor.
//
// **Copied from nib's kFlowLibrary**, which is the same repo's family and the
// same licence -- see ATTRIBUTIONS.md. It is copied rather than re-derived
// because a second eigen decomposition is a second thing to get wrong, and
// because "obeys the same flow field as nib" is a claim this plugin makes and
// would rather be able to keep.
//
// For a symmetric 2x2 [[E,F],[F,G]] the larger eigenvalue is
//
//     l1 = 0.5 * ( E + G + sqrt( (E-G)^2 + 4F^2 ) )
//
// and the *minor* eigenvector -- the direction in which the tone changes
// least, i.e. along the edge -- is perpendicular to the major one. Written
// directly as (l1 - E, -F), which needs no branch and degrades gracefully: in
// a flat region the tensor is zero, the vector is zero, and the caller gets
// the fallback rather than a normalize() of nothing.
//---------------------------------------------------------------------------
static const char* const kFlowLibrary = R"(
uniform sampler2D FlowTexture;

//The local structure direction, unit length, plus two measures of how much to
//believe it.
//
//`anisotropy`, (l1 - l2) / (l1 + l2), is nib's: 1 where the tensor says one
//direction dominates completely, 0 where it has no opinion at all.
//
//`coherence` is sqrt( l1 - l2 ), and it is the one this plugin steers by.
//nib only needs to know whether to TURN, so a normalised measure is right for
//it; a plate has to decide whether to curve its ruling at all, and anisotropy
//cannot tell it, because it is scale free. A gradient of one code value in a
//near-flat sky is just as directional as the edge of a building -- anisotropy
//reads near 1 for both -- so a plate steered by anisotropy alone curves its
//lines to follow the noise floor across every plain area in the picture.
//sqrt( l1 - l2 ) is the same judgement with the magnitude left in: it is a
//tone gradient per pixel, zero where the tone is flat, zero again where the
//directions disagree, and large only where there is real structure that
//really does run one way.
vec4 flowAt( vec2 p )
{
	vec3 j = texture( FlowTexture, p ).xyz;
	float E = j.x, G = j.y, F = j.z;

	float d = E - G;
	float disc = sqrt( max( d * d + 4.0 * F * F, 0.0 ) );

	float l1 = 0.5 * ( E + G + disc );
	float l2 = 0.5 * ( E + G - disc );

	vec2 t = vec2( l1 - E, -F );
	float len = length( t );

	//The fallback is deliberate and not arbitrary: with no measurable
	//structure there is no edge to follow. nib points everything the same way
	//so its line integral convolution becomes a plain 1D blur; here the
	//fallback is overridden by the caller, which blends toward the plate's own
	//Angle Offset in proportion to the anisotropy -- so a flat field rules an
	//exact grating at the angle the operator asked for.
	vec2 dir = len > 1e-9 ? t / len : vec2( 0.0, 1.0 );

	float sum = l1 + l2;
	float anisotropy = sum > 1e-9 ? ( l1 - l2 ) / sum : 0.0;
	float coherence = sqrt( max( l1 - l2, 0.0 ) );

	return vec4( dir, anisotropy, coherence );
}
)";

//---------------------------------------------------------------------------
// Pass 5: the phase. See Shaders.h for what it computes and why.
//
// Three things in here will cost time if they are changed without care.
//
// **An eigenvector has no sign.** `flowAt` may hand back a direction pointing
// 180 degrees the other way at the very next sample, for no reason but the
// arithmetic. The walk carries a frame and aligns every sample against where
// it was already going, exactly as nib's line integral convolution does --
// but here the consequence is worse than a weak blur. A frame that flips
// negates both increments from then on, so the integral comes back as the
// difference of two halves of a path instead of their sum, and the lines it
// produces have a seam along every ray where the flip happened.
//
// **The global direction has to be aligned too.** The blend toward Angle
// Offset is `mix( base, measured, belief )`, and `mix` of two nearly opposite
// unit vectors is nearly zero -- so `base` is flipped into the walk's current
// sense before the blend, not after.
//
// **The walk is a midpoint rule, not a left-endpoint one.** With the left
// endpoint every walk starts by sampling the flow exactly at the frame centre,
// which on a centred subject is the one place the flow is least well defined.
//---------------------------------------------------------------------------
static const char* const kPhaseMain = R"(
uniform vec2 PictureSize;  //the picture, in pixels
uniform vec2 Centre;       //where the walk starts, in picture pixels
uniform float BaseAngle;   //Angle Offset, radians
uniform float Belief;      //the Coherence gain on anisotropy
uniform float StepPx;      //how far apart the walk samples the flow, in pixels

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 target = uv * PictureSize;
	vec2 delta  = target - Centre;
	float len   = length( delta );

	vec2 t0 = vec2( cos( BaseAngle ), sin( BaseAngle ) );

	float A = 0.0;//across the flow: the hatch coordinate of the first set
	float B = 0.0;//along it: the coordinate of a set at ninety degrees

	if( len > 1e-6 )
	{
		float wanted = ceil( len / max( StepPx, 1e-3 ) );
		int steps = int( clamp( wanted, 1.0, 256.0 ) );

		vec2 walk = ( delta / float( steps ) );

		vec2 prev = t0;

		for( int i = 0; i < 256; ++i )
		{
			if( i >= steps )
				break;

			vec2 q = Centre + walk * ( float( i ) + 0.5 );
			vec4 f = flowAt( q / PictureSize );

			vec2 measured = f.xy;
			if( dot( measured, prev ) < 0.0 )
				measured = -measured;

			vec2 base = dot( t0, prev ) < 0.0 ? -t0 : t0;

			float belief = clamp( f.w * Belief, 0.0, 1.0 );
			vec2 tb = mix( base, measured, belief );
			float l = length( tb );
			tb = l > 1e-6 ? tb / l : base;

			//The normal, with a fixed handedness so that A and B name the same
			//two axes everywhere.
			vec2 nb = vec2( tb.y, -tb.x );

			A += dot( nb, walk );
			B += dot( tb, walk );

			prev = tb;
		}
	}

	fragColor = vec4( A, B, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 6: the engraving.
//
// Everything the plate is, in one pass, from two fetches: the tone here, and
// the phase pair. No flow is read -- the direction is already inside A and B,
// which is the whole point of having integrated them.
//
// The coverage arithmetic is mirrored in Controls.cpp; every mirrored line is
// marked on both sides.
//---------------------------------------------------------------------------
const char* const kEngraveShader = R"(#version 410 core

uniform sampler2D ToneTexture;
uniform sampler2D PhaseTexture;

uniform float PitchPx;      //one pitch, in pixels
uniform float Gamma;        //the weight curve
uniform float MinWeight;
uniform float MaxWeight;
uniform float Taper;
uniform int Sets;
uniform float CrossAngle;   //radians between one set and the next
uniform float Engage;
uniform float EngageSoft;
uniform float ThirdGap;
uniform float PlateTone;
uniform float Bite;
uniform float Burr;
uniform float BurrReach;
uniform float Invert;

in vec2 uv;
out vec4 fragColor;

//Integer hashing, PCG-style. Never fract( sin( x ) * 43758.5453 ): that is the
//driver's sin, and two machines disagree about which lines bit deeper.
uint hashInt( uint x )
{
	x = x * 747796405u + 2891336453u;
	uint w = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

float hash01( uint x )
{
	return float( hashInt( x ) ) * ( 1.0 / 4294967296.0 );
}

//How much this line is bitten here: -1 to 1, zero mean, varying smoothly ALONG
//the line and independently between lines. A per-pixel hash would be noise on
//the line's edge; a per-cell one would be a staircase. Two cells, interpolated
//by the same smoothstep the edges use, is a line that swells and thins.
float biteAt( int set, float lineIndex, float alongPitch )
{
	float cell = floor( alongPitch );
	float f    = alongPitch - cell;

	uint key = uint( int( lineIndex ) + 16384 ) * 1973u + uint( set ) * 104729u;
	float h0 = hash01( key + uint( int( cell ) + 16384 ) * 7919u );
	float h1 = hash01( key + uint( int( cell ) + 16385 ) * 7919u );

	float sm = f * f * ( 3.0 - 2.0 * f );
	return ( mix( h0, h1, sm ) - 0.5 ) * 2.0;
}

float engagementAt( int set, float darkness )
{
	if( set >= Sets )
		return 0.0;//= mirrored
	if( set == 0 )
		return 1.0;//= mirrored

	float t = Engage + ( set == 2 ? ThirdGap : 0.0 );//= mirrored
	float s = max( EngageSoft, 1e-4 );               //= mirrored

	float x = clamp( ( darkness - ( t - s ) ) / ( 2.0 * s ), 0.0, 1.0 );//= mirrored
	return x * x * ( 3.0 - 2.0 * x );                                   //= mirrored
}

void main()
{
	float luma = textureLod( ToneTexture, uv, 0.0 ).r;

	//Invert makes a white-line engraving: the burin cuts the highlights
	//instead of the shadows. It is not a colour swap -- the lines still follow
	//the picture's own structure, so the drawing is a different drawing and not
	//the same one photographed as a negative.
	float darkness = clamp( Invert > 0.5 ? luma : 1.0 - luma, 0.0, 1.0 );//= mirrored

	float g = pow( darkness, Gamma );//= mirrored
	float target = clamp( MinWeight + ( MaxWeight - MinWeight ) * g, 0.0, 1.0 );//= mirrored

	float engage[ 3 ];
	engage[ 0 ] = engagementAt( 0, darkness );
	engage[ 1 ] = engagementAt( 1, darkness );
	engage[ 2 ] = engagementAt( 2, darkness );

	//Two sets that rule at the same angle are ONE set. At a Cross Angle of
	//ninety degrees the third set lands exactly on the first, and asking each
	//of them for its own share prints that share twice in the same place: the
	//plate comes out eleven per cent short of the tone it was asked for.
	//Folding first is what keeps the union exact. Mirrored in Controls.cpp.
	for( int k = 1; k < 3; ++k )
	{
		if( k >= Sets || engage[ k ] <= 0.0 )
			continue;//= mirrored

		for( int j = 0; j < k; ++j )
		{
			if( engage[ j ] <= 0.0 )
				continue;//= mirrored

			float apart = mod( abs( float( k - j ) * CrossAngle ) * 180.0 / 3.14159265, 180.0 );//= mirrored
			apart = min( apart, 180.0 - apart );//= mirrored

			if( apart < 0.5 )//= mirrored
			{
				engage[ j ] += engage[ k ];//= mirrored
				engage[ k ] = 0.0;         //= mirrored
				break;                     //= mirrored
			}
		}
	}

	float engageSum = engage[ 0 ] + engage[ 1 ] + engage[ 2 ];

	vec2 ph = texture( PhaseTexture, uv ).rg;

	//The union of the sets, accumulated as the product of what each leaves
	//bare. `keep` is the paper still showing.
	float keep = 1.0;

	for( int k = 0; k < 3; ++k )
	{
		if( k >= Sets )
			break;

		float e = engage[ k ];
		if( e <= 0.0 )
			continue;

		//The coverage this set must carry so the sets union to `target`
		//exactly, whatever their engagement. See Controls.h.
		float rest = max( 1.0 - target, 0.0 );                //= mirrored
		float cover = engageSum > 1e-6                        //= mirrored
			? 1.0 - pow( rest, e / engageSum )                //= mirrored
			: 0.0;                                            //= mirrored

		float phi = float( k ) * CrossAngle;
		float cp = cos( phi );
		float sp = sin( phi );

		//One walk, every set: rotating the frame rotates the coordinate.
		float h     = ( cp * ph.x + sp * ph.y ) / PitchPx;
		float along = ( -sp * ph.x + cp * ph.y ) / PitchPx;

		float lineIndex = floor( h + 0.5 );
		float offsetPx  = ( h - lineIndex ) * PitchPx;//in (-pitch/2, pitch/2]
		float distPx    = abs( offsetPx );

		float w = 0.5 * cover * PitchPx;

		if( Bite > 0.0 )
			w *= 1.0 + Bite * biteAt( k, lineIndex, along );
		w = clamp( w, 0.0, 0.5 * PitchPx );

		//The edge. `aa` is held to both the half-width and the paper left
		//beside it, so the ramp is always symmetric about w -- which is what
		//keeps the mean coverage exactly 2w/pitch at any Taper, and what makes
		//a fully weighted line go solid instead of leaving a lattice of
		//half-lit gaps where the ramps meet.
		float aa = 0.5 + Taper * w;
		aa = min( aa, min( w, 0.5 * PitchPx - w ) );

		//`distance` and `step` are GLSL built-ins and shadowing one gives a
		//syntax error pointing into a file that does not exist, because these
		//shaders are assembled from strings. Hence distPx, and a comparison
		//rather than a call. The comparison is `<=` on purpose: at full weight
		//w is exactly half the pitch, and `<` would leave a one-pixel line of
		//bare paper running down the middle of a plate that is meant to be
		//solid.
		float ink = aa > 1e-6
			? 1.0 - smoothstep( w - aa, w + aa, distPx )
			: ( distPx <= w ? 1.0 : 0.0 );

		//The burr: on a drypoint the needle throws a curl of metal up on one
		//side of the furrow, and the curl holds ink. So one side of every line
		//prints heavier than the other, which is the thing that tells a
		//drypoint from an engraving across a room.
		if( Burr > 0.0 )
		{
			float reach = BurrReach * PitchPx;
			float shoulder = ( 1.0 - smoothstep( w, w + reach, offsetPx ) )
			               * ( offsetPx >= 0.0 ? 1.0 : 0.0 );
			ink = max( ink, Burr * shoulder );
		}

		keep *= 1.0 - clamp( ink, 0.0, 1.0 );
	}

	float coverage = 1.0 - keep;

	//Plate tone: the film of ink a wiped plate keeps. A veil over the paper,
	//so it unions with the lines rather than replacing them.
	coverage = coverage + PlateTone * ( 1.0 - coverage );

	fragColor = vec4( clamp( coverage, 0.0, 1.0 ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 7: ink, paper, mix.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D InkTexture;
uniform sampler2D CopyTexture;

uniform vec3 Ink;
uniform vec3 Paper;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	float coverage = clamp( texture( InkTexture, uv ).r, 0.0, 1.0 );
	vec4 source = texture( CopyTexture, uv );

	//An area average, which is exactly what ink on paper is at this scale: the
	//covered fraction is ink and the rest is paper. It is also what lets the
	//harness read the coverage straight back out of the frame -- black ink on
	//white paper prints 1 - coverage, with nothing in between to undo.
	vec3 printed = mix( Paper, Ink, coverage );

	//The print exists where the picture does. Premultiplied, so a logo on
	//transparency comes back as itself and not as a rectangle of paper.
	vec4 result = vec4( printed * source.a, source.a );

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly. The phase pass and the harness's probe are the flow library plus
// their own main, so the eigen decomposition has exactly one home.
//---------------------------------------------------------------------------
std::string PhaseShaderSource()
{
	return std::string( "#version 410 core\n" ) + kFlowLibrary + kPhaseMain;
}

std::string FlowProbeShaderSource()
{
	//No integral and no tone: just the flow, written out. Folding any of the
	//rest in would mean a disagreement could not be attributed to the
	//direction estimate, which is the only thing being measured.
	static const char* const probeMain = R"(
in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = flowAt( uv );
}
)";

	return std::string( "#version 410 core\n" ) + kFlowLibrary + probeMain;
}

} // namespace intaglio
