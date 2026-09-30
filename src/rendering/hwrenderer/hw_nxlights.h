#pragma once
// Switch port: dynamic light budget (see hw_nxlights.cpp)

#include "a_dynlight.h"

extern int nxLightFrame;		// frame stamp of the current selection
extern bool nxLightBudget;		// the budget is active this frame

// true = the light may be used this frame
inline bool NXLightAllowed(const FDynamicLight *light)
{
	return !nxLightBudget || light->nxFrame == nxLightFrame;
}

// brightness multiplier of an allowed light (fades in/out at the budget's edges)
inline float NXLightFade(const FDynamicLight *light)
{
	return nxLightBudget ? light->nxFade : 1.f;
}

struct FLevelLocals;
void NXLights_Select(FLevelLocals *Level, const DVector3 &viewpos);
