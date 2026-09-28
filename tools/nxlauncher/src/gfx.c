#include "gfx.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

static SDL_Window *win;
static SDL_Renderer *ren;

//==========================================================================
// fonts
//==========================================================================

static const char *const font_files[FONT_COUNT] =
{
	"fonts/BebasNeue-Regular.ttf",
	"fonts/Poppins-Medium.ttf",
	"fonts/Poppins-Bold.ttf",
	"fonts/Poppins-Regular.ttf",
};

static unsigned char *font_data[FONT_COUNT];
static stbtt_fontinfo font_info[FONT_COUNT];

#define FIRST_CP 32
#define NUM_CP 224	// 32..255
#define NUM_EXTRA 6
static const int extra_cp[NUM_EXTRA] = { 0x2022, 0x2026, 0x2013, 0x2014, 0x201C, 0x201D };

typedef struct
{
	int font, size;
	int aw, ah;
	SDL_Texture *tex;
	stbtt_packedchar pc[NUM_CP];
	stbtt_packedchar px[NUM_EXTRA];
	float ascent, lineh;
} Atlas;

#define MAX_ATLAS 40
static Atlas atlases[MAX_ATLAS];
static int natlases;

static Atlas *GetAtlas(int font, int size)
{
	for (int i = 0; i < natlases; i++)
		if (atlases[i].font == font && atlases[i].size == size) return &atlases[i];
	if (natlases >= MAX_ATLAS || !font_data[font]) return NULL;

	Atlas *a = &atlases[natlases];
	memset(a, 0, sizeof(*a));
	a->font = font;
	a->size = size;

	// smallest atlas that fits (the Switch shares RAM with the GPU)
	int ov = size <= 24 ? 2 : 1;
	float cell = (float)(size * ov + 2);
	float need = 240.f * cell * cell * 0.62f;
	int aw = 256, ah = 256;
	while ((float)aw * ah < need) { if (aw <= ah) aw *= 2; else ah *= 2; }
	unsigned char *bmp = NULL;
	for (int attempt = 0; attempt < 4; attempt++)
	{
		bmp = calloc(aw, ah);
		stbtt_pack_context spc;
		stbtt_PackBegin(&spc, bmp, aw, ah, 0, 1, NULL);
		stbtt_PackSetOversampling(&spc, ov, ov);
		stbtt_pack_range ranges[2];
		memset(ranges, 0, sizeof(ranges));
		ranges[0].font_size = (float)size;
		ranges[0].first_unicode_codepoint_in_range = FIRST_CP;
		ranges[0].num_chars = NUM_CP;
		ranges[0].chardata_for_range = a->pc;
		ranges[1].font_size = (float)size;
		ranges[1].array_of_unicode_codepoints = (int *)extra_cp;
		ranges[1].num_chars = NUM_EXTRA;
		ranges[1].chardata_for_range = a->px;
		int ok = stbtt_PackFontRanges(&spc, font_data[font], 0, ranges, 2);
		stbtt_PackEnd(&spc);
		if (ok) break;
		free(bmp);
		bmp = NULL;
		if (aw <= ah) aw *= 2; else ah *= 2;
	}
	if (!bmp) return NULL;
	a->aw = aw;
	a->ah = ah;

	uint32_t *rgba = malloc((size_t)aw * ah * 4);
	for (int i = 0; i < aw * ah; i++)
	{
		unsigned char *p = (unsigned char *)&rgba[i];
		p[0] = p[1] = p[2] = 255;
		p[3] = bmp[i];
	}
	free(bmp);
	a->tex = gfx_make_texture(rgba, aw, ah, 1);
	free(rgba);
	SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);

	float scale = stbtt_ScaleForPixelHeight(&font_info[font], (float)size);
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font_info[font], &asc, &desc, &gap);
	a->ascent = asc * scale;
	a->lineh = (asc - desc + gap) * scale;
	natlases++;
	return a;
}

static int DecodeUTF8(const char **ps)
{
	const unsigned char *p = (const unsigned char *)*ps;
	int c = *p++;
	if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
	else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
	else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
	*ps = (const char *)p;
	return c;
}

static stbtt_packedchar *FindChar(Atlas *a, int cp, int *index, int *isextra)
{
	if (cp >= FIRST_CP && cp < FIRST_CP + NUM_CP) { *index = cp - FIRST_CP; *isextra = 0; return a->pc; }
	for (int i = 0; i < NUM_EXTRA; i++)
		if (extra_cp[i] == cp) { *index = i; *isextra = 1; return a->px; }
	*index = '?' - FIRST_CP;
	*isextra = 0;
	return a->pc;
}

static float DrawOrMeasure(int font, int size, float x, float y, uint32_t c, const char *s, int draw, float maxw)
{
	Atlas *a = GetAtlas(font, size);
	if (!a || !s) return 0;
	if (draw)
	{
		SDL_SetTextureColorMod(a->tex, c >> 24, (c >> 16) & 255, (c >> 8) & 255);
		SDL_SetTextureAlphaMod(a->tex, c & 255);
	}
	float px = x, py = y + a->ascent;
	float limit = maxw > 0 ? x + maxw : 1e9f;
	while (*s)
	{
		if (*s == '\n') break;
		int cp = DecodeUTF8(&s);
		int idx, ex;
		stbtt_packedchar *pc = FindChar(a, cp, &idx, &ex);
		stbtt_aligned_quad q;
		float nx = px;
		stbtt_GetPackedQuad(pc, a->aw, a->ah, idx, &nx, &py, &q, 0);
		if (nx > limit) break;
		if (draw)
		{
			SDL_Rect src = { (int)(q.s0 * a->aw + 0.5f), (int)(q.t0 * a->ah + 0.5f), (int)((q.s1 - q.s0) * a->aw + 0.5f), (int)((q.t1 - q.t0) * a->ah + 0.5f) };
			SDL_FRect dst = { q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0 };
			if (src.w > 0 && src.h > 0) SDL_RenderCopyF(ren, a->tex, &src, &dst);
		}
		px = nx;
	}
	return px - x;
}

float gfx_text(int font, int size, float x, float y, uint32_t c, const char *s)
{
	return DrawOrMeasure(font, size, x, y, c, s, 1, 0);
}

float gfx_text_w(int font, int size, const char *s)
{
	return DrawOrMeasure(font, size, 0, 0, 0, s, 0, 0);
}

float gfx_text_r(int font, int size, float xr, float y, uint32_t c, const char *s)
{
	float w = gfx_text_w(font, size, s);
	gfx_text(font, size, xr - w, y, c, s);
	return w;
}

float gfx_text_c(int font, int size, float cx, float y, uint32_t c, const char *s)
{
	float w = gfx_text_w(font, size, s);
	gfx_text(font, size, cx - w * 0.5f, y, c, s);
	return w;
}

float gfx_text_fit(int font, int size, float x, float y, float maxw, uint32_t c, const char *s)
{
	float w = gfx_text_w(font, size, s);
	if (w <= maxw) return gfx_text(font, size, x, y, c, s);
	float ew = gfx_text_w(font, size, "\xE2\x80\xA6");
	float used = DrawOrMeasure(font, size, x, y, c, s, 1, maxw - ew);
	gfx_text(font, size, x + used, y, c, "\xE2\x80\xA6");
	return used + ew;
}

float gfx_line_h(int font, int size)
{
	Atlas *a = GetAtlas(font, size);
	return a ? a->lineh : size;
}

//==========================================================================
// software rendered shapes, cached as textures
//==========================================================================

typedef struct
{
	int kind, w, h;
	float r, t;
	uint32_t c1, c2;
	SDL_Texture *tex;
	int age;
} Shape;

#define MAX_SHAPES 256
static Shape shapes[MAX_SHAPES];
static int nshapes;
static int frameno;

static float Coverage(float px, float py, float w, float h, float r)
{
	float qx = fabsf(px - w * 0.5f) - (w * 0.5f - r);
	float qy = fabsf(py - h * 0.5f) - (h * 0.5f - r);
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;
	float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
	float cov = 0.5f - d;
	return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

static uint32_t Lerp(uint32_t a, uint32_t b, float t)
{
	int r = (int)((a >> 24) + (((int)(b >> 24) - (int)(a >> 24)) * t));
	int g = (int)(((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t));
	int bl = (int)(((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t));
	int al = (int)((a & 255) + ((((int)b & 255) - (int)(a & 255)) * t));
	return RGBA(r, g, bl, al);
}

// kinds: 0 = fill (vertical gradient), 1 = fill (horizontal gradient), 2 = outline, 3 = glow
static SDL_Texture *GetShape(int kind, int w, int h, float r, float t, uint32_t c1, uint32_t c2)
{
	for (int i = 0; i < nshapes; i++)
	{
		Shape *s = &shapes[i];
		if (s->kind == kind && s->w == w && s->h == h && s->r == r && s->t == t && s->c1 == c1 && s->c2 == c2)
		{
			s->age = frameno;
			return s->tex;
		}
	}
	if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
	if (nshapes >= MAX_SHAPES)
	{
		// evict the oldest
		int old = 0;
		for (int i = 1; i < nshapes; i++) if (shapes[i].age < shapes[old].age) old = i;
		SDL_DestroyTexture(shapes[old].tex);
		shapes[old] = shapes[--nshapes];
	}
	uint32_t *px = calloc(w * h, 4);
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float fx = x + 0.5f, fy = y + 0.5f;
			float a = 0;
			uint32_t col = c1;
			if (kind == 0 || kind == 1)
			{
				a = Coverage(fx, fy, (float)w, (float)h, r);
				if (c2 != c1) col = Lerp(c1, c2, kind == 0 ? fy / h : fx / w);
			}
			else if (kind == 2)
			{
				float outer = Coverage(fx, fy, (float)w, (float)h, r);
				float ri = r - t > 0 ? r - t : 0;
				float inner = Coverage(fx - t, fy - t, w - 2 * t, h - 2 * t, ri);
				if (fx < t || fy < t || fx > w - t || fy > h - t) inner = 0;
				a = outer - inner;
				if (a < 0) a = 0;
			}
			else if (kind == 3)
			{
				float dx = (fx - w * 0.5f) / (w * 0.5f), dy = (fy - h * 0.5f) / (h * 0.5f);
				float d2 = dx * dx + dy * dy;
				a = d2 < 1 ? (1 - d2) * (1 - d2) : 0;
			}
			unsigned char *p = (unsigned char *)&px[y * w + x];
			p[0] = col >> 24;
			p[1] = (col >> 16) & 255;
			p[2] = (col >> 8) & 255;
			p[3] = (unsigned char)((col & 255) * a);
		}
	}
	SDL_Texture *tex = gfx_make_texture(px, w, h, 1);
	free(px);
	Shape *s = &shapes[nshapes++];
	s->kind = kind; s->w = w; s->h = h; s->r = r; s->t = t; s->c1 = c1; s->c2 = c2; s->tex = tex; s->age = frameno;
	return tex;
}

static void DrawShape(SDL_Texture *t, float x, float y, float w, float h)
{
	if (!t) return;
	SDL_FRect dst = { x, y, w, h };
	SDL_RenderCopyF(ren, t, NULL, &dst);
}

void gfx_rect(float x, float y, float w, float h, uint32_t c)
{
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(ren, c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255);
	SDL_FRect r = { x, y, w, h };
	SDL_RenderFillRectF(ren, &r);
}

void gfx_rrect(float x, float y, float w, float h, float r, uint32_t c)
{
	gfx_rrect_grad(x, y, w, h, r, c, c, 1);
}

void gfx_rrect_grad(float x, float y, float w, float h, float r, uint32_t c1, uint32_t c2, int vert)
{
	int iw = (int)ceilf(w), ih = (int)ceilf(h);
	DrawShape(GetShape(vert ? 0 : 1, iw, ih, r, 0, c1, c2), x, y, (float)iw, (float)ih);
}

void gfx_bar(float x, float y, float w, float h, uint32_t c1, uint32_t c2)
{
	// fixed-size texture stretched horizontally, for animated widths
	int ih = (int)ceilf(h);
	SDL_Texture *t = GetShape(1, 256, ih, h * 0.5f, 0, c1, c2);
	if (!t) return;
	SDL_FRect dst = { x, y, w, (float)ih };
	SDL_RenderCopyF(ren, t, NULL, &dst);
}

void gfx_rrect_line(float x, float y, float w, float h, float r, float thick, uint32_t c)
{
	int iw = (int)ceilf(w), ih = (int)ceilf(h);
	DrawShape(GetShape(2, iw, ih, r, thick, c, c), x, y, (float)iw, (float)ih);
}

void gfx_circle(float cx, float cy, float r, uint32_t c)
{
	int d = (int)ceilf(r * 2);
	DrawShape(GetShape(0, d, d, d * 0.5f, 0, c, c), cx - d * 0.5f, cy - d * 0.5f, (float)d, (float)d);
}

void gfx_glow(float cx, float cy, float r, uint32_t c)
{
	SDL_Texture *t = GetShape(3, 128, 128, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
	if (!t) return;
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
	SDL_SetTextureColorMod(t, c >> 24, (c >> 16) & 255, (c >> 8) & 255);
	SDL_SetTextureAlphaMod(t, c & 255);
	SDL_FRect dst = { cx - r, cy - r, r * 2, r * 2 };
	SDL_RenderCopyF(ren, t, NULL, &dst);
	SDL_SetTextureColorMod(t, 255, 255, 255);
	SDL_SetTextureAlphaMod(t, 255);
}

void gfx_arrow(float cx, float cy, float size, int dir, uint32_t c)
{
	SDL_Vertex v[3];
	SDL_Color col = { c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255 };
	float s = size * 0.5f;
	SDL_FPoint p[3];
	switch (dir)
	{
	case 0: p[0] = (SDL_FPoint){ cx, cy - s }; p[1] = (SDL_FPoint){ cx + s, cy + s * 0.6f }; p[2] = (SDL_FPoint){ cx - s, cy + s * 0.6f }; break;
	case 1: p[0] = (SDL_FPoint){ cx, cy + s }; p[1] = (SDL_FPoint){ cx - s, cy - s * 0.6f }; p[2] = (SDL_FPoint){ cx + s, cy - s * 0.6f }; break;
	case 2: p[0] = (SDL_FPoint){ cx - s, cy }; p[1] = (SDL_FPoint){ cx + s * 0.6f, cy - s }; p[2] = (SDL_FPoint){ cx + s * 0.6f, cy + s }; break;
	default: p[0] = (SDL_FPoint){ cx + s, cy }; p[1] = (SDL_FPoint){ cx - s * 0.6f, cy + s }; p[2] = (SDL_FPoint){ cx - s * 0.6f, cy - s }; break;
	}
	for (int i = 0; i < 3; i++) { v[i].position = p[i]; v[i].color = col; v[i].tex_coord = (SDL_FPoint){ 0, 0 }; }
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

//==========================================================================
// button icons
//==========================================================================

float gfx_button(float x, float cy, int button, float size)
{
	const char *label = NULL;
	int round = 1;
	switch (button)
	{
	case BTN_A: label = "A"; break;
	case BTN_B: label = "B"; break;
	case BTN_X: label = "X"; break;
	case BTN_Y: label = "Y"; break;
	case BTN_L: label = "L"; round = 0; break;
	case BTN_R: label = "R"; round = 0; break;
	case BTN_ZL: label = "ZL"; round = 0; break;
	case BTN_ZR: label = "ZR"; round = 0; break;
	case BTN_PLUS: label = "+"; break;
	case BTN_MINUS: label = "\xE2\x80\x93"; break;
	default: break;
	}
	int fs = (int)(size * 0.62f);
	if (button == BTN_UP || button == BTN_DOWN || button == BTN_LEFT || button == BTN_RIGHT || button == BTN_LSTICK)
	{
		// d-pad: a little cross, the direction highlighted
		float s = size, t = s * 0.34f;
		float x0 = x, y0 = cy - s * 0.5f;
		gfx_rrect(x0 + (s - t) * 0.5f, y0, t, s, t * 0.3f, 0xE8E0DAFFu);
		gfx_rrect(x0, y0 + (s - t) * 0.5f, s, t, t * 0.3f, 0xE8E0DAFFu);
		uint32_t hi = C_ACCENT;
		if (button == BTN_UP || button == BTN_LSTICK) gfx_rrect(x0 + (s - t) * 0.5f, y0, t, s * 0.36f, t * 0.3f, hi);
		if (button == BTN_DOWN || button == BTN_LSTICK) gfx_rrect(x0 + (s - t) * 0.5f, y0 + s * 0.64f, t, s * 0.36f, t * 0.3f, hi);
		if (button == BTN_LEFT) gfx_rrect(x0, y0 + (s - t) * 0.5f, s * 0.36f, t, t * 0.3f, hi);
		if (button == BTN_RIGHT) gfx_rrect(x0 + s * 0.64f, y0 + (s - t) * 0.5f, s * 0.36f, t, t * 0.3f, hi);
		return s;
	}
	if (!label) return 0;
	float w = round ? size : fmaxf(size * 1.35f, gfx_text_w(FONT_BOLD, fs, label) + size * 0.7f);
	gfx_rrect(x, cy - size * 0.5f, w, size, round ? size * 0.5f : size * 0.3f, 0xECE4DEFFu);
	float lh = gfx_line_h(FONT_BOLD, fs);
	gfx_text_c(FONT_BOLD, fs, x + w * 0.5f, cy - lh * 0.5f + fs * 0.06f, 0x1A1210FFu, label);
	return w;
}

float gfx_hint(float x, float cy, int button, const char *label)
{
	float w = gfx_button(x, cy, button, 28);
	float lh = gfx_line_h(FONT_BODY, 19);
	float tw = gfx_text(FONT_BODY, 19, x + w + 8, cy - lh * 0.5f + 1, C_TEXT, label);
	return w + 8 + tw + 26;
}

//==========================================================================
// general
//==========================================================================

SDL_Texture *gfx_make_texture(const uint32_t *rgba, int w, int h, int linear)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "1" : "0");
	SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
	if (!t) return NULL;
	SDL_UpdateTexture(t, NULL, rgba, w * 4);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

void gfx_draw_texture(SDL_Texture *t, float x, float y, float w, float h, uint8_t alpha)
{
	if (!t) return;
	SDL_SetTextureAlphaMod(t, alpha);
	SDL_FRect dst = { x, y, w, h };
	SDL_RenderCopyF(ren, t, NULL, &dst);
}

int gfx_init(void)
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0) return -1;
	win = SDL_CreateWindow("UZDoom Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCR_W, SCR_H, 0);
	if (!win) return -2;
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	if (!ren) return -3;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

	for (int i = 0; i < FONT_COUNT; i++)
	{
		size_t sz = 0;
		font_data[i] = plat_load_resource(font_files[i], &sz);
		if (!font_data[i] || !stbtt_InitFont(&font_info[i], font_data[i], stbtt_GetFontOffsetForIndex(font_data[i], 0)))
		{
			free(font_data[i]);
			font_data[i] = NULL;
			if (i == FONT_BODY) return -4;
		}
	}
	// fall back to the body font
	for (int i = 0; i < FONT_COUNT; i++)
	{
		if (!font_data[i])
		{
			font_data[i] = font_data[FONT_BODY];
			font_info[i] = font_info[FONT_BODY];
		}
	}
	return 0;
}

void gfx_free(void)
{
	for (int i = 0; i < natlases; i++) SDL_DestroyTexture(atlases[i].tex);
	natlases = 0;
	for (int i = 0; i < nshapes; i++) SDL_DestroyTexture(shapes[i].tex);
	nshapes = 0;
	if (ren) SDL_DestroyRenderer(ren);
	if (win) SDL_DestroyWindow(win);
	ren = NULL;
	win = NULL;
	SDL_Quit();
}

SDL_Renderer *gfx_renderer(void)
{
	return ren;
}

void gfx_begin(void)
{
	frameno++;
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
}

static void TakeScreenshot(void);

void gfx_end(void)
{
	TakeScreenshot();
	SDL_RenderPresent(ren);
}

#ifndef __SWITCH__
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

static char shotpath[256];

// the picture is taken at the end of the next frame, before it is presented
void gfx_screenshot(const char *path)
{
	strncpy(shotpath, path, sizeof(shotpath) - 1);
}

static void TakeScreenshot(void)
{
#ifndef __SWITCH__
	if (!shotpath[0]) return;
	uint32_t *px = malloc(SCR_W * SCR_H * 4);
	if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGBA32, px, SCR_W * 4) == 0)
		stbi_write_png(shotpath, SCR_W, SCR_H, 4, px, SCR_W * 4);
	free(px);
#endif
	shotpath[0] = 0;
}
