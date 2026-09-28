#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#ifdef __SWITCH__
#include <switch.h>

static PadState pad;
static char cwd[512];

void userAppInit(void)
{
	romfsInit();
}

void userAppExit(void)
{
	romfsExit();
}

int plat_init(void)
{
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	padUpdate(&pad);
	if (!getcwd(cwd, sizeof(cwd) - 1)) strcpy(cwd, "sdmc:/switch");
	return 0;
}

void plat_free(void)
{
}

int plat_loop(void)
{
	if (!appletMainLoop()) return 0;
	padUpdate(&pad);
	SDL_Event ev;
	while (SDL_PollEvent(&ev))
	{
		if (ev.type == SDL_QUIT) return 0;
	}
	return 1;
}

uint32_t plat_buttons(void)
{
	u64 b = padGetButtons(&pad);
	uint32_t r = 0;
	if (b & HidNpadButton_A) r |= 1u << BTN_A;
	if (b & HidNpadButton_B) r |= 1u << BTN_B;
	if (b & HidNpadButton_X) r |= 1u << BTN_X;
	if (b & HidNpadButton_Y) r |= 1u << BTN_Y;
	if (b & HidNpadButton_L) r |= 1u << BTN_L;
	if (b & HidNpadButton_R) r |= 1u << BTN_R;
	if (b & HidNpadButton_ZL) r |= 1u << BTN_ZL;
	if (b & HidNpadButton_ZR) r |= 1u << BTN_ZR;
	if (b & HidNpadButton_Plus) r |= 1u << BTN_PLUS;
	if (b & HidNpadButton_Minus) r |= 1u << BTN_MINUS;
	if (b & (HidNpadButton_Up | HidNpadButton_StickLUp)) r |= 1u << BTN_UP;
	if (b & (HidNpadButton_Down | HidNpadButton_StickLDown)) r |= 1u << BTN_DOWN;
	if (b & (HidNpadButton_Left | HidNpadButton_StickLLeft)) r |= 1u << BTN_LEFT;
	if (b & (HidNpadButton_Right | HidNpadButton_StickLRight)) r |= 1u << BTN_RIGHT;
	if (b & HidNpadButton_StickL) r |= 1u << BTN_LSTICK;
	if (b & HidNpadButton_StickR) r |= 1u << BTN_RSTICK;
	return r;
}

int plat_keyboard(const char *header, const char *initial, char *out, int outlen)
{
	SwkbdConfig kbd;
	if (R_FAILED(swkbdCreate(&kbd, 0))) return 0;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetHeaderText(&kbd, header);
	swkbdConfigSetGuideText(&kbd, header);
	if (initial) swkbdConfigSetInitialText(&kbd, initial);
	char *tmp = calloc(1, outlen + 1);
	Result rc = swkbdShow(&kbd, tmp, outlen);
	swkbdClose(&kbd);
	if (R_SUCCEEDED(rc))
	{
		strncpy(out, tmp, outlen - 1);
		out[outlen - 1] = 0;
	}
	free(tmp);
	return R_SUCCEEDED(rc);
}

int plat_applet_mode(void)
{
	AppletType t = appletGetAppletType();
	return t != AppletType_Application && t != AppletType_SystemApplication;
}

void *plat_load_resource(const char *name, size_t *size)
{
	char path[256];
	snprintf(path, sizeof(path), "romfs:/%s", name);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc(n + 1);
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (size) *size = n;
	return buf;
}

void plat_set_next_load(const char *nro, const char *argv)
{
	envSetNextLoad(nro, argv);
}

uint64_t plat_ticks(void)
{
	return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

const char *plat_cwd(void)
{
	return cwd;
}

#else // PC build for testing

static uint32_t keys;
static char cwd[512];

int plat_init(void)
{
	if (!getcwd(cwd, sizeof(cwd) - 1)) strcpy(cwd, ".");
	return 0;
}

void plat_free(void)
{
}

static uint32_t keybit(SDL_Keycode k)
{
	switch (k)
	{
	case SDLK_RETURN: case SDLK_a: return 1u << BTN_A;
	case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_b: return 1u << BTN_B;
	case SDLK_x: return 1u << BTN_X;
	case SDLK_y: return 1u << BTN_Y;
	case SDLK_q: return 1u << BTN_L;
	case SDLK_e: return 1u << BTN_R;
	case SDLK_1: return 1u << BTN_ZL;
	case SDLK_3: return 1u << BTN_ZR;
	case SDLK_p: return 1u << BTN_PLUS;
	case SDLK_m: return 1u << BTN_MINUS;
	case SDLK_UP: return 1u << BTN_UP;
	case SDLK_DOWN: return 1u << BTN_DOWN;
	case SDLK_LEFT: return 1u << BTN_LEFT;
	case SDLK_RIGHT: return 1u << BTN_RIGHT;
	default: return 0;
	}
}

// scripted input for automated screenshots: LAUNCHER_SCRIPT="down,down,a,shot:foo.png,..."
extern void gfx_screenshot(const char *path);
static char *script;
static char *scriptpos;
static uint64_t scriptnext;
static uint32_t scriptkeys;

static uint32_t name2btn(const char *n)
{
	static const char *names[BTN_COUNT] = { "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "up", "down", "left", "right", "ls", "rs" };
	for (int i = 0; i < BTN_COUNT; i++) if (!strcmp(n, names[i])) return 1u << i;
	return 0;
}

int plat_loop(void)
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev))
	{
		if (ev.type == SDL_QUIT) return 0;
		if (ev.type == SDL_KEYDOWN) keys |= keybit(ev.key.keysym.sym);
		if (ev.type == SDL_KEYUP) keys &= ~keybit(ev.key.keysym.sym);
	}
	if (!script)
	{
		const char *s = getenv("LAUNCHER_SCRIPT");
		if (s) { script = strdup(s); scriptpos = script; scriptnext = plat_ticks() + 600; }
	}
	if (script && plat_ticks() >= scriptnext)
	{
		if (scriptkeys) { scriptkeys = 0; scriptnext = plat_ticks() + 120; return 1; }
		if (!scriptpos || !*scriptpos) return 0; // script done -> quit
		char tok[128];
		size_t n = strcspn(scriptpos, ",");
		if (n >= sizeof(tok)) n = sizeof(tok) - 1;
		memcpy(tok, scriptpos, n); tok[n] = 0;
		scriptpos += n; if (*scriptpos == ',') scriptpos++;
		if (!strncmp(tok, "shot:", 5)) { gfx_screenshot(tok + 5); scriptnext = plat_ticks() + 50; }
		else if (!strncmp(tok, "wait:", 5)) { scriptnext = plat_ticks() + atoi(tok + 5); }
		else { scriptkeys = name2btn(tok); scriptnext = plat_ticks() + 120; }
	}
	return 1;
}

uint32_t plat_buttons(void)
{
	return keys | scriptkeys;
}

int plat_keyboard(const char *header, const char *initial, char *out, int outlen)
{
	const char *t = getenv("LAUNCHER_KBD");
	if (!t) t = "Meu perfil";
	strncpy(out, t, outlen - 1);
	out[outlen - 1] = 0;
	(void)header; (void)initial;
	return 1;
}

int plat_applet_mode(void)
{
	return 0;
}

void *plat_load_resource(const char *name, size_t *size)
{
	char path[512];
	const char *dir = getenv("LAUNCHER_RES");
	snprintf(path, sizeof(path), "%s/%s", dir ? dir : "res", name);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc(n + 1);
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (size) *size = n;
	return buf;
}

void plat_set_next_load(const char *nro, const char *argv)
{
	printf("NEXT LOAD: %s\nARGV: %s\n", nro, argv);
}

uint64_t plat_ticks(void)
{
	return SDL_GetTicks64();
}

const char *plat_cwd(void)
{
	return cwd;
}

#endif
