//==========================================================================
//
// Frame rate stabilizer (Switch port)
//
// The Switch's CPU runs out of time in scenes with lots of fire, torches,
// effects or monsters: the game logic and the GL driver's per-draw work
// grow with the number of lights and sprites on screen. Instead of letting
// the frame rate drop, this watches the frame times and, while frames miss
// the target (nx_stabilizer = target FPS, 0 = off), trims the purely visual
// load step by step:
//   - fewer dynamic lights (the nearest ones stay, the others fade out),
//   - effects/decorations far away are not drawn (monsters, projectiles,
//     pickups and everything that can be shot are always drawn),
//   - almost invisible effects are not drawn,
//   - translucent effects (smoke) get a screen area budget: they are drawn
//     from near to far, so what goes is the smoke behind other smoke,
//   - sprite shadows reach less far.
// Nothing that affects the game (actors, collisions, damage, scripts) is
// changed. When there is time to spare again, detail comes back slowly.
//
//==========================================================================

#include "nx_governor.h"
#include "c_cvars.h"
#include "printf.h"
#include <algorithm>
#include <math.h>

// target FPS, 0 = off. On by default on the Switch: Brutal Doom's smoke made
// half the frames of a fight late on the Switch Lite even at 60% resolution
// (dozens of translucent layers); only purely visual detail is trimmed and
// only while frames are late.
#ifdef __SWITCH__
CVAR(Int, nx_stabilizer, 60, CVAR_GLOBALCONFIG)
#else
CVAR(Int, nx_stabilizer, 0, CVAR_GLOBALCONFIG)
#endif
CVAR(Bool, nx_stabilizer_debug, false, 0)

int nxGovLevel = 0;

static double hist[8];
static int nhist = 0, histpos = 0;
static int hold = 0;			// frames before the next change
static int good = 0;			// frames in a row with plenty of time to spare
// good frames needed before detail comes back (grows when it had to go down
// again right away). 1 s: a fight with an occasional late frame every ~85
// frames never reached 105 and kept the top level for minutes.
static const int baseLowerDelay = 60;
static int lowerDelay = baseLowerDelay;
static int sinceRaise = 0;
static int sinceLower = 1000;

// Order of the trims: what costs the GPU most for the least visible change
// first (faint/hidden smoke), the lights only at the top levels (on the
// Switch Lite the stabilizer sat at level 5-6 for whole fights, lights cut
// to 2-3, while the bottleneck was the smoke's blending).
static const int	lightMaxT[NXGOV_MAXLEVEL + 1] = { 0, 0, 0, 24, 16, 10, 6 };
static const double	lightDistT[NXGOV_MAXLEVEL + 1] = { 0, 0, 0, 2400, 1800, 1400, 1000 };
static const double	cullT[NXGOV_MAXLEVEL + 1] = { 0, 4000, 3000, 2200, 1600, 1200, 900 };
static const double	minAlphaT[NXGOV_MAXLEVEL + 1] = { 0, 0.03, 0.05, 0.07, 0.09, 0.12, 0.15 };
static const double	shadowT[NXGOV_MAXLEVEL + 1] = { 1, 1, 1, 0.75, 0.5, 0.35, 0.2 };
// translucent effects (smoke) on screen, in screen areas: the GPU blends every
// layer, and a fight in Brutal Doom stacks dozens of them
static const double	effectAreaT[NXGOV_MAXLEVEL + 1] = { 0, 10, 7, 5, 4, 3, 2 };

// "0 = no limit" values: the stricter of both
template<class T> static T Tighter(T preset, T table)
{
	if (preset <= 0) return table;
	if (table <= 0) return preset;
	return std::min(preset, table);
}

int NXGov_LightMax(int preset) { return Tighter(preset, lightMaxT[nxGovLevel]); }
double NXGov_LightDist(double preset) { return Tighter(preset, lightDistT[nxGovLevel]); }
double NXGov_EffectCull(double preset) { return Tighter(preset, cullT[nxGovLevel]); }
double NXGov_MinAlpha(double preset) { return std::max(preset, minAlphaT[nxGovLevel]); }
double NXGov_ShadowDist(double preset) { return preset * shadowT[nxGovLevel]; }
double NXGov_EffectArea(double preset) { return Tighter(preset, effectAreaT[nxGovLevel]); }

static void SetLevel(int level, const char *why, double value)
{
	if (level == nxGovLevel) return;
	if (nx_stabilizer_debug) Printf("stabilizer: level %d -> %d (%s %.1f ms)\n", nxGovLevel, level, why, value);
	nxGovLevel = level;
}

void NXGovernor_Frame(double frame_ms, double waits_ms, bool inlevel)
{
	const int fps = nx_stabilizer;
	if (fps <= 0)
	{
		nxGovLevel = 0;
		nhist = 0;
		return;
	}
	if (!inlevel || frame_ms <= 0 || frame_ms > 1000)
	{
		// menus, loading, wipes: no decisions, start over afterwards
		nhist = 0;
		good = 0;
		hold = std::max(hold, 10);
		return;
	}
	const double target = 1000.0 / fps;

	hist[histpos] = frame_ms;
	histpos = (histpos + 1) & 7;
	if (nhist < 8) nhist++;
	if (hold > 0) hold--;
	sinceRaise++;
	sinceLower++;
	if (sinceRaise > 900) lowerDelay = baseLowerDelay;	// calm for ~15 s: back to the normal pace

	if (nhist >= 6 && hold == 0 && nxGovLevel < NXGOV_MAXLEVEL)
	{
		// sustained: the median of the last 6 frames misses the target
		double last6[6];
		for (int i = 0; i < 6; i++) last6[i] = hist[(histpos + 7 - i) & 7];
		std::nth_element(last6, last6 + 3, last6 + 6);
		double median = last6[3];
		// or at least 2 of the last 4 frames were clearly late
		int late = 0;
		for (int i = 0; i < 4; i++) if (hist[(histpos + 7 - i) & 7] > target * 1.35) late++;

		if (median > target * 1.10 || late >= 2)
		{
			// detail came back only a moment ago and it's too much again: wait longer next time
			if (sinceLower < 60) lowerDelay = std::min(lowerDelay * 2, 300);
			SetLevel(nxGovLevel + 1, late >= 2 ? "late frames" : "median", median);
			hold = 6;	// give the change a few frames to show
			good = 0;
			sinceRaise = 0;
			return;
		}
	}

	// plenty of time to spare (the frame was on time and less than ~2/3 of it was actual work)
	double work = frame_ms - waits_ms;
	if (frame_ms <= target * 1.03 && work <= target * 0.62) good++;
	else good = 0;

	if (good >= lowerDelay && nxGovLevel > 0 && hold == 0)
	{
		SetLevel(nxGovLevel - 1, "spare time", work);
		good = 0;
		sinceLower = 0;
		hold = 30;
	}
}
