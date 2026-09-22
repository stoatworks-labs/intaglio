#include "Intaglio.h"

#include "Diag.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;

namespace intaglio
{
namespace
{
constexpr float kPi = 3.14159265358979324f;

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[] = { "Luma", "Red", "Green", "Blue", "Saturation" };
const char* const kSetsNames[]   = { "1", "2", "3" };

/// Taps needed to cover a Gaussian out to where it stops contributing. Three
/// sigma is the usual answer and is not arbitrary: past it the weight is under
/// 1.2%, which is below the quantisation of the buffers being summed.
int tapsFor( float sigma )
{
	return std::clamp( static_cast< int >( std::ceil( sigma * 3.0f ) ), 1, kMaxTaps );
}

int pow2AtMost( float value )
{
	int p = 1;
	while( p * 2 <= static_cast< int >( value ) )
		p *= 2;
	return p;
}

int pow2AtLeast( float value )
{
	int p = 1;
	while( static_cast< float >( p ) < value )
		p *= 2;
	return p;
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

//---------------------------------------------------------------------------
float IntaglioPlugin::PitchPixels( float pitchParam, int pictureHeight )
{
	const float lines = PitchLinesFromParam( pitchParam );
	return std::max( static_cast< float >( pictureHeight ) / std::max( lines, 1.0f ), 1.0f );
}

//---------------------------------------------------------------------------
// How coarse the phase buffer may be.
//
// The phase is a smooth field -- it is an integral -- so it does not need the
// picture's resolution, it needs the *ruling's*. Two texels per pitch is the
// Nyquist rate for a field whose interesting variation is one cycle per pitch,
// and it is what this returns: the largest power of two at or below half a
// pitch.
//
// The consequence is the useful part. The phase pass is the only expensive one
// here, and sizing it from the ruling rather than from the raster means it
// costs about the same at 720p and at 4K -- a 90-line plate gets a 480x270
// phase buffer at either. `igtest --bench` is where that shows.
//
// The floor is the exception, and it is a real limit rather than a tidy one:
// a very fine ruling on a very large frame would ask for a phase buffer close
// to full resolution and a walk hundreds of steps long. kMaxPhaseSide caps it,
// and past that cap the phase is sampled below the ruling's Nyquist rate and
// the finest lines alias. See AGENTS.md.
//---------------------------------------------------------------------------
int IntaglioPlugin::PhaseDivisor( float pitchPx, int pictureWidth, int pictureHeight )
{
	const int longest = std::max( pictureWidth, pictureHeight );
	const int floorDiv =
		pow2AtLeast( static_cast< float >( longest ) / static_cast< float >( kMaxPhaseSide ) );

	const int wanted = pow2AtMost( std::max( pitchPx / 3.0f, 1.0f ) );

	return std::clamp( std::max( floorDiv, std::min( wanted, 32 ) ), 1, 64 );
}

//---------------------------------------------------------------------------
IntaglioPlugin::IntaglioPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Nothing here depends on time. The plate is a pure function of the frame:
	//no history, no animation, no ping-pong. Saying so stops a host calling
	//SetTime sixty times a second into a function that discards it, and it is
	//also the honest answer to `oxbow probe`, which prints it.
	SetTimeSupported( false );

	//-------------------------------------------------------------------
	// Defaults. Chosen so that dropping the effect on ordinary footage
	// prints something that reads as an engraving straight away -- a
	// plate whose default is bare paper reads as broken.
	//-------------------------------------------------------------------
	params[ PT_SOURCE ] = static_cast< float >( Source::Luma );

	params[ PT_DETAIL ]    = 0.45f;//about 2.6 px at 1080: structure, not grain
	params[ PT_SMOOTHING ] = 0.50f;//about 4.2 px at 1080
	params[ PT_COHERENCE ] = 0.30f;//gain 36: believes structure of about 3 code values a pixel
	params[ PT_ANGLE ]     = 0.25f;//45 degrees, the engraver's default lay

	params[ PT_PITCH ]      = 0.38f;//about 59 lines across the frame
	params[ PT_CURVE ]      = 0.67f;//gamma 1.6: the darks carry the detail
	params[ PT_MIN_WEIGHT ] = 0.0f;
	params[ PT_MAX_WEIGHT ] = 1.0f;
	params[ PT_TAPER ]      = 0.35f;

	params[ PT_SETS ]          = 1.0f;//element 1 == two sets
	params[ PT_CROSS_ANGLE ]   = 1.0f / 3.0f;//60 degrees
	params[ PT_ENGAGE ]        = 0.52f;
	params[ PT_ENGAGE_SOFT ]   = 0.64f;//about 0.12 of the tone range

	params[ PT_INK_R ] = 0.06f;
	params[ PT_INK_G ] = 0.05f;
	params[ PT_INK_B ] = 0.045f;

	params[ PT_PAPER_R ] = 0.95f;
	params[ PT_PAPER_G ] = 0.93f;
	params[ PT_PAPER_B ] = 0.87f;

	params[ PT_PLATE_TONE ] = 0.11f;//about 4% coverage: a wiped plate, not a clean one
	params[ PT_BITE ]       = 0.30f;
	params[ PT_BURR ]       = 0.25f;

	params[ PT_MIX ]    = 1.0f;
	params[ PT_INVERT ] = 0.0f;

	//-------------------------------------------------------------------
	// Declaration. Every numeric parameter is a plain 0..1 float even where
	// it stands for an angle in degrees or a ruling in lines: SetParamInfo
	// clamps a standard default into 0..1 before SetParamRange could widen
	// it, so the ranges live in Controls.cpp and nowhere else.
	//-------------------------------------------------------------------
	SetOptionParamInfo( PT_SOURCE, "Detect On", static_cast< int >( Source::Count ), params[ PT_SOURCE ] );
	for( int i = 0; i < static_cast< int >( Source::Count ); ++i )
		SetParamElementInfo( PT_SOURCE, i, kSourceNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_DETAIL, "Detail", FF_TYPE_STANDARD );
	SetParamInfof( PT_SMOOTHING, "Flow Smoothing", FF_TYPE_STANDARD );
	SetParamInfof( PT_COHERENCE, "Coherence", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE, "Angle Offset", FF_TYPE_STANDARD );

	SetParamInfof( PT_PITCH, "Line Pitch", FF_TYPE_STANDARD );
	SetParamInfof( PT_CURVE, "Weight Curve", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIN_WEIGHT, "Min Weight", FF_TYPE_STANDARD );
	SetParamInfof( PT_MAX_WEIGHT, "Max Weight", FF_TYPE_STANDARD );
	SetParamInfof( PT_TAPER, "Taper", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SETS, "Sets", kMaxSets, params[ PT_SETS ] );
	for( int i = 0; i < kMaxSets; ++i )
		SetParamElementInfo( PT_SETS, i, kSetsNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_CROSS_ANGLE, "Cross Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_ENGAGE, "Engage Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_ENGAGE_SOFT, "Engage Softness", FF_TYPE_STANDARD );

	//Consecutive red/green/blue parameters are what a host needs to show a
	//swatch rather than three sliders.
	SetParamInfof( PT_INK_R, "Ink", FF_TYPE_RED );
	SetParamInfof( PT_INK_G, "Ink_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_INK_B, "Ink_Blue", FF_TYPE_BLUE );

	SetParamInfof( PT_PAPER_R, "Paper", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );

	SetParamInfof( PT_PLATE_TONE, "Plate Tone", FF_TYPE_STANDARD );
	SetParamInfof( PT_BITE, "Bite", FF_TYPE_STANDARD );
	SetParamInfof( PT_BURR, "Burr", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );

	//-------------------------------------------------------------------
	// Groups. SetParamGroup collapses runs of consecutive same-group ids, so
	// this depends entirely on the id order in Controls.h -- reorder that and
	// a group silently splits in two.
	//-------------------------------------------------------------------
	for( unsigned int id = PT_SOURCE; id <= PT_ANGLE; ++id )
		SetParamGroup( id, "Flow" );
	for( unsigned int id = PT_PITCH; id <= PT_TAPER; ++id )
		SetParamGroup( id, "Line" );
	for( unsigned int id = PT_SETS; id <= PT_ENGAGE_SOFT; ++id )
		SetParamGroup( id, "Cross-hatch" );
	for( unsigned int id = PT_INK_R; id <= PT_BURR; ++id )
		SetParamGroup( id, "Plate" );
	for( unsigned int id = PT_MIX; id <= PT_INVERT; ++id )
		SetParamGroup( id, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

//---------------------------------------------------------------------------
FFResult IntaglioPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();

	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Held in a local so the pointer handed to Compile outlives the call.
	const std::string phaseSource = PhaseShaderSource();

	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &toneShader, kToneShader, "tone" },
		{ &tensorShader, kTensorShader, "tensor" },
		{ &tensorBlurShader, kTensorBlurShader, "tensor blur" },
		{ &phaseShader, phaseSource.c_str(), "phase" },
		{ &engraveShader, kEngraveShader, "engrave" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the plugin
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Intaglio: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool IntaglioPlugin::ensureBuffers( GLsizei width, GLsizei height, GLsizei phaseWidth, GLsizei phaseHeight )
{
	//Every intermediate is floating point. The tensor is a product of
	//gradients, so its components run to a few thousandths of the tone
	//squared; eight bits would quantise the whole field to zero in anything
	//but a hard edge.
	//
	//The phase is the one that has to be 32-bit rather than 16. It is a
	//distance in pixels measured from the frame centre, so it runs to a couple
	//of thousand; a half float has eleven bits of mantissa, which at that
	//magnitude is a quantum of about one pixel. One pixel is a tenth of a
	//pitch. The lines would step.
	const bool ok =
		copyBuffer.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		//Mipmapped, because Detail reads the tone at a level rather than at a
		//pixel -- a wide Sobel over an un-reduced texture is an aliased fine
		//gradient, not a coarse one.
		&& toneBuffer.Ensure( width, height, GL_R16F, PassBuffer::Sampling::Mipmapped )
		&& tensorBuffer.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& tensorTemp.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& phaseBuffer.Ensure( phaseWidth, phaseHeight, GL_RG32F, PassBuffer::Sampling::Linear )
		&& inkBuffer.Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear );

	if( !ok )
		return false;

	bufferWidth  = width;
	bufferHeight = height;
	return true;
}

//---------------------------------------------------------------------------
FFResult IntaglioPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );

	const GLsizei width  = static_cast< GLsizei >( source.Width );
	const GLsizei height = static_cast< GLsizei >( source.Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	//The host's viewport, captured before anything rebinds it. ScopedFBOBinding
	//restores the framebuffer but *not* the viewport (SDK b1afaf9), so without
	//this the composite inherits whatever the last pass set -- which in most
	//viewers reads as "blown out to white with a small picture in the corner"
	//rather than as a viewport bug.
	GLint hostViewport[ 4 ] = {};
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//-------------------------------------------------------------------
	// Physical units. Every length is a fraction of the frame height, so the
	// same settings give the same drawing at any raster -- see Controls.h.
	//-------------------------------------------------------------------
	const float pictureHeight = static_cast< float >( height );

	const float pitchPx   = PitchPixels( params[ PT_PITCH ], height );
	const float detailPx  = std::max( DetailFromParam( params[ PT_DETAIL ] ) * pictureHeight, 0.5f );
	const float smoothPx  = std::max( SmoothingFromParam( params[ PT_SMOOTHING ] ) * pictureHeight, 0.5f );

	const int phaseDiv          = PhaseDivisor( pitchPx, width, height );
	const GLsizei phaseWidth    = std::max( 1, ( width + phaseDiv - 1 ) / phaseDiv );
	const GLsizei phaseHeight   = std::max( 1, ( height + phaseDiv - 1 ) / phaseDiv );

	//Every allocation up front. See ensureBuffers.
	if( !ensureBuffers( width, height, phaseWidth, phaseHeight ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );

	const float texelX = 1.0f / static_cast< float >( width );
	const float texelY = 1.0f / static_cast< float >( height );

	//-------------------------------------------------------------------
	// 1. Copy.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( source.Handle );

		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel", 0.5f * texelX, 0.5f * texelY );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 2. Tone, then its mip chain.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( toneBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		toneBuffer.ResizeViewPort();
		ScopedShaderBinding shader( toneShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		toneShader.Set( "CopyTexture", 0 );
		toneShader.Set( "SourceMode", params[ PT_SOURCE ] );
		quad.Draw();
	}
	//After the draw and before anything samples it. A stale chain does not
	//look like an error; it looks like the wrong footage.
	toneBuffer.GenerateMipmaps();

	//-------------------------------------------------------------------
	// 3. The structure tensor, at the Detail scale.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( tensorBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		tensorBuffer.ResizeViewPort();
		ScopedShaderBinding shader( tensorShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( toneBuffer.TextureID() );

		tensorShader.Set( "ToneTexture", 0 );
		tensorShader.Set( "TexelSize", texelX, texelY );
		tensorShader.Set( "Reach", detailPx );
		//One mip level per doubling of the reach, held to the chain that
		//exists. A level past the 1x1 one is clamped by the driver, but
		//asking for it would make Detail stop moving before its travel does.
		tensorShader.Set( "Level",
		                  std::clamp( std::log2( std::max( detailPx, 1.0f ) ), 0.0f,
		                              toneBuffer.MaxMipLevel() ) );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 4. Smooth it, separably. Horizontal into the temp, vertical back.
	//-------------------------------------------------------------------
	{
		const int taps = tapsFor( smoothPx );

		struct Axis
		{
			PassBuffer* target;
			PassBuffer* origin;
			float dx, dy;
		};
		const Axis axes[] = {
			{ &tensorTemp, &tensorBuffer, texelX, 0.0f },
			{ &tensorBuffer, &tensorTemp, 0.0f, texelY },
		};

		for( const Axis& axis : axes )
		{
			ScopedFBOBinding fbo( axis.target->GetGLID(), ScopedFBOBinding::RB_REVERT );
			axis.target->ResizeViewPort();
			ScopedShaderBinding shader( tensorBlurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( axis.origin->TextureID() );

			tensorBlurShader.Set( "TensorTexture", 0 );
			tensorBlurShader.Set( "Direction", axis.dx, axis.dy );
			tensorBlurShader.Set( "Sigma", smoothPx );
			tensorBlurShader.Set( "Taps", taps );
			quad.Draw();
		}
	}

	//-------------------------------------------------------------------
	// 5. The phase: the two line integrals the hatch coordinate is made of.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( phaseBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		phaseBuffer.ResizeViewPort();
		ScopedShaderBinding shader( phaseShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( tensorBuffer.TextureID() );

		phaseShader.Set( "FlowTexture", 0 );
		phaseShader.Set( "PictureSize", static_cast< float >( width ), static_cast< float >( height ) );
		phaseShader.Set( "Centre", 0.5f * static_cast< float >( width ), 0.5f * static_cast< float >( height ) );
		phaseShader.Set( "BaseAngle", AngleFromParam( params[ PT_ANGLE ] ) * kPi / 180.0f );
		phaseShader.Set( "Belief", CoherenceFromParam( params[ PT_COHERENCE ] ) );
		//Sampling the flow finer than it varies buys nothing, and the walk is
		//capped at kMaxWalk steps anyway: half the smoothing sigma keeps the
		//two ends of that trade in the same units.
		phaseShader.Set( "StepPx", std::max( 0.5f * smoothPx, 1.0f ) );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 6. The engraving.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( inkBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		inkBuffer.ResizeViewPort();
		ScopedShaderBinding shader( engraveShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding toneTexture( toneBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding phaseTexture( phaseBuffer.TextureID() );

		engraveShader.Set( "ToneTexture", 0 );
		engraveShader.Set( "PhaseTexture", 1 );
		engraveShader.Set( "PitchPx", pitchPx );
		engraveShader.Set( "Gamma", CurveFromParam( params[ PT_CURVE ] ) );
		engraveShader.Set( "MinWeight", MinWeightFromParam( params[ PT_MIN_WEIGHT ] ) );
		engraveShader.Set( "MaxWeight", MaxWeightFromParam( params[ PT_MAX_WEIGHT ] ) );
		engraveShader.Set( "Taper", TaperFromParam( params[ PT_TAPER ] ) );
		engraveShader.Set( "Sets",
		                   std::clamp( static_cast< int >( std::lround( params[ PT_SETS ] ) ) + 1, 1, kMaxSets ) );
		engraveShader.Set( "CrossAngle", CrossAngleFromParam( params[ PT_CROSS_ANGLE ] ) * kPi / 180.0f );
		engraveShader.Set( "Engage", EngageFromParam( params[ PT_ENGAGE ] ) );
		engraveShader.Set( "EngageSoft", EngageSoftnessFromParam( params[ PT_ENGAGE_SOFT ] ) );
		engraveShader.Set( "ThirdGap", kThirdSetGap );
		engraveShader.Set( "PlateTone", PlateToneFromParam( params[ PT_PLATE_TONE ] ) );
		engraveShader.Set( "Bite", BiteFromParam( params[ PT_BITE ] ) );
		engraveShader.Set( "Burr", BurrFromParam( params[ PT_BURR ] ) );
		engraveShader.Set( "BurrReach", kBurrReach );
		engraveShader.Set( "Invert", params[ PT_INVERT ] );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 7. Composite, back into the host's framebuffer and viewport.
	//-------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inkTexture( inkBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );

		compositeShader.Set( "InkTexture", 0 );
		compositeShader.Set( "CopyTexture", 1 );
		compositeShader.Set( "Ink", params[ PT_INK_R ], params[ PT_INK_G ], params[ PT_INK_B ] );
		compositeShader.Set( "Paper", params[ PT_PAPER_R ], params[ PT_PAPER_G ], params[ PT_PAPER_B ] );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult IntaglioPlugin::DeInitGL()
{
	copyShader.FreeGLResources();
	toneShader.FreeGLResources();
	tensorShader.FreeGLResources();
	tensorBlurShader.FreeGLResources();
	phaseShader.FreeGLResources();
	engraveShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	toneBuffer.Destroy();
	tensorBuffer.Destroy();
	tensorTemp.Destroy();
	phaseBuffer.Destroy();
	inkBuffer.Destroy();

	bufferWidth  = 0;
	bufferHeight = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
char* IntaglioPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult IntaglioPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult IntaglioPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
float IntaglioPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

} // namespace intaglio
