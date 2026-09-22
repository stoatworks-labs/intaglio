#pragma once

#include "Controls.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace intaglio
{
/**
    The plugin.

    One effect, seven passes, no variants. See Shaders.h for the chain and
    AGENTS.md for why the phase pass exists at all.
*/
class IntaglioPlugin : public CFFGLPlugin
{
public:
	IntaglioPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// The smoothed structure tensor, for `igtest --flow` to probe. Nothing in
	/// the plugin's own operation reads this from outside; it is here so the
	/// harness can measure the direction estimate the shipping phase pass
	/// actually walks, rather than a copy of it written for the test.
	GLuint FlowTextureID() const
	{
		return tensorBuffer.TextureID();
	}

	/// How many pixels one hatch pitch is at this picture height. The harness
	/// needs it to say what "one line" means, and deriving it a second time in
	/// the test would be deriving the thing under test.
	static float PitchPixels( float pitchParam, int pictureHeight );

	/// The phase buffer's divisor, for the harness to assert against. See
	/// ProcessOpenGL: it is chosen from the ruling, not the raster.
	static int PhaseDivisor( float pitchPx, int pictureWidth, int pictureHeight );

private:
	/// Allocate every buffer for this picture size. Called once per frame
	/// before anything binds a texture, and a no-op unless something changed.
	///
	/// All of it up front, never mid-chain: `FFGLFBO::Initialise` sizes its
	/// colour texture under a scoped binding, and every `ffglex::Scoped*`
	/// binding clears to 0 on scope exit rather than restoring what was there
	/// -- so allocating between passes silently unbinds the texture the
	/// current pass is reading. The symptom is the dangerous part: correct on
	/// every frame except the one that allocates.
	bool ensureBuffers( GLsizei width, GLsizei height, GLsizei phaseWidth, GLsizei phaseHeight );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader toneShader;
	ffglex::FFGLShader tensorShader;
	ffglex::FFGLShader tensorBlurShader;
	ffglex::FFGLShader phaseShader;
	ffglex::FFGLShader engraveShader;
	ffglex::FFGLShader compositeShader;

	ffglex::FFGLScreenQuad quad;

	PassBuffer copyBuffer;
	PassBuffer toneBuffer;
	PassBuffer tensorBuffer;
	PassBuffer tensorTemp;   ///< the horizontal half of the separable blur
	PassBuffer phaseBuffer;
	PassBuffer inkBuffer;

	GLsizei bufferWidth  = 0;
	GLsizei bufferHeight = 0;
};

} // namespace intaglio
