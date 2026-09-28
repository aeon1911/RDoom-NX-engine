/*
** i_nxsystem.cpp
** Switch-specific system code.
**
**---------------------------------------------------------------------------
** Copyright 2023 fgsfds
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdio.h>
#include <unistd.h>
#include <switch.h>
#include "nx_loadscreen.h"

#ifdef _DEBUG
static int nxlink_sock = -1;
#endif

extern "C" void userAppInit(void)
{
	socketInitializeDefault();
#ifdef _DEBUG
	nxlink_sock = nxlinkStdio();
#endif
}

extern "C" void userAppExit(void)
{
#ifdef _DEBUG
	if (nxlink_sock >= 0)
		close(nxlink_sock);
#endif
	socketExit();
}

bool I_OnScreenKeyboard(const char *hint, char *out, int outlen)
{
	if (!out || !outlen) return false;

	SwkbdConfig kbd;
	Result rc = swkbdCreate(&kbd, 0);
	if (R_FAILED(rc)) return false;

	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetHeaderText(&kbd, hint);
	swkbdConfigSetGuideText(&kbd, hint);
	swkbdConfigSetInitialText(&kbd, out);

	rc = swkbdShow(&kbd, out, outlen-1);
	swkbdClose(&kbd);

	return R_SUCCEEDED(rc);
}

// Shows a fatal error on screen (the SDL window is already gone at this point),
// so the user doesn't just get dumped back to the Homebrew Menu without a clue.
void I_NXShowFatalError(const char *title, const char *message)
{
	// the loading screen owns the framebuffer until video is up
	NXLS_Stop();
	NXLS_SetBoost(false);
	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);

	printf("\x1b[31;1m%s\x1b[0m\n\n", title);
	printf("%s\n\n", message);
	printf("\x1b[33mThis message was also saved to error.log in the gzdoom folder.\x1b[0m\n\n");
	printf("Press + to exit.\n");
	consoleUpdate(NULL);

	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_Minus))
			break;
		consoleUpdate(NULL);
	}

	consoleExit(NULL);
}

//==========================================================================
//
// Crash diagnostics and big main stack (Switch)
//
// The main thread that hbloader gives us only has a 1 MB stack, which is
// not a lot for GZDoom/UZDoom with big mods, so the engine runs in its own
// thread with a 16 MB stack. CPU exceptions and std::terminate are logged to
// crash.log (next to the .nro) with addresses relative to the module base,
// so they can be symbolized against the unstripped .elf.
//
//==========================================================================

#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <exception>
#include <typeinfo>

extern "C" {
	extern char __start__;
	extern char __rodata_start;
	// libnx's default exception stack is only 1 KB, not enough to write a log
	alignas(16) u8 __nx_exception_stack[0x10000];
	u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

static char crashbuf[16384];
static size_t crashlen;

static void CrashPrintf(const char *fmt, ...)
{
	if (crashlen >= sizeof(crashbuf) - 1) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(crashbuf + crashlen, sizeof(crashbuf) - crashlen, fmt, ap);
	va_end(ap);
	if (n > 0) crashlen += (size_t)n;
	if (crashlen > sizeof(crashbuf) - 1) crashlen = sizeof(crashbuf) - 1;
}

static void CrashFlush()
{
	int fd = open("crash.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
	{
		write(fd, crashbuf, crashlen);
		close(fd);
	}
}

// the code segment of the .nro (taken from the memory map: the linker
// symbols can't be used for this in a relocated homebrew)
static u64 modBase, modEnd;
static void FindModuleRange()
{
	MemoryInfo mi;
	u32 pi;
	if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&CrashFlush)))
	{
		modBase = mi.addr;
		modEnd = mi.addr + mi.size;
	}
}
static inline u64 ModBase() { if (!modBase) FindModuleRange(); return modBase; }
static inline bool InText(u64 a) { return ModBase() != 0 && a >= modBase && a < modEnd; }

static void CrashStackScan(u64 sp)
{
	CrashPrintf("\nstack scan (return addresses inside the module):\n");
	u64 *p = (u64 *)(uintptr_t)(sp & ~7ULL);
	int found = 0;
	for (int i = 0; i < 4096 && found < 48; i++)
	{
		u64 v = p[i];
		if (InText(v))
		{
			CrashPrintf("  [sp+0x%04x] +0x%lx\n", i * 8, (unsigned long)(v - ModBase()));
			found++;
		}
	}
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
	crashlen = 0;
	CrashPrintf("UZDoom Switch crash report\nCPU exception, error_desc=0x%x esr=0x%x far=0x%lx\n",
		ctx->error_desc, ctx->esr, (unsigned long)ctx->far.x);
	CrashPrintf("module base: 0x%lx\n", (unsigned long)ModBase());
	CrashPrintf("pc: 0x%lx (+0x%lx)\nlr: 0x%lx (+0x%lx)\nsp: 0x%lx fp: 0x%lx\n",
		(unsigned long)ctx->pc.x, (unsigned long)(ctx->pc.x - ModBase()),
		(unsigned long)ctx->lr.x, (unsigned long)(ctx->lr.x - ModBase()),
		(unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
	for (int i = 0; i < 29; i++)
		CrashPrintf("x%-2d: 0x%016lx%s", i, (unsigned long)ctx->cpu_gprs[i].x, (i % 2) ? "\n" : "  ");
	CrashPrintf("\n");
	// frame pointer chain
	CrashPrintf("\nframe chain:\n");
	u64 fp = ctx->fp.x;
	for (int i = 0; i < 64 && fp && !(fp & 7); i++)
	{
		u64 *frame = (u64 *)(uintptr_t)fp;
		u64 ret = frame[1];
		if (!InText(ret)) break;
		CrashPrintf("  #%02d +0x%lx\n", i, (unsigned long)(ret - ModBase()));
		if (frame[0] <= fp) break;
		fp = frame[0];
	}
	CrashStackScan(ctx->sp.x);
	CrashFlush();
	NXLS_SetBoost(false);	// don't leave the CPU boost on for the Homebrew Menu
	svcReturnFromException(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

static void I_NXTerminate()
{
	crashlen = 0;
	CrashPrintf("UZDoom Switch crash report\nstd::terminate called\n");
	CrashPrintf("module base: 0x%lx\n", (unsigned long)ModBase());
	if (auto eptr = std::current_exception())
	{
		try { std::rethrow_exception(eptr); }
		catch (const std::exception &e) { CrashPrintf("uncaught exception (%s): %s\n", typeid(e).name(), e.what()); }
		catch (...) { CrashPrintf("uncaught unknown exception\n"); }
	}
	u64 sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	CrashStackScan(sp);
	CrashFlush();
	NXLS_SetBoost(false);
	abort();
}

struct NXMainArgs
{
	int (*fn)(int, char **);
	int argc;
	char **argv;
	int ret;
};

// Background workers (asset decoding) run on cores 1 and 2 with a lower
// priority than everything else, the engine's main thread keeps core 0.
void I_NXSetupWorkerThread(int core)
{
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ULL << core);
	// the lowest priority an application may use; the game's thread is 0x2C
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
}

void I_NXWorkerYield()
{
	svcSleepThread(YieldType_ToAnyThread);
}

// GZDoom's render worker (gl_multithread): libnx starts threads on core 0
// with the lowest priority, where it could only run while the game's own
// thread waits. Give it core 1 and the same priority as the game.
void I_NXSetupRenderThread()
{
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1ULL << 1);
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
}

static void *NXMainThread(void *p)
{
	NXMainArgs *a = (NXMainArgs *)p;
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1ULL << 0);
	// libnx starts every thread with the lowest priority (0x3B); the game gets
	// the normal main-thread priority so audio/music/loader threads that land
	// on core 0 can never hold it up when it wakes from waiting on the GPU
	svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
	std::set_terminate(I_NXTerminate);
	a->ret = a->fn(a->argc, a->argv);
	return nullptr;
}

static void NXAtExit()
{
	NXLS_Stop();
	NXLS_SetBoost(false);
}

int I_NXRunWithBigStack(int (*fn)(int, char **), int argc, char **argv)
{
	std::set_terminate(I_NXTerminate);
	FindModuleRange();

	// switches that are only meant for this port and must not reach the engine
	bool boost = false, loadscreen = true;
	int n = 0;
	for (int i = 0; i < argc; i++)
	{
		// CPU boost was removed on purpose: the port runs at the console's stock clocks only
		if (i > 0 && !strcmp(argv[i], "-nxboost")) continue;
		if (i > 0 && !strcmp(argv[i], "-nxnoload")) { loadscreen = false; continue; }
		argv[n++] = argv[i];
	}
	argc = n;
	argv[argc] = nullptr;

	atexit(NXAtExit);
	if (boost) NXLS_SetBoost(true);
	if (loadscreen) NXLS_Start(argc, argv);

	NXMainArgs args = { fn, argc, argv, 0 };
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
	pthread_t thread;
	if (pthread_create(&thread, &attr, NXMainThread, &args) != 0)
	{
		pthread_attr_destroy(&attr);
		return fn(argc, argv); // couldn't get a bigger stack, run as usual
	}
	pthread_attr_destroy(&attr);
	pthread_join(thread, nullptr);
	NXAtExit();
	return args.ret;
}
