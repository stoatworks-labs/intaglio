/**
 * Intaglio — browser demo.
 *
 * Engraving as an effect. The one idea, from `AGENTS.md`: **an engraver cannot
 * choose a grey.** The plate has lines, and tone is made by how close together
 * they are and how fat each one is — a line swells where the tone darkens and
 * tapers where it lightens, solid at the dark limit and bare paper at the light
 * one. The lines follow a flow field measured from the picture (nib's structure
 * tensor, copied rather than re-derived), and the hatch coordinate that spaces
 * them evenly is an integral walked from the frame centre, in its own pass.
 *
 * ---------------------------------------------------- what is the plugin's own
 *
 * All of the picture. Intaglio is seven GPU passes and no CPU stage worth the
 * name: copy, tone (then its mip chain), structure tensor, a separable tensor
 * blur, the phase walk, the engraving and the composite. `VERTEX`, `COPY`,
 * `TONE`, `TENSOR`, `TENSOR_BLUR`, `FLOW_LIBRARY`, `PHASE_MAIN`, `ENGRAVE` and
 * `COMPOSITE` below are `kVertexShader` … `kCompositeShader` from
 * `source/Shaders.cpp`, copied across unedited, and `PHASE` is assembled the way
 * `PhaseShaderSource()` assembles it. `demo/tools/check_shaders.py` compares all
 * nine character for character, and the assembly order too, and
 * `tools/verify.sh` runs it.
 *
 * ---------------------------------------------------- what is a port
 *
 * The arithmetic between the host's 0..1 and the uniforms: every
 * `...FromParam` in `Controls.cpp`, and `PitchPixels`, `PhaseDivisor`,
 * `tapsFor`, `pow2AtMost` and `pow2AtLeast` from `Intaglio.cpp`, plus the pass
 * order and the uniforms `ProcessOpenGL` sets. Nothing checks those but a
 * reader. They are small and they are the difference between a plausible plate
 * and the plugin's plate, so they are ported line for line rather than
 * paraphrased. JavaScript does them in double where the plugin uses float; the
 * difference is far below anything a uniform can carry.
 *
 * ---------------------------------------------------- what is missing
 *
 * **Nothing audio.** Intaglio has no audio path, no FFT parameter and no beat
 * input, so there is no audio caveat to make.
 *
 * **Nothing temporal.** The plugin declares SetTimeSupported(false): the plate is
 * a pure function of the frame. The page's clock only moves the generated clip.
 *
 * **The About block is absent**, as on every page in this suite: a text line and
 * four buttons that open a browser.
 *
 * ---------------------------------------------------- decided, not asked
 *
 * **The clip list starts on the Synthetic scene, then Ramps and steps.** The
 * scene has the full tone range and moving structure, which is what a plate is
 * for; the ramps are the flat panels that separate this plugin from nib — a flat
 * grey must still be ruled, evenly, at the pitch and weight that grey calls for.
 *
 * **There is a line under the canvas** giving the ruling in lines and pixels and
 * the phase buffer the page sized from it — the same figures `igtest --bench`
 * prints — because the phase buffer being sized from the ruling rather than the
 * raster is the plugin's least visible decision.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The backticks inside the comments are escaped, because a template literal has
// nowhere else to put them; check_shaders.py decodes that one escape before
// comparing and rejects any other backslash, so the escape cannot hide a
// difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COPY = `#version 410 core

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
`;

const TONE = `#version 410 core

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
`;

const TENSOR = `#version 410 core

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
`;

const TENSOR_BLUR = `#version 410 core

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
`;

const FLOW_LIBRARY = `
uniform sampler2D FlowTexture;

//The local structure direction, unit length, plus two measures of how much to
//believe it.
//
//\`anisotropy\`, (l1 - l2) / (l1 + l2), is nib's: 1 where the tensor says one
//direction dominates completely, 0 where it has no opinion at all.
//
//\`coherence\` is sqrt( l1 - l2 ), and it is the one this plugin steers by.
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
`;

const PHASE_MAIN = `
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
`;

const ENGRAVE = `#version 410 core

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
	//bare. \`keep\` is the paper still showing.
	float keep = 1.0;

	for( int k = 0; k < 3; ++k )
	{
		if( k >= Sets )
			break;

		float e = engage[ k ];
		if( e <= 0.0 )
			continue;

		//The coverage this set must carry so the sets union to \`target\`
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

		//The edge. \`aa\` is held to both the half-width and the paper left
		//beside it, so the ramp is always symmetric about w -- which is what
		//keeps the mean coverage exactly 2w/pitch at any Taper, and what makes
		//a fully weighted line go solid instead of leaving a lattice of
		//half-lit gaps where the ramps meet.
		float aa = 0.5 + Taper * w;
		aa = min( aa, min( w, 0.5 * PitchPx - w ) );

		//\`distance\` and \`step\` are GLSL built-ins and shadowing one gives a
		//syntax error pointing into a file that does not exist, because these
		//shaders are assembled from strings. Hence distPx, and a comparison
		//rather than a call. The comparison is \`<=\` on purpose: at full weight
		//w is exactly half the pitch, and \`<\` would leave a one-pixel line of
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
`;

const COMPOSITE = `#version 410 core

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
`;

// Assembled exactly as PhaseShaderSource() assembles it: a version line, the
// flow library, the phase pass's own main. check_shaders.py checks this line.
const PHASE = `#version 410 core\n${FLOW_LIBRARY}${PHASE_MAIN}`;

//===========================================================================
// The port. Controls.cpp, then the helpers at the top of Intaglio.cpp.
//===========================================================================

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/// value^0 = low, value^1 = high, with every octave the same width of travel.
const geometric = (value, low, high) => low * Math.pow(high / low, value);
const linear = (value, low, high) => low + (high - low) * value;

const kPitchCoarse = 8.0;
const kPitchFine = 200.0;

/// Reversed: parameter up is pitch up is fewer lines.
const PitchLinesFromParam = (v) => geometric(1.0 - v, kPitchCoarse, kPitchFine);
const DetailFromParam = (v) => geometric(v, 0.0005, 0.012);
const SmoothingFromParam = (v) => geometric(v, 0.0005, 0.03);
const CoherenceFromParam = (v) => linear(v, 0.0, 120.0);
const AngleFromParam = (v) => linear(v, 0.0, 180.0);
const CurveFromParam = (v) => geometric(v, 0.25, 4.0);
const MinWeightFromParam = (v) => v;
const MaxWeightFromParam = (v) => v;
const TaperFromParam = (v) => v;
const CrossAngleFromParam = (v) => linear(v, 0.0, 180.0);
const EngageFromParam = (v) => v;
const EngageSoftnessFromParam = (v) => geometric(v, 0.01, 0.5);
const PlateToneFromParam = (v) => linear(v, 0.0, 0.35);
const BiteFromParam = (v) => linear(v, 0.0, 0.75);
const BurrFromParam = (v) => v;

// Controls.h and Shaders.h.
const kThirdSetGap = 0.22;
const kBurrReach = 0.28;
const kMaxSets = 3;
const kMaxTaps = 32;
const kMaxPhaseSide = 1024;

const kPi = 3.14159265358979324;

/// Taps needed to cover a Gaussian out to three sigma, held to the shader's loop.
const tapsFor = (sigma) => clamp(Math.ceil(sigma * 3.0), 1, kMaxTaps);

/// `while( p * 2 <= static_cast< int >( value ) )` -- the cast truncates.
function pow2AtMost(value) {
  let p = 1;
  while (p * 2 <= Math.trunc(value)) p *= 2;
  return p;
}

function pow2AtLeast(value) {
  let p = 1;
  while (p < value) p *= 2;
  return p;
}

/// IntaglioPlugin::PitchPixels.
const PitchPixels = (pitchParam, pictureHeight) =>
  Math.max(pictureHeight / Math.max(PitchLinesFromParam(pitchParam), 1.0), 1.0);

/// IntaglioPlugin::PhaseDivisor: the largest power of two at or below a third
/// of a pitch, so there are always three or four phase texels per pitch
/// whatever the raster -- floored by kMaxPhaseSide.
function PhaseDivisor(pitchPx, pictureWidth, pictureHeight) {
  const longest = Math.max(pictureWidth, pictureHeight);
  const floorDiv = pow2AtLeast(longest / kMaxPhaseSide);
  const wanted = pow2AtMost(Math.max(pitchPx / 3.0, 1.0));
  return clamp(Math.max(floorDiv, Math.min(wanted, 32)), 1, 64);
}

/// What the line under the canvas reports. Written by the renderer.
const telemetry = { lines: 0, pitchPx: 0, phase: '', divisor: 0, height: 0 };

//===========================================================================
// The passes, in ProcessOpenGL's order.
//===========================================================================

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const toneShader = new Program(gl, VERTEX, TONE, 'tone');
  const tensorShader = new Program(gl, VERTEX, TENSOR, 'tensor');
  const tensorBlurShader = new Program(gl, VERTEX, TENSOR_BLUR, 'tensor blur');
  const phaseShader = new Program(gl, VERTEX, PHASE, 'phase');
  const engraveShader = new Program(gl, VERTEX, ENGRAVE, 'engrave');
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');

  // ensureBuffers(), format for format. Every intermediate is floating point:
  // the tensor's components run to a few thousandths of the tone squared, and
  // the phase is a distance in pixels that runs to a couple of thousand, which
  // is why it alone is 32-bit.
  const copyBuffer = new PassBuffer(gl, { filter: 'linear' });
  const toneBuffer = new PassBuffer(gl, { filter: 'linear', mip: true });
  const tensorBuffer = new PassBuffer(gl, { filter: 'linear' });
  const tensorTemp = new PassBuffer(gl, { filter: 'linear' });
  const phaseBuffer = new PassBuffer(gl, { filter: 'linear' });
  const inkBuffer = new PassBuffer(gl, { filter: 'linear' });

  return {
    render({ input, params, width, height }) {
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // Physical units. Every length is a fraction of the frame height.
      //------------------------------------------------------------------
      const pictureHeight = height;
      const pitchPx = PitchPixels(p('pitch'), height);
      const detailPx = Math.max(DetailFromParam(p('detail')) * pictureHeight, 0.5);
      const smoothPx = Math.max(SmoothingFromParam(p('smoothing')) * pictureHeight, 0.5);

      const phaseDiv = PhaseDivisor(pitchPx, width, height);
      const phaseWidth = Math.max(1, Math.floor((width + phaseDiv - 1) / phaseDiv));
      const phaseHeight = Math.max(1, Math.floor((height + phaseDiv - 1) / phaseDiv));

      copyBuffer.ensure(width, height, gl.RGBA16F);
      toneBuffer.ensure(width, height, gl.R16F);
      tensorBuffer.ensure(width, height, gl.RGBA16F);
      tensorTemp.ensure(width, height, gl.RGBA16F);
      phaseBuffer.ensure(phaseWidth, phaseHeight, gl.RG32F);
      inkBuffer.ensure(width, height, gl.R16F);

      telemetry.lines = PitchLinesFromParam(p('pitch'));
      telemetry.pitchPx = pitchPx;
      telemetry.phase = `${phaseWidth} × ${phaseHeight}`;
      telemetry.divisor = phaseDiv;
      telemetry.height = height;

      const texelX = 1.0 / width;
      const texelY = 1.0 / height;

      gl.disable(gl.BLEND);

      // 1. Copy. The kit's clip fills its texture, so MaxUV is (1, 1).
      copyBuffer.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      copyShader.set('MaxUV', 1.0, 1.0);
      copyShader.set('HalfTexel', 0.5 * texelX, 0.5 * texelY);
      quad.draw();

      // 2. Tone, then its mip chain -- after the draw and before anything
      //    samples it.
      toneBuffer.bind();
      toneShader.use();
      bindTexture(gl, 0, copyBuffer.texture);
      toneShader.setSampler('CopyTexture', 0);
      toneShader.set('SourceMode', p('source'));
      quad.draw();
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      toneBuffer.generateMipmap();

      // 3. The structure tensor, at the Detail scale. MaxMipLevel() is
      //    floor( log2( max( w, h ) ) ).
      const maxMipLevel = Math.floor(Math.log2(Math.max(width, height)));
      tensorBuffer.bind();
      tensorShader.use();
      bindTexture(gl, 0, toneBuffer.texture);
      tensorShader.setSampler('ToneTexture', 0);
      tensorShader.set('TexelSize', texelX, texelY);
      tensorShader.set('Reach', detailPx);
      tensorShader.set('Level', clamp(Math.log2(Math.max(detailPx, 1.0)), 0.0, maxMipLevel));
      quad.draw();

      // 4. Smooth it, separably. Horizontal into the temp, vertical back.
      const taps = tapsFor(smoothPx);
      const axes = [
        { target: tensorTemp, origin: tensorBuffer, dx: texelX, dy: 0.0 },
        { target: tensorBuffer, origin: tensorTemp, dx: 0.0, dy: texelY },
      ];
      for (const axis of axes) {
        axis.target.bind();
        tensorBlurShader.use();
        bindTexture(gl, 0, axis.origin.texture);
        tensorBlurShader.setSampler('TensorTexture', 0);
        tensorBlurShader.set('Direction', axis.dx, axis.dy);
        tensorBlurShader.set('Sigma', smoothPx);
        tensorBlurShader.setInt('Taps', taps);
        quad.draw();
      }

      // 5. The phase: the two line integrals the hatch coordinate is made of.
      phaseBuffer.bind();
      phaseShader.use();
      bindTexture(gl, 0, tensorBuffer.texture);
      phaseShader.setSampler('FlowTexture', 0);
      phaseShader.set('PictureSize', width, height);
      phaseShader.set('Centre', 0.5 * width, 0.5 * height);
      phaseShader.set('BaseAngle', (AngleFromParam(p('angle')) * kPi) / 180.0);
      phaseShader.set('Belief', CoherenceFromParam(p('coherence')));
      phaseShader.set('StepPx', Math.max(0.5 * smoothPx, 1.0));
      quad.draw();

      // 6. The engraving.
      inkBuffer.bind();
      engraveShader.use();
      bindTexture(gl, 0, toneBuffer.texture);
      bindTexture(gl, 1, phaseBuffer.texture);
      engraveShader.setSampler('ToneTexture', 0);
      engraveShader.setSampler('PhaseTexture', 1);
      engraveShader.set('PitchPx', pitchPx);
      engraveShader.set('Gamma', CurveFromParam(p('curve')));
      engraveShader.set('MinWeight', MinWeightFromParam(p('minWeight')));
      engraveShader.set('MaxWeight', MaxWeightFromParam(p('maxWeight')));
      engraveShader.set('Taper', TaperFromParam(p('taper')));
      engraveShader.setInt('Sets', clamp(Math.round(p('sets')) + 1, 1, kMaxSets));
      engraveShader.set('CrossAngle', (CrossAngleFromParam(p('crossAngle')) * kPi) / 180.0);
      engraveShader.set('Engage', EngageFromParam(p('engage')));
      engraveShader.set('EngageSoft', EngageSoftnessFromParam(p('engageSoft')));
      engraveShader.set('ThirdGap', kThirdSetGap);
      engraveShader.set('PlateTone', PlateToneFromParam(p('plateTone')));
      engraveShader.set('Bite', BiteFromParam(p('bite')));
      engraveShader.set('Burr', BurrFromParam(p('burr')));
      engraveShader.set('BurrReach', kBurrReach);
      engraveShader.set('Invert', p('invert'));
      quad.draw();

      // 7. Composite, to the canvas. The kit bound it before calling us, and
      //    six framebuffers of other sizes have been bound since.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      compositeShader.use();
      bindTexture(gl, 0, inkBuffer.texture);
      bindTexture(gl, 1, copyBuffer.texture);
      compositeShader.setSampler('InkTexture', 0);
      compositeShader.setSampler('CopyTexture', 1);
      compositeShader.set('Ink', p('inkR'), p('inkG'), p('inkB'));
      compositeShader.set('Paper', p('paperR'), p('paperG'), p('paperB'));
      compositeShader.set('MixAmount', p('mix'));
      quad.draw();
    },
  };
}

//===========================================================================
// The controls, read out of IntaglioPlugin's constructor. Same names, same
// groups, same order, same defaults, same dropdown elements. Absent: the About
// block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({
  id, name, type: 'standard', default: def, group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const demo = mountDemo({
  name: 'Intaglio',
  pluginId: 'IG01',
  tagline:
    'Engraving. An engraver cannot choose a grey: the plate has lines, and tone is made by how close together they are and how fat each one is. A line swells where the picture darkens and tapers where it lightens — solid at the dark limit, bare paper at the light one — and the lines run along a flow field measured from the picture, so they curve with its structure. Cross-hatching adds a second and a third set at other angles when one set runs out of range. Every pass here is the plugin’s own GLSL.',
  repo: 'https://github.com/stoatworks-labs/intaglio',
  page: 'https://stoatworks-labs.com/software/intaglio/',
  video: 'https://www.youtube.com/watch?v=nMA4ogsfsdQ',

  // The print exists where the picture does, premultiplied: a logo on
  // transparency comes back as itself and not as a rectangle of paper.
  showBackdrop: true,

  // RGBA16F, R16F and an RG32F phase buffer, all rendered to.
  needFloat: true,

  params: [
    opt('source', 'Detect On', ['Luma', 'Red', 'Green', 'Blue', 'Saturation'], 0, 'Flow',
      'Which scalar the flow and the tone are read from. Luma is right most of the time; Saturation is for material whose subject is separated by colour rather than by tone.'),
    std('detail', 'Detail', 0.45, 'Flow', {
      display: (v) => `${(DetailFromParam(v) * 1080).toFixed(1)} px @1080`,
      hint: 'The gradient operator’s reach, as a fraction of the frame height: small finds every pore, large finds only the shapes.',
    }),
    std('smoothing', 'Flow Smoothing', 0.5, 'Flow', {
      display: (v) => `σ ${(SmoothingFromParam(v) * 1080).toFixed(1)} px @1080`,
      hint: 'The structure tensor’s smoothing. How far apart two pieces of structure have to be before they may disagree about which way the plate runs. A smoother field also shears the phase less along the walk.',
    }),
    std('coherence', 'Coherence', 0.3, 'Flow', {
      display: (v) => `gain ${CoherenceFromParam(v).toFixed(0)}`,
      hint: 'How much to believe the measured flow. At 0 the flow is ignored entirely and the plate is a plain global hatch at Angle Offset — which is what this effect would be without the idea in it, and is the A/B.',
    }),
    std('angle', 'Angle Offset', 0.25, 'Flow', {
      display: (v) => `${AngleFromParam(v).toFixed(0)}°`,
      hint: 'The plate’s own angle. Where the flow has no opinion, this is the angle the lines run at.',
    }),

    std('pitch', 'Line Pitch', 0.38, 'Line', {
      display: (v) => `${PitchLinesFromParam(v).toFixed(0)} lines`,
      hint: 'The ruling, in lines across the frame height — reversed, so turning it up widens the spacing. A fraction of the frame, not pixels: the same settings engrave the same drawing at any raster.',
    }),
    std('curve', 'Weight Curve', 0.67, 'Line', {
      display: (v) => `γ ${CurveFromParam(v).toFixed(2)}`,
      hint: 'Coverage is Min + (Max − Min) · darkness^γ. Above 1 the darks carry the detail.',
    }),
    std('minWeight', 'Min Weight', 0.0, 'Line', {
      display: (v) => MinWeightFromParam(v).toFixed(2),
      hint: 'The coverage floor. At 0 a white input leaves bare paper — one of the two limits that are the whole claim.',
    }),
    std('maxWeight', 'Max Weight', 1.0, 'Line', {
      display: (v) => MaxWeightFromParam(v).toFixed(2),
      hint: 'The coverage ceiling. At 1 a black input makes the plate solid.',
    }),
    std('taper', 'Taper', 0.35, 'Line', {
      display: (v) => TaperFromParam(v).toFixed(2),
      hint: 'The softness of a line’s edge as a fraction of its own half-width: 0 is a burin line, 1 a bitten one. It does not change the coverage — the ramp is symmetric about the nominal edge.',
    }),

    opt('sets', 'Sets', ['1', '2', '3'], 1, 'Cross-hatch',
      'How many hatch sets the plate may carry. The second arrives at Engage Threshold, the third a fixed 0.22 further into the shadows.'),
    std('crossAngle', 'Cross Angle', 1 / 3, 'Cross-hatch', {
      display: (v) => `${CrossAngleFromParam(v).toFixed(0)}°`,
      hint: 'The angle between one hatch set and the next. At 90° the third set lands on the first, and the plugin folds the two together so the tone stays exact.',
    }),
    std('engage', 'Engage Threshold', 0.52, 'Cross-hatch', {
      display: (v) => EngageFromParam(v).toFixed(2),
      hint: 'The darkness at which the second set starts to arrive.',
    }),
    std('engageSoft', 'Engage Softness', 0.64, 'Cross-hatch', {
      display: (v) => EngageSoftnessFromParam(v).toFixed(3),
      hint: 'The width of that arrival. The sets share the tone so their union is exact at any engagement, so a soft arrival does not step.',
    }),

    colour('inkR', 'Ink', 0.06, 'Plate', 'The ink.'),
    colour('inkG', 'Ink_Green', 0.05, 'Plate'),
    colour('inkB', 'Ink_Blue', 0.045, 'Plate'),
    colour('paperR', 'Paper', 0.95, 'Plate', 'The paper.'),
    colour('paperG', 'Paper_Green', 0.93, 'Plate'),
    colour('paperB', 'Paper_Blue', 0.87, 'Plate'),
    std('plateTone', 'Plate Tone', 0.11, 'Plate', {
      display: (v) => `${(PlateToneFromParam(v) * 100).toFixed(1)}% veil`,
      hint: 'The veil of ink a wiped plate keeps.',
    }),
    std('bite', 'Bite', 0.3, 'Plate', {
      display: (v) => `${(BiteFromParam(v) * 100).toFixed(0)}% of width`,
      hint: 'How irregular the bite is. Zero-mean, so it roughens a line without moving its coverage.',
    }),
    std('burr', 'Burr', 0.25, 'Plate', {
      display: (v) => BurrFromParam(v).toFixed(2),
      hint: 'The curl of metal a drypoint needle throws up on one side of the furrow. It holds extra ink, so one side of every line prints heavier.',
    }),

    std('mix', 'Mix', 1.0, 'Output'),
    bool('invert', 'Invert', 0, 'Output', 'Engrave the light instead of the dark: a white-line plate.'),
  ],

  sources: ['scene', 'ramp', 'grid', 'alpha', 'spot', 'bars', 'detail'],

  // The plugin ships no factory presets, so these are the page's own —
  // expressed entirely in the plugin's parameters and reachable with the sliders.
  presets: {
    'Plain global hatch (Coherence 0)': { coherence: 0 },
    'One set only': { sets: 0 },
    'Three sets': { sets: 2, engage: 0.4 },
    'Coarse plate': { pitch: 0.7, curve: 0.55 },
    'Fine plate': { pitch: 0.12 },
    'Burin: clean lines': { taper: 0.0, bite: 0.0, burr: 0.0, plateTone: 0.0 },
    'Drypoint: heavy burr': { burr: 0.9, bite: 0.6, taper: 0.6, plateTone: 0.3 },
    'White line (Invert)': { invert: 1, inkR: 0.9, inkG: 0.88, inkB: 0.82, paperR: 0.05, paperG: 0.05, paperB: 0.06 },
    'Blue-black on cream': { inkR: 0.08, inkG: 0.1, inkB: 0.22, paperR: 0.96, paperG: 0.92, paperB: 0.82 },
  },

  differences: [
    'Every pass on this page is the plugin’s own GLSL: copy, tone, structure tensor, tensor blur, phase walk, engraving and composite. demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the nine shader strings drifts, or if the phase pass stops being assembled in the plugin’s order.',
    'What is a port is the arithmetic around them — the Controls.cpp conversions and PitchPixels, PhaseDivisor and tapsFor from Intaglio.cpp — done here in JavaScript doubles where the plugin uses floats. Nothing checks that port but a reader.',
    'The phase buffer is sized from the ruling, not the raster, exactly as the plugin sizes it: the line under the picture gives the size and the divisor. At the finest rulings on a large composition the plugin’s own cap applies here too, and the finest lines alias there as they do in the host.',
    'There is no audio caveat, because Intaglio has no audio path; and nothing on this page is temporal, because the plate is a pure function of the frame. The clock only moves the generated clip.',
    'The plugin’s numerical proof — the ruling counted against width over pitch, the coverage at five tones against the weight curve, the two limits, and the angles the lines come out at, each at two rasters — is igtest in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The ruling line. Reports what the renderer sized; measures nothing. Skipped
// in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { lines, pitchPx, phase, divisor, height } = telemetry;
      if (!height) return;
      line.textContent =
        `Ruling: ${lines.toFixed(0)} lines across ${height} rows, one pitch every ${pitchPx.toFixed(1)} px. `
        + `Phase buffer ${phase} (the picture ÷ ${divisor}), sized from the ruling rather than the raster.`;
    }, 250);
  }
}
