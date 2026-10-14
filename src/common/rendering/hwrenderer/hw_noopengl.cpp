//
// Pieces of the OpenGL backend that the rest of the engine uses whatever the
// renderer. Compiled only in a build without that backend (HAVE_OPENGL off,
// as in RDoom NX, which renders with Vulkan only).
//

#include "c_cvars.h"
#include "menu.h"
#include "zstring.h"

CVAR(Int, gl_multisample, 1, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);
CVAR(Int, gl_dither_bpc, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

// Same as gl_stereo3d.cpp: no quad-buffered stereo outside Windows
void UpdateVRModes(bool considerQuadBuffered)
{
	FOptionValues** pVRModes = OptionValues.CheckKey("VRMode");
	if (pVRModes == nullptr) return;

	TArray<FOptionValues::Pair>& vals = (*pVRModes)->mValues;
	TArray<FOptionValues::Pair> filteredValues;
	for (unsigned i = 0; i < vals.Size(); ++i)
	{
		if (vals[i].Value == 7) continue;	// quad-buffered stereo
		filteredValues.Push(vals[i]);
	}
	vals = filteredValues;
}

// The vram stat only reports the GL vendor extensions
void PrintVRAM_NV(FString &out) {}
void PrintVRAM_ATI(FString &out) {}
