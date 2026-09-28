#pragma once
#include <stdint.h>

// RGBA image, bytes in memory order R,G,B,A
typedef struct
{
	uint32_t *px;
	int w, h;
} Image;

// Finds the title screen picture (TITLEPIC / TITLE) in the given game file
// (WAD or PK3/IPK3) - the user's own game data - and converts it to RGBA.
// palette_fallback: an optional WAD to take PLAYPAL from.
int art_load_title(const char *path, const char *palette_fallback, Image *out);
void art_free(Image *img);

// procedural background (dark, warm glow at the bottom)
void art_make_background(Image *out, int w, int h);

// writes the 1280x720 background for the engine's loading screen
// (art may be NULL); returns 0 on success
int art_write_loadbg(const Image *art, const char *outpath);
