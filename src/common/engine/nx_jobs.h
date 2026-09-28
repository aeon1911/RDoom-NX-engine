#pragma once
// Small worker pool for background asset preparation (Switch port):
// decoding sprites/HUD graphics and sounds off the main thread so that the
// game doesn't stall every time something new appears or makes a noise.
#include <functional>

// priorities: something is waiting for it right now / likely needed soon / prefetching
enum { NXJOB_LOW = 0, NXJOB_HIGH = 1, NXJOB_NOW = 2 };
void NXJobs_Push(std::function<void()> job, int priority);
// waits until every queued and running job has finished
void NXJobs_WaitAll();
// stops the worker threads (also done automatically at exit)
void NXJobs_Shutdown();
bool NXJobs_Enabled();
