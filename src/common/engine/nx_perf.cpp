/*
** nx_perf.cpp
** Frame profiler for the Switch port: slow frames go to perf.log.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "nx_governor.h"
#include <stdio.h>
#include <time.h>
#include "nx_perf.h"
#ifdef __SWITCH__
#include "common/platform/posix/nx/rdoom_nx.h"
#endif
#include <stdarg.h>
#include "c_cvars.h"
#include "i_time.h"
#include "fs_files.h"
#include "gamestate.h"

bool NXInPlay()
{
	return gamestate == GS_LEVEL;
}

// 0 = off, 1 = log frames slower than nx_perfthreshold, 2 = log every frame
#ifdef __SWITCH__
CVAR(Int, nx_perflog, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Int, nx_perflog, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif
CVAR(Int, nx_perfthreshold, 40, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

NXPerfCounters nxperf;

static double lastFrame = -1;
static double lastFrameMS = 0;
static double lastSimMS = 0;
static double lastTicMS = 0;
static double startTime = -1;
static int linesWritten = 0;
static FILE *perfFile = nullptr;
static double sumFrame = 0, maxFrame = 0;
static double lastTicWaitMS = 0;
static int frames = 0;
static int missed1 = 0, missed2 = 0;	// frames over one / over two vsync intervals
static double lastSummary = 0;
static uint64_t lastMisses = 0, lastBytes = 0;
void (*NXPerf_SummaryHook)(FILE *f) = nullptr;

// totals over the current summary window (all frames, not only the slow ones)
static struct
{
	double tic, think, display, swap, sndupd, sndstart, tex, scene, bsp, hud, update, gpuwait, frame, gc, wait, net, input, music, client, glsync, glfinish, draw2d, frameend, present, vksubmit, vkclean, vkacquire, vkpresent, vkpending, gpuframe, gpuscene, gpupost, gpuend;
	int gpuframes;
	double scalesum; float scalemin, scalemax;
	double halfarea, halfareamax;
	double checkpos_ms; int checkpos, checkthings;
	double sight_ms, spawnms; int sights, spawns;
	double spcreate, splink, spplace, spbegin;
	double nmalloc, ncopy, nctor, nspec; double nbytes;
	double hmalloc, hfree; int hmallocs, hfrees;
	int tics, thinkers, sndstarts, drawitems, transitems, glsyncs, spriteskips, draws, lightuploads, lightskips, dynlights, dynlightsall, govlevel, vksubmits, effectsculled, halfeffects;
} win;

#ifdef __SWITCH__
#include <switch.h>
// handheld and docked run the GPU at different (stock) clocks
static const char *NXOperationMode() { return appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld"; }
#include "common/platform/posix/nx/nx_glthread.h"
static bool NXGLThreadOn() { return NX_GLThreadEnabled(); }
static const char *NXGLThreadStatus() { return NX_GLThreadStatus(); }
static void NXGLThreadStats(FILE *f) { NX_GLThreadWriteStats(f); }
#else
static const char *NXOperationMode() { return "pc"; }
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
		"vid_vsync", "vid_maxfps", "cl_capfps", "vid_scalefactor", "nx_effect_cull", "nx_effect_minalpha", "r_actorspriteshadowdist", "vid_preferbackend", "gl_multithread", "r_dynlights",
		"r_actorspriteshadow", "gl_texture_filter", "gl_texture_filter_anisotropic", "r_maxparticles", "cl_maxdecals",
		"snd_channels", "snd_efx", "snd_samplerate", "snd_asyncload", "nx_workers",
		"gl_sort_textures", "nx_dynlight_max", "nx_dynlight_dist", "nx_stabilizer", "nx_glthread", "nx_glthread_core", "nx_swapinterval", "r_ticstability", "nx_lateswap", "nx_simcap", nullptr };
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

double NXPerf_LastFrameMS()
{
	return lastFrameMS;
}

double NXPerf_LastTicWaitMS()
{
	return lastTicWaitMS;
}

double NXPerf_LastSimMS()
{
	return lastSimMS;
}

double NXPerf_LastTicMS()
{
	return lastTicMS;
}

void NXPerf_Note(const char *fmt, ...)
{
	if (!perfFile)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(perfFile, "#   note %.2f: ", startTime < 0 ? 0.0 : (NXPerf_NowMS() - startTime) / 1000.0);
	vfprintf(perfFile, fmt, ap);
	fputc('\n', perfFile);
	va_end(ap);
}

void NXPerf_FlushFile()
{
	if (perfFile)
		fflush(perfFile);
}

void NXPerf_EndFrame()
{
	double now = NXPerf_NowMS();
	if (startTime < 0) startTime = now;
	double frame = lastFrame < 0 ? 0 : now - lastFrame;
	lastFrame = now;
	lastFrameMS = frame;
	lastTicWaitMS = nxperf.wait_ms;
	lastSimMS = nxperf.tic_ms;
	lastTicMS = (nxperf.tics > 0) ? nxperf.tic_ms / nxperf.tics : 0;

	uint64_t hits, misses, bytes;
	FileSys::GetBlockCacheStats(hits, misses, bytes);
	uint64_t dmiss = misses - lastMisses, dbytes = bytes - lastBytes;
	lastMisses = misses;
	lastBytes = bytes;

	{
		extern int nx_levelframes;
		// Vulkan waits for the display/GPU inside acquire, present and the frame-start wait
		// frames held back by the tic rate (menu open, paused: 35 fps) are not late frames
		const bool ticCapped = nxperf.wait_ms > 8.0;
		NXGovernor_Frame(frame, nxperf.swap_ms + nxperf.gpuwait_ms + nxperf.wait_ms + nxperf.vkacquire_ms + nxperf.vkpresent_ms + nxperf.vkpending_ms, nx_levelframes > 3 && !ticCapped);
	}

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
		win.spriteskips += nxperf.spriteskips;
		win.effectsculled += nxperf.effectsculled;
		win.halfeffects += nxperf.halfeffects;
		win.halfarea += nxperf.halfarea;
		win.checkpos_ms += nxperf.checkpos_ms;
		win.checkpos += nxperf.checkpos;
		win.checkthings += nxperf.checkthings;
		win.sight_ms += nxperf.sight_ms;
		win.sights += nxperf.sights;
		win.spawnms += nxperf.spawn_ms;
		win.spawns += nxperf.spawns;
		win.spcreate += nxperf.spawn_create_ms;
		win.splink += nxperf.spawn_link_ms;
		win.spplace += nxperf.spawn_place_ms;
		win.spbegin += nxperf.spawn_begin_ms;
		win.nmalloc += nxperf.new_malloc_ms;
		win.ncopy += nxperf.new_copy_ms;
		win.nctor += nxperf.new_ctor_ms;
		win.nspec += nxperf.new_specials_ms;
		win.nbytes += nxperf.new_bytes;
		win.hmalloc += nxperf.heap_malloc_ms;
		win.hfree += nxperf.heap_free_ms;
		win.hmallocs += nxperf.heap_mallocs;
		win.hfrees += nxperf.heap_frees;
		win.halfareamax = std::max(win.halfareamax, nxperf.halfarea);
		win.draw2d += nxperf.draw2d_ms;
		win.present += nxperf.present_ms;
		win.frameend += nxperf.frameend_ms;
		win.vksubmits += nxperf.vksubmits;
		win.vksubmit += nxperf.vksubmit_ms;
		win.vkclean += nxperf.vkclean_ms;
		win.vkacquire += nxperf.vkacquire_ms;
		win.vkpresent += nxperf.vkpresent_ms;
		win.vkpending += nxperf.vkpending_ms;
		win.gpuframes += nxperf.gpuframes;
		win.gpuframe += nxperf.gpu_frame_ms;
		win.gpuscene += nxperf.gpu_scene_ms;
		win.gpupost += nxperf.gpu_post_ms;
		win.gpuend += nxperf.gpu_end_ms;
		win.scalesum += nxperf.scene_scale;
		if (win.scalemin <= 0 || nxperf.scene_scale < win.scalemin) win.scalemin = nxperf.scene_scale;
		if (nxperf.scene_scale > win.scalemax) win.scalemax = nxperf.scene_scale;
		win.draws += nxperf.draws;
		win.lightuploads += nxperf.lightuploads;
		win.lightskips += nxperf.lightskips;
		win.dynlights += nxperf.dynlights;
		win.dynlightsall += nxperf.dynlightsall;
		win.govlevel += nxGovLevel;
		frames++;
		sumFrame += frame;
		if (frame > maxFrame) maxFrame = frame;
		if (frame > 1000.0 / 60.0 + 1.5) missed1++;
		if (frame > 2000.0 / 60.0 + 1.5) missed2++;
		bool slow = frame >= nx_perfthreshold;
		if (slow || nx_perflog >= 2)
		{
			if (!perfFile)
			{
#ifdef __SWITCH__
				perfFile = fopen("logs/perf.log", "w");
#else
				perfFile = fopen("perf.log", "w");
#endif
				if (perfFile)
				{
					// written to the card by the background writer (NXPerf_FlushFile)
					setvbuf(perfFile, nullptr, _IOFBF, 64 * 1024);
#ifdef __SWITCH__
					RDoom_BufferFile(perfFile);
#endif
					WriteSettings(perfFile);
					fprintf(perfFile, "# time(s) frame(ms) | tics tic(ms) think(ms) | display(ms): scene bsp hud update gpuwait swap | items transl | shaders n/ms | textures n/ms/kpix | sounds n/ms | sndupdate(ms) sndstart n/ms | io misses/KB | async tex up/wait/budget/pre snd | draws | lights used/all up/same | stab | player(ms) spawn n/ms skipped | startsound n/ms | begin(ms) pending(ms) = wait/save/buffers/sprites, slowest upload\n");
				}
			}
			if (perfFile)
			{
				fprintf(perfFile, "%8.2f %7.1f | %d %7.1f %6.1f | %7.1f: %5.1f %5.1f %5.1f %5.1f %5.1f %4.1f | %d %d | %d/%.1f | %d/%.1f/%lld | %d/%.1f | %.1f %d/%.1f | %llu/%llu | %d/%d/%d/%d %d | %d | %d/%d %d/%d | %d | %.1f %d/%.1f %d | %d/%.1f | %.1f %.1f = %.1f/%.1f/%.1f/%.1f %.1f\n",
					(now - startTime) / 1000.0, frame, nxperf.tics, nxperf.tic_ms, nxperf.think_ms, nxperf.display_ms,
					nxperf.scene_ms, nxperf.bsp_ms, nxperf.hud_ms, nxperf.update_ms, nxperf.gpuwait_ms, nxperf.swap_ms, nxperf.drawitems, nxperf.transitems,
					nxperf.shaders, nxperf.shader_ms, nxperf.textures, nxperf.tex_ms, (long long)(nxperf.texpixels / 1000),
					nxperf.sounds, nxperf.sound_ms, nxperf.sndupd_ms, nxperf.sndstarts, nxperf.sndstart_ms,
					(unsigned long long)dmiss, (unsigned long long)(dbytes >> 10),
					nxperf.tex_async, nxperf.tex_wait, nxperf.tex_budget, nxperf.tex_preupload, nxperf.snd_async,
					nxperf.draws, nxperf.dynlights, nxperf.dynlightsall, nxperf.lightuploads, nxperf.lightskips, nxGovLevel,
					nxperf.player_ms, nxperf.spawns, nxperf.spawn_ms, nxperf.spriteskips, nxperf.sndcalls, nxperf.sndcall_ms, nxperf.begin_ms, nxperf.vkpending_ms,
					nxperf.begin_wait_ms, nxperf.begin_save_ms, nxperf.begin_buffers_ms, nxperf.begin_sprites_ms, nxperf.upload_max_ms);
				linesWritten++;
			}
		}
		// every 5 s: tests on the console are often short sessions
		if (perfFile && now - lastSummary > 5000)
		{
			lastSummary = now;
			fprintf(perfFile, "# summary: %d frames, avg %.1f ms (%.1f fps), worst %.1f ms | late frames: %d over 18 ms, %d over 35 ms\n", frames, frames ? sumFrame / frames : 0, frames && sumFrame > 0 ? 1000.0 * frames / sumFrame : 0, maxFrame, missed1, missed2);
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
				fprintf(perfFile, "#   per tic: %.1f ms, %d thinkers | draw items per frame %d (%d translucent) | sound starts/s %.0f | sprites waiting for their picture per frame %.1f\n",
					win.tics ? win.tic / win.tics : 0.0, win.tics ? win.thinkers / win.tics : 0, win.drawitems / frames, win.transitems / frames,
					win.sndstarts * 1000.0 / (sumFrame > 0 ? sumFrame : 1), win.spriteskips / n);
				if (win.tics > 0)
					fprintf(perfFile, "#   per tic: collision %d position checks (%.2f ms), %d actors tested | sight checks %d (%.2f ms) | actors spawned %d (%.2f ms: create %.2f, link %.2f, place %.2f, BeginPlay %.2f)\n",
						win.checkpos / win.tics, win.checkpos_ms / win.tics, win.checkthings / win.tics,
						win.sights / win.tics, win.sight_ms / win.tics, win.spawns / win.tics, win.spawnms / win.tics,
						win.spcreate / win.tics, win.splink / win.tics, win.spplace / win.tics, win.spbegin / win.tics);
				if (win.tics > 0)
					fprintf(perfFile, "#   object creation per tic: malloc %.2f ms, defaults copy %.2f ms (%.0f KB), constructor %.2f ms, script fields %.2f ms\n",
						win.nmalloc / win.tics, win.ncopy / win.tics, win.nbytes / 1024.0 / win.tics, win.nctor / win.tics, win.nspec / win.tics);
				if (frames > 0)
					fprintf(perfFile, "#   heap per frame (game thread): %d malloc/realloc (%.2f ms), %d free (%.2f ms)\n",
						win.hmallocs / frames, win.hmalloc / frames, win.hfrees / frames, win.hfree / frames);
				fprintf(perfFile, "#   GL draw calls per frame %.0f | dynamic lights per frame: %.1f used of %.1f | light uploads %.0f, identical skipped %.0f | stabilizer level avg %.2f (now %d), effects left out per frame %.1f, in the half-resolution layer %.1f (their area %.2f screens, most %.2f)\n",
					win.draws / n, win.dynlights / n, win.dynlightsall / n, win.lightuploads / n, win.lightskips / n, win.govlevel / n, nxGovLevel, win.effectsculled / n, win.halfeffects / n, win.halfarea / n, win.halfareamax);
			}
			if (frames > 0 && win.vksubmits > 0)
			{
				double n = frames;
				fprintf(perfFile, "#   vulkan: submits %.1f (%.2f ms in vkQueueSubmit) | cache clean %.2f | acquire %.2f | present %.2f | fence wait at frame end %.2f | wait for previous frame at frame start %.2f (ms per frame)\n",
					win.vksubmits / n, win.vksubmit / n, win.vkclean / n, win.vkacquire / n, win.vkpresent / n, win.gpuwait / n, win.vkpending / n);
			}
			if (win.gpuframes > 0)
			{
				double g = win.gpuframes;
				fprintf(perfFile, "#   gpu (%s): frame %.2f ms = 3D scene %.2f + post-processing %.2f + 2D/present %.2f (avg of %d frames) | 3D resolution avg %.0f%%, min %.0f%%, max %.0f%%\n",
					NXOperationMode(), win.gpuframe / g, win.gpuscene / g, win.gpupost / g, win.gpuend / g, win.gpuframes,
					frames ? 100.0 * win.scalesum / frames : 100.0, win.scalemin > 0 ? 100.0 * win.scalemin : 100.0, win.scalemax > 0 ? 100.0 * win.scalemax : 100.0);
			}
			NXGLThreadStats(perfFile);
			if (NXPerf_SummaryHook) NXPerf_SummaryHook(perfFile);
			win = {};
#ifndef __SWITCH__
			fflush(perfFile);
#endif
			frames = 0;
			missed1 = missed2 = 0;
			sumFrame = maxFrame = 0;
			linesWritten++;
		}
	}
	nxperf = NXPerfCounters();
}
