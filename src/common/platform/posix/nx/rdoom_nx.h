#pragma once
// RDoom start-up flow on Horizon (rdoom_nx.cpp).

#define RDOOM_SD_ROOT "sdmc:/switch/rdoom"

// Fixes the working directory, creates the SD layout and opens the log.
void RDoom_NXEarlyInit();
// Shows the launcher unless engine arguments were given. Returns 0 if the
// user chose to exit, 1 to start the engine with outArgc/outArgv.
int RDoom_NXRunLauncher(int argc, char **argv, int &outArgc, char **&outArgv);
// True if the launcher drew on the default NWindow (its format must be reset
// before a Vulkan swapchain or EGL surface is created on it).
bool RDoom_NXLauncherUsedDisplay();
void RDoom_NXAtExit();

// "key: value" lines in logs/engine-session.txt (renderer, device, errors).
void RDoom_SessionSet(const char *key, const char *value);
void RDoom_SessionLog(const char *category, const char *message);
// Appends one line to logs/boot.log with an unbuffered write (start-up steps).
void RDoom_Crumb(const char *what);
