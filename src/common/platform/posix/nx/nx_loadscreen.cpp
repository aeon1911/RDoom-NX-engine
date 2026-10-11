/*
** nx_loadscreen.cpp
** Loading screen for the Nintendo Switch build.
**
** Everything here is drawn in software into a linear RGBA buffer by a
** helper thread running on its own CPU core, so it doesn't slow down the
** engine's main thread. The startup stages are tracked by watching the
** console output, and the timings of the previous start with the same set
** of files are remembered, so from the second start on the progress bar and
** the remaining time are accurate.
**
**---------------------------------------------------------------------------
** Copyright 2026 UZDoom Switch port contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
void NX_AudioAfterLoadScreen();
#else
#include <thread>
#include <chrono>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../../thirdparty/stb/stb_truetype.h"

#include "nx_font_bebas.h"
#include "nx_font_poppins.h"
#include "nx_loadscreen.h"

namespace {

constexpr int SW = 1280;
constexpr int SH = 720;

//==========================================================================
//
// time
//
//==========================================================================

uint64_t NowMS()
{
#ifdef __SWITCH__
	return armTicksToNs(armGetSystemTick()) / 1000000ull;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000ull + uint64_t(ts.tv_nsec) / 1000000ull;
#endif
}

//==========================================================================
//
// fonts
//
//==========================================================================

struct Glyph
{
	int w = 0, h = 0, xoff = 0, yoff = 0;
	float adv = 0;
	std::vector<uint8_t> bmp;
};

struct Font
{
	stbtt_fontinfo info;
	bool ok = false;
	std::unordered_map<uint64_t, Glyph> cache;

	bool Init(const unsigned char *data)
	{
		ok = stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
		return ok;
	}

	const Glyph &Get(int cp, int px)
	{
		uint64_t key = (uint64_t(px) << 32) | uint32_t(cp);
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		Glyph g;
		if (ok)
		{
			float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
			int adv, lsb;
			stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
			g.adv = adv * scale;
			unsigned char *b = stbtt_GetCodepointBitmap(&info, scale, scale, cp, &g.w, &g.h, &g.xoff, &g.yoff);
			if (b)
			{
				g.bmp.assign(b, b + g.w * g.h);
				stbtt_FreeBitmap(b, nullptr);
			}
			else g.w = g.h = 0;
		}
		return cache.emplace(key, std::move(g)).first->second;
	}

	float Kern(int a, int b, int px)
	{
		if (!ok || !a) return 0;
		return stbtt_GetCodepointKernAdvance(&info, a, b) * stbtt_ScaleForPixelHeight(&info, (float)px);
	}
};

Font fTitle, fBody;

int DecodeUTF8(const char *&s)
{
	const unsigned char *p = (const unsigned char *)s;
	int c = *p++;
	if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
	else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
	else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
	s = (const char *)p;
	return c;
}

//==========================================================================
//
// canvas
//
//==========================================================================

struct Canvas
{
	uint32_t *px;
	int stride;

	static inline uint32_t Pack(int r, int g, int b) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u; }

	inline void Blend(int x, int y, uint32_t rgb, int a)
	{
		if ((unsigned)x >= (unsigned)SW || (unsigned)y >= (unsigned)SH || a <= 0) return;
		uint32_t &d = px[y * stride + x];
		if (a >= 255) { d = Pack((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); return; }
		int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
		int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
		d = Pack(dr + (((sr - dr) * a) >> 8), dg + (((sg - dg) * a) >> 8), db + (((sb - db) * a) >> 8));
	}

	inline void Add(int x, int y, uint32_t rgb, int a)
	{
		if ((unsigned)x >= (unsigned)SW || (unsigned)y >= (unsigned)SH || a <= 0) return;
		uint32_t &d = px[y * stride + x];
		int r = (d & 255) + ((((rgb >> 16) & 255) * a) >> 8);
		int g = ((d >> 8) & 255) + ((((rgb >> 8) & 255) * a) >> 8);
		int b = ((d >> 16) & 255) + (((rgb & 255) * a) >> 8);
		d = Pack(std::min(r, 255), std::min(g, 255), std::min(b, 255));
	}

	void FillRect(int x, int y, int w, int h, uint32_t rgb, int a)
	{
		int x0 = std::max(x, 0), y0 = std::max(y, 0), x1 = std::min(x + w, SW), y1 = std::min(y + h, SH);
		for (int j = y0; j < y1; j++)
			for (int i = x0; i < x1; i++) Blend(i, j, rgb, a);
	}

	// anti-aliased rounded rectangle; if rgb2 differs, a horizontal gradient is used
	void RoundRect(float x, float y, float w, float h, float r, uint32_t rgb, int a, uint32_t rgb2 = 0xFFFFFFFF)
	{
		if (w <= 0 || h <= 0) return;
		r = std::min(r, std::min(w, h) * 0.5f);
		int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
		for (int j = y0; j < y1; j++)
		{
			for (int i = x0; i < x1; i++)
			{
				float cx = i + 0.5f, cy = j + 0.5f;
				// distance to the rounded rectangle
				float qx = fabsf(cx - (x + w * 0.5f)) - (w * 0.5f - r);
				float qy = fabsf(cy - (y + h * 0.5f)) - (h * 0.5f - r);
				float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
				float dist = sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
				float cov = std::min(std::max(0.5f - dist, 0.f), 1.f);
				if (cov <= 0) continue;
				uint32_t c = rgb;
				if (rgb2 != 0xFFFFFFFF)
				{
					float t = std::min(std::max((cx - x) / w, 0.f), 1.f);
					int r1 = (rgb >> 16) & 255, g1 = (rgb >> 8) & 255, b1 = rgb & 255;
					int r2 = (rgb2 >> 16) & 255, g2 = (rgb2 >> 8) & 255, b2 = rgb2 & 255;
					c = (uint32_t(r1 + (r2 - r1) * t) << 16) | (uint32_t(g1 + (g2 - g1) * t) << 8) | uint32_t(b1 + (b2 - b1) * t);
				}
				Blend(i, j, c, int(a * cov));
			}
		}
	}

	void Glow(float cx, float cy, float radius, uint32_t rgb, float intensity)
	{
		int x0 = (int)(cx - radius), x1 = (int)(cx + radius + 1), y0 = (int)(cy - radius), y1 = (int)(cy + radius + 1);
		float inv = 1.f / (radius * radius);
		for (int j = y0; j <= y1; j++)
		{
			if ((unsigned)j >= (unsigned)SH) continue;
			for (int i = x0; i <= x1; i++)
			{
				float dx = i - cx, dy = j - cy;
				float d2 = (dx * dx + dy * dy) * inv;
				if (d2 >= 1.f) continue;
				float f = (1.f - d2);
				Add(i, j, rgb, int(255.f * intensity * f * f));
			}
		}
	}

	// anti-aliased thick line segment
	void Line(float x0, float y0, float x1, float y1, float width, uint32_t rgb, int a)
	{
		float hw = width * 0.5f;
		int ix0 = (int)floorf(std::min(x0, x1) - hw - 1), ix1 = (int)ceilf(std::max(x0, x1) + hw + 1);
		int iy0 = (int)floorf(std::min(y0, y1) - hw - 1), iy1 = (int)ceilf(std::max(y0, y1) + hw + 1);
		float dx = x1 - x0, dy = y1 - y0;
		float len2 = std::max(dx * dx + dy * dy, 1e-4f);
		for (int j = iy0; j <= iy1; j++)
		{
			for (int i = ix0; i <= ix1; i++)
			{
				float px_ = i + 0.5f - x0, py_ = j + 0.5f - y0;
				float t = std::min(std::max((px_ * dx + py_ * dy) / len2, 0.f), 1.f);
				float ex = px_ - t * dx, ey = py_ - t * dy;
				float d = sqrtf(ex * ex + ey * ey) - hw;
				float cov = std::min(std::max(0.5f - d, 0.f), 1.f);
				if (cov > 0) Blend(i, j, rgb, int(a * cov));
			}
		}
	}

	float TextWidth(Font &f, int size, const char *s)
	{
		float w = 0;
		int prev = 0;
		while (*s)
		{
			int cp = DecodeUTF8(s);
			w += f.Kern(prev, cp, size) + f.Get(cp, size).adv;
			prev = cp;
		}
		return w;
	}

	// y is the top of the text line
	float Text(Font &f, int size, float x, float y, const char *s, uint32_t rgb, int a = 255, float maxw = 0)
	{
		if (!f.ok) return 0;
		int ascent, descent, gap;
		stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
		float scale = stbtt_ScaleForPixelHeight(&f.info, (float)size);
		float base = y + ascent * scale;
		float pen = x;
		int prev = 0;
		float ellw = maxw > 0 ? TextWidth(f, size, "...") : 0;
		while (*s)
		{
			const char *save = s;
			int cp = DecodeUTF8(s);
			pen += f.Kern(prev, cp, size);
			const Glyph &g = f.Get(cp, size);
			if (maxw > 0 && pen + g.adv > x + maxw - ellw && *s)
			{
				// truncate with an ellipsis
				(void)save;
				Text(f, size, pen, y, "...", rgb, a);
				pen += ellw;
				break;
			}
			int gx = (int)lroundf(pen) + g.xoff, gy = (int)lroundf(base) + g.yoff;
			for (int j = 0; j < g.h; j++)
			{
				const uint8_t *row = &g.bmp[j * g.w];
				for (int i = 0; i < g.w; i++)
				{
					if (row[i]) Blend(gx + i, gy + j, rgb, (row[i] * a) >> 8);
				}
			}
			pen += g.adv;
			prev = cp;
		}
		return pen - x;
	}
};

//==========================================================================
//
// state shared between the engine thread and the drawing thread
//
//==========================================================================

enum Milestone
{
	MS_START,
	MS_WINIT,
	MS_SOUND,
	MS_TEXTURES,
	MS_ACTORS,
	MS_SCRIPTSDONE,
	MS_RINIT,
	MS_MENUS,
	MS_PLAYLOOP,
	MS_NETGAME,
	MS_END,
	MS_COUNT
};

// fallback distribution for the first start with a set of files
const float DefaultFrac[MS_COUNT] = { 0.f, 0.01f, 0.12f, 0.14f, 0.24f, 0.88f, 0.90f, 0.93f, 0.96f, 0.98f, 1.f };

const char *const StageText[MS_COUNT] =
{
	"Iniciando o UZDoom",
	"Abrindo arquivos do jogo e mods",
	"Iniciando áudio",
	"Carregando texturas e sprites",
	"Compilando scripts do mod (ZScript / DECORATE)",
	"Finalizando scripts",
	"Preparando gráficos",
	"Carregando menus",
	"Iniciando o jogo",
	"Quase pronto",
	"Iniciando vídeo",
};

struct FileItem
{
	std::string name;
	double mb = 0;
	int state = 0;	// 0 = waiting, 1 = loading, 2 = done
};

std::mutex mtx;
std::string gTitle = "UZDoom";
std::string gSubtitle;
std::string gDetail;
std::vector<FileItem> gFiles;
int gMilestone = MS_START;
uint64_t gStart = 0;
uint32_t gTimes[MS_COUNT];	// ms since start when each milestone was reached (this run)
float gSubFrac = 0;	// progress inside the current milestone (textures, codegen)
double gFilesMBTotal = 0, gFilesMBDone = 0;

bool gHaveHistory = false;
uint32_t gHist[MS_COUNT];	// previous run
std::string gHistKey;

std::atomic<bool> gActive{ false };
bool gClaimed = false;
std::atomic<bool> gQuit{ false };
bool gBoost = false;

std::vector<uint32_t> gBackground;	// SW*SH, prepared once

struct Ember { float x, y, vx, vy, life, max, size; };
std::vector<Ember> gEmbers;
uint32_t gRand = 0x12345678;
float Rand01() { gRand = gRand * 1664525u + 1013904223u; return (gRand >> 8) * (1.f / 16777216.f); }

const char *const Tips[] =
{
	"Dica: na aba Mods do launcher, aperte A em um mod e use o direcional para mudar a ordem de carregamento.",
	"Dica: a primeira abertura de um conjunto de mods é a mais lenta. Nas próximas, o tempo restante aparece aqui.",
	"Dica: mods de mapas vão antes dos mods de gameplay na ordem de carregamento.",
	"Dica: o perfil \"Switch (leve)\" reduz luzes, partículas e efeitos distantes.",
	"Dica: abra o launcher segurando R em um jogo (modo aplicativo) para ter a RAM completa do console.",
	"Dica: cada perfil do launcher guarda seus próprios mods, ordem e opções.",
};

//==========================================================================
//
// history of load times
//
//==========================================================================

const char *HistoryFile = "loadtimes.txt";

uint32_t FNV(const std::string &s)
{
	uint32_t h = 2166136261u;
	for (unsigned char c : s) { h ^= c; h *= 16777619u; }
	return h;
}

void LoadHistory()
{
	gHaveHistory = false;
	FILE *f = fopen(HistoryFile, "r");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		char key[64];
		uint32_t t[MS_COUNT];
		int n = sscanf(line, "%63s %u %u %u %u %u %u %u %u %u %u %u", key, &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7], &t[8], &t[9], &t[10]);
		if (n == 1 + MS_COUNT && gHistKey == key && t[MS_END] > 0)
		{
			memcpy(gHist, t, sizeof(gHist));
			gHaveHistory = true;
		}
	}
	fclose(f);
}

void SaveHistory()
{
	if (gTimes[MS_END] == 0) return;
	std::vector<std::string> lines;
	FILE *f = fopen(HistoryFile, "r");
	if (f)
	{
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			char key[64];
			if (sscanf(line, "%63s", key) == 1 && gHistKey != key) lines.push_back(line);
		}
		fclose(f);
	}
	if (lines.size() > 40) lines.erase(lines.begin(), lines.begin() + (lines.size() - 40));
	f = fopen(HistoryFile, "w");
	if (!f) return;
	for (auto &l : lines) fputs(l.c_str(), f);
	fprintf(f, "%s", gHistKey.c_str());
	// milestones that were never reached get the time of the next one
	uint32_t t[MS_COUNT];
	memcpy(t, gTimes, sizeof(t));
	for (int i = MS_COUNT - 2; i > 0; i--) if (t[i] == 0) t[i] = t[i + 1];
	for (int i = 0; i < MS_COUNT; i++) fprintf(f, " %u", t[i]);
	fprintf(f, "\n");
	fclose(f);
}

//==========================================================================
//
// progress computation (call with mtx held)
//
//==========================================================================

float ComputeProgress(uint32_t elapsed, float *remainingSec)
{
	int m = gMilestone;
	if (m >= MS_END) { *remainingSec = 0; return 1.f; }
	float frac;
	if (gHaveHistory)
	{
		float total = (float)gHist[MS_END];
		float f0 = gHist[m] / total, f1 = gHist[m + 1] / total;
		float segExpected = std::max(1.f, float(gHist[m + 1]) - float(gHist[m]));
		float segElapsed = float(elapsed) - float(gTimes[m]);
		float t = std::min(segElapsed / segExpected, 1.f);
		// prefer measured sub-progress if we have it
		if (gSubFrac > 0) t = std::max(t * 0.5f, gSubFrac);
		frac = f0 + (f1 - f0) * std::min(t, 0.98f);
		// time left: what's left of the previous run, corrected by how fast we are this time
		float prevAtThisPoint = frac * total;
		float speed = prevAtThisPoint > 500 ? std::min(std::max(elapsed / prevAtThisPoint, 0.5f), 2.f) : 1.f;
		*remainingSec = std::max(0.f, (total - prevAtThisPoint) * speed / 1000.f);
	}
	else
	{
		float f0 = DefaultFrac[m], f1 = DefaultFrac[m + 1];
		float t;
		if (m == MS_WINIT && gFilesMBTotal > 0) t = float(gFilesMBDone / gFilesMBTotal);
		else if (gSubFrac > 0) t = gSubFrac;
		else
		{
			// no idea how long this takes - approach the end of the segment slowly
			float segElapsed = (float(elapsed) - float(gTimes[m])) / 1000.f;
			float tau = (m == MS_ACTORS) ? 40.f : 6.f;
			t = 1.f - expf(-segElapsed / tau);
		}
		frac = f0 + (f1 - f0) * std::min(t, 0.98f);
		*remainingSec = -1;
	}
	return std::min(std::max(frac, 0.f), 0.995f);
}

//==========================================================================
//
// drawing
//
//==========================================================================

void PrepareBackground()
{
	gBackground.assign(SW * SH, 0);
	// a picture prepared by the launcher (already darkened / blurred)
	bool loaded = false;
	FILE *f = fopen("temp/loadbg.rgba", "rb");
	if (f)
	{
		size_t n = fread(gBackground.data(), 4, SW * SH, f);
		fclose(f);
		loaded = (n == (size_t)SW * SH);
	}
	if (!loaded)
	{
		for (int y = 0; y < SH; y++)
		{
			float fy = y / float(SH);
			for (int x = 0; x < SW; x++)
			{
				float fx = x / float(SW);
				// dark gradient with a warm glow at the bottom
				float dx = fx - 0.5f, dy = fy - 1.05f;
				float glow = expf(-(dx * dx * 2.2f + dy * dy * 4.f));
				float vign = 1.f - 0.55f * (dx * dx * 1.6f + (fy - 0.5f) * (fy - 0.5f) * 1.2f);
				int r = int((14 + 150 * glow) * vign);
				int g = int((6 + 30 * glow) * vign);
				int b = int((6 + 12 * glow) * vign);
				gBackground[y * SW + x] = Canvas::Pack(std::min(r, 255), std::min(g, 255), std::min(b, 255));
			}
		}
	}
	// darken the lower third for the progress area
	for (int y = SH * 2 / 3; y < SH; y++)
	{
		float t = (y - SH * 2 / 3) / float(SH / 3);
		int k = int(255 - 150 * t);
		for (int x = 0; x < SW; x++)
		{
			uint32_t c = gBackground[y * SW + x];
			int r = ((c & 255) * k) >> 8, g = (((c >> 8) & 255) * k) >> 8, b = (((c >> 16) & 255) * k) >> 8;
			gBackground[y * SW + x] = Canvas::Pack(r, g, b);
		}
	}
	gEmbers.resize(70);
	for (auto &e : gEmbers) { e.life = e.max = 0; }
}

void UpdateEmbers(float dt)
{
	for (auto &e : gEmbers)
	{
		e.life -= dt;
		if (e.life <= 0)
		{
			e.x = Rand01() * SW;
			e.y = SH + 10 + Rand01() * 40;
			e.vx = (Rand01() - 0.5f) * 30.f;
			e.vy = -(40.f + Rand01() * 90.f);
			e.max = e.life = 3.f + Rand01() * 5.f;
			e.size = 1.5f + Rand01() * 3.f;
		}
		e.x += e.vx * dt + sinf(e.life * 2.f + e.size) * 12.f * dt;
		e.y += e.vy * dt;
	}
}

std::string FormatTime(float sec)
{
	int s = (int)(sec + 0.5f);
	char buf[32];
	snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
	return buf;
}

void RenderFrame(uint32_t *dst, int stride, float dt)
{
	Canvas c{ dst, stride };
	for (int y = 0; y < SH; y++) memcpy(dst + y * stride, &gBackground[y * SW], SW * 4);

	UpdateEmbers(dt);
	for (auto &e : gEmbers)
	{
		float a = std::min(1.f, e.life / e.max * 1.5f) * std::min(1.f, (e.max - e.life) * 2.f);
		c.Glow(e.x, e.y, e.size * 3.f, 0xFF7A2A, 0.55f * a);
	}

	// snapshot the shared state
	std::string title, subtitle, detail;
	std::vector<FileItem> files;
	int milestone;
	float progress, remaining;
	uint32_t elapsed = uint32_t(NowMS() - gStart);
	{
		std::lock_guard<std::mutex> lock(mtx);
		title = gTitle;
		subtitle = gSubtitle;
		detail = gDetail;
		files = gFiles;
		milestone = gMilestone;
		progress = ComputeProgress(elapsed, &remaining);
	}

	// header
	const uint32_t accent = 0xFF4A1C, accent2 = 0xFFB020;
	c.Text(fTitle, 76, 64, 44, "UZDOOM", 0xFFFFFF);
	float tw = c.TextWidth(fTitle, 76, "UZDOOM");
	c.Text(fTitle, 34, 64 + tw + 14, 78, "NINTENDO SWITCH", accent);
	c.RoundRect(64, 128, 86, 5, 2.5f, accent, 255, accent2);
	{
		// port credit
		const char *by = "PORT POR RENAN";
		const char *tg = "Telegram @aeon1911";
		float w1 = c.TextWidth(fTitle, 30, by);
		float w2 = c.TextWidth(fBody, 18, tg);
		c.Text(fTitle, 30, 1216 - w1, 62, by, 0xF2ECE8);
		c.Text(fBody, 18, 1216 - w2, 98, tg, accent2);
	}
	if (!subtitle.empty()) c.Text(fBody, 24, 64, 146, subtitle.c_str(), 0xE8E0DA, 255, 1100);

	// file list
	{
		int rows = std::min<int>((int)files.size(), 8) + ((int)files.size() > 8 ? 1 : 0);
		if (rows > 0) c.RoundRect(52, 192, SW - 104, rows * 36 + 20, 16, 0x000000, 120);
	}
	float y = 202;
	int shown = 0;
	int first = 0;
	// keep the file being loaded visible
	for (size_t i = 0; i < files.size(); i++) if (files[i].state == 1 && i >= 7) first = int(i) - 6;
	for (size_t i = first; i < files.size() && shown < 8; i++, shown++)
	{
		auto &fi = files[i];
		uint32_t col = fi.state == 2 ? 0xF2ECE8 : fi.state == 1 ? 0xFFFFFF : 0x9A8F8A;
		// status marker
		float mx = 76, my = y + 14;
		if (fi.state == 2)
		{
			c.RoundRect(mx - 9, my - 9, 18, 18, 9, 0x3DDC84, 255);
			c.Line(mx - 4.5f, my + 0.5f, mx - 1.f, my + 4.f, 2.4f, 0x08301A, 255);
			c.Line(mx - 1.f, my + 4.f, mx + 5.f, my - 3.5f, 2.4f, 0x08301A, 255);
		}
		else if (fi.state == 1)
		{
			float pulse = 0.5f + 0.5f * sinf(elapsed / 150.f);
			c.Glow(mx, my, 16, accent, 0.5f + 0.4f * pulse);
			c.RoundRect(mx - 7, my - 7, 14, 14, 7, accent2, 255);
		}
		else
		{
			c.RoundRect(mx - 6, my - 6, 12, 12, 6, 0x5A504C, 255);
		}
		c.Text(fBody, 22, 100, y, fi.name.c_str(), col, 255, 820);
		if (fi.mb > 0)
		{
			char buf[32];
			if (fi.mb >= 1024) snprintf(buf, sizeof(buf), "%.1f GB", fi.mb / 1024.0);
			else snprintf(buf, sizeof(buf), "%.0f MB", std::max(fi.mb, 1.0));
			float w = c.TextWidth(fBody, 18, buf);
			c.Text(fBody, 18, 1216 - w, y + 3, buf, 0x9A8F8A);
		}
		y += 36;
	}
	if (first + shown < (int)files.size())
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "+ %d arquivo(s)", int(files.size()) - first - shown);
		c.Text(fBody, 18, 100, y, buf, 0x9A8F8A);
	}

	// stage + progress bar
	const float bx = 64, by = 604, bw = SW - 128, bh = 16;
	std::string stage = StageText[std::min(milestone, MS_COUNT - 1)];
	int dots = (elapsed / 400) % 4;
	for (int i = 0; i < dots; i++) stage += '.';
	c.Text(fBody, 28, bx, 514, stage.c_str(), 0xFFFFFF, 255, 900);
	if (!detail.empty()) c.Text(fBody, 20, bx, 556, detail.c_str(), 0xC9BDB6, 255, 900);

	char pct[16];
	snprintf(pct, sizeof(pct), "%d%%", int(progress * 100.f));
	float pw = c.TextWidth(fTitle, 64, pct);
	c.Text(fTitle, 64, bx + bw - pw, 520, pct, 0xFFFFFF);

	c.RoundRect(bx, by, bw, bh, bh / 2, 0x2A1712, 235);
	float fillw = std::max(bh, bw * progress);
	c.RoundRect(bx, by, fillw, bh, bh / 2, 0xC21E0E, 255, accent2);
	c.Glow(bx + fillw - 4, by + bh / 2, 34, 0xFF8A2A, 0.45f);
	// moving highlight on the bar
	float sweep = fmodf(elapsed / 1400.f, 1.f);
	float hx = bx + fillw * sweep;
	c.Glow(hx, by + bh / 2, 22, 0xFFE0A0, 0.25f);

	std::string times = "Tempo: " + FormatTime(elapsed / 1000.f);
	if (remaining >= 0) times += "     Restante: ~" + FormatTime(remaining);
	else times += "     (primeira abertura com estes arquivos: medindo o tempo)";
	c.Text(fBody, 19, bx, 634, times.c_str(), 0xB5A9A2);

	// rotating tips
	int tipn = int(sizeof(Tips) / sizeof(Tips[0]));
	const char *tip = Tips[(elapsed / 7000) % tipn];
	c.Text(fBody, 18, bx, 672, tip, 0x8C817B, 255, bw);
}

//==========================================================================
//
// platform part
//
//==========================================================================

#ifdef __SWITCH__
Framebuffer gFB;
Thread gThread;
bool gFBOpen = false;
bool gThreadOpen = false;
std::atomic<int> gFrames{ 0 };

void PresentOnce(float dt)
{
	u32 stride = 0;
	uint32_t *buf = (uint32_t *)framebufferBegin(&gFB, &stride);
	if (buf) RenderFrame(buf, stride / 4, dt);
	framebufferEnd(&gFB);
	gFrames.fetch_add(1);
}

void DrawThread(void *)
{
	uint64_t last = NowMS();
	while (!gQuit.load())
	{
		uint64_t now = NowMS();
		float dt = std::min((now - last) / 1000.f, 0.1f);
		last = now;
		PresentOnce(dt);
		// ~30 fps is plenty and leaves the CPU alone
		uint64_t spent = NowMS() - now;
		if (spent < 33) svcSleepThread((33 - spent) * 1000000ull);
	}
}
#endif

void ReadLoadInfo()
{
	// written by the launcher: first line = profile name
	FILE *f = fopen("temp/loadinfo.txt", "r");
	if (!f) return;
	char line[512];
	if (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (line[0]) gSubtitle = std::string("Perfil: ") + line;
	}
	fclose(f);
}

std::string BaseName(const std::string &p)
{
	size_t s = p.find_last_of("/\\");
	return s == std::string::npos ? p : p.substr(s + 1);
}

double FileMB(const std::string &path)
{
	static const char *const dirs[] = { "", "iwads/", "pwads/", nullptr };
	struct stat st;
	for (int i = 0; dirs[i]; i++)
	{
		std::string p = std::string(dirs[i]) + path;
		if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return st.st_size / 1048576.0;
	}
	return 0;
}

void SetMilestone(int m)
{
	if (m <= gMilestone) return;
	uint32_t t = uint32_t(NowMS() - gStart);
	for (int i = gMilestone + 1; i <= m; i++) gTimes[i] = t;
	gMilestone = m;
	gSubFrac = 0;
	if (m > MS_WINIT)
	{
		for (auto &f : gFiles) f.state = 2;
		gFilesMBDone = gFilesMBTotal;
	}
}

} // namespace

//==========================================================================
//
// public interface
//
//==========================================================================

bool NXLS_Active()
{
	return gActive.load();
}

bool NXLS_ClaimedDisplay()
{
	return gClaimed;
}

// The port never changes the console's CPU/GPU clocks (not even the
// official loading boost), so this does nothing any more.
void NXLS_SetBoost(bool on)
{
	(void)on;
	gBoost = false;
}

void NXLS_Start(int argc, char **argv)
{
	if (gActive.load()) return;

	gStart = NowMS();
	memset(gTimes, 0, sizeof(gTimes));
	gMilestone = MS_START;

	// collect the files that will be loaded
	std::string key;
	bool infiles = false;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		if (a[0] == '-' || a[0] == '+')
		{
			infiles = !strcmp(a, "-file");
			if (!strcmp(a, "-iwad") && i + 1 < argc)
			{
				FileItem fi;
				fi.name = BaseName(argv[i + 1]);
				fi.mb = FileMB(argv[i + 1]);
				gFiles.insert(gFiles.begin(), fi);
				key += argv[i + 1];
				key += '|';
				i++;
			}
			continue;
		}
		if (infiles)
		{
			FileItem fi;
			fi.name = BaseName(a);
			fi.mb = FileMB(a);
			gFiles.push_back(fi);
			key += a;
			key += '|';
		}
	}
	for (auto &f : gFiles)
	{
		gFilesMBTotal += f.mb;
		char buf[32];
		snprintf(buf, sizeof(buf), "%.0f,", f.mb * 10);
		key += buf;
	}
	char hk[16];
	snprintf(hk, sizeof(hk), "%08x", FNV(key));
	gHistKey = hk;
	LoadHistory();
	ReadLoadInfo();
	if (gSubtitle.empty() && !gFiles.empty()) gSubtitle = "Jogo: " + gFiles[0].name;

	fTitle.Init(nxls_font_bebas);
	fBody.Init(nxls_font_poppins);
	PrepareBackground();

#ifdef __SWITCH__
	if (R_FAILED(framebufferCreate(&gFB, nwindowGetDefault(), SW, SH, PIXEL_FORMAT_RGBA_8888, 2)))
	{
		NX_AudioAfterLoadScreen();
		return;
	}
	gFBOpen = true;
	// framebufferClose leaves the window format at 0. Vulkan must see that
	// this process owned the scanout, or the compositor stays black.
	gClaimed = true;
	if (R_FAILED(framebufferMakeLinear(&gFB)))
	{
		framebufferClose(&gFB);
		gFBOpen = false;
		NX_AudioAfterLoadScreen();
		return;
	}
	gQuit = false;
	gFrames = 0;
	// Core 2, priority 0x30: this is the setup that actually put the screen
	// on the display. The audio mixer stays off this core until NXLS_Stop
	// has joined this thread (see nx_glthread.cpp).
	if (R_FAILED(threadCreate(&gThread, DrawThread, nullptr, nullptr, 0x40000, 0x30, 2)) &&
		R_FAILED(threadCreate(&gThread, DrawThread, nullptr, nullptr, 0x40000, 0x30, -2)))
	{
		framebufferClose(&gFB);
		gFBOpen = false;
		NX_AudioAfterLoadScreen();
		return;
	}
	gThreadOpen = true;
	threadStart(&gThread);
#endif
	gActive = true;
}

void NXLS_Stop()
{
	if (!gActive.exchange(false)) return;
	{
		std::lock_guard<std::mutex> lock(mtx);
		SetMilestone(MS_END);
	}
#ifdef __SWITCH__
	gQuit = true;
	if (gThreadOpen)
	{
		threadWaitForExit(&gThread);
		threadClose(&gThread);
		gThreadOpen = false;
	}
	if (gFBOpen)
	{
		framebufferClose(&gFB);
		gFBOpen = false;
	}
	// The mixer stays on the game core through Brutal's title map. Moving it
	// here made that opening present once and never return, so the button
	// was never read. It moves when a real level starts.
#endif
	SaveHistory();
	gBackground.clear();
	gBackground.shrink_to_fit();
	fTitle.cache.clear();
	fBody.cache.clear();
}

void NXLS_Progress(int cur, int max)
{
	if (!gActive.load() || max <= 0) return;
	std::lock_guard<std::mutex> lock(mtx);
	if (gMilestone == MS_TEXTURES) gSubFrac = std::min(1.f, float(cur) / float(max));
}

void NXLS_ScriptProgress(int cur, int max)
{
	if (!gActive.load() || max <= 0) return;
	std::lock_guard<std::mutex> lock(mtx);
	if (gMilestone == MS_ACTORS)
	{
		// code generation is roughly the second half of the script stage
		gSubFrac = std::max(gSubFrac, 0.5f + 0.5f * float(cur) / float(max));
		char buf[96];
		snprintf(buf, sizeof(buf), "Gerando código: %d de %d funções", cur, max);
		gDetail = buf;
	}
}

void NXLS_OnPrint(const char *text)
{
	if (!gActive.load() || text == nullptr) return;
	// skip color escapes
	while (*text == '\x1c' && text[1]) text += 2;
	std::lock_guard<std::mutex> lock(mtx);

	if (!strncmp(text, "W_Init", 6)) SetMilestone(MS_WINIT);
	else if (!strncmp(text, "adding ", 7))
	{
		SetMilestone(MS_WINIT);
		std::string s = text + 7;
		size_t comma = s.rfind(", ");
		if (comma != std::string::npos) s.resize(comma);
		// archives inside archives show up as "file.pk3:inner.wad"
		std::string base = BaseName(s);
		bool matched = false;
		for (size_t i = 0; i < gFiles.size(); i++)
		{
			if (!strcasecmp(gFiles[i].name.c_str(), base.c_str()))
			{
				for (size_t j = 0; j < i; j++) if (gFiles[j].state != 2) { gFiles[j].state = 2; gFilesMBDone += gFiles[j].mb; }
				if (gFiles[i].state != 2) { gFiles[i].state = 2; gFilesMBDone += gFiles[i].mb; }
				if (i + 1 < gFiles.size() && gFiles[i + 1].state == 0) gFiles[i + 1].state = 1;
				matched = true;
				break;
			}
		}
		gDetail = "Lendo " + base;
		(void)matched;
	}
	else if (!strncmp(text, "S_Init", 6)) { SetMilestone(MS_SOUND); gDetail.clear(); }
	else if (!strncmp(text, "Texman.Init", 11)) { SetMilestone(MS_TEXTURES); gDetail.clear(); }
	else if (!strncmp(text, "LoadActors", 10)) { SetMilestone(MS_ACTORS); gDetail = "Isso é o que mais demora com mods grandes"; }
	else if (!strncmp(text, "script parsing took", 19)) { SetMilestone(MS_SCRIPTSDONE); gDetail.clear(); }
	else if (!strncmp(text, "R_Init", 6)) { SetMilestone(MS_RINIT); gDetail.clear(); }
	else if (!strncmp(text, "M_Init", 6)) { SetMilestone(MS_MENUS); gDetail.clear(); }
	else if (!strncmp(text, "P_Init", 6)) { SetMilestone(MS_PLAYLOOP); gDetail.clear(); }
	else if (!strncmp(text, "D_CheckNetGame", 14)) { SetMilestone(MS_NETGAME); gDetail.clear(); }
	else if (!strncmp(text, "Execution could not continue", 28)) { /* fatal error follows */ }

	if (gMilestone == MS_WINIT && gFiles.size() > 0 && gFiles[0].state == 0) gFiles[0].state = 1;
}

//==========================================================================
//
// native test harness: renders a few frames to PNG files
//
//==========================================================================

#if defined(NXLS_TEST_MAIN) || defined(NXLS_TEST_SIM)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
void NXLS_TestShot(const char *name)
{
	std::vector<uint32_t> buf(SW * SH);
	for (int i = 0; i < 20; i++) RenderFrame(buf.data(), SW, 1.f / 30);
	stbi_write_png(name, SW, SH, 4, buf.data(), SW * 4);
}
#endif

#ifdef NXLS_TEST_MAIN
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int main(int argc, char **argv)
{
	const char *args[] = { "uzdoom", "-iwad", "doom_fusion.ipk3", "-file", "lexicon-core-v1.0.pk3", "lexicon-base-v1.0.pk3", "gameplay.pk3", "+logfile", "x.log" };
	NXLS_Start(9, (char **)args);
	std::vector<uint32_t> buf(SW * SH);
	auto shot = [&](const char *name) {
		for (int i = 0; i < 40; i++) RenderFrame(buf.data(), SW, 1.f / 30);
		// RGBA in memory order R,G,B,A
		stbi_write_png(name, SW, SH, 4, buf.data(), SW * 4);
	};
	NXLS_OnPrint("W_Init: Init WADfiles.\n");
	NXLS_OnPrint("adding doom_fusion.ipk3, 3000 lumps\n");
	shot("ls1.png");
	NXLS_OnPrint("adding lexicon-core-v1.0.pk3, 3000 lumps\n");
	NXLS_OnPrint("adding lexicon-base-v1.0.pk3, 3000 lumps\n");
	NXLS_OnPrint("S_Init: Setting up sound.\n");
	NXLS_OnPrint("Texman.Init: Init texture manager.\n");
	NXLS_Progress(500, 1000);
	shot("ls2.png");
	NXLS_OnPrint("LoadActors: Load actor definitions.\n");
	NXLS_ScriptProgress(1200, 5000);
	shot("ls3.png");
	NXLS_Stop();
	return 0;
}
#endif
