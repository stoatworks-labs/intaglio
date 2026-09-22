#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace intaglio
{
namespace
{
/// value^0 = low, value^1 = high, with every octave the same width of travel.
float geometric( float value, float low, float high )
{
	return low * std::pow( high / low, value );
}

float linear( float value, float low, float high )
{
	return low + ( high - low ) * value;
}

constexpr float kPitchCoarse = 8.0f;
constexpr float kPitchFine   = 200.0f;
} // namespace

float PitchLinesFromParam( float value )
{
	//Reversed: parameter up is pitch up is fewer lines.
	return geometric( 1.0f - value, kPitchCoarse, kPitchFine );
}

float ParamFromPitchLines( float lines )
{
	const float clamped = std::clamp( lines, kPitchCoarse, kPitchFine );
	return 1.0f - std::log( clamped / kPitchCoarse ) / std::log( kPitchFine / kPitchCoarse );
}

float DetailFromParam( float value )
{
	return geometric( value, 0.0005f, 0.012f );
}

float SmoothingFromParam( float value )
{
	return geometric( value, 0.0005f, 0.03f );
}

float CoherenceFromParam( float value )
{
	return linear( value, 0.0f, 120.0f );
}

float AngleFromParam( float value )
{
	return linear( value, 0.0f, 180.0f );
}

float CurveFromParam( float value )
{
	return geometric( value, 0.25f, 4.0f );
}

float MinWeightFromParam( float value )
{
	return value;
}

float MaxWeightFromParam( float value )
{
	return value;
}

float TaperFromParam( float value )
{
	return value;
}

float CrossAngleFromParam( float value )
{
	return linear( value, 0.0f, 180.0f );
}

float EngageFromParam( float value )
{
	return value;
}

float EngageSoftnessFromParam( float value )
{
	return geometric( value, 0.01f, 0.5f );
}

float PlateToneFromParam( float value )
{
	return linear( value, 0.0f, 0.35f );
}

float BiteFromParam( float value )
{
	return linear( value, 0.0f, 0.75f );
}

float BurrFromParam( float value )
{
	return value;
}

//---------------------------------------------------------------------------
// The coverage model. Mirrored in GLSL; every mirrored line is marked on both
// sides, the way rosette marks its spot functions.
//---------------------------------------------------------------------------
float TargetCoverage( float darkness, float gamma, float minWeight, float maxWeight )
{
	const float d = std::clamp( darkness, 0.0f, 1.0f );//= mirrored
	const float g = std::pow( d, gamma );              //= mirrored
	return std::clamp( minWeight + ( maxWeight - minWeight ) * g, 0.0f, 1.0f );//= mirrored
}

float SetEngagement( int set, int sets, float darkness, float engage, float softness )
{
	if( set >= sets )
		return 0.0f;//= mirrored
	if( set == 0 )
		return 1.0f;//= mirrored

	//The second set arrives at Engage Threshold, the third a fixed distance
	//further into the shadows. A separate control for the third would be a
	//parameter nobody moves; the gap is what makes three sets read as a
	//progression rather than as two sets that both appear at once.
	const float t = engage + ( set == 2 ? kThirdSetGap : 0.0f );//= mirrored
	const float s = std::max( softness, 1e-4f );                //= mirrored

	//smoothstep, written out: the GLSL built-in and std:: have no shared
	//definition, and this is one of the numbers --crosshatch checks.
	const float x = std::clamp( ( darkness - ( t - s ) ) / ( 2.0f * s ), 0.0f, 1.0f );//= mirrored
	return x * x * ( 3.0f - 2.0f * x );                                                //= mirrored
}

void FoldCoincidentSets( float engagement[ 3 ], int sets, float crossAngleDegrees )
{
	for( int k = 1; k < 3; ++k )
	{
		if( k >= sets || engagement[ k ] <= 0.0f )
			continue;//= mirrored

		for( int j = 0; j < k; ++j )
		{
			if( engagement[ j ] <= 0.0f )
				continue;//= mirrored

			float apart = std::fmod( std::fabs( static_cast< float >( k - j ) * crossAngleDegrees ), 180.0f );//= mirrored
			apart       = std::min( apart, 180.0f - apart );//= mirrored

			if( apart < 0.5f )//= mirrored
			{
				engagement[ j ] += engagement[ k ];//= mirrored
				engagement[ k ] = 0.0f;            //= mirrored
				break;                             //= mirrored
			}
		}
	}
}

float SetCoverage( float target, float engagement, float engagementSum )
{
	if( engagementSum <= 1e-6f || engagement <= 0.0f )
		return 0.0f;//= mirrored

	const float rest = std::max( 1.0f - target, 0.0f );//= mirrored
	return 1.0f - std::pow( rest, engagement / engagementSum );//= mirrored
}

} // namespace intaglio
