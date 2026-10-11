#pragma once
#include <stdio.h>
#include <stdint.h>

// Switch port: Mesa glthread (GL driver work on its own core), see nx_glthread.cpp
void NX_GLThreadStart();		// call with the GL context current, after init
void NX_GLThreadFinish();		// wait until the driver thread executed everything (before swap / EGL calls)
bool NX_GLThreadEnabled();
const char *NX_GLThreadStatus();	// for perf.log
void NX_GLCrumb(const char *what);	// last step, kept for hang.log
void NX_Phase(const char *what);		// same, and one line appended to phase.log
void NX_StartHangWatch();		// arm the hang report from the first presented frame
extern volatile uint32_t nx_heartbeat;	// bumped by every presented frame
void NX_GLThreadWriteStats(FILE *f);
// Call once the loading-screen thread has exited. SDL audio threads created
// before that stay off its core; afterwards they run on core 2.
void NX_AudioAfterLoadScreen();
