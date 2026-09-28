#pragma once
// Lightweight frame profiler (Switch port): writes slow frames to perf.log
// with a breakdown of where the time went, so problems can be diagnosed
// from a log instead of guesses.
#include <stdint.h>
#include <stdio.h>

struct NXPerfCounters
{
	double tic_ms = 0, display_ms = 0, shader_ms = 0, tex_ms = 0, sound_ms = 0, sndupd_ms = 0, music_ms = 0;
	double swap_ms = 0;		// waiting for the GPU / display inside the buffer swap
	double scene_ms = 0;	// 3D view (part of display_ms)
	double bsp_ms = 0;		// building the draw lists: BSP walk + walls/flats/sprites (part of scene_ms)
	double hud_ms = 0;		// status bar / HUD (mods draw it with scripts; part of display_ms)
	double update_ms = 0;	// 2D flush + present + swap (part of display_ms)
	double gpuwait_ms = 0;	// waiting for the GPU to release a buffer (part of update_ms)
	int drawitems = 0, transitems = 0;	// walls/flats/sprites in the draw lists (translucent ones separately)
	double gc_ms = 0;		// garbage collector (script objects)
	double wait_ms = 0;		// waiting for the next game tic (FPS cap)
	double net_ms = 0;		// building/processing input commands
	double input_ms = 0;	// input events
	double think_ms = 0;	// actors and other thinkers (part of tic_ms)
	double sndstart_ms = 0;	// starting sounds in the sound system (mostly part of tic_ms)
	double client_ms = 0;	// client-side logic per tic: menus, UI event handlers, HUD tick, client-side thinkers
	double glsync_ms = 0;	// game thread waiting for the GL driver thread in the middle of a frame
	double glfinish_ms = 0;	// game thread waiting for the GL driver thread before the buffer swap
	int glsyncs = 0;
	int fxskipped = 0;		// effect actors skipped by nx_fx_thin
	int spriteskips = 0;	// sprite draws skipped because the picture wasn't on the GPU yet
	double present_ms = 0;	// copy of the finished frame to the window (waits for a free window buffer)
	double draw2d_ms = 0;	// 2D drawing (HUD, menus, messages) - part of update_ms
	double frameend_ms = 0;	// end of the previous frame done at the start of this one (GL driver thread: fence, swap, waits)
	int tics = 0, shaders = 0, textures = 0, sounds = 0, sndstarts = 0;
	int thinkers = 0;		// thinkers ticked (summed over the frame's tics)
	int tex_async = 0, tex_wait = 0, tex_budget = 0, snd_async = 0, tex_preupload = 0;
	int64_t texpixels = 0;
};

extern NXPerfCounters nxperf;
double NXPerf_NowMS();
void NXPerf_EndFrame();
// set by the game code: writes a breakdown (e.g. which actor classes cost the most) into the summary
extern void (*NXPerf_SummaryHook)(FILE *f);
// true for the one game tic per second whose thinkers get profiled by class
bool NXPerf_ProfileThisTic();

struct NXPerfScope
{
	double *acc;
	int *count;
	double start;
	NXPerfScope(double *a, int *c) : acc(a), count(c), start(NXPerf_NowMS()) {}
	~NXPerfScope() { *acc += NXPerf_NowMS() - start; if (count) (*count)++; }
};

#define NXPERF_SCOPE(msfield, countfield) NXPerfScope nxperf_scope_(&nxperf.msfield, countfield)
