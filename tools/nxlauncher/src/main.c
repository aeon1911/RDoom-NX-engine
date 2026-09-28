// UZDoom Switch launcher

#include "platform.h"
#include "gfx.h"
#include "data.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (plat_init()) return 1;
	if (gfx_init())
	{
		FILE *f = fopen(BASEDIR "/launcher_error.log", "w");
		if (f)
		{
			fprintf(f, "could not initialize graphics or load the fonts (romfs)\n");
			fclose(f);
		}
		gfx_free();
		return 1;
	}
	data_init();
	ui_run();
	ui_free();
	gfx_free();
	plat_free();
	return 0;
}
