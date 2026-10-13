#pragma once
// Switch port: frame rate stabilizer (see nx_governor.cpp)

// 0 = full detail. Each level trims a little more of the purely visual load
// (dynamic lights, distant effects, sprite shadows) while the frame rate is
// below its target.
extern int nxGovLevel;
constexpr int NXGOV_MAXLEVEL = 6;

// called once per frame by the frame profiler with the frame's timings
void NXGovernor_Frame(double frame_ms, double waits_ms, bool inlevel);

// effective limits for the current level combined with the user's settings (0 = no limit)
int NXGov_LightMax(int preset);
double NXGov_LightDist(double preset);
double NXGov_EffectCull(double preset);
double NXGov_MinAlpha(double preset);
double NXGov_ShadowDist(double preset);
// screen area (in whole screens) translucent effects may cover per frame, 0 = no limit
double NXGov_EffectArea(double preset);
