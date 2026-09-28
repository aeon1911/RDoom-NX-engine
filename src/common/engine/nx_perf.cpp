/*
** nx_perf.cpp
** Frame profiler for the Switch port: slow frames go to perf.log.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <stdio.h>
#include <time.h>
#include "nx_perf.h"
#include "c_cvars.h"
#include "i_time.h"
#include "fs_files.h"
#include "c_cvars.h"

// 0 = off, 1 = log frames slower than nx_perfthreshold, 2 = log every frame
#ifdef __SWITCH__
CVAR(Int, nx_perflog, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Int, nx_perflog, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif
CVAR(Int, nx_perfthreshold, 90, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

NXPerfCounters nxperf;

static double lastFrame = -1;
static double startTime = -1;
static int linesWritten = 0;
static FILE *perfFile = nullptr;
static double sumFrame = 0, maxFrame = 0;
static int frames = 0;
static double lastSummary = 0;
static uint64_t lastMisses = 0, lastBytes = 0;
void (*NXPerf_SummaryHook)(FILE *f) = nullptr;

// totals over the current summary window (all frames, not only the slow ones)
static struct
{
	double tic, think, display, swap, sndupd, sndstart, tex, scene, bsp, hud, update, gpuwait, frame, gc, wait, net, input, music, client, glsync, glfinish, draw2d, frameend, present;
	int tics, thinkers, sndstarts, drawitems, transitems, glsyncs, fxskipped, spriteskips;
} win;

#ifdef __SWITCH__
#include "common/platform/posix/nx/nx_glthread.h"
static bool NXGLThreadOn() { return NX_GLThreadEnabled(); }
static const char *NXGLThreadStatus() { return NX_GLThreadStatus(); }
static void NXGLThreadStats(FILE *f) { NX_GLThreadWriteStats(f); }
#else
static bool NXGLThreadOn() { return false; }
static const char *NXGLThreadStatus() { return "not available"; }
static void NXGLThreadStats(FILE *) {}
#endif

static int ticCounter = 0;
bool NXPerf_ProfileThisTic()
{
	if (nx_perflog <= 0 || NXPerf_SummaryHook == nullptr) return false;
	return (++ticCounter % 35) == 0;
}

static void WriteSettings(FILE *f)
{
	static const char *const names[] = {
		"vid_vsync", "vid_maxfps", "cl_capfps", "vid_scalefactor", "nx_effect_cull", "nx_effect_minalpha", "r_actorspriteshadowdist", "vid_preferbackend", "gl_multithread", "r_dynlights", "gles_max_lights_per_surface",
		"r_actorspriteshadow", "gl_texture_filter", "gl_texture_filter_anisotropic", "r_maxparticles", "cl_maxdecals",
		"snd_channels", "snd_efx", "snd_samplerate", "gles_async_textures", "snd_asyncload", "nx_workers",
		"nashgore_maxgore", "nashgore_maxcasings", "nashgore_bloodmult", "nashgore_gibamount", "nashgore_corpseblood",
		"pb_simplemonsterbehavior", "pb_performance_fire", "pb_tracerlight", "pb_cheapcasings", "pb_disable_decorations",
		"pb_decorative_flame_barrels", "gl_sort_textures", "nx_glthread", "nx_glthread_core", "nx_swapinterval", "r_ticstability", "nx_fx_far", "nx_fx_thin", "nx_lateswap", "pb_gunsmoketype", nullptr };
	fprintf(f, "# settings:");
	for (int i = 0; names[i]; i++)
	{
		FBaseCVar *var = FindCVar(names[i], nullptr);
		if (var) fprintf(f, " %s=%s", names[i], var->GetHumanString());
	}
	fprintf(f, "\n");
	fprintf(f, "# glthread: %s\n", NXGLThreadStatus());
}

double NXPerf_NowMS()
{
	return I_nsTime() / 1000000.0;
}

void NXPerf_EndFrame()
{
	double now = NXPerf_NowMS();
	if (startTime < 0) startTime = now;
	double frame = lastFrame < 0 ? 0 : now - lastFrame;
	lastFrame = now;

	uint64_t hits, misses, bytes;
	FileSys::GetBlockCacheStats(hits, misses, bytes);
	uint64_t dmiss = misses - lastMisses, dbytes = bytes - lastBytes;
	lastMisses = misses;
	lastBytes = bytes;

	if (nx_perflog > 0 && linesWritten < 3000)
	{
		win.tic += nxperf.tic_ms;
		win.think += nxperf.think_ms;
		win.display += nxperf.display_ms;
		win.swap += nxperf.swap_ms;
		win.sndupd += nxperf.sndupd_ms;
		win.sndstart += nxperf.sndstart_ms;
		win.tex += nxperf.tex_ms;
		win.tics += nxperf.tics;
		win.thinkers += nxperf.thinkers;
		win.sndstarts += nxperf.sndstarts;
		win.scene += nxperf.scene_ms;
		win.bsp += nxperf.bsp_ms;
		win.hud += nxperf.hud_ms;
		win.update += nxperf.update_ms;
		win.gpuwait += nxperf.gpuwait_ms;
		win.drawitems += nxperf.drawitems;
		win.transitems += nxperf.transitems;
		win.frame += frame;
		win.gc += nxperf.gc_ms;
		win.wait += nxperf.wait_ms;
		win.net += nxperf.net_ms;
		win.input += nxperf.input_ms;
		win.music += nxperf.music_ms;
		win.client += nxperf.client_ms;
		win.glsync += nxperf.glsync_ms;
		win.glfinish += nxperf.glfinish_ms;
		win.glsyncs += nxperf.glsyncs;
		win.fxskipped += nxperf.fxskipped;
		win.spriteskips += nxperf.spriteskips;
		win.draw2d += nxperf.draw2d_ms;
		win.present += nxperf.present_ms;
		win.frameend += nxperf.frameend_ms;
		frames++;
		sumFrame += frame;
		if (frame > maxFrame) maxFrame = frame;
		bool slow = frame >= nx_perfthreshold;
		if (slow || nx_perflog >= 2)
		{
			if (!perfFile)
			{
				perfFile = fopen("perf.log", "w");
				if (perfFile)
				{
					WriteSettings(perfFile);
					fprintf(perfFile, "# time(s) frame(ms) | tics tic(ms) think(ms) | display(ms): scene bsp hud update gpuwait swap | items transl | shaders n/ms | textures n/ms/kpix | sounds n/ms | sndupdate(ms) sndstart n/ms | io misses/KB | async tex up/wait/budget/pre snd\n");
				}
			}
			if (perfFile)
			{
				fprintf(perfFile, "%8.2f %7.1f | %d %7.1f %6.1f | %7.1f: %5.1f %5.1f %5.1f %5.1f %5.1f %4.1f | %d %d | %d/%.1f | %d/%.1f/%lld | %d/%.1f | %.1f %d/%.1f | %llu/%llu | %d/%d/%d/%d %d\n",
					(now - startTime) / 1000.0, frame, nxperf.tics, nxperf.tic_ms, nxperf.think_ms, nxperf.display_ms,
					nxperf.scene_ms, nxperf.bsp_ms, nxperf.hud_ms, nxperf.update_ms, nxperf.gpuwait_ms, nxperf.swap_ms, nxperf.drawitems, nxperf.transitems,
					nxperf.shaders, nxperf.shader_ms, nxperf.textures, nxperf.tex_ms, (long long)(nxperf.texpixels / 1000),
					nxperf.sounds, nxperf.sound_ms, nxperf.sndupd_ms, nxperf.sndstarts, nxperf.sndstart_ms,
					(unsigned long long)dmiss, (unsigned long long)(dbytes >> 10),
					nxperf.tex_async, nxperf.tex_wait, nxperf.tex_budget, nxperf.tex_preupload, nxperf.snd_async);
				linesWritten++;
			}
		}
		if (perfFile && now - lastSummary > 10000)
		{
			lastSummary = now;
			fprintf(perfFile, "# summary: %d frames, avg %.1f ms (%.1f fps), worst %.1f ms\n", frames, frames ? sumFrame / frames : 0, frames && sumFrame > 0 ? 1000.0 * frames / sumFrame : 0, maxFrame);
			if (frames > 0)
			{
				double n = frames;
				fprintf(perfFile, "#   per frame: tic %.1f (thinkers %.1f) | display %.1f = scene %.1f (lists %.1f) + hud %.1f + update %.1f (2D drawing %.1f, present %.1f, gpu wait %.1f, swap %.1f) + previous frame's end %.1f | sndupdate %.1f sndstart %.1f | textures %.1f | other %.1f\n",
					win.tic / n, win.think / n, win.display / n, win.scene / n, win.bsp / n, win.hud / n, win.update / n, win.draw2d / n, win.present / n, win.gpuwait / n, win.swap / n, win.frameend / n,
					win.sndupd / n, win.sndstart / n, win.tex / n, (win.frame - win.tic - win.display - win.sndupd) / n);
				fprintf(perfFile, "#   outside: gc %.1f, waiting for tic %.1f, input commands %.1f, input events %.1f, music %.1f, client logic/HUD tick %.1f, rest %.1f\n",
					win.gc / n, win.wait / n, win.net / n, win.input / n, win.music / n, win.client / n,
					(win.frame - win.tic - win.display - win.sndupd - win.gc - win.wait - win.net - win.input - win.music - win.client) / n);
				fprintf(perfFile, "#   gl thread: %s | waits per frame: mid-frame %.1f ms (%.1f syncs), before swap %.1f ms\n",
					NXGLThreadOn() ? "on" : "off", win.glsync / n, win.glsyncs / n, win.glfinish / n);
				fprintf(perfFile, "#   per tic: %.1f ms, %d thinkers | draw items per frame %d (%d translucent) | sound starts/s %.0f | effects skipped/s %.0f | sprites waiting for their picture per frame %.1f\n",
					win.tics ? win.tic / win.tics : 0.0, win.tics ? win.thinkers / win.tics : 0, win.drawitems / frames, win.transitems / frames,
					win.sndstarts * 1000.0 / (sumFrame > 0 ? sumFrame : 1), win.fxskipped * 1000.0 / (sumFrame > 0 ? sumFrame : 1), win.spriteskips / n);
			}
			NXGLThreadStats(perfFile);
			if (NXPerf_SummaryHook) NXPerf_SummaryHook(perfFile);
			win = {};
			fflush(perfFile);
			frames = 0;
			sumFrame = maxFrame = 0;
			linesWritten++;
		}
	}
	nxperf = NXPerfCounters();
}
