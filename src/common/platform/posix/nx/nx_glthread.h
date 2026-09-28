#pragma once
#include <stdio.h>
#include <stdint.h>

// Switch port: Mesa glthread (GL driver work on its own core), see nx_glthread.cpp
void NX_GLThreadStart();		// call with the GL context current, after init
void NX_GLThreadFinish();		// wait until the driver thread executed everything (before swap / EGL calls)
bool NX_GLThreadEnabled();
const char *NX_GLThreadStatus();	// for perf.log
void NX_GLCrumb(const char *what);	// last GL step (hang.log)
extern volatile uint32_t nx_heartbeat;	// bumped by every presented frame
void NX_GLThreadWriteStats(FILE *f);
