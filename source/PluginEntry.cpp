#include "Intaglio.h"

/**
    The one registration.

    This file is listed directly in the Intaglio MODULE target, not in
    intaglio_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC archive
    the linker is entitled to drop the whole translation unit -- giving a
    bundle that loads, exports `plugMain`, and reports that it contains no
    plugins. The core stays an OBJECT library for the same reason.

        nm -gU Intaglio.bundle/Contents/MacOS/Intaglio | grep plugMain

    The name is `SW Intaglio`, eleven characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so -- a longer name comes back cut off in the effect list and
    nothing anywhere reports an error. `oxbow probe` is what reads it back the
    way a host does.
*/
namespace
{
class IntaglioEffect : public intaglio::IntaglioPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< IntaglioEffect >,                      // Create method
	"IG01",                                               // Plugin unique ID of maximum length 4
	"SW Intaglio",                                        // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Engraved line shading: tone carried by the pitch and weight of a ruled line",
	"Intaglio FFGL effect"                                // About
);

extern "C" const char* IntaglioBuildStamp()
{
	return "intaglio " INTAGLIO_VERSION ", built " __DATE__ " " __TIME__;
}
