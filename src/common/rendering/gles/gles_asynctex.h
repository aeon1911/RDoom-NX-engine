#pragma once
// Background preparation of textures (Switch port).
//
// Reading and decoding a sprite from the mod's archive takes several
// milliseconds on the Switch's CPU. Large gameplay mods have
// thousands of sprite frames, so every shot, explosion or new enemy used
// to stall the game while dozens of new frames were decoded. Sprites and
// HUD graphics are now decoded by worker threads; until a picture is ready
// it is simply not drawn (for a frame or two), and a few finished ones are
// uploaded to the GPU per frame.

#include "textures.h"

namespace OpenGLESRenderer
{
	// 1 = ready (out is filled), 0 = still being prepared, -1 = do it synchronously
	int AsyncTex_Request(FTexture *tex, int translation, int flags, FTextureBuffer &out);
	void AsyncTex_BindPlaceholder(int texunit);
	void AsyncTex_NewFrame();
	void AsyncTex_Uploaded();
	void AsyncTex_Flush();
	void AsyncTex_Shutdown();
	void AsyncTex_Set2D(bool on);
	void AsyncTex_Install();
}
