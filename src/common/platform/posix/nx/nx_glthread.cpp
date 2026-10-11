// Switch port: run the GL driver on its own CPU core.
//
// Homebrew renders through Mesa (nouveau). Most of the CPU cost of drawing is
// spent inside the driver, on the game's main thread. Mesa has a built-in
// "glthread" mode that turns every GL call into a small command that is
// executed by a separate driver thread, so the game thread only records
// commands while the driver work runs on another core.
//
// RDoom: with the Mesa 26.2.4 SDK this file no longer starts glthread itself
// (see NX_GLThreadStart); it keeps the audio thread placement, the sync
// statistics and the hang reporter. Original notes for Mesa 20.1:
// The Switch build of Mesa (egl_switch, Mesa 20.1) ships the glthread code but
// never switches it on, so it is started here by hand once the GL context
// exists. The structure offsets below come from the disassembly of that exact
// library build; they are checked at runtime and glthread stays off if
// anything does not match.

#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <utility>
#include "c_cvars.h"
#include "printf.h"
#include "nx_perf.h"
#include "nx_glthread.h"

CVAR(Bool, nx_glthread, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nx_glthread_core, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

extern "C"
{
	void __real__mesa_glthread_finish_before(void *ctx, const char *func);
	int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
	Result __real_nwindowDequeueBuffer(NWindow *nw, s32 *out_slot, NvMultiFence *out_fence);
	Result __real_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence);
	void __real__util_queue_fence_wait(void *fence);
}

// struct gl_context (Mesa 20.1, devkitPro switch build)
static const size_t CTX_MARSHAL_EXEC = 56;
static const size_t CTX_CLIENT_DISPATCH = 64;
static const size_t CTX_GLTHREAD_ENABLED = 80 + 168;
static const size_t CTX_ST = 0x20000 + 11896;
// struct st_manager: screen, get_egl_image, get_param, set_background_context
static const size_t SMAPI_SET_BACKGROUND_CONTEXT = 24;

static void *gCtx;
static bool gEnabled;
static volatile int gCreatingThread;
static int gThreadCreateResult = -1;
static volatile bool gThreadRan = false;
static Handle gMainHandle, gWorkerHandle;

template<class T> static T &At(void *base, size_t off) { return *(T *)((char *)base + off); }

// The driver thread is created with the default (small) pthread stack; Mesa
// compiles shaders on it, so give it a large one.
//
// SDL's audio thread also runs the OpenAL mixer. libnx starts it on core 0 at
// the lowest priority, next to the game thread (core 0, priority 0x2C). The
// mixer can then run only while the game waits, and every time the game needs
// the lock the mixer is holding the frame stalls. Brutal Doom hits that on
// almost every tic. Core 2 is the fix, but the loading screen draws on core 2
// until NXLS_Stop joins it: moving the mixer there any earlier starves that
// screen and Jogar stays black. Threads created during the load stay on core 0
// and are moved once the screen thread is gone. Threads created later go
// straight to core 2.

extern "C" int SDL_SYS_CreateThread(void *thread);

struct NXThreadStart { void *(*fn)(void *); void *arg; };
static Mutex gAudioLock;
static Handle gAudioHandles[16];
static int gAudioCount;
static bool gAudioCoreOpen;

static void PinAudioHandle(Handle h)
{
	if (!h) return;
	svcSetThreadCoreMask(h, 2, 1u << 2);
	svcSetThreadPriority(h, 0x2D);
}

static void *NXSDLThreadEntry(void *p)
{
	NXThreadStart t = *(NXThreadStart *)p;
	free(p);
	Handle h = threadGetCurHandle();
	bool pin = false;
	mutexLock(&gAudioLock);
	if (gAudioCoreOpen) pin = true;
	else if (gAudioCount < 16) gAudioHandles[gAudioCount++] = h;
	mutexUnlock(&gAudioLock);
	if (pin) PinAudioHandle(h);
	return t.fn(t.arg);
}

void NX_AudioAfterLoadScreen()
{
	if (gAudioCoreOpen) return;
	Handle local[16];
	int n = 0;
	mutexLock(&gAudioLock);
	gAudioCoreOpen = true;
	n = gAudioCount;
	if (n > 16) n = 16;
	for (int i = 0; i < n; i++) local[i] = gAudioHandles[i];
	gAudioCount = 0;
	mutexUnlock(&gAudioLock);
	for (int i = 0; i < n; i++) PinAudioHandle(local[i]);
}

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
	uintptr_t ra = (uintptr_t)__builtin_return_address(0);
	uintptr_t sdl = (uintptr_t)&SDL_SYS_CreateThread;
	if (ra >= sdl && ra < sdl + 0x100)
	{
		auto t = (NXThreadStart *)malloc(sizeof(NXThreadStart));
		if (t)
		{
			t->fn = fn;
			t->arg = arg;
			int r = __real_pthread_create(thread, attr, NXSDLThreadEntry, t);
			if (r != 0) free(t);
			return r;
		}
	}
	if (gCreatingThread && attr == nullptr)
	{
		pthread_attr_t big;
		pthread_attr_init(&big);
		pthread_attr_setstacksize(&big, 2 * 1024 * 1024);
		int r = __real_pthread_create(thread, &big, fn, arg);
		pthread_attr_destroy(&big);
		gThreadCreateResult = r;
		return r;
	}
	return __real_pthread_create(thread, attr, fn, arg);
}

// Called by Mesa on the new driver thread before it executes anything.
static void NXGLThreadStarted(void *stctxi, void *queue_info)
{
	gThreadRan = true;
	gWorkerHandle = threadGetCurHandle();
	int core = nx_glthread_core;
	if (core < 0 || core > 2) core = 1;
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ULL << core);
	// just below the game thread and the render worker (0x2C), so the BSP
	// worker that shares the core always gets it first
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2D);
}

// ---- sync statistics: every time the game thread has to wait for the driver thread

struct SyncStat { const char *name; int count; double ms; };
static SyncStat syncStats[48];
static int numSyncStats;

static void CountSync(const char *name, double ms)
{
	nxperf.glsyncs++;
	nxperf.glsync_ms += ms;
	for (int i = 0; i < numSyncStats; i++)
	{
		if (syncStats[i].name == name) { syncStats[i].count++; syncStats[i].ms += ms; return; }
	}
	if (numSyncStats < 48) syncStats[numSyncStats++] = { name, 1, ms };
}

extern "C" void __wrap__mesa_glthread_finish_before(void *ctx, const char *func)
{
	double start = NXPerf_NowMS();
	NX_GLCrumb(func ? func : "sync");
	__real__mesa_glthread_finish_before(ctx, func);
	CountSync(func ? func : "?", NXPerf_NowMS() - start);
}

void NX_GLThreadWriteStats(FILE *f)
{
	if (!gEnabled) return;
	if (numSyncStats == 0) { fprintf(f, "#   gl thread: no mid-frame syncs\n"); return; }
	// sort by time
	for (int i = 1; i < numSyncStats; i++)
		for (int j = i; j > 0 && syncStats[j].ms > syncStats[j - 1].ms; j--)
			std::swap(syncStats[j], syncStats[j - 1]);
	fprintf(f, "#   gl thread syncs (count, ms waited):");
	for (int i = 0; i < numSyncStats && i < 10; i++)
		fprintf(f, " %s %d/%.1f", syncStats[i].name, syncStats[i].count, syncStats[i].ms);
	fprintf(f, "\n");
	numSyncStats = 0;
}

bool NX_GLThreadEnabled()
{
	return gEnabled;
}

void NX_GLThreadFinish()
{
	if (!gEnabled) return;
	double start = NXPerf_NowMS();
	NX_GLCrumb("glthread finish (end of frame)");
	(void)gCtx;
	NX_GLCrumb("glthread finish done");
	nxperf.glfinish_ms += NXPerf_NowMS() - start;
}

static char gStatus[256] = "not started";

//==========================================================================
//
// Hang reporter: while the GL driver thread is on, a watchdog thread checks
// that the game keeps presenting frames. If nothing happens for a long time
// it writes hang.log (next to the .nro) with what the game thread and the
// driver thread are doing: their registers and the return addresses on their
// stacks (relative to the module, for the debug .elf) plus the last GL step
// each one reached.
//
//==========================================================================

volatile uint32_t nx_heartbeat;
static volatile const char *gCrumbMain = "none";
static volatile const char *gCrumbWorker = "none";
static volatile uint64_t gCrumbMainTime, gCrumbWorkerTime;
static bool gWatchdogStarted;

void NX_GLCrumb(const char *what)
{
	if (gEnabled && threadGetCurHandle() == gWorkerHandle) { gCrumbWorker = what; gCrumbWorkerTime = armGetSystemTick(); }
	else { gCrumbMain = what ? what : "?"; gCrumbMainTime = armGetSystemTick(); }
}

void NX_Phase(const char *what)
{
	NX_GLCrumb(what);
	static const char *last = nullptr;
	if (what == last) return;
	last = what;
	FILE *f = fopen("phase.log", "a");
	if (!f) return;
	fprintf(f, "%llu %s\n", (unsigned long long)gCrumbMainTime, what ? what : "?");
	fclose(f);
}

extern "C" Result __wrap_nwindowDequeueBuffer(NWindow *nw, s32 *out_slot, NvMultiFence *out_fence)
{
	NX_GLCrumb("nwindowDequeueBuffer (waiting for a free screen buffer)");
	Result r = __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
	NX_GLCrumb("nwindowDequeueBuffer done");
	return r;
}

extern "C" Result __wrap_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence)
{
	NX_GLCrumb("nwindowQueueBuffer");
	Result r = __real_nwindowQueueBuffer(nw, slot, fence);
	NX_GLCrumb("nwindowQueueBuffer done");
	return r;
}

extern "C" void __wrap__util_queue_fence_wait(void *fence)
{
	NX_GLCrumb("util_queue_fence_wait (waiting for the driver thread)");
	__real__util_queue_fence_wait(fence);
	NX_GLCrumb("util_queue_fence_wait done");
}

static char hangbuf[24576];
static size_t hanglen;
static void HangPrintf(const char *fmt, ...)
{
	if (hanglen >= sizeof(hangbuf) - 1) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(hangbuf + hanglen, sizeof(hangbuf) - hanglen, fmt, ap);
	va_end(ap);
	if (n > 0) hanglen += (size_t)n;
	if (hanglen > sizeof(hangbuf) - 1) hanglen = sizeof(hangbuf) - 1;
}

static u64 hModBase, hModEnd;
static bool HInText(u64 a) { return a >= hModBase && a < hModEnd; }

static bool StackReadable(u64 addr)
{
	MemoryInfo mi;
	u32 pi;
	if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return false;
	return (mi.perm & Perm_R) && mi.type != MemType_Unmapped;
}

static void DumpThread(const char *name, Handle h)
{
	HangPrintf("\n== %s thread (handle 0x%x)\n", name, h);
	if (h == 0) { HangPrintf("  unknown handle\n"); return; }
	Result rc = svcSetThreadActivity(h, ThreadActivity_Paused);
	if (R_FAILED(rc)) { HangPrintf("  pause failed 0x%x\n", rc); return; }
	ThreadContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	rc = svcGetThreadContext3(&ctx, h);
	if (R_FAILED(rc))
	{
		HangPrintf("  get context failed 0x%x\n", rc);
		svcSetThreadActivity(h, ThreadActivity_Runnable);
		return;
	}
	HangPrintf("  pc 0x%lx (+0x%lx)  lr 0x%lx (+0x%lx)  sp 0x%lx  fp 0x%lx\n",
		(unsigned long)ctx.pc.x, (unsigned long)(HInText(ctx.pc.x) ? ctx.pc.x - hModBase : 0),
		(unsigned long)ctx.lr, (unsigned long)(HInText(ctx.lr) ? ctx.lr - hModBase : 0),
		(unsigned long)ctx.sp, (unsigned long)ctx.fp);
	for (int i = 0; i < 29; i++)
		HangPrintf("  x%-2d 0x%016lx%s", i, (unsigned long)ctx.cpu_gprs[i].x, (i % 3 == 2) ? "\n" : "");
	HangPrintf("\n  frame chain:\n");
	u64 fp = ctx.fp;
	for (int i = 0; i < 48 && fp && !(fp & 7) && StackReadable(fp); i++)
	{
		u64 *frame = (u64 *)(uintptr_t)fp;
		u64 ret = frame[1];
		if (!HInText(ret)) break;
		HangPrintf("    #%02d +0x%lx\n", i, (unsigned long)(ret - hModBase));
		if (frame[0] <= fp) break;
		fp = frame[0];
	}
	HangPrintf("  stack scan:\n");
	u64 sp = ctx.sp & ~7ULL;
	int found = 0;
	for (int i = 0; i < 8192 && found < 64; i++)
	{
		u64 a = sp + i * 8;
		if ((a & 0xfff) == 0 && !StackReadable(a)) break;
		u64 v = *(u64 *)(uintptr_t)a;
		if (HInText(v)) { HangPrintf("    [sp+0x%04x] +0x%lx\n", i * 8, (unsigned long)(v - hModBase)); found++; }
	}
	svcSetThreadActivity(h, ThreadActivity_Runnable);
}

static void WriteHangLog(double seconds)
{
	hanglen = 0;
	uint64_t now = armGetSystemTick();
	HangPrintf("UZDoom Switch hang report (GL driver thread on)\nno frame for %.0f s\nmodule base 0x%lx\n", seconds, (unsigned long)hModBase);
	HangPrintf("status: %s\n", gStatus);
	HangPrintf("driver thread ran: %d, pthread_create %d\n", (int)gThreadRan, gThreadCreateResult);
	HangPrintf("game thread last step: %s (%.1f s ago)\n", (const char *)gCrumbMain, armTicksToNs(now - gCrumbMainTime) / 1e9);
	HangPrintf("driver thread last step: %s (%.1f s ago)\n", (const char *)gCrumbWorker, gCrumbWorkerTime ? armTicksToNs(now - gCrumbWorkerTime) / 1e9 : -1.0);
	DumpThread("game", gMainHandle);
	DumpThread("driver", gWorkerHandle);
	FILE *f = fopen("hang.log", "w");
	if (f)
	{
		fwrite(hangbuf, 1, hanglen, f);
		fclose(f);
	}
}

static void *WatchdogThread(void *)
{
	// lowest priority on core 2: never competes with the game
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1ULL << 2);
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
	uint32_t last = nx_heartbeat;
	int still = 0;
	int reported = 0;
	bool seenFrame = false;
	for (;;)
	{
		svcSleepThread(1000000000ULL);
		uint32_t hb = nx_heartbeat;
		if (hb != last)
		{
			last = hb;
			still = 0;
			reported = 0;
			seenFrame = true;
			continue;
		}
		// The homebrew loader and the engine mod screen present nothing through
		// this counter. Only time the gap after a real frame has been shown,
		// which is the opening picture that stops taking buttons.
		if (!seenFrame) continue;
		still++;
		if (still >= 8 && (reported == 0 || still - reported >= 30))
		{
			WriteHangLog(still);
			reported = still;
		}
	}
	return nullptr;
}

static void StartWatchdog();

void NX_StartHangWatch()
{
	if (!gMainHandle) gMainHandle = threadGetCurHandle();
	StartWatchdog();
}

static void StartWatchdog()
{
	if (gWatchdogStarted) return;
	gWatchdogStarted = true;
	MemoryInfo mi;
	u32 pi;
	if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&StartWatchdog)))
	{
		hModBase = mi.addr;
		hModEnd = mi.addr + mi.size;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_t t;
	pthread_create(&t, &attr, WatchdogThread, nullptr);
	pthread_attr_destroy(&attr);
}

static int gAttempts = 0;

const char *NX_GLThreadStatus()
{
	return gStatus;
}

void NX_GLThreadStart()
{
	// RDoom links the Mesa 26.2.4 Horizon SDK, whose glthread is switched with
	// MESA_SWITCH_GLTHREAD before the EGL context exists (see sdlglvideo.cpp).
	// The Mesa 20.1 structure-offset method used by the earlier port must not
	// touch Mesa 26 internals, so nothing is patched here any more.
	snprintf(gStatus, sizeof(gStatus), "Mesa 26 glthread %s (MESA_SWITCH_GLTHREAD)", nx_glthread ? "requested" : "off");
}
