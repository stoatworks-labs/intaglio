/**
    igtest -- render Intaglio offline, and measure what its plate is doing.

    It drives the REAL plugin class over synthetic cards. A test that exercises
    a reimplementation tests the reimplementation.

        igtest --out /tmp/frame.png       the card, engraved
        igtest --card /tmp/card.png       the test card on its own
        igtest --list                     every parameter and its default
        igtest --flow                     is the flow field any good?
        igtest --pitch                    does the ruling come out at the pitch asked for?
        igtest --weight                   does coverage follow the weight curve?
        igtest --limits                   does the plate go solid, and go bare?
        igtest --perpendicular            do the lines run across the gradient?
        igtest --crosshatch               does the second set arrive where it should?
        igtest --negative                 do the checks above actually fail when they should?
        igtest --bench                    time a frame at 720p through 4K
        igtest --pipe                     raw frames in, raw frames out

    `--pipe` takes the fleet's frame format, rosette's `rztest --pipe` exactly,
    so one filming script can drive any of the FFGL plugins:

        ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
          | igtest --pipe --size 1920x1080 [--script cues.txt] \
          | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov

    The cue sheet is one `frame Parameter Name value` per line, `#` comments,
    linear between keys and held before the first and after the last. There
    is no clock to drive: the plate is a pure function of the frame
    (`SetTimeSupported( false )`), so `--fps` is accepted for the fleet's
    command line and changes nothing.

    ------------------------------------------------------------------ rasters

    Every check that measures a *position* runs at two rasters and fails if the
    two disagree, not only if either is wrong. Intaglio's lengths are all
    fractions of the frame height, so the same settings must give the same
    drawing at any size; a check that ran at one raster could pass there and be
    half a cell out somewhere else and nobody would know. The two are chosen so
    the smaller one is about the size a CI machine would use.
*/

#include "Controls.h"
#include "Intaglio.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace intaglio;

namespace
{
constexpr float kPi = 3.14159265358979324f;

using Image = std::vector< unsigned char >;
using Field = std::vector< float >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 512 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( Image& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Image& out, const char* type, const Image& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const Image& rgba )
{
	Image raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Image compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Image png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	Image ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Integer hashing, for the noise. Never fract(sin(x)*43758.5453): that is the
// driver's answer, and two machines disagree about it.
//---------------------------------------------------------------------------
uint32_t lowbias32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

float unitNoise( uint32_t a, uint32_t b )
{
	return static_cast< float >( lowbias32( a ^ lowbias32( b ) ) ) / 4294967296.0f;
}

//---------------------------------------------------------------------------
// Cards. Row 0 is v = 0, the same way glTexImage2D lays a texture out and the
// same way glReadPixels hands one back -- so every measurement below works in
// the picture's own coordinates and only the PNG writer ever flips.
//---------------------------------------------------------------------------
unsigned char toByte( float v )
{
	return static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

/// What an 8-bit card really carries once it is quantised. Every expectation
/// below is computed from this rather than from the float that was asked for:
/// one code value is 0.4% of the range, which is most of a tolerance.
float quantised( float v )
{
	return static_cast< float >( toByte( v ) ) / 255.0f;
}

Image flatField( int width, int height, float tone )
{
	Image image( static_cast< size_t >( width ) * height * 4, 255 );
	const unsigned char b = toByte( tone );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = b;
		image[ i + 1 ] = b;
		image[ i + 2 ] = b;
		image[ i + 3 ] = 255;
	}
	return image;
}

/// A plane sinusoid whose wave vector runs at `degrees` in PIXEL space, with a
/// wavelength given as a fraction of the frame height. The tone's gradient is
/// therefore at `degrees` everywhere, and the structure it carries is the same
/// fraction of the frame at any raster -- which is what the plugin's own
/// lengths are, so the two scale together.
///
/// It replaced a linear ramp, and the two-raster check is what found the
/// reason. A ramp across a whole frame moves through at most 255 code values,
/// so it arrives as a staircase whose steps are several pixels apart; the
/// steps are perpendicular to the gradient, but their *ends* land on the pixel
/// grid and the jaggies they leave are axis-aligned. At 960x540 that bias was
/// 0.3 degrees and at 432x243 it was 1.5, because the step spacing is fixed in
/// code values while everything else scales. A sinusoid whose wavelength is a
/// fraction of the frame carries a gradient of several code values per pixel
/// at any size, and has no staircase to speak of.
Image waveField( int width, int height, float degrees, float wavelengthFraction )
{
	Image image( static_cast< size_t >( width ) * height * 4, 255 );

	const float rad = degrees * kPi / 180.0f;
	const float dx  = std::cos( rad );
	const float dy  = std::sin( rad );
	const float k   = 2.0f * kPi / std::max( wavelengthFraction * height, 4.0f );

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float phase = k * ( ( x + 0.5f ) * dx + ( y + 0.5f ) * dy );
			const unsigned char b = toByte( 0.5f + 0.45f * std::sin( phase ) );

			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ o + 0 ] = b;
			image[ o + 1 ] = b;
			image[ o + 2 ] = b;
			image[ o + 3 ] = 255;
		}

	return image;
}

//---------------------------------------------------------------------------
// The test card.
//
// The rings on the left are the load-bearing part for --flow: their tangent
// direction is known in closed form. The FLAT PANEL right of centre is the
// load-bearing part for everything else, and for the argument this plugin
// makes at all: it has no edges in it. nib draws nothing there. Intaglio has
// to fill it with evenly spaced lines of the weight that grey calls for.
//---------------------------------------------------------------------------
constexpr float kRingsCentreU = 0.22f;
constexpr float kRingsCentreV = 0.5f;
constexpr float kRingsInner   = 0.05f;
constexpr float kRingsOuter   = 0.19f;
constexpr float kRingPeriod   = 0.022f;

constexpr float kPanelU0 = 0.52f, kPanelU1 = 0.74f;
constexpr float kPanelV0 = 0.30f, kPanelV1 = 0.90f;
constexpr float kPanelTone = 0.62f;

Image buildCard( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4, 0 );

	const float w      = static_cast< float >( width );
	const float h      = static_cast< float >( height );
	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//A soft gradient everywhere, as the floor. Not flat: a plate that
			//only works on flat backgrounds is not a plate.
			float r = 0.46f + 0.34f * v;
			float g = r;
			float b = r;

			//Concentric rings, left. Analytic tangent, and the same geometry
			//nib's card uses, so the two plugins' flow measurements are
			//comparable.
			const float rdx = ( u - kRingsCentreU ) * aspect;
			const float rdy = v - kRingsCentreV;
			const float rr  = std::sqrt( rdx * rdx + rdy * rdy );
			if( rr > kRingsInner && rr < kRingsOuter )
			{
				const float phase = std::fmod( rr, kRingPeriod ) / kRingPeriod;
				r = g = b = phase < 0.5f ? 0.88f : 0.12f;
			}

			//The flat panel. No edges anywhere inside it.
			if( u > kPanelU0 && u < kPanelU1 && v > kPanelV0 && v < kPanelV1 )
				r = g = b = kPanelTone;

			//A plain disc, right. One clean closed contour, and a light tone
			//so the lines have to taper away over it.
			const float ddx = ( u - 0.87f ) * aspect;
			const float ddy = v - 0.62f;
			if( std::sqrt( ddx * ddx + ddy * ddy ) < 0.10f )
				r = g = b = 0.93f;

			//A tone ramp along the bottom: every weight the curve can ask for,
			//in one strip.
			if( v > 0.03f && v < 0.17f && u > 0.46f )
				r = g = b = std::clamp( ( u - 0.46f ) / 0.50f, 0.0f, 1.0f );

			//Two fields of equal luminance, bottom left: invisible to a luma
			//reading, obvious to a saturation one.
			if( v < 0.14f && u < 0.42f )
			{
				const bool right = u > 0.21f;
				r                = right ? 0.10f : 0.9333f;
				g                = right ? 0.2775f : 0.0f;
				b                = 0.0f;
			}

			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ o + 0 ] = toByte( r );
			image[ o + 1 ] = toByte( g );
			image[ o + 2 ] = toByte( b );
			image[ o + 3 ] = 255;
		}
	}

	return image;
}

Image addNoise( const Image& card, int frame, float amount )
{
	Image noisy = card;
	if( amount <= 0.0f )
		return noisy;

	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		const float jitter =
			( unitNoise( static_cast< uint32_t >( i / 4 ), static_cast< uint32_t >( frame ) ) - 0.5f ) * scale;

		for( int c = 0; c < 3; ++c )
		{
			const float value = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]    = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

/// A rectangle of a finished frame, in the picture's own 0..1 coordinates with
/// v up -- the same coordinates the card is written in. It exists so a figure
/// in the README can be reproduced by a command rather than by somebody
/// cropping a screenshot, which is a picture nobody can re-render later.
Image cropTo( const Image& image, int width, int height, float u0, float v0, float u1, float v1,
              int& outWidth, int& outHeight )
{
	const int x0 = std::clamp( static_cast< int >( u0 * width ), 0, width - 1 );
	const int x1 = std::clamp( static_cast< int >( u1 * width ), x0 + 1, width );
	//Row 0 of a flipped frame is the top, which is v = 1.
	const int y0 = std::clamp( static_cast< int >( ( 1.0f - v1 ) * height ), 0, height - 1 );
	const int y1 = std::clamp( static_cast< int >( ( 1.0f - v0 ) * height ), y0 + 1, height );

	outWidth  = x1 - x0;
	outHeight = y1 - y0;

	Image out( static_cast< size_t >( outWidth ) * outHeight * 4 );
	for( int y = 0; y < outHeight; ++y )
		std::memcpy( out.data() + static_cast< size_t >( y ) * outWidth * 4,
		             image.data() + ( static_cast< size_t >( y0 + y ) * width + x0 ) * 4,
		             static_cast< size_t >( outWidth ) * 4 );
	return out;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// A rendering rig: context objects, a card, and a render() that drives the
// real plugin the way a host would.
//
// `precise` attaches an RGBA32F target instead of an RGBA8 one. Every check
// that reads a coverage back uses it, because eight bits is one four-hundredth
// of the quantity being measured and that is a large slice of a tolerance
// derived from anything else.
//---------------------------------------------------------------------------
struct Rig
{
	IntaglioPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	Image card;

	ProcessOpenGLStruct process   = {};
	FFGLTextureStruct inputStruct = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		Destroy();
	}

	bool Init( int w, int h, bool precise = true )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		card          = buildCard( width, height );
		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, card.data() );
		outputTexture = makeTexture( width, height, precise ? GL_RGBA32F : GL_RGBA8,
		                             precise ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "the harness's own output framebuffer is not complete\n" );
			return false;
		}

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		return true;
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	/// Ink black, paper white, nothing else laid on top: what comes back is
	/// then exactly `1 - coverage` in every channel, with no model in between.
	void Monochrome()
	{
		Set( PT_INK_R, 0.0f );
		Set( PT_INK_G, 0.0f );
		Set( PT_INK_B, 0.0f );
		Set( PT_PAPER_R, 1.0f );
		Set( PT_PAPER_G, 1.0f );
		Set( PT_PAPER_B, 1.0f );
		Set( PT_PLATE_TONE, 0.0f );
		Set( PT_BITE, 0.0f );
		Set( PT_BURR, 0.0f );
		Set( PT_MIX, 1.0f );
	}

	void Upload( const Image& image )
	{
		card = image;
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Render( int frames = 2, float noise = 0.0f )
	{
		for( int frame = 0; frame < frames; ++frame )
		{
			if( noise > 0.0f )
			{
				const Image noisy = addNoise( card, frame, noise );
				glBindTexture( GL_TEXTURE_2D, sourceTexture );
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, noisy.data() );
				glBindTexture( GL_TEXTURE_2D, 0 );
			}

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );

			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Coverage, straight out of the frame. Unflipped: row 0 is v = 0, which
	/// is what every geometric expectation here is written in.
	Field Coverage() const
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );

		Field coverage( static_cast< size_t >( width ) * height );
		for( size_t i = 0; i < coverage.size(); ++i )
			coverage[ i ] = std::clamp( 1.0f - pixels[ i * 4 ], 0.0f, 1.0f );
		return coverage;
	}

	Image Bytes() const
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	void Destroy()
	{
		if( outputFBO != 0 )
		{
			plugin.DeInitGL();
			glDeleteTextures( 1, &sourceTexture );
			glDeleteTextures( 1, &outputTexture );
			glDeleteFramebuffers( 1, &outputFBO );
			sourceTexture = outputTexture = outputFBO = 0;
		}
	}
};

//---------------------------------------------------------------------------
// Parameter helpers, so the checks below read as English and the conversions
// live in exactly one place -- Controls.cpp, which is the plugin's own.
//---------------------------------------------------------------------------
float angleParam( float degrees )
{
	return std::clamp( degrees / 180.0f, 0.0f, 1.0f );
}

/// The parameter that asks for a given weight curve exponent.
float curveParam( float gamma )
{
	return std::log( std::clamp( gamma, 0.25f, 4.0f ) / 0.25f ) / std::log( 16.0f );
}

/// The parameter that asks for a given Flow Smoothing, as a fraction of the
/// frame height.
float smoothingParam( float fraction )
{
	return std::log( std::clamp( fraction, 0.0005f, 0.03f ) / 0.0005f ) / std::log( 60.0f );
}

float detailParam( float fraction )
{
	return std::log( std::clamp( fraction, 0.0005f, 0.012f ) / 0.0005f ) / std::log( 24.0f );
}

float engageSoftParam( float softness )
{
	return std::log( std::clamp( softness, 0.01f, 0.5f ) / 0.01f ) / std::log( 50.0f );
}

/// Coherence, as the gain on anisotropy the shader gets.
float coherenceParam( float gain )
{
	return std::clamp( gain / 120.0f, 0.0f, 1.0f );
}

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------

/// The mean of a rectangle of a field.
double meanOver( const Field& field, int width, int x0, int y0, int x1, int y1 )
{
	double sum = 0.0;
	long n     = 0;
	for( int y = y0; y < y1; ++y )
		for( int x = x0; x < x1; ++x )
		{
			sum += field[ static_cast< size_t >( y ) * width + x ];
			++n;
		}
	return n > 0 ? sum / static_cast< double >( n ) : 0.0;
}

/// How many lines a horizontal row crosses: the number of runs of ink, judged
/// at half coverage.
int countRunsAlongRow( const Field& field, int width, int y, int x0, int x1 )
{
	int runs   = 0;
	bool inInk = false;
	for( int x = x0; x < x1; ++x )
	{
		const bool ink = field[ static_cast< size_t >( y ) * width + x ] > 0.5f;
		if( ink && !inInk )
			++runs;
		inInk = ink;
	}
	return runs;
}

double median( std::vector< double > values )
{
	if( values.empty() )
		return 0.0;
	std::sort( values.begin(), values.end() );
	return values[ values.size() / 2 ];
}

/// How strongly a region answers to a grating of a KNOWN pitch at a given line
/// angle: the discrete Fourier transform of the coverage, evaluated at exactly
/// the frequency the model predicts, under a Hann window.
///
/// This replaced an orientation histogram of the coverage's gradient, and the
/// reason is the whole point of running these checks at two rasters. Where two
/// hatch sets cross, the first set's lines chop the second's into segments,
/// and the gradient at a chopped end is a mixture of the two sets' normals --
/// so a gradient histogram reads the two peaks as being pulled toward each
/// other. It read 61.1 degrees at 960x540 and 59.8 at 432x243 for sets that
/// are 62 degrees apart by construction, and the bias grew as the raster
/// shrank, which is exactly the failure mode a single-raster check would have
/// shipped.
///
/// Evaluating the transform at a known frequency does not have that bias.
/// Chopping is a MULTIPLICATION by the first set's mask, and a product's
/// spectrum is the second set's line convolved with the first set's comb --
/// replicas at other frequencies, not a shift of the original. The peak stays
/// where the model put it.
///
/// `degrees` is the LINE angle; the grating's wave vector is its normal.
double gratingResponse( const Field& field, int width, int x0, int y0, int x1, int y1,
                        double pitchPx, double degrees, int stride )
{
	const double radians = degrees * M_PI / 180.0;
	const double nx      = std::sin( radians );
	const double ny      = -std::cos( radians );
	const double k       = 2.0 * M_PI / std::max( pitchPx, 1e-3 );

	const double spanX = std::max( 1.0, static_cast< double >( x1 - x0 - 1 ) );
	const double spanY = std::max( 1.0, static_cast< double >( y1 - y0 - 1 ) );

	//A Hann window on both axes. Without it the rectangle's own sidelobes are
	//brighter than the second set's fundamental and the peak search finds the
	//first set twice.
	double re = 0.0, im = 0.0, weight = 0.0, mean = 0.0;
	for( int y = y0; y < y1; y += stride )
		for( int x = x0; x < x1; x += stride )
		{
			const double wx = 0.5 - 0.5 * std::cos( 2.0 * M_PI * ( x - x0 ) / spanX );
			const double wy = 0.5 - 0.5 * std::cos( 2.0 * M_PI * ( y - y0 ) / spanY );
			const double w  = wx * wy;
			mean += w * field[ static_cast< size_t >( y ) * width + x ];
			weight += w;
		}
	mean /= std::max( weight, 1e-9 );

	for( int y = y0; y < y1; y += stride )
		for( int x = x0; x < x1; x += stride )
		{
			const double wx = 0.5 - 0.5 * std::cos( 2.0 * M_PI * ( x - x0 ) / spanX );
			const double wy = 0.5 - 0.5 * std::cos( 2.0 * M_PI * ( y - y0 ) / spanY );
			const double w  = wx * wy;

			const double v     = field[ static_cast< size_t >( y ) * width + x ] - mean;
			const double phase = k * ( ( x + 0.5 ) * nx + ( y + 0.5 ) * ny );
			re += w * v * std::cos( phase );
			im += w * v * std::sin( phase );
		}

	return std::sqrt( re * re + im * im ) / std::max( weight, 1e-9 );
}

struct Grating
{
	double degrees = 0.0;
	double power   = 0.0;
};

/// The two strongest line angles, at least `apartDeg` apart, found on a
/// quarter-degree grid and refined by a parabola through the winning sample
/// and its neighbours.
void topGratings( const Field& field, int width, int x0, int y0, int x1, int y1,
                  double pitchPx, int stride, double apartDeg, Grating& first, Grating& second )
{
	constexpr int kSteps = 720;//a quarter of a degree
	std::vector< double > power( kSteps, 0.0 );
	for( int i = 0; i < kSteps; ++i )
		power[ static_cast< size_t >( i ) ] =
			gratingResponse( field, width, x0, y0, x1, y1, pitchPx, i * 180.0 / kSteps, stride );

	auto refine = [ & ]( int index ) {
		const double y0v = power[ static_cast< size_t >( ( index - 1 + kSteps ) % kSteps ) ];
		const double y1v = power[ static_cast< size_t >( index ) ];
		const double y2v = power[ static_cast< size_t >( ( index + 1 ) % kSteps ) ];
		const double den = y0v - 2.0 * y1v + y2v;
		const double shift = std::fabs( den ) > 1e-15 ? 0.5 * ( y0v - y2v ) / den : 0.0;

		Grating g;
		g.degrees = std::fmod( ( index + std::clamp( shift, -1.0, 1.0 ) ) * 180.0 / kSteps + 180.0, 180.0 );
		g.power   = y1v;
		return g;
	};

	int best = 0;
	for( int i = 1; i < kSteps; ++i )
		if( power[ static_cast< size_t >( i ) ] > power[ static_cast< size_t >( best ) ] )
			best = i;
	first = refine( best );

	const int guard = std::max( 1, static_cast< int >( apartDeg * kSteps / 180.0 ) );
	int runnerUp    = -1;
	for( int i = 0; i < kSteps; ++i )
	{
		int away = std::abs( i - best );
		away     = std::min( away, kSteps - away );
		if( away < guard )
			continue;
		if( runnerUp < 0 || power[ static_cast< size_t >( i ) ] > power[ static_cast< size_t >( runnerUp ) ] )
			runnerUp = i;
	}
	second = runnerUp >= 0 ? refine( runnerUp ) : Grating {};
}

double angleApart( double a, double b )
{
	double d = std::fmod( std::fabs( a - b ), 180.0 );
	return std::min( d, 180.0 - d );
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. The plain run leaves it at its defaults; `--negative`
// runs the same code with the model deliberately wrong and requires the check
// to FAIL. A check that cannot fail is not a check.
//===========================================================================
struct Perturb
{
	float pitchExpectationFactor = 1.0f;///< --pitch: expect this many times as many lines
	bool disableFlow             = false;///< --perpendicular: Coherence to zero
	float weightModelGamma       = 0.0f;///< --weight: use this gamma in the model, not the plugin's
	float crossExpectationOffset = 0.0f;///< --crosshatch: expect this much more than asked for
	float limitSlack             = 0.0f;///< --limits: ask the plate for less than everything
};

//---------------------------------------------------------------------------
// --flow. nib's measurement, run against THIS plugin's shader.
//
// Claim under test: averaging the *structure tensor* over a neighbourhood is a
// better estimate of which way the picture runs than the per-pixel gradient
// is, once there is any noise at all. The rings give an analytic answer, so
// the flow field's direction can simply be subtracted from the truth.
//
// It is nib's check and nib's claim, but it is re-run here rather than assumed
// to carry over: the probe is assembled from THIS repo's kFlowLibrary, and the
// tensor it reads has been through this repo's Detail and Flow Smoothing,
// which are not nib's controls.
//
// Run at two rasters. The absolute error legitimately differs between them --
// the rings are a fixed fraction of the frame, so at half the height they are
// half as many pixels across and a fixed noise amplitude is a larger
// disturbance -- so what is required at both is the same *floor on the
// improvement*, not the same number of degrees.
//---------------------------------------------------------------------------
GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint stage    = glCreateShader( type );
	const char* const src = source.c_str();
	glShaderSource( stage, 1, &src, nullptr );
	glCompileShader( stage );

	GLint ok = GL_FALSE;
	glGetShaderiv( stage, GL_COMPILE_STATUS, &ok );
	if( ok == GL_TRUE )
		return stage;

	GLint length = 0;
	glGetShaderiv( stage, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( stage, length, nullptr, log.data() );
	error = log;
	glDeleteShader( stage );
	return 0;
}

GLuint buildProbeProgram( std::string& error )
{
	static const char* const vertex = R"(#version 410 core
layout( location = 0 ) in vec2 vPosition;
out vec2 uv;
void main()
{
	uv = vPosition * 0.5 + 0.5;
	gl_Position = vec4( vPosition, 0.0, 1.0 );
}
)";

	const GLuint vs = compileStage( GL_VERTEX_SHADER, vertex, error );
	if( vs == 0 )
		return 0;

	const GLuint fs = compileStage( GL_FRAGMENT_SHADER, FlowProbeShaderSource(), error );
	if( fs == 0 )
	{
		glDeleteShader( vs );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );
	glDeleteShader( vs );
	glDeleteShader( fs );

	GLint ok = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &ok );
	if( ok == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

/// The floor on how much smoothing the tensor has to buy. A third is not a
/// tuned number, it is a floor: anything less and the pass is not worth its
/// cost, and the honest thing would be to delete it and use the raw gradient.
constexpr double kFlowImprovementFloor = 0.33;

bool flowAtRaster( int width, int height, double& worstOut, double& bestOut, double& sampledOut )
{
	constexpr float kNoise = 0.10f;

	std::string error;
	const GLuint probe = buildProbeProgram( error );
	if( probe == 0 )
	{
		std::fprintf( stderr, "flow probe failed to build: %s\n", error.c_str() );
		return false;
	}

	//A float target, because the direction is a unit vector and an 8-bit
	//readback would quantise the error being measured to coarser than the
	//differences between the settings being compared.
	const GLuint probeTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
	glBindTexture( GL_TEXTURE_2D, probeTexture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	const GLuint probeFBO = makeFramebuffer( probeTexture );

	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float quad[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( quad ), quad, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
	glBindVertexArray( 0 );

	//From barely any smoothing -- as close to the raw gradient as the plugin
	//gets -- up to a working value.
	const float settings[] = { 0.0f, 0.25f, 0.5f, 0.75f };

	worstOut = 0.0;
	bestOut  = 1e30;
	sampledOut = 0.0;

	bool ok = true;
	for( size_t index = 0; index < sizeof( settings ) / sizeof( settings[ 0 ] ) && ok; ++index )
	{
		Rig rig;
		if( !rig.Init( width, height ) )
		{
			ok = false;
			break;
		}
		rig.Set( PT_SMOOTHING, settings[ index ] );
		if( !rig.Render( 4, kNoise ) )
		{
			ok = false;
			break;
		}

		glBindFramebuffer( GL_FRAMEBUFFER, probeFBO );
		glViewport( 0, 0, width, height );
		glUseProgram( probe );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, rig.plugin.FlowTextureID() );
		glUniform1i( glGetUniformLocation( probe, "FlowTexture" ), 0 );
		glBindVertexArray( vao );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glBindVertexArray( 0 );

		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );

		const float aspect = static_cast< float >( width ) / static_cast< float >( height );

		double sum = 0.0, count = 0.0;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );
				const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( height );

				const float dx = ( u - kRingsCentreU ) * aspect;
				const float dy = v - kRingsCentreV;
				const float rr = std::sqrt( dx * dx + dy * dy );

				//Inside the ring field only, and away from its two rims where
				//the truth stops being the rings' tangent.
				if( rr < kRingsInner + 0.02f || rr > kRingsOuter - 0.02f )
					continue;

				//The truth: tangent to a circle is perpendicular to the radius.
				const float tx = -dy / rr;
				const float ty = dx / rr;

				const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
				float fx = pixels[ o + 0 ];
				float fy = pixels[ o + 1 ];

				const float len = std::sqrt( fx * fx + fy * fy );
				if( len < 1e-6f )
					continue;
				fx /= len;
				fy /= len;

				//An orientation, not a direction: the eigenvector's sign is
				//arbitrary, so 179 degrees out is the same as 1 degree out.
				const float dot   = std::fabs( fx * tx + fy * ty );
				const float angle = std::acos( std::min( 1.0f, dot ) ) * 180.0f / kPi;

				sum += angle;
				count += 1.0;
			}

		const double mean = count > 0 ? sum / count : 0.0;
		if( index == 0 )
		{
			worstOut   = mean;
			sampledOut = count;
		}
		bestOut = std::min( bestOut, mean );

		std::printf( "    %4dx%-4d  smoothing sigma %5.2f px -> %6.2f degrees   (%.0f px sampled)\n",
		             width, height, SmoothingFromParam( settings[ index ] ) * height, mean, count );
	}

	glDeleteBuffers( 1, &vbo );
	glDeleteVertexArrays( 1, &vao );
	glDeleteFramebuffers( 1, &probeFBO );
	glDeleteTextures( 1, &probeTexture );
	glDeleteProgram( probe );
	return ok;
}

int runFlow()
{
	std::printf( "flow: mean angular error against the rings' analytic tangent, 10%% noise\n\n" );

	const int rasters[][ 2 ] = { { 960, 540 }, { 480, 270 } };

	for( const auto& raster : rasters )
	{
		double worst = 0.0, best = 0.0, sampled = 0.0;
		if( !flowAtRaster( raster[ 0 ], raster[ 1 ], worst, best, sampled ) )
			return 1;

		const double improvement = worst > 0.0 ? ( worst - best ) / worst : 0.0;
		Check( sampled >= 1000.0,
		       fmt( "%dx%d: %.0f pixels of known tangent sampled", raster[ 0 ], raster[ 1 ], sampled ) );
		Check( improvement >= kFlowImprovementFloor,
		       fmt( "%dx%d: best is %.0f%% better than the least-smoothed setting (floor %.0f%%)",
		            raster[ 0 ], raster[ 1 ], improvement * 100.0, kFlowImprovementFloor * 100.0 ) );
	}

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --pitch.
//
// On a flat field the tensor has no opinion, so the plate rules a plain
// grating at Angle Offset -- and with Angle Offset at ninety degrees the lines
// are vertical and a horizontal row crosses one every pitch.
//
// **The tolerance comes from the lattice, not from a rendered count.** Lines
// sit at `x = centre + k * pitch`; a window of width W therefore contains
// either floor( W / pitch ) or that plus one of them, depending on where the
// centre falls. So the count must be within 1 of W / pitch and the check does
// not get to choose a looser number.
//
// Two preconditions are asserted rather than assumed, because the measurement
// is a run count and a run count needs runs:
//
//   * the pitch must be at least six pixels at the SMALLER raster, or the ink
//     and the paper are not both wide enough to survive a half-coverage
//     threshold;
//   * the coverage must sit between a quarter and two thirds, so neither the
//     line nor the gap is thinner than about a pixel and a half.
//
// And it runs at two rasters. The ruling is a fraction of the frame height, so
// the count across the width is a property of the aspect ratio and the ruling
// alone -- it must come back the SAME at both. A check that ran at one raster
// could be half a cell out at another and nobody would know.
//---------------------------------------------------------------------------
constexpr float kPitchLines      = 24.0f;
constexpr float kPitchCoverage   = 0.40f;
constexpr float kMinCountablePitch = 6.0f;

int runPitch( const Perturb& perturb )
{
	std::printf( "pitch: lines counted across a flat field, against width / pitch\n\n" );

	const int rasters[][ 2 ] = { { 1280, 720 }, { 480, 270 } };
	double counts[ 2 ] = { 0.0, 0.0 };

	for( int index = 0; index < 2; ++index )
	{
		const int width  = rasters[ index ][ 0 ];
		const int height = rasters[ index ][ 1 ];

		const float pitchParam = ParamFromPitchLines( kPitchLines );
		const float pitchPx    = IntaglioPlugin::PitchPixels( pitchParam, height );

		Check( pitchPx >= kMinCountablePitch,
		       fmt( "%dx%d: pitch is %.2f px, at least the %.0f px a run count needs",
		            width, height, pitchPx, kMinCountablePitch ) );

		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.Monochrome();
		rig.Upload( flatField( width, height, 0.5f ) );
		rig.Set( PT_PITCH, pitchParam );
		rig.Set( PT_ANGLE, angleParam( 90.0f ) );//vertical lines
		rig.Set( PT_SETS, 0.0f );                //one set
		rig.Set( PT_TAPER, 0.0f );
		//A flat coverage, independent of the tone, so the count is about the
		//lattice and nothing else.
		rig.Set( PT_MIN_WEIGHT, kPitchCoverage );
		rig.Set( PT_MAX_WEIGHT, kPitchCoverage );
		if( !rig.Render() )
			return 1;

		const Field coverage = rig.Coverage();

		const int x0 = width / 8, x1 = width - width / 8;
		const double measuredCoverage = meanOver( coverage, width, x0, height / 4, x1, 3 * height / 4 );
		Check( measuredCoverage > 0.25 && measuredCoverage < 0.67,
		       fmt( "%dx%d: coverage %.3f, so ink and paper are both at least %.1f px wide",
		            width, height, measuredCoverage, kPitchCoverage * pitchPx ) );

		std::vector< double > runs;
		for( int y = height / 4; y < 3 * height / 4; y += 7 )
			runs.push_back( countRunsAlongRow( coverage, width, y, x0, x1 ) );
		const double count = median( runs );
		counts[ index ]    = count;

		const double window   = static_cast< double >( x1 - x0 );
		const double expected = window / ( pitchPx * perturb.pitchExpectationFactor );

		Check( std::fabs( count - expected ) <= 1.0,
		       fmt( "%dx%d: %.0f lines across %.0f px, expected %.2f (tolerance one line, from the lattice)",
		            width, height, count, window, expected ) );
	}

	Check( counts[ 0 ] == counts[ 1 ],
	       fmt( "the two rasters agree: %.0f lines and %.0f lines", counts[ 0 ], counts[ 1 ] ) );

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --weight.
//
// Coverage over a flat patch, at five tones, against the model in Controls.cpp.
//
// **Where the tolerance comes from.** Three terms, and the first dominates:
//
//   * the *lattice sampling*. The model is a statement about a continuous
//     plate; the render samples it at pixel centres. The ink profile across a
//     line is C1 -- it is a smoothstep -- so its Fourier coefficients fall off
//     as the cube of the harmonic, and a unit pixel grid can only fold a
//     harmonic onto the mean when the lattice very nearly resonates with it.
//     Holding the pitch to at least twelve pixels and the angle off every
//     multiple of forty-five degrees keeps the lowest foldable harmonic near
//     n = pitch, so the term is of order 1e-3. It is *verified* rather than
//     merely argued by the two-raster agreement below, which measures it: the
//     two rasters have different pitches in pixels and so different
//     resonances.
//   * the *tone buffer*, which is R16F. Eleven bits of mantissa is a quantum
//     of 2^-12 in the darkness, and the curve's slope is at most
//     gamma * ( Max - Min ) <= 4, so at most 1e-3 in coverage.
//   * `pow`, which GLSL 4.10 section 8.2 bounds through exp2 and log2: log2 is
//     allowed an absolute error under 2^-21 on [0.5, 2] and exp2 three ULP, so
//     pow( x, g ) carries at most about ( g * 2^-21 * ln2 + 3 * 2^-23 ), which
//     is 1.7e-6 at gamma 4. Negligible here, and the reason --limits states it
//     rather than measuring it.
//
// The input's own 8-bit quantisation is NOT in the budget, because every
// expectation is computed from the quantised code value rather than from the
// float that was asked for.
//---------------------------------------------------------------------------
constexpr double kWeightTolerance      = 0.005;
constexpr double kWeightRasterAgreement = 0.003;
constexpr float kWeightAngle           = 17.0f;
constexpr float kWeightGamma           = 1.8f;
constexpr float kWeightMin             = 0.05f;
constexpr float kWeightMax             = 0.90f;
constexpr float kWeightMinPitchPx      = 12.0f;

int runWeight( const Perturb& perturb )
{
	std::printf( "weight: mean coverage over a flat patch against the weight curve\n\n" );

	const int rasters[][ 2 ] = { { 1024, 576 }, { 448, 252 } };
	const float tones[]      = { 0.05f, 0.30f, 0.50f, 0.72f, 0.95f };

	const float modelGamma = perturb.weightModelGamma > 0.0f ? perturb.weightModelGamma : kWeightGamma;

	double measured[ 2 ][ 5 ] = {};
	double previous           = -1.0;
	bool monotonic            = true;

	for( int index = 0; index < 2; ++index )
	{
		const int width  = rasters[ index ][ 0 ];
		const int height = rasters[ index ][ 1 ];

		const float pitchParam = ParamFromPitchLines( 18.0f );
		const float pitchPx    = IntaglioPlugin::PitchPixels( pitchParam, height );
		Check( pitchPx >= kWeightMinPitchPx,
		       fmt( "%dx%d: pitch is %.2f px, at least the %.0f px the sampling argument assumes",
		            width, height, pitchPx, kWeightMinPitchPx ) );

		previous = -1.0;
		for( int t = 0; t < 5; ++t )
		{
			Rig rig;
			if( !rig.Init( width, height ) )
				return 1;
			rig.Monochrome();
			rig.Upload( flatField( width, height, tones[ t ] ) );
			rig.Set( PT_PITCH, pitchParam );
			rig.Set( PT_ANGLE, angleParam( kWeightAngle ) );
			rig.Set( PT_SETS, 0.0f );//one set: the union is --limits' business
			rig.Set( PT_CURVE, curveParam( kWeightGamma ) );
			rig.Set( PT_MIN_WEIGHT, kWeightMin );
			rig.Set( PT_MAX_WEIGHT, kWeightMax );
			rig.Set( PT_TAPER, 0.4f );
			if( !rig.Render() )
				return 1;

			const Field coverage = rig.Coverage();
			//Well inside the frame: the copy pass clamps half a texel in at
			//the border, and a partial cell there is not what is being
			//measured.
			const double mean = meanOver( coverage, width, width / 6, height / 6,
			                              width - width / 6, height - height / 6 );
			measured[ index ][ t ] = mean;

			const float darkness = 1.0f - quantised( tones[ t ] );
			const double expect  = TargetCoverage( darkness, modelGamma, kWeightMin, kWeightMax );

			Check( std::fabs( mean - expect ) <= kWeightTolerance,
			       fmt( "%dx%d: tone %.2f -> coverage %.4f, model %.4f, off by %.4f (tolerance %.3f)",
			            width, height, tones[ t ], mean, expect, std::fabs( mean - expect ), kWeightTolerance ) );

			if( previous >= 0.0 && mean > previous + 1e-4 )
				monotonic = false;
			previous = mean;
		}
	}

	Check( monotonic, "coverage falls monotonically as the input lightens" );

	//---------------------------------------------------------------
	// The cross-hatch union, measured rather than assumed.
	//
	// Two gratings at an angle cover exactly c1 * c2 of the plane between
	// them -- integrate along one set's lines and the other set's phase
	// advances linearly, so it equidistributes -- which is why the
	// independence formula in Controls.cpp is not an approximation for one or
	// two sets, and why it can be held to the same tolerance as everything
	// else here.
	//
	// THREE sets can resonate. At sixty degrees the three wave vectors satisfy
	// k2 = k1 + k3 exactly, so the sets are phase-locked and the triple
	// overlap is not the product of the three coverages. That is a real limit
	// of the model and it is reported below with a tolerance that is MEASURED
	// rather than derived -- it is a regression guard, not a claim.
	//
	// At ninety degrees the third set used to land on the first: same
	// direction, same phase, same lines. The plate came out eleven per cent
	// short. `FoldCoincidentSets` is the fix and this is what holds it.
	//---------------------------------------------------------------
	{
		constexpr int width = 1024, height = 576;
		const float tone = 0.45f;
		const float darkness = 1.0f - quantised( tone );
		const double target  = TargetCoverage( darkness, 1.0f, 0.0f, 1.0f );

		struct UnionCase
		{
			float crossDeg;
			int sets;
			double tolerance;
			const char* note;
		};
		const UnionCase cases[] = {
			{ 47.0f, 1, kWeightTolerance, "one set" },
			{ 47.0f, 2, kWeightTolerance, "two sets, no resonance" },
			{ 47.0f, 3, kWeightTolerance, "three sets, no resonance" },
			{ 90.0f, 2, kWeightTolerance, "two sets at a right angle" },
			{ 90.0f, 3, kWeightTolerance, "three sets at a right angle -- the third folds onto the first" },
			{ 60.0f, 3, 0.04, "three sets at sixty degrees -- PHASE-LOCKED, tolerance measured not derived" },
		};

		for( const UnionCase& c : cases )
		{
			Rig rig;
			if( !rig.Init( width, height ) )
				return 1;
			rig.Monochrome();
			rig.Upload( flatField( width, height, tone ) );
			rig.Set( PT_PITCH, ParamFromPitchLines( 18.0f ) );
			rig.Set( PT_ANGLE, angleParam( kWeightAngle ) );
			rig.Set( PT_SETS, static_cast< float >( c.sets - 1 ) );
			rig.Set( PT_CROSS_ANGLE, angleParam( c.crossDeg ) );
			rig.Set( PT_ENGAGE, 0.0f );//every set engaged, whatever the tone
			rig.Set( PT_ENGAGE_SOFT, engageSoftParam( 0.01f ) );
			rig.Set( PT_CURVE, curveParam( 1.0f ) );
			rig.Set( PT_MIN_WEIGHT, 0.0f );
			rig.Set( PT_MAX_WEIGHT, 1.0f );
			rig.Set( PT_TAPER, 0.5f );
			if( !rig.Render() )
				return 1;

			const Field coverage = rig.Coverage();
			const double mean    = meanOver( coverage, width, width / 6, height / 6,
			                                 width - width / 6, height - height / 6 );

			Check( std::fabs( mean - target ) <= c.tolerance,
			       fmt( "union, %s: coverage %.5f, target %.5f, off %.5f (tolerance %.3f)",
			            c.note, mean, target, mean - target, c.tolerance ) );
		}
	}

	double worstSpread = 0.0;
	for( int t = 0; t < 5; ++t )
		worstSpread = std::max( worstSpread, std::fabs( measured[ 0 ][ t ] - measured[ 1 ][ t ] ) );
	Check( worstSpread <= kWeightRasterAgreement,
	       fmt( "the two rasters agree to %.4f (tolerance %.3f) -- which is also the measurement of the "
	            "lattice-sampling term in the budget above",
	            worstSpread, kWeightRasterAgreement ) );

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --limits.
//
// The two ends are exact statements about the model, so they are checked
// against the model as well as against the render.
//
// **The model tolerance is the GLSL specification's, not this GPU's.** The
// plate's coverage at full weight is `1 - pow( 1 - target, e / sum( e ) )`,
// and `target` at a black input is `pow( 1, gamma )`. GLSL 4.10 section 8.2
// bounds pow through exp2 and log2: log2 is allowed an absolute error under
// 2^-21 over [0.5, 2] and exp2 three ULP, so pow( 1, gamma ) may come back as
// 1 +/- ( gamma * 2^-21 * ln2 + 3 * 2^-23 ), which is 1.7e-6 at gamma 4. This
// machine returns exactly 1.0; a conforming driver need not, and a tolerance
// taken from this machine would have shipped a check that fails on somebody
// else's.
//
// The render tolerance is that number plus the sliver of paper it opens: at
// `target = 1 - 1.7e-6` the half-width is short of half a pitch by the same
// relative amount, so the bare fraction is of the same order. A hundredfold
// margin on 2e-5 is 2e-3; the check uses 1e-3, which is fifty times the bound
// and still four hundred times tighter than the failure it is here to catch --
// a plate that does not go solid leaves a visible lattice, not a part per
// thousand.
//---------------------------------------------------------------------------
constexpr double kPowSpecBound       = 4.0e-6; ///< 2x the GLSL 4.10 bound at gamma 4
constexpr double kLimitRenderTolerance = 1.0e-3;

int runLimits( const Perturb& perturb )
{
	std::printf( "limits: the plate goes solid at the dark end and bare at the light end\n\n" );

	//---------------------------------------------------------------
	// The model, first. These are exact statements and are checked as such.
	//---------------------------------------------------------------
	const float maxWeight = 1.0f - perturb.limitSlack;
	for( float gamma : { 0.25f, 1.0f, 1.8f, 4.0f } )
	{
		const double dark  = TargetCoverage( 1.0f, gamma, 0.0f, maxWeight );
		const double light = TargetCoverage( 0.0f, gamma, 0.0f, maxWeight );
		Check( std::fabs( dark - 1.0 ) <= kPowSpecBound,
		       fmt( "model, gamma %.2f: target at black is %.8f (spec bound %.1e)", gamma, dark, kPowSpecBound ) );
		Check( light <= kPowSpecBound,
		       fmt( "model, gamma %.2f: target at white is %.8f", gamma, light ) );

		//And the union: however many sets, however engaged, they have to
		//reach exactly the target. This is the part a naive target/sets split
		//would get wrong in the middle and right at the ends.
		for( int sets = 1; sets <= 3; ++sets )
			for( float darkness : { 0.0f, 0.4f, 0.55f, 0.8f, 1.0f } )
			{
				const double target = TargetCoverage( darkness, gamma, 0.0f, maxWeight );
				float e[ 3 ] = { SetEngagement( 0, sets, darkness, 0.5f, 0.1f ),
				                 SetEngagement( 1, sets, darkness, 0.5f, 0.1f ),
				                 SetEngagement( 2, sets, darkness, 0.5f, 0.1f ) };
				FoldCoincidentSets( e, sets, 60.0f );

				const double sum = e[ 0 ] + e[ 1 ] + e[ 2 ];

				double bare = 1.0;
				for( int k = 0; k < 3; ++k )
					bare *= 1.0 - SetCoverage( static_cast< float >( target ), e[ k ], static_cast< float >( sum ) );
				const double united = 1.0 - bare;
				if( std::fabs( united - target ) > kPowSpecBound )
					Check( false, fmt( "model, gamma %.2f, %d sets, darkness %.2f: united %.8f vs target %.8f",
					                   gamma, sets, darkness, united, target ) );
			}
	}
	Check( true, "the union of any number of sets, at any engagement, is exactly the target" );

	//---------------------------------------------------------------
	// Then the render, at two rasters.
	//---------------------------------------------------------------
	const int rasters[][ 2 ] = { { 960, 540 }, { 400, 225 } };
	for( const auto& raster : rasters )
	{
		for( int sets = 1; sets <= 3; ++sets )
		{
			for( int end = 0; end < 2; ++end )
			{
				const float tone = end == 0 ? 0.0f : 1.0f;//black in, white in

				Rig rig;
				if( !rig.Init( raster[ 0 ], raster[ 1 ] ) )
					return 1;
				rig.Monochrome();
				rig.Upload( flatField( raster[ 0 ], raster[ 1 ], tone ) );
				rig.Set( PT_PITCH, ParamFromPitchLines( 30.0f ) );
				rig.Set( PT_ANGLE, angleParam( kWeightAngle ) );
				rig.Set( PT_SETS, static_cast< float >( sets - 1 ) );
				rig.Set( PT_CURVE, curveParam( 1.8f ) );
				rig.Set( PT_MIN_WEIGHT, 0.0f );
				rig.Set( PT_MAX_WEIGHT, maxWeight );
				rig.Set( PT_TAPER, 0.5f );
				if( !rig.Render() )
					return 1;

				const Field coverage = rig.Coverage();
				const double mean = meanOver( coverage, raster[ 0 ], raster[ 0 ] / 6, raster[ 1 ] / 6,
				                              raster[ 0 ] - raster[ 0 ] / 6, raster[ 1 ] - raster[ 1 ] / 6 );

				const double want = end == 0 ? 1.0 : 0.0;
				Check( std::fabs( mean - want ) <= kLimitRenderTolerance,
				       fmt( "%dx%d, %d set%s: %s input prints %.6f, wanted %.0f (tolerance %.0e)",
				            raster[ 0 ], raster[ 1 ], sets, sets == 1 ? "" : "s",
				            end == 0 ? "black" : "white", mean, want, kLimitRenderTolerance ) );
			}
		}
	}

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --perpendicular.
//
// On a plane sinusoid the gradient's direction is known: it runs at the angle
// the wave was built at, everywhere. The flow is the minor eigenvector, so it
// runs across the gradient, and the lines run along the flow. So the lines
// must come out perpendicular to the wave's gradient -- which is the whole of
// "the lines obey the picture".
//
// Angle Offset is set thirty degrees away from the right answer on purpose. If
// the flow were being ignored the lines would sit at Angle Offset and the
// check would read thirty degrees out, which is ten times the tolerance. That
// is also the negative control.
//
// **The tolerance.** The estimator is a Fourier response on a quarter-degree
// grid with a parabolic refinement, so its own resolution is under a tenth of
// a degree. What it is really allowing for is the flow field: `--flow`
// measures that at about a degree on a noisy ring field, and this card is
// clean and has a constant orientation, so a degree is generous. Two degrees
// is twice that and fifteen times inside the thirty-degree failure it is here
// to catch.
//---------------------------------------------------------------------------
constexpr double kPerpendicularTolerance = 2.0;
constexpr double kPerpendicularAgreement = 1.0;
constexpr float kWaveDegrees             = 34.0f;
constexpr float kWaveLength              = 0.22f;///< of the frame height

int runPerpendicular( const Perturb& perturb )
{
	std::printf( "perpendicular: on a plane wave, the lines run across the gradient\n\n" );

	const int rasters[][ 2 ] = { { 960, 540 }, { 432, 243 } };
	double lineAngles[ 2 ]   = { 0.0, 0.0 };

	for( int index = 0; index < 2; ++index )
	{
		const int width  = rasters[ index ][ 0 ];
		const int height = rasters[ index ][ 1 ];

		const float pitchParam = ParamFromPitchLines( 14.0f );
		const float pitchPx    = IntaglioPlugin::PitchPixels( pitchParam, height );

		Rig rig;
		if( !rig.Init( width, height ) )
			return 1;
		rig.Monochrome();
		rig.Upload( waveField( width, height, kWaveDegrees, kWaveLength ) );
		rig.Set( PT_PITCH, pitchParam );
		//Thirty degrees away from the truth: if the flow were ignored, this is
		//where the lines would be.
		rig.Set( PT_ANGLE, angleParam( std::fmod( kWaveDegrees + 90.0f + 30.0f, 180.0f ) ) );
		rig.Set( PT_SETS, 0.0f );
		rig.Set( PT_COHERENCE, perturb.disableFlow ? 0.0f : coherenceParam( 60.0f ) );
		rig.Set( PT_DETAIL, detailParam( 0.004f ) );
		rig.Set( PT_SMOOTHING, smoothingParam( 0.012f ) );
		//A flat coverage, so this is about direction and nothing else.
		rig.Set( PT_MIN_WEIGHT, 0.35f );
		rig.Set( PT_MAX_WEIGHT, 0.35f );
		rig.Set( PT_TAPER, 0.85f );
		if( !rig.Render() )
			return 1;

		const Field coverage = rig.Coverage();

		Grating first, second;
		topGratings( coverage, width, width / 4, height / 4, width - width / 4, height - height / 4,
		             pitchPx, std::max( 1, height / 180 ), 20.0, first, second );

		lineAngles[ index ] = first.degrees;

		const double want = std::fmod( kWaveDegrees + 90.0, 180.0 );
		const double off  = angleApart( first.degrees, want );

		Check( off <= kPerpendicularTolerance,
		       fmt( "%dx%d: lines at %.2f deg, gradient at %.0f deg, so %.2f deg from perpendicular "
		            "(tolerance %.0f)",
		            width, height, first.degrees, kWaveDegrees, off, kPerpendicularTolerance ) );
	}

	Check( angleApart( lineAngles[ 0 ], lineAngles[ 1 ] ) <= kPerpendicularAgreement,
	       fmt( "the two rasters agree to %.2f deg (tolerance %.0f)",
	            angleApart( lineAngles[ 0 ], lineAngles[ 1 ] ), kPerpendicularAgreement ) );

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --crosshatch.
//
// Two flat fields, one either side of the engage threshold. Above it one set;
// below it two, at Cross Angle to each other.
//
// A flat field has no structure, so the plate rules its sets at exactly Angle
// Offset and Angle Offset + Cross Angle -- the claim is about the model, and
// the measurement is the only thing between it and the answer. So the
// measurement is a Fourier response at the pitch the model was asked for,
// rather than a gradient histogram, for the reason written over
// `gratingResponse`: chopped lines bias a gradient estimate, and the bias
// grows as the raster shrinks.
//
// **The tolerance.** The search grid is a quarter of a degree and the
// parabolic refinement takes that to well under a tenth. The one thing that
// could shift a peak is the beat between the two sets, which lands at 1.03
// times the pitch frequency at 51 degrees for the angles used here, and
// carries about a fifth of a fundamental -- far enough away in angle to be
// excluded by the twenty-degree guard. One degree is the tolerance, and it is
// the estimator's number rather than the model's, because the model's is zero.
//---------------------------------------------------------------------------
constexpr double kCrossAngleTolerance = 1.0;
constexpr double kSingleSetRunnerUp   = 0.20;
constexpr float kCrossAngleAsked      = 62.0f;

int runCrosshatch( const Perturb& perturb )
{
	std::printf( "crosshatch: the second set arrives below the threshold, at the angle asked for\n\n" );

	const int rasters[][ 2 ] = { { 960, 540 }, { 432, 243 } };
	const float engage       = 0.55f;
	const float softness     = 0.05f;

	double separations[ 2 ] = { 0.0, 0.0 };

	for( int index = 0; index < 2; ++index )
	{
		const int width  = rasters[ index ][ 0 ];
		const int height = rasters[ index ][ 1 ];

		const float pitchParam = ParamFromPitchLines( 14.0f );
		const float pitchPx    = IntaglioPlugin::PitchPixels( pitchParam, height );

		for( int dark = 0; dark < 2; ++dark )
		{
			//Well clear of the threshold on both sides, so the softness is
			//not part of what is being measured here.
			const float darkness = dark != 0 ? engage + 0.25f : engage - 0.25f;
			const float tone     = 1.0f - darkness;

			Rig rig;
			if( !rig.Init( width, height ) )
				return 1;
			rig.Monochrome();
			rig.Upload( flatField( width, height, tone ) );
			rig.Set( PT_PITCH, pitchParam );
			rig.Set( PT_ANGLE, angleParam( 20.0f ) );
			rig.Set( PT_SETS, 1.0f );//two sets available
			rig.Set( PT_CROSS_ANGLE, angleParam( kCrossAngleAsked ) );
			rig.Set( PT_ENGAGE, engage );
			rig.Set( PT_ENGAGE_SOFT, engageSoftParam( softness ) );
			//A coverage that never closes up: two sets at full weight would be
			//a solid plate with no orientation left in it to measure.
			rig.Set( PT_MIN_WEIGHT, 0.34f );
			rig.Set( PT_MAX_WEIGHT, 0.34f );
			rig.Set( PT_TAPER, 0.85f );
			if( !rig.Render() )
				return 1;

			const Field coverage = rig.Coverage();

			Grating first, second;
			topGratings( coverage, width, width / 5, height / 5, width - width / 5, height - height / 5,
			             pitchPx, std::max( 1, height / 180 ), 20.0, first, second );

			const double ratio = first.power > 0.0 ? second.power / first.power : 0.0;

			if( dark == 0 )
			{
				Check( ratio < kSingleSetRunnerUp,
				       fmt( "%dx%d: above the threshold the runner-up grating carries %.3f of the peak "
				            "(must be under %.2f)",
				            width, height, ratio, kSingleSetRunnerUp ) );
				Check( angleApart( first.degrees, 20.0 ) <= kCrossAngleTolerance,
				       fmt( "%dx%d: the one set rules at %.2f deg, asked for 20", width, height,
				            first.degrees ) );
			}
			else
			{
				const double separation = angleApart( first.degrees, second.degrees );
				separations[ index ]    = separation;

				const double want = kCrossAngleAsked + perturb.crossExpectationOffset;
				const double off  = angleApart( separation, std::fmod( want, 180.0 ) );

				Check( ratio >= kSingleSetRunnerUp,
				       fmt( "%dx%d: below the threshold a second grating appears, carrying %.3f of the peak",
				            width, height, ratio ) );
				Check( off <= kCrossAngleTolerance,
				       fmt( "%dx%d: the two sets are %.2f deg apart, asked for %.0f, off by %.2f (tolerance %.0f)",
				            width, height, separation, want, off, kCrossAngleTolerance ) );
			}
		}
	}

	Check( angleApart( separations[ 0 ], separations[ 1 ] ) <= kCrossAngleTolerance,
	       fmt( "the two rasters agree to %.2f deg", angleApart( separations[ 0 ], separations[ 1 ] ) ) );

	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --negative.
//
// Every check above, run against a model that is deliberately wrong, and
// required to FAIL. A check that cannot fail is not a check, and the way a
// numeric check stops being able to fail is not that somebody deletes it --
// it is that a tolerance grows past the error it was meant to catch, or a
// measurement quietly starts measuring something that is true either way.
//---------------------------------------------------------------------------
int runNegative()
{
	struct Case
	{
		const char* name;
		int ( *check )( const Perturb& );
		Perturb perturb;
		const char* what;
	};

	std::vector< Case > cases;
	{
		Perturb p;
		p.pitchExpectationFactor = 1.15f;
		cases.push_back( { "pitch", runPitch, p, "expect a ruling 15% finer than the one asked for" } );
	}
	{
		Perturb p;
		p.disableFlow = true;
		cases.push_back( { "perpendicular", runPerpendicular, p, "turn the flow off with Coherence" } );
	}
	{
		Perturb p;
		p.weightModelGamma = 1.0f;
		cases.push_back( { "weight", runWeight, p, "score against a flat weight curve instead of gamma 1.8" } );
	}
	{
		Perturb p;
		p.crossExpectationOffset = 18.0f;
		cases.push_back( { "crosshatch", runCrosshatch, p, "expect the sets 18 degrees further apart" } );
	}
	{
		Perturb p;
		p.limitSlack = 0.01f;
		cases.push_back( { "limits", runLimits, p, "ask the plate for 99% weight instead of 100%" } );
	}

	int unfalsifiable = 0;

	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;

		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed,
			             observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n",
			             c.name );
			++unfalsifiable;
		}
	}

	std::printf( "\nnegative controls: %zu perturbations, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };

	for( const Size& size : sizes )
	{
		Rig rig;
		if( !rig.Init( size.w, size.h, false ) )
			return 1;

		//Warm up: the first frames allocate and compile pipeline state.
		//Timing them measures the driver.
		if( !rig.Render( 10 ) )
			return 1;
		glFinish();

		constexpr int kTimed = 30;
		const auto start     = std::chrono::steady_clock::now();
		if( !rig.Render( kTimed ) )
			return 1;
		glFinish();
		const auto end = std::chrono::steady_clock::now();

		const double ms =
			std::chrono::duration< double, std::milli >( end - start ).count() / static_cast< double >( kTimed );

		const float pitchPx = IntaglioPlugin::PitchPixels( rig.plugin.GetFloatParameter( PT_PITCH ), size.h );
		const int divisor   = IntaglioPlugin::PhaseDivisor( pitchPx, size.w, size.h );
		std::printf( "  %-6s %6.2f ms/frame   (phase buffer %dx%d, one texel per %d px)\n", size.name, ms,
		             ( size.w + divisor - 1 ) / divisor, ( size.h + divisor - 1 ) / divisor, divisor );
	}
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	std::string kind;
};

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( IntaglioPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ),
		                                 kindName( plugin.GetParamType( i ) ) } );
	}
	return list;
}

bool applySetting( IntaglioPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Parameter Name value' per line. The same format
// and the same interpolation as rosette's rztest, so one filming script drives
// any of the fleet's plugins.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// Raw RGBA frames on stdin, the plate on stdout, until stdin runs dry.
int runPipe( Rig& rig, const std::string& scriptPath )
{
	//Resolve the script's parameter names once, up front, and refuse to run
	//on a name that is not a parameter: a misspelled cue that silently did
	//nothing would produce a take that looks deliberate and is wrong.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( const NamedParameter& parameter : listParameters( rig.plugin ) )
			{
				if( parameter.name == entry.first )
				{
					automation[ parameter.index ] = entry.second;
					found                         = true;
					break;
				}
			}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	const int width  = rig.width;
	const int height = rig.height;
	Image frame( static_cast< size_t >( width ) * height * 4 );
	for( int index = 0;; ++index )
	{
		size_t filled = 0;
		while( filled < frame.size() )
		{
			const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
			if( got <= 0 )
				break;
			filled += static_cast< size_t >( got );
		}
		if( filled < frame.size() )
			break;

		for( const auto& track : automation )
			rig.Set( track.first, valueAt( track.second, index ) );

		//A raw frame arrives top row first and GL wants bottom row first.
		//One render per frame: the plate holds no history, so the output is
		//this frame's and nothing else's.
		rig.Upload( flipRows( frame, width, height ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Image out = flipRows( rig.Bytes(), width, height );
		size_t written  = 0;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
				return 1;
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/intaglio.png";
	std::string cardPath;
	std::string scriptPath;
	bool wantPipe = false;
	std::vector< std::string > settings;
	int width   = 1280;
	int height  = 720;
	int frames  = 2;
	float noise = 0.0f;
	std::string mode;
	float crop[ 4 ] = { 0.0f, 0.0f, 1.0f, 1.0f };
	bool cropping   = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" || argument == "-h" )
		{
			std::printf(
				"igtest -- render Intaglio offline and measure its plate\n\n"
				"  --out PATH            render the card and write it here\n"
				"  --card PATH           write the test card itself\n"
				"  --size WxH            render size (default 1280x720)\n"
				"  --frames N            frames to render before reading back (default 2)\n"
				"  --noise F             per-frame noise on the card, 0..1\n"
				"  --set \"Name=V\"        set a parameter by its display name. Repeatable.\n"
				"  --crop u0,v0,u1,v1    write only this rectangle of the frame, v up\n"
				"  --list                print every parameter and its default, then exit\n"
				"  --flow                measure the flow field against an analytic tangent\n"
				"  --pitch               count the lines a flat field is ruled with\n"
				"  --weight              coverage against the weight curve, at five tones\n"
				"  --limits              solid at the dark end, bare at the light end\n"
				"  --perpendicular       on a ramp, the lines run across the gradient\n"
				"  --crosshatch          the second set, and the angle between the sets\n"
				"  --negative            every check above, against a wrong model, must fail\n"
				"  --bench               time a frame at 720p through 4K\n"
				"  --pipe                raw RGBA frames on stdin, raw RGBA frames on stdout\n"
				"  --script PATH         parameter cues for --pipe: 'frame Name value'\n"
				"  --width N / --height N / --fps N   the fleet's --pipe spelling; --fps changes nothing\n"
				"  --help\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--crop" && hasNext )
		{
			if( std::sscanf( argv[ ++i ], "%f,%f,%f,%f", &crop[ 0 ], &crop[ 1 ], &crop[ 2 ], &crop[ 3 ] ) != 4 )
			{
				std::fprintf( stderr, "--crop wants u0,v0,u1,v1\n" );
				return 2;
			}
			cropping = true;
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			++i;//no clock: see the header
		else if( argument == "--flow" || argument == "--pitch" || argument == "--weight"
		         || argument == "--limits" || argument == "--perpendicular" || argument == "--crosshatch"
		         || argument == "--negative" || argument == "--bench" )
			mode = argument.substr( 2 );
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}

	//--list needs no GL at all: the parameters are declared in the constructor.
	if( mode == "list" )
	{
		IntaglioPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(),
			             parameter.kind.c_str(), parameter.value );
		return 0;
	}

	if( !cardPath.empty() )
	{
		const Image card = buildCard( width, height );
		if( !writePng( cardPath, width, height, flipRows( card, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s -- the test card, %dx%d\n", cardPath.c_str(), width, height );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	const Perturb none;

	if( mode == "flow" )
		result = runFlow();
	else if( mode == "pitch" )
		result = runPitch( none );
	else if( mode == "weight" )
		result = runWeight( none );
	else if( mode == "limits" )
		result = runLimits( none );
	else if( mode == "perpendicular" )
		result = runPerpendicular( none );
	else if( mode == "crosshatch" )
		result = runCrosshatch( none );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else
	{
		Rig rig;
		if( !rig.Init( width, height, false ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					CGLSetCurrentContext( nullptr );
					CGLDestroyContext( context );
					return 2;
				}
			}

			if( wantPipe )
				result = runPipe( rig, scriptPath );
			else if( !rig.Render( std::max( frames, 1 ), noise ) )
				result = 1;
			else
			{
				Image frame = flipRows( rig.Bytes(), width, height );
				int outW = width, outH = height;
				if( cropping )
					frame = cropTo( frame, width, height, crop[ 0 ], crop[ 1 ], crop[ 2 ], crop[ 3 ], outW, outH );

				if( writePng( outPath, outW, outH, frame ) )
					std::printf( "wrote %s -- %dx%d of a %dx%d frame, %d frames\n", outPath.c_str(), outW,
					             outH, width, height, frames );
				else
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					result = 1;
				}
			}
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
