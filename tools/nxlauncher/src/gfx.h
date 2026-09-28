#pragma once

#include <stdint.h>
#include <SDL2/SDL.h>

#define SCR_W 1280
#define SCR_H 720

// colors are 0xRRGGBBAA
#define RGBA(r, g, b, a) ((uint32_t)(((r) & 255) << 24 | ((g) & 255) << 16 | ((b) & 255) << 8 | ((a) & 255)))
#define WITH_ALPHA(c, a) (((c) & 0xFFFFFF00u) | ((uint32_t)(a) & 255))

enum Fonts
{
	FONT_TITLE,	// Bebas Neue
	FONT_BODY,	// Poppins Medium
	FONT_BOLD,	// Poppins Bold
	FONT_LIGHT,	// Poppins Regular
	FONT_COUNT
};

// palette
#define C_WHITE   0xFFFFFFFFu
#define C_TEXT    0xF2ECE8FFu
#define C_DIM     0xB3A7A0FFu
#define C_MUTED   0x8A7F7AFFu
#define C_ACCENT  0xFF4A1CFFu
#define C_ACCENT2 0xFFB020FFu
#define C_RED     0xC21E0EFFu
#define C_GREEN   0x3DDC84FFu
#define C_PANEL   0x0C0808B4u
#define C_PANEL2  0x1A1210D8u

int gfx_init(void);
void gfx_free(void);
SDL_Renderer *gfx_renderer(void);

void gfx_begin(void);
void gfx_end(void);
void gfx_screenshot(const char *path);

// shapes
void gfx_rect(float x, float y, float w, float h, uint32_t c);
void gfx_rrect(float x, float y, float w, float h, float r, uint32_t c);
// vertical (vert=1) or horizontal gradient
void gfx_rrect_grad(float x, float y, float w, float h, float r, uint32_t c1, uint32_t c2, int vert);
void gfx_bar(float x, float y, float w, float h, uint32_t c1, uint32_t c2);
void gfx_rrect_line(float x, float y, float w, float h, float r, float thick, uint32_t c);
void gfx_circle(float cx, float cy, float r, uint32_t c);
void gfx_glow(float cx, float cy, float r, uint32_t c);
// small triangle pointing up/down/left/right (0..3)
void gfx_arrow(float cx, float cy, float size, int dir, uint32_t c);

// text; y is the top of the line
float gfx_text(int font, int size, float x, float y, uint32_t c, const char *s);
float gfx_text_w(int font, int size, const char *s);
// right aligned / centered / truncated with an ellipsis
float gfx_text_r(int font, int size, float xr, float y, uint32_t c, const char *s);
float gfx_text_c(int font, int size, float cx, float y, uint32_t c, const char *s);
float gfx_text_fit(int font, int size, float x, float y, float maxw, uint32_t c, const char *s);
float gfx_line_h(int font, int size);

// button icon (A/B/X/Y/L/R/ZL/ZR/+/-/dpad); returns the width used
float gfx_button(float x, float cy, int button, float size);
// icon + label, returns width used
float gfx_hint(float x, float cy, int button, const char *label);

// textures from RGBA memory
SDL_Texture *gfx_make_texture(const uint32_t *rgba, int w, int h, int linear);
void gfx_draw_texture(SDL_Texture *t, float x, float y, float w, float h, uint8_t alpha);
