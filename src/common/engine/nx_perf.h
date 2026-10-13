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
	double player_ms = 0;	// P_PlayerThink: movement and weapon actions (part of tic_ms, not of think_ms)
	double spawn_ms = 0;	// spawning actors, outermost calls only (inside think_ms/player_ms)
	int spawns = 0;
	double begin_ms = 0;
	double begin_wait_ms = 0, begin_save_ms = 0, begin_buffers_ms = 0, begin_sprites_ms = 0;	// parts of begin_ms (Vulkan)
	double upload_max_ms = 0;	// slowest single texture upload this frame	// screen->BeginFrame: waits for frame N-2, cache saves, sprite uploads (part of display_ms)
	double sndcall_ms = 0;	// whole SoundEngine::StartSound calls (lookup, async load queueing, start)
	int sndcalls = 0;
	double sndstart_ms = 0;	// starting sounds in the sound system (mostly part of tic_ms)
	double client_ms = 0;	// client-side logic per tic: menus, UI event handlers, HUD tick, client-side thinkers
	double glsync_ms = 0;	// game thread waiting for the GL driver thread in the middle of a frame
	double glfinish_ms = 0;	// game thread waiting for the GL driver thread before the buffer swap
	int glsyncs = 0;
	int spriteskips = 0;	// sprite draws skipped because the picture wasn't on the GPU yet
	int effectsculled = 0;	// translucent effects left out by the stabilizer's screen area budget
	double present_ms = 0;	// copy of the finished frame to the window (waits for a free window buffer)
	double draw2d_ms = 0;	// 2D drawing (HUD, menus, messages) - part of update_ms
	double frameend_ms = 0;	// end of the previous frame done at the start of this one (GL driver thread: fence, swap, waits)
	// Vulkan frame end (parts of update_ms; acquire/present also go to present_ms, fence waits to gpuwait_ms)
	int vksubmits = 0;			// vkQueueSubmit calls
	double vksubmit_ms = 0;		// inside vkQueueSubmit
	double vkclean_ms = 0;		// dcache clean of persistent buffers before a submit
	double vkacquire_ms = 0;	// vkAcquireNextImageKHR
	double vkpresent_ms = 0;	// vkQueuePresentKHR
	double vkpending_ms = 0;	// waiting at the start of a frame for the previous frame's GPU work (frame overlap)
	// GPU time of completed frames (timestamps, read back a frame later)
	int gpuframes = 0;
	double gpu_frame_ms = 0, gpu_scene_ms = 0, gpu_post_ms = 0, gpu_end_ms = 0;	// whole frame = scene + post-processing + 2D/present
	float scene_scale = 1;	// dynamic resolution of the 3D view this frame
	int tics = 0, shaders = 0, textures = 0, sounds = 0, sndstarts = 0;
	int thinkers = 0;		// thinkers ticked (summed over the frame's tics)
	int tex_async = 0, tex_wait = 0, tex_budget = 0, snd_async = 0, tex_preupload = 0;
	int64_t texpixels = 0;
	int draws = 0;				// GL draw calls of the 3D scene and 2D
	int lightuploads = 0, lightskips = 0;	// dynamic light data uploaded / skipped because it was the same as the last one
	int dynlights = 0, dynlightsall = 0;	// dynamic lights allowed this frame / active in the level
};

extern NXPerfCounters nxperf;
double NXPerf_NowMS();
void NXPerf_EndFrame();
void NXPerf_FlushFile();	// background writer (Switch)
// a "#   note:" line in perf.log (rare events worth naming, e.g. a slow script action)
void NXPerf_Note(const char *fmt, ...);
// last completed frame, the simulation time inside it, and the average cost of one tic (0 if none ran)
double NXPerf_LastFrameMS();
double NXPerf_LastSimMS();
double NXPerf_LastTicWaitMS();	// the last frame's wait for the next game tic
double NXPerf_LastTicMS();
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

// True only while a map is being played. The title screen and both loading
// screens stay on the pre-4.1 render path; the light budget and CPU mipmaps
// turn on after the player is actually in a level.
bool NXInPlay();
