#pragma once
// Thin platform layer so the launcher can also be built and tested on a PC.

#include <stdint.h>
#include <stddef.h>

#define BASEDIR "gzdoom"
#define TMPDIR BASEDIR "/temp"

#undef VERSION
#define VERSION "uz5.0.3-nx3.7"

enum Button
{
	BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_L, BTN_R, BTN_ZL, BTN_ZR,
	BTN_PLUS, BTN_MINUS,
	BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_LSTICK, BTN_RSTICK,
	BTN_COUNT
};

int plat_init(void);
void plat_free(void);
// pumps events; returns 0 when the app should quit
int plat_loop(void);
// bit mask of held buttons (1 << Button)
uint32_t plat_buttons(void);
// software keyboard; returns 1 if the text was confirmed
int plat_keyboard(const char *header, const char *initial, char *out, int outlen);
// 1 when running as an applet (album) with little memory
int plat_applet_mode(void);
// load a file from the launcher's own resources (romfs); caller frees
void *plat_load_resource(const char *name, size_t *size);
// schedules the engine to start after the launcher exits
void plat_set_next_load(const char *nro, const char *argv);
// milliseconds
uint64_t plat_ticks(void);
// current working directory (absolute)
const char *plat_cwd(void);
