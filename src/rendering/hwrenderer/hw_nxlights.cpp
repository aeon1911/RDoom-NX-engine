//==========================================================================
//
// Dynamic light budget (Switch port)
//
// Every dynamic light (torches, candles, fire, fireballs, muzzle flashes...)
// costs CPU time on every wall, flat and sprite it touches: the light data is
// gathered and uploaded for each draw call, and the GL driver's per-draw work
// grows with it. In maps and mods with lots of fire this dominated the frame
// on the Switch's CPU.
//
// Once per frame only the lights nearest to the camera are kept (at most
// nx_dynlight_max of them, none further than nx_dynlight_dist map units from
// the camera). Lights entering or leaving the set fade in and out over a
// fraction of a second, and lights close to the distance limit are dimmed
// gradually, so there is no popping. Both limits 0 = vanilla behaviour.
//
//==========================================================================

#include "hw_nxlights.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "i_time.h"
#include "tarray.h"
#include <algorithm>
#include <cmath>
#include "nx_perf.h"
#include "nx_governor.h"

EXTERN_CVAR(Int, nx_perflog)

CVAR(Int, nx_dynlight_max, 0, CVAR_GLOBALCONFIG)		// 0 = no limit
CVAR(Float, nx_dynlight_dist, 0.f, CVAR_GLOBALCONFIG)	// 0 = no limit (map units from the camera to the light's edge)

int nxLightFrame = 0;
bool nxLightBudget = false;

static uint64_t nxLastTime = 0;

void NXLights_Select(FLevelLocals *Level, const DVector3 &viewpos)
{
	// Title, intermission and both loading screens use the pre-4.1 light path.
	if (!NXInPlay())
	{
		nxLightBudget = false;
		return;
	}

	// the frame rate stabilizer can tighten both limits while frames are late
	const int maxlights = NXGov_LightMax(nx_dynlight_max);
	const double maxdist = NXGov_LightDist(nx_dynlight_dist);

	if (maxlights <= 0 && maxdist <= 0)
	{
		nxLightBudget = false;
		if (nx_perflog > 0)
		{
			int n = 0;
			for (auto light = Level->lights; light; light = light->next) if (light->IsActive()) n++;
			nxperf.dynlights += n;
			nxperf.dynlightsall += n;
		}
		return;
	}
	nxLightBudget = true;
	nxLightFrame++;

	// fade speed follows real time so it looks the same at any frame rate
	uint64_t now = I_msTime();
	double dt = nxLastTime == 0 ? 33. : double(now - nxLastTime);
	nxLastTime = now;
	if (dt > 100.) dt = 100.;
	const float step = float(dt / 250.);	// ~1/4 second for a full fade

	struct Cand { double d; double dist; FDynamicLight *light; };
	static TArray<Cand> cands;
	cands.Clear();

	// a corrupt or cyclic light list must not hang the frame (intro maps of
	// heavy packs walk this list on the first rendered view)
	int seen = 0;
	for (auto light = Level->lights; light && seen < 20000; light = light->next, seen++)
	{
		if (!light->IsActive())
		{
			light->nxFade = 0.f;
			light->nxCut = false;
			continue;
		}
		double radius = light->GetRadius();
		if (radius <= 0) continue;
		// distance from the camera to the edge of the light's sphere (0 = inside it)
		double d = (light->Pos - viewpos).Length() - radius;
		if (!std::isfinite(d))
		{
			light->nxFade = 0.f;
			light->nxCut = false;
			continue;
		}
		if (d < 0) d = 0;
		if (maxdist > 0 && d >= maxdist)
		{
			// out of range: it was dimmed to 0 on its way out already
			light->nxFade = 0.f;
			light->nxCut = false;
			continue;
		}
		// lights that are lit already are preferred a little (no flickering at the edge of the set)
		double key = light->nxFade > 0.5f ? d * 0.8 : d;
		cands.Push({ key, d, light });
	}

	unsigned keep = cands.Size();
	if (maxlights > 0 && keep > (unsigned)maxlights)
	{
		std::nth_element(cands.begin(), cands.begin() + maxlights, cands.end(), [](const Cand &a, const Cand &b) { return a.d < b.d; });
		keep = maxlights;
	}
	for (unsigned i = 0; i < cands.Size(); i++)
	{
		FDynamicLight *light = cands[i].light;
		if (i < keep)
		{
			float t = 1.f;
			if (maxdist > 0)
			{
				// dim gradually over the last quarter of the allowed distance
				double fadelen = maxdist * 0.25;
				double d = cands[i].dist;
				if (d > maxdist - fadelen) t = float(std::clamp((maxdist - d) / fadelen, 0., 1.));
			}
			if (light->nxCut)
			{
				// it was left out by the light count before: fade back in
				light->nxFade = std::min(t, light->nxFade + step);
				if (light->nxFade >= t) light->nxCut = false;
			}
			else light->nxFade = t;	// new lights (muzzle flashes etc.) show up at once
		}
		else
		{
			// over the light count: fade out
			light->nxFade = std::max(0.f, light->nxFade - step);
			light->nxCut = true;
		}
		if (light->nxFade > 0.01f)
		{
			light->nxFrame = nxLightFrame;
			nxperf.dynlights++;
		}
	}
	nxperf.dynlightsall += cands.Size();
}
