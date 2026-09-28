// UZDoom Switch launcher - user interface

#include "platform.h"
#include "gfx.h"
#include "data.h"
#include "art.h"
#include "ui.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

//==========================================================================
// input with key repeat
//==========================================================================

static uint32_t held, prevheld, fired;
static uint64_t repeat_at[BTN_COUNT];

static void InputUpdate(void)
{
	prevheld = held;
	held = plat_buttons();
	fired = 0;
	uint64_t now = plat_ticks();
	for (int b = 0; b < BTN_COUNT; b++)
	{
		uint32_t bit = 1u << b;
		if ((held & bit) && !(prevheld & bit))
		{
			fired |= bit;
			repeat_at[b] = now + 380;
		}
		else if ((held & bit) && (b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT || b == BTN_ZL || b == BTN_ZR) && now >= repeat_at[b])
		{
			fired |= bit;
			repeat_at[b] = now + 70;
		}
	}
}

static int Pressed(int b)
{
	return (fired >> b) & 1;
}

//==========================================================================
// background: procedural gradient, the game's own title picture, embers
//==========================================================================

static SDL_Texture *bg_tex;

typedef struct
{
	char iwad[MAX_PATH_LEN];
	Image img;
	SDL_Texture *tex;
	int tried;
} ArtCache;

#define ART_CACHE 8
static ArtCache art_cache[ART_CACHE];
static int art_next;
static ArtCache *cur_art, *prev_art;
static float art_fade = 1.f;

typedef struct { float x, y, vx, vy, life, max, size; } Ember;
#define NUM_EMBERS 56
static Ember embers[NUM_EMBERS];
static uint32_t rnd = 0x9E3779B9u;
static float Rand01(void) { rnd = rnd * 1664525u + 1013904223u; return (rnd >> 8) * (1.f / 16777216.f); }

static ArtCache *GetArt(const char *iwad)
{
	for (int i = 0; i < ART_CACHE; i++)
		if (art_cache[i].tried && !strcasecmp(art_cache[i].iwad, iwad)) return &art_cache[i];
	ArtCache *a = &art_cache[art_next];
	art_next = (art_next + 1) % ART_CACHE;
	if (a == cur_art || a == prev_art)
	{
		a = &art_cache[art_next];
		art_next = (art_next + 1) % ART_CACHE;
	}
	if (a->tex) SDL_DestroyTexture(a->tex);
	art_free(&a->img);
	memset(a, 0, sizeof(*a));
	strncpy(a->iwad, iwad, MAX_PATH_LEN - 1);
	a->tried = 1;
	char path[600], pal[600];
	data_iwad_path(iwad, path, sizeof(path));
	// a palette to fall back to for game files without one
	pal[0] = 0;
	static const char *const palwads[] = { "doom2.wad", "doom.wad", "plutonia.wad", "tnt.wad", "freedoom2.wad", "freedoom1.wad", "doom1.wad", NULL };
	for (int i = 0; palwads[i]; i++)
	{
		char pp[600];
		data_iwad_path(palwads[i], pp, sizeof(pp));
		struct stat st;
		if (stat(pp, &st) == 0) { strcpy(pal, pp); break; }
	}
	if (art_load_title(path, pal[0] ? pal : NULL, &a->img))
	{
		// darkened copy that fades out to the left and to the bottom
		int w = a->img.w, h = a->img.h;
		uint32_t *px = malloc(w * h * 4);
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				float u = x / (float)(w - 1), v = y / (float)(h - 1);
				float left = u < 0.55f ? u / 0.55f : 1.f;
				left = left * left * (3 - 2 * left);
				float bottom = v > 0.6f ? 1.f - (v - 0.6f) / 0.4f * 0.85f : 1.f;
				const unsigned char *s = (const unsigned char *)&a->img.px[y * w + x];
				unsigned char *d = (unsigned char *)&px[y * w + x];
				d[0] = (unsigned char)(s[0] * 0.6f);
				d[1] = (unsigned char)(s[1] * 0.6f);
				d[2] = (unsigned char)(s[2] * 0.6f);
				d[3] = (unsigned char)(s[3] * left * bottom);
			}
		}
		a->tex = gfx_make_texture(px, w, h, 1);
		free(px);
	}
	return a;
}

static void SelectArt(const char *iwad)
{
	ArtCache *a = GetArt(iwad);
	if (a == cur_art) return;
	prev_art = cur_art;
	cur_art = a;
	art_fade = 0.f;
}

static void BackgroundInit(void)
{
	Image bg;
	art_make_background(&bg, SCR_W, SCR_H);
	bg_tex = gfx_make_texture(bg.px, bg.w, bg.h, 1);
	art_free(&bg);

	for (int i = 0; i < NUM_EMBERS; i++) embers[i].life = 0;
}

static void DrawArt(ArtCache *a, uint8_t alpha)
{
	if (!a || !a->tex) return;
	// classic 320x200 / 640x400 pictures have non-square pixels
	float aspect = a->img.w / (float)a->img.h;
	if (a->img.h == 200 || a->img.h == 400) aspect /= 1.2f;
	const float h = SCR_H, w = h * aspect, x = SCR_W - w;
	gfx_draw_texture(a->tex, x, 0, w, h, alpha);
}

static void DrawBackground(float dt)
{
	gfx_draw_texture(bg_tex, 0, 0, SCR_W, SCR_H, 255);
	art_fade += dt * 3.f;
	if (art_fade > 1.f) art_fade = 1.f;
	if (art_fade < 1.f) DrawArt(prev_art, (uint8_t)(255 * (1.f - art_fade)));
	DrawArt(cur_art, (uint8_t)(255 * art_fade));

	for (int i = 0; i < NUM_EMBERS; i++)
	{
		Ember *e = &embers[i];
		e->life -= dt;
		if (e->life <= 0)
		{
			e->x = Rand01() * SCR_W;
			e->y = SCR_H + 10 + Rand01() * 60;
			e->vx = (Rand01() - 0.5f) * 26.f;
			e->vy = -(30.f + Rand01() * 80.f);
			e->max = e->life = 4.f + Rand01() * 6.f;
			e->size = 2.f + Rand01() * 4.f;
		}
		e->x += e->vx * dt + sinf(e->life * 1.7f + e->size) * 10.f * dt;
		e->y += e->vy * dt;
		float a = fminf(1.f, e->life / e->max * 1.5f) * fminf(1.f, (e->max - e->life) * 2.f);
		gfx_glow(e->x, e->y, e->size * 3.2f, WITH_ALPHA(0xFF7A2AFFu, (int)(150 * a)));
	}
}

//==========================================================================
// common pieces
//==========================================================================

enum Tabs { TAB_GAMES, TAB_MODS, TAB_OPTIONS, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "JOGOS", "MODS", "OPÇÕES" };
static int tab = TAB_GAMES;
static float tab_x[TAB_COUNT], tab_w[TAB_COUNT];
static float tab_bar_x = -1, tab_bar_w;

static char toast[256];
static float toast_time;

static void Toast(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(toast, sizeof(toast), fmt, ap);
	va_end(ap);
	toast_time = 2.5f;
}

static void DrawHeader(float dt)
{
	gfx_text(FONT_TITLE, 62, 48, 22, C_WHITE, "UZDOOM");
	float w = gfx_text_w(FONT_TITLE, 62, "UZDOOM");
	float lx = 48 + w + 12;
	lx += gfx_text(FONT_TITLE, 26, lx, 48, C_ACCENT, "LAUNCHER");
	gfx_circle(lx + 10, 61, 2.5f, C_MUTED);
	gfx_text(FONT_TITLE, 26, lx + 20, 48, C_DIM, "POR RENAN");

	// tabs
	float x = 590;
	float bw = gfx_button(x, 50, BTN_L, 30);
	x += bw + 24;
	for (int i = 0; i < TAB_COUNT; i++)
	{
		tab_w[i] = gfx_text_w(FONT_TITLE, 34, tab_names[i]);
		tab_x[i] = x;
		gfx_text(FONT_TITLE, 34, x, 30, i == tab ? C_WHITE : C_MUTED, tab_names[i]);
		x += tab_w[i] + 38;
	}
	gfx_button(x - 14, 50, BTN_R, 30);

	float tx = tab_x[tab] - 4, tw = tab_w[tab] + 8;
	if (tab_bar_x < 0) { tab_bar_x = tx; tab_bar_w = tw; }
	float k = fminf(1.f, dt * 14.f);
	tab_bar_x += (tx - tab_bar_x) * k;
	tab_bar_w += (tw - tab_bar_w) * k;
	gfx_bar(tab_bar_x, 70, tab_bar_w, 5, C_RED, C_ACCENT2);
	gfx_glow(tab_bar_x + tab_bar_w * 0.5f, 72, tab_bar_w * 0.6f, 0xFF5A2A40u);
}

typedef struct { int button; const char *label; } Hint;

static void DrawToast(float dt)
{
	if (toast_time <= 0) return;
	toast_time -= dt;
	float a = fminf(1.f, toast_time * 3.f);
	float w = gfx_text_w(FONT_BODY, 20, toast) + 48;
	float x = (SCR_W - w) * 0.5f, y = SCR_H - 120;
	gfx_rrect(x, y, w, 44, 22, WITH_ALPHA(0x1E1411FFu, (int)(235 * a)));
	gfx_rrect_line(x, y, w, 44, 22, 1.5f, WITH_ALPHA(C_ACCENT, (int)(200 * a)));
	gfx_text_c(FONT_BODY, 20, SCR_W * 0.5f, y + 9, WITH_ALPHA(C_TEXT, (int)(255 * a)), toast);
}

static void FormatSize(double mb, char *buf, int n)
{
	if (mb >= 1024) snprintf(buf, n, "%.1f GB", mb / 1024.0);
	else if (mb >= 10) snprintf(buf, n, "%.0f MB", mb);
	else if (mb > 0) snprintf(buf, n, "%.1f MB", mb);
	else buf[0] = 0;
}

static double IwadMB(const char *iwad)
{
	for (int i = 0; i < n_iwad_files; i++) if (!strcasecmp(iwad_files[i].name, iwad)) return iwad_files[i].mb;
	return 0;
}

static double PwadMB(const char *name)
{
	for (int i = 0; i < n_pwad_files; i++) if (!strcasecmp(pwad_files[i].name, name)) return pwad_files[i].mb;
	return -1;	// missing
}

// scrolling list helper
static void ClampScroll(int sel, int *scroll, int visible, int count)
{
	if (sel < *scroll) *scroll = sel;
	if (sel >= *scroll + visible) *scroll = sel - visible + 1;
	if (*scroll > count - visible) *scroll = count - visible;
	if (*scroll < 0) *scroll = 0;
}

static void DrawScrollbar(float x, float y, float h, int scroll, int visible, int count)
{
	if (count <= visible) return;
	gfx_rrect(x, y, 4, h, 2, 0xFFFFFF14u);
	float th = h * visible / count;
	float ty = y + (h - th) * scroll / (float)(count - visible);
	gfx_rrect(x, ty, 4, th, 2, 0xFFFFFF60u);
}

// word wrap; returns number of lines drawn
static int DrawWrapped(int font, int size, float x, float y, float maxw, uint32_t c, const char *text, float lh)
{
	char line[512];
	int lines = 0;
	const char *p = text;
	while (*p)
	{
		int len = 0, lastspace = -1;
		while (p[len] && p[len] != '\n' && len < 510)
		{
			line[len] = p[len];
			line[len + 1] = 0;
			if (p[len] == ' ') lastspace = len;
			if (gfx_text_w(font, size, line) > maxw && lastspace > 0)
			{
				len = lastspace;
				break;
			}
			len++;
		}
		line[len] = 0;
		gfx_text(font, size, x, y + lines * lh, c, line);
		lines++;
		p += len;
		if (*p == ' ' || *p == '\n') p++;
	}
	return lines;
}

//==========================================================================
// modal dialogs (blocking)
//==========================================================================

static uint64_t last_frame;
static void (*draw_screen)(float dt);

static float FrameDT(void)
{
	uint64_t now = plat_ticks();
	float dt = (now - last_frame) / 1000.f;
	last_frame = now;
	if (dt > 0.1f) dt = 0.1f;
	if (dt < 0) dt = 0;
	return dt;
}

static void DrawDialogBox(float w, float h, float *ox, float *oy)
{
	gfx_rect(0, 0, SCR_W, SCR_H, 0x000000A8u);
	float x = (SCR_W - w) * 0.5f, y = (SCR_H - h) * 0.5f;
	gfx_glow(SCR_W * 0.5f, y + h * 0.5f, w * 0.7f, 0xFF4A1C30u);
	gfx_rrect_grad(x, y, w, h, 18, 0x221613F6u, 0x140D0BF6u, 1);
	gfx_rrect_line(x, y, w, h, 18, 1.5f, 0xFFFFFF22u);
	gfx_rrect_grad(x + 28, y + 70, 72, 4, 2, C_RED, C_ACCENT2, 0);
	*ox = x;
	*oy = y;
}

static int RunDialog(const char *title, const char *text, int confirm)
{
	for (;;)
	{
		if (!plat_loop()) return 0;
		InputUpdate();
		float dt = FrameDT();
		gfx_begin();
		draw_screen(dt);
		float x, y;
		DrawDialogBox(740, 340, &x, &y);
		gfx_text(FONT_TITLE, 46, x + 28, y + 20, C_WHITE, title);
		DrawWrapped(FONT_BODY, 21, x + 28, y + 92, 684, C_TEXT, text, 31);
		float hx = x + 28;
		if (confirm)
		{
			hx += gfx_hint(hx, y + 300, BTN_A, "Sim");
			gfx_hint(hx, y + 300, BTN_B, "Não");
		}
		else gfx_hint(hx, y + 300, BTN_A, "OK");
		gfx_end();
		if (Pressed(BTN_A)) { fired = 0; return 1; }
		if (Pressed(BTN_B)) { fired = 0; return 0; }
	}
}

void ui_message(const char *title, const char *text)
{
	RunDialog(title, text, 0);
}

int ui_confirm(const char *title, const char *text)
{
	return RunDialog(title, text, 1);
}

// list picker; returns the chosen index or -1
static int RunPicker(const char *title, const char *const *items, const char *const *extra, int n, int sel)
{
	int scroll = 0;
	const int visible = 8;
	if (sel < 0 || sel >= n) sel = 0;
	for (;;)
	{
		if (!plat_loop()) return -1;
		InputUpdate();
		float dt = FrameDT();
		if (Pressed(BTN_DOWN) && n) sel = (sel + 1) % n;
		if (Pressed(BTN_UP) && n) sel = (sel + n - 1) % n;
		ClampScroll(sel, &scroll, visible, n);
		gfx_begin();
		draw_screen(dt);
		float x, y;
		DrawDialogBox(780, 580, &x, &y);
		gfx_text(FONT_TITLE, 46, x + 28, y + 20, C_WHITE, title);
		if (n == 0) gfx_text(FONT_BODY, 21, x + 28, y + 100, C_DIM, "Nenhum arquivo encontrado.");
		for (int i = scroll; i < n && i < scroll + visible; i++)
		{
			float ry = y + 90 + (i - scroll) * 52;
			if (i == sel)
			{
				gfx_rrect_grad(x + 18, ry, 744, 46, 10, 0xFF4A1C50u, 0xFF4A1C10u, 0);
				gfx_rrect(x + 18, ry + 9, 4, 28, 2, C_ACCENT);
			}
			gfx_text_fit(FONT_BODY, 21, x + 36, ry + 9, extra ? 470 : 700, i == sel ? C_WHITE : C_TEXT, items[i]);
			if (extra && extra[i]) gfx_text_r(FONT_LIGHT, 17, x + 744, ry + 12, C_DIM, extra[i]);
		}
		DrawScrollbar(x + 766, y + 90, visible * 52 - 6, scroll, visible, n);
		float hx = x + 28;
		hx += gfx_hint(hx, y + 544, BTN_A, "Escolher");
		gfx_hint(hx, y + 544, BTN_B, "Cancelar");
		gfx_end();
		if (Pressed(BTN_A) && n) { fired = 0; return sel; }
		if (Pressed(BTN_B)) { fired = 0; return -1; }
	}
}

//==========================================================================
// GAMES tab
//==========================================================================

static int games_scroll;
static float games_sel_y = -1;

static void Games_Draw(float dt)
{
	Profile *cp = cur_profile >= 0 ? &profiles[cur_profile] : NULL;
	const int visible = 6;
	const float lx = 40, ly = 100, cw = 520, ch = 80, gap = 10;
	ClampScroll(cur_profile, &games_scroll, visible, nprofiles);

	// profile cards
	float target = ly + (cur_profile - games_scroll) * (ch + gap);
	if (games_sel_y < 0) games_sel_y = target;
	games_sel_y += (target - games_sel_y) * fminf(1.f, dt * 16.f);
	for (int i = games_scroll; i < nprofiles && i < games_scroll + visible; i++)
	{
		float y = ly + (i - games_scroll) * (ch + gap);
		gfx_rrect(lx, y, cw, ch, 14, i == cur_profile ? 0x2A1A15ECu : 0x160E0CC8u);
		if (i != cur_profile) gfx_rrect_line(lx, y, cw, ch, 14, 1.f, 0xFFFFFF14u);
	}
	if (cur_profile >= 0)
	{
		gfx_glow(lx + 10, games_sel_y + ch * 0.5f, 70, 0xFF4A1C48u);
		gfx_rrect_line(lx, games_sel_y, cw, ch, 14, 2.f, 0xFF6A3AC8u);
		gfx_rrect_grad(lx + 9, games_sel_y + 16, 5, ch - 32, 2.5f, C_ACCENT2, C_RED, 1);
	}
	for (int i = games_scroll; i < nprofiles && i < games_scroll + visible; i++)
	{
		Profile *p = &profiles[i];
		float y = ly + (i - games_scroll) * (ch + gap);
		uint32_t tc = p->present ? (i == cur_profile ? C_WHITE : C_TEXT) : C_MUTED;
		gfx_text_fit(FONT_BOLD, 22, lx + 30, y + 12, cw - 60, tc, p->name);
		char sub[300];
		const char *t = data_iwad_title(p->iwad);
		const char *base = (t && !p->builtin) ? t : p->iwad;
		if (!p->present) snprintf(sub, sizeof(sub), "Jogo base não encontrado: %s", p->iwad);
		else if (p->nmods == 0) snprintf(sub, sizeof(sub), "%s  \xE2\x80\xA2  sem mods", base);
		else snprintf(sub, sizeof(sub), "%s  \xE2\x80\xA2  %d mod%s", base, p->nmods, p->nmods == 1 ? "" : "s");
		gfx_text_fit(FONT_LIGHT, 16, lx + 30, y + 46, cw - 60, p->present ? C_DIM : 0xE0503AFFu, sub);
	}
	DrawScrollbar(lx + cw + 8, ly, visible * (ch + gap) - gap, games_scroll, visible, nprofiles);

	if (!cp) return;

	// details
	const float dx = 620, dw = SCR_W - dx - 48;
	gfx_text_fit(FONT_TITLE, 66, dx, 96, dw, C_WHITE, cp->name);
	char line[400], sz[32];
	FormatSize(IwadMB(cp->iwad), sz, sizeof(sz));
	const char *title = data_iwad_title(cp->iwad);
	snprintf(line, sizeof(line), "Jogo base: %s%s%s%s", title ? title : cp->iwad, sz[0] ? "  (" : "", sz, sz[0] ? ")" : "");
	gfx_text_fit(FONT_BODY, 19, dx, 172, dw, C_TEXT, line);

	gfx_text(FONT_TITLE, 26, dx, 218, C_ACCENT, "ORDEM DE CARREGAMENTO");
	if (cp->nmods == 0)
	{
		gfx_text(FONT_LIGHT, 18, dx, 254, C_DIM, "Nenhum mod. Vá para a aba MODS (botão R) para adicionar.");
	}
	else
	{
		int show = cp->nmods > 7 ? 6 : cp->nmods;
		for (int i = 0; i < show; i++)
		{
			float y = 254 + i * 34;
			char num[8];
			snprintf(num, sizeof(num), "%d", i + 1);
			gfx_circle(dx + 13, y + 14, 13, 0xFFFFFF22u);
			gfx_text_c(FONT_BOLD, 15, dx + 13, y + 4, C_TEXT, num);
			double mb = PwadMB(cp->mods[i]);
			gfx_text_fit(FONT_BODY, 19, dx + 38, y + 1, dw - 150, mb < 0 ? 0xE0503AFFu : C_TEXT, cp->mods[i]);
			if (mb < 0) gfx_text_r(FONT_LIGHT, 15, dx + dw, y + 4, 0xE0503AFFu, "não encontrado");
			else { FormatSize(mb, sz, sizeof(sz)); gfx_text_r(FONT_LIGHT, 15, dx + dw, y + 4, C_MUTED, sz); }
		}
		if (cp->nmods > show)
		{
			snprintf(line, sizeof(line), "+ %d outros", cp->nmods - show);
			gfx_text(FONT_LIGHT, 17, dx + 38, 254 + show * 34 + 2, C_DIM, line);
		}
	}

	// setting chips
	float cx = dx, cy = 510;
	char chip[3][96];
	static const char *const skills[6] = { "no jogo", "1", "2", "3", "4", "5" };
	snprintf(chip[0], sizeof(chip[0]), "Desempenho: %s", preset_names[cp->preset]);
	snprintf(chip[1], sizeof(chip[1]), "Resolução: %d%%", cp->scale);
	snprintf(chip[2], sizeof(chip[2]), "Dificuldade: %s", skills[cp->skill]);
	for (int i = 0; i < 3; i++)
	{
		float w = gfx_text_w(FONT_BODY, 16, chip[i]) + 28;
		if (cx + w > dx + dw) break;
		gfx_rrect(cx, cy, w, 34, 17, 0x00000070u);
		gfx_rrect_line(cx, cy, w, 34, 17, 1.f, 0xFFFFFF26u);
		gfx_text(FONT_BODY, 16, cx + 14, cy + 7, C_TEXT, chip[i]);
		cx += w + 10;
	}

	// play button
	float bw = 250, bh = 64, bx = dx + dw - bw, by = 566;
	if (cp->present)
	{
		float pulse = 0.5f + 0.5f * sinf(plat_ticks() / 400.f);
		gfx_glow(bx + bw * 0.5f, by + bh * 0.5f, bw * 0.75f, WITH_ALPHA(0xFF4A1CFFu, (int)(40 + 40 * pulse)));
		gfx_rrect_grad(bx, by, bw, bh, 16, C_RED, 0xFF6A1CFFu, 0);
		gfx_rrect_line(bx, by, bw, bh, 16, 1.5f, 0xFFFFFF40u);
		gfx_text(FONT_TITLE, 42, bx + 84, by + 10, C_WHITE, "JOGAR");
		gfx_button(bx + 28, by + bh * 0.5f, BTN_A, 38);
	}
}

static void NewProfile(int copy)
{
	char name[96] = { 0 };
	Profile *cp = cur_profile >= 0 ? &profiles[cur_profile] : NULL;
	if (copy && cp)
	{
		snprintf(name, sizeof(name), "%.80s (cópia)", cp->name);
		int idx = data_add_profile(name, cp->iwad, cp);
		if (idx >= 0) { cur_profile = idx; data_save(); Toast("Perfil duplicado: %s", profiles[idx].name); }
		return;
	}
	int ok = plat_keyboard("Nome do novo perfil", "", name, sizeof(name));
	fired = 0;
	if (!ok || !name[0]) return;
	static const char *items[MAX_FILES], *extra[MAX_FILES];
	static char sizes[MAX_FILES][32];
	for (int i = 0; i < n_iwad_files; i++)
	{
		const char *t = data_iwad_title(iwad_files[i].name);
		items[i] = t ? t : iwad_files[i].name;
		FormatSize(iwad_files[i].mb, sizes[i], 32);
		extra[i] = t ? iwad_files[i].name : sizes[i];
	}
	int sel = 0;
	for (int i = 0; cp && i < n_iwad_files; i++) if (!strcasecmp(iwad_files[i].name, cp->iwad)) sel = i;
	int pick = RunPicker("Escolha o jogo base", items, extra, n_iwad_files, sel);
	if (pick < 0) return;
	int copymods = cp && cp->nmods > 0 && ui_confirm("Copiar mods?", "Copiar os mods, a ordem e as opções do perfil selecionado para o novo perfil?");
	int idx = data_add_profile(name, iwad_files[pick].name, copymods ? cp : NULL);
	if (idx < 0) { ui_message("Erro", "Não foi possível criar o perfil (limite atingido)."); return; }
	cur_profile = idx;
	data_save();
	Toast("Perfil criado: %s", profiles[idx].name);
}

static void Games_Update(void)
{
	if (nprofiles == 0) return;
	if (Pressed(BTN_DOWN)) cur_profile = (cur_profile + 1) % nprofiles;
	if (Pressed(BTN_UP)) cur_profile = (cur_profile + nprofiles - 1) % nprofiles;
	if (Pressed(BTN_Y)) NewProfile(0);
	if (Pressed(BTN_MINUS)) NewProfile(1);
	if (Pressed(BTN_X))
	{
		Profile *p = &profiles[cur_profile];
		if (p->builtin && p->present)
			ui_message("Perfil do jogo", "Este perfil é criado automaticamente para o jogo base e não pode ser excluído. Os mods dele podem ser removidos na aba MODS.");
		else
		{
			char q[300];
			snprintf(q, sizeof(q), "Excluir o perfil \"%s\"? Os arquivos dos mods não são apagados.", p->name);
			if (ui_confirm("Excluir perfil", q))
			{
				data_delete_profile(cur_profile);
				data_save();
				Toast("Perfil excluído");
			}
		}
	}
}

//==========================================================================
// MODS tab
//==========================================================================

static int mods_col;	// 0 = load order, 1 = available
static int order_sel, order_scroll, avail_sel, avail_scroll;
static int moving = -1;	// index being moved
static int move_from;
static int avail_list[MAX_FILES], n_avail;

static void BuildAvail(const Profile *p)
{
	n_avail = 0;
	for (int i = 0; i < n_pwad_files; i++)
		if (data_find_mod(p, pwad_files[i].name) < 0) avail_list[n_avail++] = i;
	if (avail_sel >= n_avail) avail_sel = n_avail - 1;
	if (avail_sel < 0) avail_sel = 0;
}

static void SwapMods(Profile *p, int a, int b)
{
	char tmp[MAX_PATH_LEN];
	memcpy(tmp, p->mods[a], MAX_PATH_LEN);
	memcpy(p->mods[a], p->mods[b], MAX_PATH_LEN);
	memcpy(p->mods[b], tmp, MAX_PATH_LEN);
}

static void Mods_Draw(float dt)
{
	(void)dt;
	Profile *p = &profiles[cur_profile];
	BuildAvail(p);
	char buf[300], sz[32];
	snprintf(buf, sizeof(buf), "MODS  \xE2\x80\x94  %s", p->name);
	gfx_text_fit(FONT_TITLE, 40, 48, 94, SCR_W - 96, C_WHITE, buf);

	const float py = 146, ph = 456, rowh = 46;
	const int visible = 8;
	float colx[2] = { 40, 660 }, colw = 580;
	for (int c = 0; c < 2; c++)
	{
		gfx_rrect(colx[c], py, colw, ph, 16, 0x0C0808C8u);
		if (c == mods_col) gfx_rrect_line(colx[c], py, colw, ph, 16, 2.f, moving >= 0 ? 0xFFB020D0u : 0xFF6A3AB0u);
	}
	snprintf(buf, sizeof(buf), "ORDEM DE CARREGAMENTO  (%d)", p->nmods);
	gfx_text(FONT_TITLE, 26, colx[0] + 22, py + 14, mods_col == 0 ? C_ACCENT : C_MUTED, buf);
	gfx_text(FONT_LIGHT, 14, colx[0] + 22, py + 44, C_MUTED, "carregados de cima para baixo (o último tem prioridade)");
	snprintf(buf, sizeof(buf), "DISPONÍVEIS  (%d)", n_avail);
	gfx_text(FONT_TITLE, 26, colx[1] + 22, py + 14, mods_col == 1 ? C_ACCENT : C_MUTED, buf);
	gfx_text(FONT_LIGHT, 14, colx[1] + 22, py + 44, C_MUTED, "arquivos em /switch/gzdoom/pwads");

	// load order
	ClampScroll(order_sel, &order_scroll, visible, p->nmods);
	if (p->nmods == 0)
		DrawWrapped(FONT_LIGHT, 18, colx[0] + 22, py + 90, colw - 44, C_DIM, "Nenhum mod ativo. Aperte o direcional para a direita e depois A nos arquivos que quiser carregar.", 28);
	for (int i = order_scroll; i < p->nmods && i < order_scroll + visible; i++)
	{
		float y = py + 74 + (i - order_scroll) * rowh;
		int sel = mods_col == 0 && i == order_sel;
		if (sel)
		{
			if (moving >= 0)
			{
				gfx_glow(colx[0] + colw * 0.5f, y + 20, colw * 0.45f, 0xFFB02030u);
				gfx_rrect(colx[0] + 12, y, colw - 24, rowh - 6, 10, 0x3A2412F0u);
				gfx_rrect_line(colx[0] + 12, y, colw - 24, rowh - 6, 10, 2.f, C_ACCENT2);
				gfx_arrow(colx[0] + colw - 40, y + 11, 12, 0, i > 0 ? C_ACCENT2 : 0xFFFFFF30u);
				gfx_arrow(colx[0] + colw - 40, y + 29, 12, 1, i < p->nmods - 1 ? C_ACCENT2 : 0xFFFFFF30u);
			}
			else
			{
				gfx_rrect_grad(colx[0] + 12, y, colw - 24, rowh - 6, 10, 0xFF4A1C48u, 0xFF4A1C10u, 0);
				gfx_rrect(colx[0] + 12, y + 8, 4, rowh - 22, 2, C_ACCENT);
			}
		}
		char num[8];
		snprintf(num, sizeof(num), "%d", i + 1);
		gfx_circle(colx[0] + 42, y + 20, 14, sel ? C_ACCENT : 0xFFFFFF22u);
		gfx_text_c(FONT_BOLD, 15, colx[0] + 42, y + 10, sel ? C_WHITE : C_TEXT, num);
		double mb = PwadMB(p->mods[i]);
		gfx_text_fit(FONT_BODY, 19, colx[0] + 68, y + 7, colw - 200, mb < 0 ? 0xE0503AFFu : (sel ? C_WHITE : C_TEXT), p->mods[i]);
		if (!(sel && moving >= 0))
		{
			if (mb < 0) gfx_text_r(FONT_LIGHT, 15, colx[0] + colw - 26, y + 10, 0xE0503AFFu, "faltando");
			else { FormatSize(mb, sz, sizeof(sz)); gfx_text_r(FONT_LIGHT, 15, colx[0] + colw - 26, y + 10, C_MUTED, sz); }
		}
	}
	DrawScrollbar(colx[0] + colw - 10, py + 74, visible * rowh - 6, order_scroll, visible, p->nmods);

	// available
	ClampScroll(avail_sel, &avail_scroll, visible, n_avail);
	if (n_avail == 0)
		DrawWrapped(FONT_LIGHT, 18, colx[1] + 22, py + 90, colw - 44, C_DIM, n_pwad_files ? "Todos os arquivos já estão na ordem de carregamento." : "Nenhum arquivo. Copie os mods (.pk3, .wad, .zip, .deh) para a pasta /switch/gzdoom/pwads do cartão SD.", 28);
	for (int k = avail_scroll; k < n_avail && k < avail_scroll + visible; k++)
	{
		FileEntry *f = &pwad_files[avail_list[k]];
		float y = py + 74 + (k - avail_scroll) * rowh;
		int sel = mods_col == 1 && k == avail_sel;
		if (sel)
		{
			gfx_rrect_grad(colx[1] + 12, y, colw - 24, rowh - 6, 10, 0xFF4A1C48u, 0xFF4A1C10u, 0);
			gfx_rrect(colx[1] + 12, y + 8, 4, rowh - 22, 2, C_ACCENT);
		}
		gfx_text_fit(FONT_BODY, 19, colx[1] + 30, y + 7, colw - 170, sel ? C_WHITE : C_TEXT, f->name);
		FormatSize(f->mb, sz, sizeof(sz));
		gfx_text_r(FONT_LIGHT, 15, colx[1] + colw - 26, y + 10, C_MUTED, sz);
	}
	DrawScrollbar(colx[1] + colw - 10, py + 74, visible * rowh - 6, avail_scroll, visible, n_avail);

}

static void Mods_Update(void)
{
	Profile *p = &profiles[cur_profile];
	BuildAvail(p);
	if (moving >= 0)
	{
		if (Pressed(BTN_UP) && order_sel > 0) { SwapMods(p, order_sel, order_sel - 1); order_sel--; }
		if (Pressed(BTN_DOWN) && order_sel < p->nmods - 1) { SwapMods(p, order_sel, order_sel + 1); order_sel++; }
		if (Pressed(BTN_A))
		{
			moving = -1;
			data_save();
			Toast("Ordem salva");
		}
		else if (Pressed(BTN_B))
		{
			// put it back where it was
			while (order_sel > move_from) { SwapMods(p, order_sel, order_sel - 1); order_sel--; }
			while (order_sel < move_from) { SwapMods(p, order_sel, order_sel + 1); order_sel++; }
			moving = -1;
		}
		return;
	}

	if (Pressed(BTN_RIGHT) && mods_col == 0) mods_col = 1;
	if (Pressed(BTN_LEFT) && mods_col == 1) mods_col = 0;

	if (mods_col == 0)
	{
		if (p->nmods == 0) return;
		if (order_sel >= p->nmods) order_sel = p->nmods - 1;
		if (Pressed(BTN_DOWN)) order_sel = (order_sel + 1) % p->nmods;
		if (Pressed(BTN_UP)) order_sel = (order_sel + p->nmods - 1) % p->nmods;
		if (Pressed(BTN_A)) { moving = order_sel; move_from = order_sel; return; }
		// quick move without picking up
		int dir = Pressed(BTN_ZL) ? -1 : Pressed(BTN_ZR) ? 1 : 0;
		if (dir && order_sel + dir >= 0 && order_sel + dir < p->nmods)
		{
			SwapMods(p, order_sel, order_sel + dir);
			order_sel += dir;
			data_save();
		}
		if (Pressed(BTN_X))
		{
			char name[MAX_PATH_LEN];
			strcpy(name, p->mods[order_sel]);
			memmove(p->mods[order_sel], p->mods[order_sel + 1], (size_t)(p->nmods - order_sel - 1) * MAX_PATH_LEN);
			p->nmods--;
			if (order_sel >= p->nmods && order_sel > 0) order_sel--;
			data_save();
			Toast("Removido: %s", name);
		}
	}
	else
	{
		if (n_avail == 0) return;
		if (Pressed(BTN_DOWN)) avail_sel = (avail_sel + 1) % n_avail;
		if (Pressed(BTN_UP)) avail_sel = (avail_sel + n_avail - 1) % n_avail;
		if (Pressed(BTN_A))
		{
			if (p->nmods >= MAX_MODS) { ui_message("Limite", "Este perfil já tem o máximo de mods."); return; }
			FileEntry *f = &pwad_files[avail_list[avail_sel]];
			strncpy(p->mods[p->nmods++], f->name, MAX_PATH_LEN - 1);
			order_sel = p->nmods - 1;
			data_save();
			Toast("Adicionado: %s", f->name);
		}
	}
}

//==========================================================================
// OPTIONS tab
//==========================================================================

enum Opts
{
	OPT_PRESET, OPT_SCALE, OPT_FPSLIMIT, OPT_GLTHREAD, OPT_FPS, OPT_LOADSCREEN, OPT_SKILL, OPT_MONSTERS, OPT_WARP, OPT_CLASS,
	OPT_LOADGAME, OPT_LOG, OPT_EXTRA, OPT_RENAME, OPT_IWAD, OPT_COUNT
};

static int opt_sel, opt_scroll;
static float opt_sel_y = -1;
static const char *const skill_names[6] = { "Escolher no jogo", "1 - Muito fácil", "2 - Fácil", "3 - Normal", "4 - Difícil", "5 - Pesadelo" };

static void OptInfo(Profile *p, int o, const char **label, const char **desc, char *value, int vlen, int *kind)
{
	// kind: 0 = choice (left/right), 1 = action (A)
	*kind = 0;
	switch (o)
	{
	case OPT_PRESET: *label = "Desempenho"; *desc = preset_desc[p->preset]; snprintf(value, vlen, "%s", preset_names[p->preset]); break;
	case OPT_SCALE: *label = "Resolução de renderização"; *desc = "Menos resolução = mais FPS em cenas pesadas (100% = 720p)."; snprintf(value, vlen, "%d%%", p->scale); break;
	case OPT_FPSLIMIT:
		*label = "Limite de FPS";
		*desc = p->fpslimit == 30 ? "Travado em 30 com VSync (1 quadro a cada 2 atualizações da tela): o mais estável."
			: p->fpslimit == 35 ? "35 = 1 quadro por tic do jogo, sem VSync."
			: p->fpslimit == 40 ? "Limite de 40 sem VSync: mais fluido quando sobra desempenho, oscila mais."
			: "VSync a 60 Hz: sobe e desce conforme a cena.";
		if (p->fpslimit == 30) snprintf(value, vlen, "30 FPS travado");
		else if (p->fpslimit > 0) snprintf(value, vlen, "%d FPS", p->fpslimit);
		else snprintf(value, vlen, "Sem limite (VSync)");
		break;
	case OPT_GLTHREAD:
		*label = "Driver de vídeo em outro núcleo";
		*desc = "EXPERIMENTAL: driver de vídeo em outro núcleo (pode dar mais FPS, mas já travou o jogo no console). Se congelar, desligue e mande o hang.log.";
		snprintf(value, vlen, "%s", p->glthread ? "Ligado" : "Desligado");
		break;
	case OPT_FPS: *label = "Mostrar FPS"; *desc = "Contador de quadros por segundo no canto da tela."; snprintf(value, vlen, "%s", p->showfps ? "Sim" : "Não"); break;
	case OPT_LOADSCREEN: *label = "Tela de carregamento"; *desc = "Mostra o progresso enquanto o jogo abre. Desligue só se der problema de vídeo ao abrir."; snprintf(value, vlen, "%s", p->noloadscreen ? "Desligada" : "Ligada"); break;
	case OPT_SKILL: *label = "Dificuldade"; *desc = "Pular a escolha de dificuldade ao iniciar um novo jogo."; snprintf(value, vlen, "%s", skill_names[p->skill]); break;
	case OPT_MONSTERS: *label = "Monstros"; *desc = "Modificadores de monstros (-nomonsters, -fast, -respawn)."; snprintf(value, vlen, "%s", monster_names[p->monsters]); break;
	case OPT_WARP: *label = "Mapa inicial"; *desc = "Começar direto em um mapa (ex.: MAP01, E1M1). A para editar, X para limpar."; snprintf(value, vlen, "%s", p->warp[0] ? p->warp : "\xE2\x80\x94"); *kind = 1; break;
	case OPT_CLASS: *label = "Classe do jogador"; *desc = "Para jogos/mods com várias classes. A para editar, X para limpar."; snprintf(value, vlen, "%s", p->pclass[0] ? p->pclass : "\xE2\x80\x94"); *kind = 1; break;
	case OPT_LOADGAME: *label = "Carregar jogo salvo"; *desc = "Abrir direto em um save (pasta /switch/gzdoom/save). X para limpar."; snprintf(value, vlen, "%s", p->loadgame[0] ? p->loadgame : "Nenhum"); *kind = 1; break;
	case OPT_LOG: *label = "Gravar log"; *desc = "Salva o console em /switch/gzdoom/gzdoom.log (para diagnosticar erros)."; snprintf(value, vlen, "%s", p->writelog ? "Sim" : "Não"); break;
	case OPT_EXTRA: *label = "Argumentos extras"; *desc = "Parâmetros avançados de linha de comando do UZDoom."; snprintf(value, vlen, "%s", p->extra[0] ? p->extra : "\xE2\x80\x94"); *kind = 1; break;
	case OPT_RENAME: *label = "Renomear perfil"; *desc = "Muda o nome que aparece na lista de jogos."; snprintf(value, vlen, "%s", p->name); *kind = 1; break;
	case OPT_IWAD: *label = "Jogo base (IWAD)"; *desc = "Troca o jogo base deste perfil, mantendo os mods."; snprintf(value, vlen, "%s", p->iwad); *kind = 1; break;
	}
}

static void Options_Draw(float dt)
{
	Profile *p = &profiles[cur_profile];
	char buf[300];
	snprintf(buf, sizeof(buf), "OPÇÕES  \xE2\x80\x94  %s", p->name);
	gfx_text_fit(FONT_TITLE, 40, 48, 94, SCR_W - 96, C_WHITE, buf);

	const float px = 40, py = 146, pw = SCR_W - 80, rowh = 64;
	const int visible = 7;
	ClampScroll(opt_sel, &opt_scroll, visible, OPT_COUNT);
	gfx_rrect(px, py, pw, visible * rowh + 16, 16, 0x0C0808C8u);
	float target = py + 8 + (opt_sel - opt_scroll) * rowh;
	if (opt_sel_y < 0) opt_sel_y = target;
	opt_sel_y += (target - opt_sel_y) * fminf(1.f, dt * 18.f);
	gfx_rrect_grad(px + 10, opt_sel_y, pw - 20, rowh - 6, 12, 0xFF4A1C48u, 0xFF4A1C08u, 0);
	gfx_rrect(px + 10, opt_sel_y + 12, 4, rowh - 30, 2, C_ACCENT);

	for (int o = opt_scroll; o < OPT_COUNT && o < opt_scroll + visible; o++)
	{
		const char *label = "", *desc = "";
		char value[300];
		int kind;
		OptInfo(p, o, &label, &desc, value, sizeof(value), &kind);
		float y = py + 8 + (o - opt_scroll) * rowh;
		int sel = o == opt_sel;
		gfx_text(FONT_BODY, 21, px + 30, y + 6, sel ? C_WHITE : C_TEXT, label);
		gfx_text_fit(FONT_LIGHT, 14, px + 30, y + 35, 720, sel ? C_DIM : C_MUTED, desc);
		float vr = px + pw - 30;
		if (kind == 0)
		{
			gfx_arrow(vr - 6, y + 23, 12, 3, sel ? C_ACCENT : 0xFFFFFF40u);
			float w = gfx_text_w(FONT_BOLD, 19, value);
			gfx_text(FONT_BOLD, 19, vr - 22 - w, y + 11, sel ? C_WHITE : C_TEXT, value);
			gfx_arrow(vr - 22 - w - 16, y + 23, 12, 2, sel ? C_ACCENT : 0xFFFFFF40u);
		}
		else
		{
			float w = gfx_text_w(FONT_BODY, 18, value);
			if (w > 360) w = 360;
			gfx_text_fit(FONT_BODY, 18, vr - w, y + 12, 360, sel ? C_WHITE : C_DIM, value);
		}
	}
	DrawScrollbar(px + pw - 8, py + 8, visible * rowh, opt_scroll, visible, OPT_COUNT);
}

static void PickSave(Profile *p)
{
	static char names[MAX_FILES][MAX_PATH_LEN];
	static const char *items[MAX_FILES];
	int n = 0;
	DIR *d = opendir(BASEDIR "/save");
	if (d)
	{
		struct dirent *e;
		while ((e = readdir(d)) && n < MAX_FILES)
		{
			const char *dot = strrchr(e->d_name, '.');
			if (!dot || strcasecmp(dot, ".zds")) continue;
			strncpy(names[n], e->d_name, MAX_PATH_LEN - 1);
			items[n] = names[n];
			n++;
		}
		closedir(d);
	}
	int pick = RunPicker("Carregar jogo salvo", items, NULL, n, 0);
	if (pick >= 0) strncpy(p->loadgame, names[pick], MAX_PATH_LEN - 1);
}

static void Options_Update(void)
{
	Profile *p = &profiles[cur_profile];
	if (Pressed(BTN_DOWN)) opt_sel = (opt_sel + 1) % OPT_COUNT;
	if (Pressed(BTN_UP)) opt_sel = (opt_sel + OPT_COUNT - 1) % OPT_COUNT;
	int dir = Pressed(BTN_RIGHT) ? 1 : Pressed(BTN_LEFT) ? -1 : 0;
	int changed = 0;
	static const int scales[] = { 100, 90, 80, 70, 60 };
	int choice = opt_sel <= OPT_MONSTERS || opt_sel == OPT_LOG;
	if (choice && !dir && Pressed(BTN_A)) dir = 1;
	if (dir && choice)
	{
		switch (opt_sel)
		{
		case OPT_PRESET: p->preset = (p->preset + dir + PRESET_COUNT) % PRESET_COUNT; changed = 1; break;
		case OPT_SCALE:
		{
			int i = 0;
			while (i < 5 && scales[i] != p->scale) i++;
			if (i >= 5) i = 0;
			i = (i - dir + 5) % 5;
			p->scale = scales[i];
			changed = 1;
			break;
		}
		case OPT_FPSLIMIT:
		{
			static const int limits[] = { 0, 30, 35, 40 };
			int i = 0;
			while (i < 4 && limits[i] != p->fpslimit) i++;
			if (i >= 4) i = 0;
			i = (i + dir + 4) % 4;
			p->fpslimit = limits[i];
			changed = 1;
			break;
		}
		case OPT_GLTHREAD: p->glthread = !p->glthread; changed = 1; break;
		case OPT_FPS: p->showfps = !p->showfps; changed = 1; break;
		case OPT_LOADSCREEN: p->noloadscreen = !p->noloadscreen; changed = 1; break;
		case OPT_SKILL: p->skill = (p->skill + dir + 6) % 6; changed = 1; break;
		case OPT_MONSTERS: p->monsters = (p->monsters + dir + 5) % 5; changed = 1; break;
		case OPT_LOG: p->writelog = !p->writelog; changed = 1; break;
		default: break;
		}
	}
	if (!choice && Pressed(BTN_A))
	{
		switch (opt_sel)
		{
		case OPT_WARP: plat_keyboard("Mapa inicial (ex.: MAP01)", p->warp, p->warp, sizeof(p->warp)); changed = 1; break;
		case OPT_CLASS: plat_keyboard("Classe do jogador", p->pclass, p->pclass, sizeof(p->pclass)); changed = 1; break;
		case OPT_EXTRA: plat_keyboard("Argumentos extras", p->extra, p->extra, sizeof(p->extra)); changed = 1; break;
		case OPT_LOADGAME: PickSave(p); changed = 1; break;
		case OPT_RENAME:
		{
			char name[96];
			strcpy(name, p->name);
			if (plat_keyboard("Nome do perfil", p->name, name, sizeof(name)) && name[0])
			{
				int dup = 0;
				for (int i = 0; i < nprofiles; i++) if (i != cur_profile && !strcasecmp(profiles[i].name, name)) dup = 1;
				if (dup) ui_message("Nome em uso", "Já existe um perfil com esse nome.");
				else { strcpy(p->name, name); p->builtin = 0; changed = 1; }
			}
			break;
		}
		case OPT_IWAD:
		{
			static const char *items[MAX_FILES], *extra[MAX_FILES];
			int sel = 0;
			for (int i = 0; i < n_iwad_files; i++)
			{
				const char *t = data_iwad_title(iwad_files[i].name);
				items[i] = t ? t : iwad_files[i].name;
				extra[i] = iwad_files[i].name;
				if (!strcasecmp(iwad_files[i].name, p->iwad)) sel = i;
			}
			int pick = RunPicker("Jogo base", items, extra, n_iwad_files, sel);
			if (pick >= 0)
			{
				strncpy(p->iwad, iwad_files[pick].name, MAX_PATH_LEN - 1);
				p->present = 1;
				p->builtin = 0;
				changed = 1;
			}
			break;
		}
		}
	}
	if (Pressed(BTN_X))
	{
		switch (opt_sel)
		{
		case OPT_WARP: p->warp[0] = 0; changed = 1; break;
		case OPT_CLASS: p->pclass[0] = 0; changed = 1; break;
		case OPT_EXTRA: p->extra[0] = 0; changed = 1; break;
		case OPT_LOADGAME: p->loadgame[0] = 0; changed = 1; break;
		}
	}
	if (changed) data_save();
}

//==========================================================================
// main screen
//==========================================================================

static void DrawScreen(float dt)
{
	DrawBackground(dt);
	DrawHeader(dt);
	if (nprofiles > 0 && cur_profile >= 0)
	{
		switch (tab)
		{
		case TAB_GAMES: Games_Draw(dt); break;
		case TAB_MODS: Mods_Draw(dt); break;
		case TAB_OPTIONS: Options_Draw(dt); break;
		}
	}
}

static void DrawHints(void)
{
	Hint h[10];
	int n = 0;
	switch (tab)
	{
	case TAB_GAMES:
		h[n++] = (Hint){ BTN_A, "Jogar" };
		h[n++] = (Hint){ BTN_Y, "Novo perfil" };
		h[n++] = (Hint){ BTN_MINUS, "Duplicar" };
		h[n++] = (Hint){ BTN_X, "Excluir" };
		h[n++] = (Hint){ BTN_B, "Sair" };
		break;
	case TAB_MODS:
		if (moving >= 0)
		{
			h[n++] = (Hint){ BTN_UP, "Mover" };
			h[n++] = (Hint){ BTN_A, "Soltar aqui" };
			h[n++] = (Hint){ BTN_B, "Cancelar" };
		}
		else if (mods_col == 0)
		{
			h[n++] = (Hint){ BTN_A, "Mover" };
			h[n++] = (Hint){ BTN_ZL, NULL };
			h[n++] = (Hint){ BTN_ZR, "Subir / descer" };
			h[n++] = (Hint){ BTN_X, "Remover" };
			h[n++] = (Hint){ BTN_RIGHT, "Disponíveis" };
			h[n++] = (Hint){ BTN_PLUS, "Jogar" };
		}
		else
		{
			h[n++] = (Hint){ BTN_A, "Adicionar" };
			h[n++] = (Hint){ BTN_LEFT, "Ordem" };
			h[n++] = (Hint){ BTN_PLUS, "Jogar" };
		}
		break;
	case TAB_OPTIONS:
		h[n++] = (Hint){ BTN_RIGHT, "Alterar" };
		h[n++] = (Hint){ BTN_A, "Editar" };
		h[n++] = (Hint){ BTN_X, "Limpar" };
		h[n++] = (Hint){ BTN_PLUS, "Jogar" };
		break;
	}
	gfx_rect(0, SCR_H - 58, SCR_W, 58, 0x0A06059Cu);
	gfx_rect(0, SCR_H - 58, SCR_W, 1, 0xFFFFFF18u);
	float x = 40;
	for (int i = 0; i < n; i++)
	{
		if (!h[i].label) x += gfx_button(x, SCR_H - 29, h[i].button, 28) + 6;
		else x += gfx_hint(x, SCR_H - 29, h[i].button, h[i].label);
	}
	// credits + version, right side of the footer
	float xr = SCR_W - 32;
	xr -= gfx_text_r(FONT_LIGHT, 14, xr, SCR_H - 38, C_MUTED, VERSION) + 16;
	gfx_rect(xr, SCR_H - 40, 1, 22, 0xFFFFFF30u);
	xr -= 16;
	xr -= gfx_text_r(FONT_BODY, 17, xr, SCR_H - 41, C_ACCENT2, "Telegram @aeon1911") + 10;
	gfx_text_r(FONT_BOLD, 17, xr, SCR_H - 41, C_TEXT, "Port por Renan");
}

static int Launch(void)
{
	Profile *p = &profiles[cur_profile];
	if (!p->present)
	{
		char msg[400];
		snprintf(msg, sizeof(msg), "O arquivo do jogo base \"%s\" não foi encontrado em /switch/gzdoom/iwads.", p->iwad);
		ui_message("Jogo não encontrado", msg);
		return 0;
	}
	for (int i = 0; i < p->nmods; i++)
	{
		if (PwadMB(p->mods[i]) < 0)
		{
			char msg[500];
			snprintf(msg, sizeof(msg), "O mod \"%s\" não está na pasta pwads. Remova-o da ordem de carregamento na aba MODS.", p->mods[i]);
			ui_message("Mod não encontrado", msg);
			return 0;
		}
	}
	if (plat_applet_mode() && p->nmods > 0 &&
		!ui_confirm("Pouca memória", "O launcher foi aberto pelo Álbum (modo applet), com só ~400 MB de RAM: mods grandes vão fechar o jogo. Abra segurando R ao iniciar um jogo. Continuar mesmo assim?"))
		return 0;
	// background for the engine's loading screen
	art_write_loadbg(cur_art && cur_art->img.px ? &cur_art->img : NULL, TMPDIR "/loadbg.rgba");
	data_launch(cur_profile);
	return 1;
}

int ui_run(void)
{
	draw_screen = DrawScreen;
	BackgroundInit();
	last_frame = plat_ticks();

	if (nprofiles == 0 || cur_profile < 0)
	{
		ui_message("Nenhum jogo encontrado",
			"Coloque o arquivo de um jogo (doom2.wad, doom.wad, doom_fusion.ipk3 do WadFusion, freedoom2.wad...) na pasta /switch/gzdoom/iwads do cartão SD e abra o launcher de novo.");
		return 0;
	}
	SelectArt(profiles[cur_profile].iwad);
	art_fade = 1.f;

	if (plat_applet_mode())
		ui_message("Modo applet (pouca RAM)",
			"O launcher foi aberto pelo Álbum. Nesse modo só ~400 MB de RAM ficam livres, o que não basta para mods grandes. Para ter a RAM completa, segure R ao abrir qualquer jogo e inicie o launcher pelo menu que aparecer.");

	for (;;)
	{
		if (!plat_loop()) return 0;
		InputUpdate();
		float dt = FrameDT();

		if (moving < 0)
		{
			if (Pressed(BTN_R)) tab = (tab + 1) % TAB_COUNT;
			else if (Pressed(BTN_L)) tab = (tab + TAB_COUNT - 1) % TAB_COUNT;
		}

		int was_moving = moving >= 0;
		if (tab == TAB_GAMES) Games_Update();
		else if (tab == TAB_MODS) Mods_Update();
		else Options_Update();

		if (cur_profile >= 0) SelectArt(profiles[cur_profile].iwad);

		if (!was_moving && moving < 0)
		{
			if (Pressed(BTN_PLUS) || (tab == TAB_GAMES && Pressed(BTN_A)))
			{
				if (Launch()) return 1;
			}
			if (Pressed(BTN_B))
			{
				if (tab == TAB_GAMES) { data_save(); return 0; }
				tab = TAB_GAMES;
			}
		}

		gfx_begin();
		DrawScreen(dt);
		DrawHints();
		DrawToast(dt);
		gfx_end();
	}
}

void ui_free(void)
{
	for (int i = 0; i < ART_CACHE; i++)
	{
		if (art_cache[i].tex) SDL_DestroyTexture(art_cache[i].tex);
		art_free(&art_cache[i].img);
	}
	if (bg_tex) SDL_DestroyTexture(bg_tex);
}
