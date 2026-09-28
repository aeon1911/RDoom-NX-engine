#include "art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

static inline uint32_t PackRGBA(int r, int g, int b, int a)
{
	uint32_t v;
	unsigned char *p = (unsigned char *)&v;
	p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = (unsigned char)a;
	return v;
}

//==========================================================================
// file helpers
//==========================================================================

static uint16_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int ext_is(const char *path, const char *ext)
{
	const char *d = strrchr(path, '.');
	return d && !strcasecmp(d, ext);
}

//==========================================================================
// WAD
//==========================================================================

typedef struct
{
	unsigned char *data;
	size_t size;
} Blob;

static void blob_free(Blob *b) { free(b->data); b->data = NULL; b->size = 0; }

// last = 1: take the last lump with that name (PWAD overrides)
static int wad_read_lump(FILE *f, const char *name, int last, Blob *out)
{
	unsigned char hdr[12];
	fseek(f, 0, SEEK_SET);
	if (fread(hdr, 1, 12, f) != 12) return 0;
	if (memcmp(hdr + 1, "WAD", 3)) return 0;
	uint32_t num = rd32(hdr + 4), dir = rd32(hdr + 8);
	if (num == 0 || num > 200000) return 0;
	unsigned char *d = malloc(num * 16);
	if (!d) return 0;
	fseek(f, dir, SEEK_SET);
	if (fread(d, 16, num, f) != num) { free(d); return 0; }
	int found = -1;
	for (uint32_t i = 0; i < num; i++)
	{
		char n[9];
		memcpy(n, d + i * 16 + 8, 8);
		n[8] = 0;
		if (!strcasecmp(n, name)) { found = (int)i; if (!last) break; }
	}
	int ok = 0;
	if (found >= 0)
	{
		uint32_t pos = rd32(d + found * 16), size = rd32(d + found * 16 + 4);
		if (size > 0 && size < 32 * 1024 * 1024)
		{
			out->data = malloc(size);
			out->size = size;
			fseek(f, pos, SEEK_SET);
			ok = out->data && fread(out->data, 1, size, f) == size;
			if (!ok) blob_free(out);
		}
	}
	free(d);
	return ok;
}

//==========================================================================
// ZIP (pk3/ipk3)
//==========================================================================

typedef struct
{
	unsigned char *cd;
	uint32_t cdsize;
	uint32_t count;
} ZipDir;

static int zip_open(FILE *f, ZipDir *z)
{
	memset(z, 0, sizeof(*z));
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	long tail = fsize < 65557 ? fsize : 65557;
	unsigned char *buf = malloc(tail);
	fseek(f, fsize - tail, SEEK_SET);
	if (fread(buf, 1, tail, f) != (size_t)tail) { free(buf); return 0; }
	long eocd = -1;
	for (long i = tail - 22; i >= 0; i--)
		if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
	if (eocd < 0) { free(buf); return 0; }
	z->count = rd16(buf + eocd + 10);
	z->cdsize = rd32(buf + eocd + 12);
	uint32_t cdoff = rd32(buf + eocd + 16);
	free(buf);
	if (z->cdsize == 0 || z->cdsize > 64 * 1024 * 1024) return 0;
	z->cd = malloc(z->cdsize);
	fseek(f, cdoff, SEEK_SET);
	if (!z->cd || fread(z->cd, 1, z->cdsize, f) != z->cdsize) { free(z->cd); z->cd = NULL; return 0; }
	return 1;
}

// finds an entry whose file name without extension is 'base' (in the root or graphics/)
static int zip_read(FILE *f, ZipDir *z, const char *base, Blob *out)
{
	unsigned char *p = z->cd, *end = z->cd + z->cdsize;
	unsigned char *best = NULL;
	int bestscore = -1;
	while (p + 46 <= end && rd32(p) == 0x02014b50)
	{
		int nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
		if (p + 46 + nlen > end) break;
		char name[512];
		int n = nlen < 511 ? nlen : 511;
		memcpy(name, p + 46, n);
		name[n] = 0;
		const char *slash = strrchr(name, '/');
		const char *fn = slash ? slash + 1 : name;
		const char *dot = strrchr(fn, '.');
		size_t blen = dot ? (size_t)(dot - fn) : strlen(fn);
		if (blen == strlen(base) && !strncasecmp(fn, base, blen))
		{
			int score = 0;
			if (!slash) score = 2;
			else if (!strncasecmp(name, "graphics/", 9) && slash == name + 8) score = 3;
			else if (strchr(name, '/') == slash) score = 1;	// some other top level dir
			if (score > bestscore) { bestscore = score; best = p; }
		}
		p += 46 + nlen + xlen + clen;
	}
	if (!best) return 0;
	int method = rd16(best + 10);
	uint32_t csize = rd32(best + 20), usize = rd32(best + 24), lho = rd32(best + 42);
	if (usize == 0 || usize > 32 * 1024 * 1024 || csize > 32 * 1024 * 1024) return 0;
	unsigned char lh[30];
	fseek(f, lho, SEEK_SET);
	if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) return 0;
	fseek(f, lho + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
	unsigned char *comp = malloc(csize ? csize : 1);
	if (!comp || fread(comp, 1, csize, f) != csize) { free(comp); return 0; }
	int ok = 0;
	if (method == 0)
	{
		out->data = comp;
		out->size = csize;
		return 1;
	}
	else if (method == 8)
	{
		out->data = malloc(usize);
		out->size = usize;
		z_stream s;
		memset(&s, 0, sizeof(s));
		if (out->data && inflateInit2(&s, -MAX_WBITS) == Z_OK)
		{
			s.next_in = comp;
			s.avail_in = csize;
			s.next_out = out->data;
			s.avail_out = usize;
			int r = inflate(&s, Z_FINISH);
			ok = (r == Z_STREAM_END || (r == Z_OK && s.avail_out == 0)) && s.total_out == usize;
			inflateEnd(&s);
		}
		if (!ok) blob_free(out);
	}
	free(comp);
	return ok;
}

//==========================================================================
// picture formats
//==========================================================================

static int decode_png(const Blob *b, Image *out)
{
	int w, h, n;
	unsigned char *px = stbi_load_from_memory(b->data, (int)b->size, &w, &h, &n, 4);
	if (!px) return 0;
	out->px = (uint32_t *)px;
	out->w = w;
	out->h = h;
	return 1;
}

static int decode_patch(const Blob *b, const unsigned char *pal, Image *out)
{
	if (b->size < 8) return 0;
	int w = rd16(b->data), h = rd16(b->data + 2);
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || b->size < 8 + (size_t)w * 4) return 0;
	out->px = calloc(w * h, 4);
	out->w = w;
	out->h = h;
	for (int x = 0; x < w; x++)
	{
		uint32_t ofs = rd32(b->data + 8 + x * 4);
		int top = -1;
		while (ofs < b->size)
		{
			int td = b->data[ofs];
			if (td == 0xFF) break;
			if (ofs + 3 > b->size) break;
			int len = b->data[ofs + 1];
			top = (td <= top) ? top + td : td;	// tall patches
			const unsigned char *src = b->data + ofs + 3;
			if (ofs + 3 + len > b->size) break;
			for (int i = 0; i < len; i++)
			{
				int y = top + i;
				if (y >= 0 && y < h)
				{
					int c = src[i];
					out->px[y * w + x] = PackRGBA(pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2], 255);
				}
			}
			ofs += len + 4;
		}
	}
	return 1;
}

static int decode_raw(const Blob *b, const unsigned char *pal, Image *out)
{
	if (b->size != 64000) return 0;
	out->w = 320;
	out->h = 200;
	out->px = malloc(320 * 200 * 4);
	for (int i = 0; i < 64000; i++)
	{
		int c = b->data[i];
		out->px[i] = PackRGBA(pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2], 255);
	}
	return 1;
}

static int is_png(const Blob *b)
{
	return b->size > 8 && !memcmp(b->data, "\x89PNG", 4);
}

static int read_game_lump(const char *path, const char *name, Blob *out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	int ok = 0;
	unsigned char magic[4] = { 0 };
	if (fread(magic, 1, 4, f) == 4)
	{
		if (!memcmp(magic + 1, "WAD", 3)) ok = wad_read_lump(f, name, 1, out);
		else if (magic[0] == 'P' && magic[1] == 'K')
		{
			ZipDir z;
			if (zip_open(f, &z))
			{
				ok = zip_read(f, &z, name, out);
				free(z.cd);
			}
		}
	}
	fclose(f);
	return ok;
}

int art_load_title(const char *path, const char *palette_fallback, Image *out)
{
	memset(out, 0, sizeof(*out));
	Blob pic = { 0 }, pal = { 0 };
	int raw = 0;
	if (!read_game_lump(path, "TITLEPIC", &pic))
	{
		// Heretic / Hexen use a raw 320x200 "TITLE" lump
		if (!read_game_lump(path, "TITLE", &pic)) return 0;
		raw = 1;
	}
	int ok = 0;
	if (is_png(&pic))
	{
		ok = decode_png(&pic, out);
	}
	else
	{
		if (!read_game_lump(path, "PLAYPAL", &pal) || pal.size < 768)
		{
			blob_free(&pal);
			if (palette_fallback) read_game_lump(palette_fallback, "PLAYPAL", &pal);
		}
		if (pal.size >= 768)
		{
			ok = raw ? decode_raw(&pic, pal.data, out) : decode_patch(&pic, pal.data, out);
			if (!ok && !raw) ok = decode_raw(&pic, pal.data, out);
		}
	}
	blob_free(&pic);
	blob_free(&pal);
	(void)ext_is;
	return ok;
}

void art_free(Image *img)
{
	free(img->px);
	img->px = NULL;
	img->w = img->h = 0;
}

//==========================================================================
// backgrounds
//==========================================================================

void art_make_background(Image *out, int w, int h)
{
	out->w = w;
	out->h = h;
	out->px = malloc(w * h * 4);
	for (int y = 0; y < h; y++)
	{
		float fy = y / (float)h;
		for (int x = 0; x < w; x++)
		{
			float fx = x / (float)w;
			float dx = fx - 0.5f, dy = fy - 1.08f;
			float glow = expf(-(dx * dx * 2.0f + dy * dy * 3.6f));
			float glow2 = expf(-((fx - 0.85f) * (fx - 0.85f) * 6.f + (fy - 0.15f) * (fy - 0.15f) * 8.f));
			float vign = 1.f - 0.5f * (dx * dx * 1.6f + (fy - 0.5f) * (fy - 0.5f) * 1.2f);
			int r = (int)((13 + 140 * glow + 20 * glow2) * vign);
			int g = (int)((7 + 28 * glow + 4 * glow2) * vign);
			int b = (int)((7 + 10 * glow + 6 * glow2) * vign);
			out->px[y * w + x] = PackRGBA(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255);
		}
	}
}

// bilinear sample
static void sample(const Image *img, float u, float v, float *rgb)
{
	float x = u * img->w - 0.5f, y = v * img->h - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;
	float acc[3] = { 0, 0, 0 };
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			int sx = x0 + i, sy = y0 + j;
			if (sx < 0) sx = 0;
			if (sy < 0) sy = 0;
			if (sx >= img->w) sx = img->w - 1;
			if (sy >= img->h) sy = img->h - 1;
			const unsigned char *p = (const unsigned char *)&img->px[sy * img->w + sx];
			float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			acc[0] += p[0] * wgt;
			acc[1] += p[1] * wgt;
			acc[2] += p[2] * wgt;
		}
	}
	rgb[0] = acc[0];
	rgb[1] = acc[1];
	rgb[2] = acc[2];
}

int art_write_loadbg(const Image *art, const char *outpath)
{
	const int W = 1280, H = 720;
	Image bg;
	art_make_background(&bg, W, H);
	if (art && art->px)
	{
		// the picture fills the height on the right side (4:3), fading out to the left
		float aspect = art->w / (float)art->h;
		if (art->h == 200 || art->h == 400) aspect /= 1.2f;
		float aw = H * aspect;
		if (aw > W) aw = W;
		float ax = W - aw;
		for (int y = 0; y < H; y++)
		{
			for (int x = (int)ax; x < W; x++)
			{
				float u = (x - ax) / aw, v = y / (float)H;
				float rgb[3];
				sample(art, u, v, rgb);
				float fade = u < 0.45f ? u / 0.45f : 1.f;
				fade = fade * fade * (3 - 2 * fade);
				float vfade = v > 0.7f ? 1.f - (v - 0.7f) / 0.3f * 0.6f : 1.f;
				float k = 0.42f * fade * vfade;
				unsigned char *d = (unsigned char *)&bg.px[y * W + x];
				for (int c = 0; c < 3; c++) d[c] = (unsigned char)(d[c] * (1 - k) + rgb[c] * k);
			}
		}
	}
	FILE *f = fopen(outpath, "wb");
	int ok = -1;
	if (f)
	{
		ok = fwrite(bg.px, 4, W * H, f) == (size_t)(W * H) ? 0 : -1;
		fclose(f);
	}
	art_free(&bg);
	return ok;
}
