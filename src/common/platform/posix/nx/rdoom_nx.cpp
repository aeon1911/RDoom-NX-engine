/*
** rdoom_nx.cpp
** RDoom on Horizon: the launcher's libnx platform (framebuffer, HID,
** software keyboard), start-up/exit flow and the engine session report.
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <string>
#include <vector>
#include <switch.h>

#include "app.h"
#include "fsutil.h"
#include "log.h"
#include "paths.h"
#include "platform.h"
#include "rdoom_nx.h"
#include "nx_nvcompat.h"
#include "rdoom_version.h"

using namespace rdoom;

namespace {

class NxPlatform : public Platform
{
public:
	NxPlatform()
	{
		framebufferCreate(&fb_, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
		framebufferMakeLinear(&fb_);
		padConfigureInput(8, HidNpadStyleSet_NpadStandard);
		padInitializeAny(&pad_);
		hidInitializeTouchScreen();
		open_ = true;
	}
	~NxPlatform() override { Shutdown(); }

	bool Poll(InputFrame &in) override
	{
		if (!appletMainLoop())
			return false;
		padUpdate(&pad_);
		const u64 h = padGetButtons(&pad_);
		uint32_t held = 0;
		if (h & HidNpadButton_A) held |= BtnA;
		if (h & HidNpadButton_B) held |= BtnB;
		if (h & HidNpadButton_X) held |= BtnX;
		if (h & HidNpadButton_Y) held |= BtnY;
		if (h & HidNpadButton_L) held |= BtnL;
		if (h & HidNpadButton_R) held |= BtnR;
		if (h & HidNpadButton_ZL) held |= BtnZL;
		if (h & HidNpadButton_ZR) held |= BtnZR;
		if (h & HidNpadButton_Plus) held |= BtnPlus;
		if (h & HidNpadButton_Minus) held |= BtnMinus;
		if (h & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) held |= BtnUp;
		if (h & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) held |= BtnDown;
		if (h & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) held |= BtnLeft;
		if (h & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) held |= BtnRight;
		in.down = held & ~lastHeld_;
		in.held = held;
		in.repeat = repeat_.Update(held, in.down, NowMs());
		lastHeld_ = held;
		HidTouchScreenState ts = {};
		in.touch = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
		if (in.touch)
		{
			in.touchX = (int)ts.touches[0].x;
			in.touchY = (int)ts.touches[0].y;
		}
		return true;
	}

	Canvas &BeginDraw() override
	{
		u32 stride = 0;
		uint32_t *px = (uint32_t *)framebufferBegin(&fb_, &stride);
		canvas_.Retarget(px, (int)(stride / 4));
		return canvas_;
	}

	void EndDraw() override { framebufferEnd(&fb_); }

	uint64_t NowMs() override { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

	bool TextInput(const std::string &title, const std::string &initial, size_t maxLen, std::string &out) override
	{
		SwkbdConfig kbd;
		if (R_FAILED(swkbdCreate(&kbd, 0)))
			return false;
		swkbdConfigMakePresetDefault(&kbd);
		swkbdConfigSetHeaderText(&kbd, title.c_str());
		swkbdConfigSetGuideText(&kbd, title.c_str());
		swkbdConfigSetInitialText(&kbd, initial.c_str());
		swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
		std::vector<char> buf(maxLen * 4 + 4, 0);
		Result rc = swkbdShow(&kbd, buf.data(), buf.size());
		swkbdClose(&kbd);
		if (R_FAILED(rc))
			return false;
		out = buf.data();
		return true;
	}

	SystemInfo Info() override
	{
		SystemInfo si;
		const AppletType t = appletGetAppletType();
		si.appletMode = !(t == AppletType_Application || t == AppletType_SystemApplication);
		svcGetInfo(&si.memTotal, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&si.memUsed, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
		si.sdFree = rdoom::FreeSpace("sdmc:/");
		si.docked = appletGetOperationMode() == AppletOperationMode_Console;
		if (R_SUCCEEDED(setsysInitialize()))
		{
			SetSysFirmwareVersion fw;
			if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
				si.firmware = fw.display_version;
			setsysExit();
		}
		if (NX_NvCompatActive())
			si.emulator = "emulador detectado (compatibilidade nvhost-as ativa)";
		return si;
	}

	void Shutdown() override
	{
		if (open_)
		{
			framebufferClose(&fb_);
			open_ = false;
		}
	}

private:
	Framebuffer fb_ = {};
	PadState pad_ = {};
	Canvas canvas_{ nullptr, 1280, 720, 1280 };
	RepeatTracker repeat_;
	uint32_t lastHeld_ = 0;
	bool open_ = false;
};

bool g_launcherUsedDisplay = false;
bool g_fromLauncher = false;
std::string g_nroPath;
std::vector<std::string> g_args;
std::vector<char *> g_argv;

Paths RootPaths()
{
	Paths p;
	p.root = RDOOM_SD_ROOT;
	return p;
}

} // namespace

#include <fcntl.h>

void RDoom_Crumb(const char *what)
{
	// unbuffered, so the last step survives any crash
	int fd = open(RDOOM_SD_ROOT "/logs/boot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	char buf[768];
	int n = snprintf(buf, sizeof(buf), "[%8llu ms] %s\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000ULL), what);
	write(fd, buf, n > 0 && n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
	close(fd);
}

// background writer (defined with the session report below)
static void StartFlushThread();
static void FlushOnce();

static bool g_prevGpuLost = false;
static int g_gpuStep = 0;

// Keeps the previous session's logs in logs/anterior/, so a test after a freeze
// or a crash does not overwrite the evidence.
static void RotateLogs()
{
	static const char *const files[] = {
		"logs/rdoom.log", "logs/engine.log", "logs/engine-stderr.log", "logs/engine-session.txt",
		"logs/error.log", "logs/boot.log", "logs/last-launch.txt", "logs/perf.log", "logs/phase.log",
		"perf.log", "phase.log" /* written next to the .nro by older builds */ };
	// (gpu-lost.txt stays in place: the start-up below reads it)
	// keep five sessions: anterior (newest) .. anterior-5 (oldest)
	{
		char from[96], to[96];
		snprintf(to, sizeof(to), RDOOM_SD_ROOT "/logs/anterior-5");
		RemoveTree(to);
		for (int i = 4; i >= 1; i--)
		{
			if (i == 1)
				snprintf(from, sizeof(from), RDOOM_SD_ROOT "/logs/anterior");
			else
				snprintf(from, sizeof(from), RDOOM_SD_ROOT "/logs/anterior-%d", i);
			snprintf(to, sizeof(to), RDOOM_SD_ROOT "/logs/anterior-%d", i + 1);
			if (DirExists(from))
				rename(from, to);
		}
	}
	MakeDirs(RDOOM_SD_ROOT "/logs/anterior");
	for (const char *f : files)
	{
		const std::string from = std::string(RDOOM_SD_ROOT "/") + f;
		if (!FileExists(from))
			continue;
		const std::string to = std::string(RDOOM_SD_ROOT "/logs/anterior/") + PathFileName(f);
		remove(to.c_str());
		rename(from.c_str(), to.c_str());
	}
}

void RDoom_NXEarlyInit()
{
	MakeDirs(RDOOM_SD_ROOT "/logs");
	RotateLogs();
	unlink(RDOOM_SD_ROOT "/logs/boot.log");
	RDoom_Crumb("early init");
	// one fixed home, wherever the .nro was started from (hbloader, a
	// forwarder or an emulator that loads the file directly)
	MakeDirs(RDOOM_SD_ROOT);
	chdir(RDOOM_SD_ROOT);
	Paths p = RootPaths();
	p.EnsureLayout();
	LogOpen(PathJoin(p.Logs(), "rdoom.log"), true);
	// engine console output (Printf -> stdout) and driver messages (stderr)
	// fully buffered: the background writer flushes them (see FlushOnce)
	if (freopen(RDOOM_SD_ROOT "/logs/engine.log", "w", stdout))
		setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
	if (freopen(RDOOM_SD_ROOT "/logs/engine-stderr.log", "w", stderr))
		setvbuf(stderr, nullptr, _IOFBF, 32 * 1024);
	StartFlushThread();
	unlink(RDOOM_SD_ROOT "/logs/engine-session.txt");
	RLOG_I("boot", "%s (%s, %s), engine %s", RDOOM_VERSION_STRING, RDOOM_BUILD_DATE, RDOOM_GIT_DESCRIBE, RDOOM_ENGINE_VERSION);
	RLOG_I("boot", "clock policy: stock (RDoom never changes CPU/GPU/memory clocks)");

	// The previous session lost the GPU: start the shader caches from scratch,
	// so a pipeline or shader binary written around the failure is never reused.
	// Workaround step (logs/gpu-workaround.txt: "<step> <build>"); a new build starts over.
	{
		std::string w;
		char build[128] = "";
		int step = 0;
		if (ReadFile(RDOOM_SD_ROOT "/logs/gpu-workaround.txt", w) && sscanf(w.c_str(), "%d %127s", &step, build) == 2 &&
			!strcmp(build, RDOOM_GIT_DESCRIBE))
			g_gpuStep = step;
	}
	if (FileExists(RDOOM_SD_ROOT "/logs/gpu-lost.txt"))
	{
		g_prevGpuLost = true;
		RLOG_W("boot", "previous session lost the GPU: clearing shader caches (%d files)", RDoom_ClearShaderCaches());
		RemoveFile(RDOOM_SD_ROOT "/logs/gpu-lost-previous.txt");
		rename(RDOOM_SD_ROOT "/logs/gpu-lost.txt", RDOOM_SD_ROOT "/logs/gpu-lost-previous.txt");
		// a loss recorded by an older build says nothing about this one: the
		// caches are still cleared, but the experiments start from step 0
		std::string lost;
		ReadFile(RDOOM_SD_ROOT "/logs/gpu-lost-previous.txt", lost);
		const bool sameBuild = lost.find(std::string("build: ") + RDOOM_GIT_DESCRIBE + "\n") != std::string::npos;
		if (sameBuild && g_gpuStep < RDOOM_GPU_STEPS)
			g_gpuStep++;
		char buf[192];
		snprintf(buf, sizeof(buf), "%d %s\n", g_gpuStep, RDOOM_GIT_DESCRIBE);
		WriteFileAtomic(RDOOM_SD_ROOT "/logs/gpu-workaround.txt", buf);
		RLOG_W("boot", "GPU workaround step is now %d", g_gpuStep);
	}
}

bool RDoom_PreviousSessionLostGpu() { return g_prevGpuLost; }
int RDoom_GpuWorkaroundStep() { return g_gpuStep; }

int RDoom_ClearShaderCaches()
{
	int n = 0;
	// UZDoom's Vulkan pipeline cache (written on exit)
	if (RemoveFile(RDOOM_SD_ROOT "/cache/uzdoom/vulkanpipelinecache"))
		n++;
	// Mesa's on-disk shader cache (NVK and the GL driver)
	const int m = RemoveTree("sdmc:/.mesa/mesa_shader_cache_sf");
	if (m > 0)
		n += m;
	return n;
}

bool RDoom_NXLauncherUsedDisplay()
{
	return g_launcherUsedDisplay;
}

int RDoom_NXRunLauncher(int argc, char **argv, int &outArgc, char **&outArgv)
{
	g_nroPath = (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:", 5)) ? argv[0] : std::string(RDOOM_SD_ROOT "/RDoom.nro");
	outArgc = argc;
	outArgv = argv;
	// engine arguments given directly (e.g. by a forwarder): skip the launcher
	if (argc > 1)
	{
		RLOG_I("boot", "%d command-line arguments: launcher skipped", argc - 1);
		return 1;
	}
	RLOG_I("boot", "nv compat: %s", NX_NvCompatStatus());

	Paths paths = RootPaths();
	AppInfo info;
	info.version = RDOOM_VERSION_STRING;
	info.engineVersion = RDOOM_ENGINE_VERSION;
	info.gfxVersion = RDOOM_GFX_VERSION;
	info.buildDate = RDOOM_BUILD_DATE;
	info.enginePk3 = PathJoin(paths.root, "game_support.pk3");
	info.lastSessionFile = PathJoin(paths.Logs(), "engine-session.txt");

	LaunchPlan plan;
	LauncherApp::Result res;
	{
		NxPlatform plat;
		g_launcherUsedDisplay = true;
		LauncherApp app(plat, paths, info);
		res = app.Run(plan);
		RDoom_Crumb("launcher: app returned");
		plat.Shutdown();
		RDoom_Crumb("launcher: framebuffer closed");
	}
	RDoom_Crumb(res == LauncherApp::Result::Launch ? "launcher: start game" : "launcher: exit");
	if (res != LauncherApp::Result::Launch)
	{
		RLOG_I("boot", "launcher closed without starting a game");
		return 0;
	}
	g_fromLauncher = true;
	g_args = plan.argv;
	g_argv.clear();
	for (auto &a : g_args)
		g_argv.push_back(const_cast<char *>(a.c_str()));
	g_argv.push_back(nullptr);
	outArgc = (int)g_args.size();
	outArgv = g_argv.data();
	return 1;
}

void RDoom_NXAtExit()
{
	// on hardware, quitting the game brings the launcher back
	if (g_fromLauncher && envHasNextLoad())
	{
		std::string args = "\"" + g_nroPath + "\"";
		envSetNextLoad(g_nroPath.c_str(), args.c_str());
		RLOG_I("boot", "returning to the launcher (%s)", g_nroPath.c_str());
	}
	FlushOnce();
	LogClose();
}

// ---------------------------------------------------------------------------
// engine session report (shown in the launcher's diagnostics screen)
// ---------------------------------------------------------------------------

static std::vector<std::pair<std::string, std::string>> g_session;

// ---------------------------------------------------------------------------
// Background writer: SD card writes take milliseconds to tens of milliseconds
// on the Switch, and the game used to make them inside frames (every log line,
// every new pipeline, the session report every 600 frames, sounds printing
// decoder notes to stderr). Logs are now fully buffered and this low-priority
// thread flushes them, rewrites engine-session.txt when it changed and writes
// queued files (the pipeline cache, warm lists) within 0.25 s. Quitting with
// HOME loses at most the last two seconds of log.

static std::mutex g_ioMutex;	// g_session, g_sessionDirty, g_fileQueue
static bool g_sessionDirty = false;
static std::vector<std::pair<std::string, std::vector<uint8_t>>> g_fileQueue;
static Thread g_flushThread;
static volatile bool g_flushRun = false;
void NXPerf_FlushFile();

static void WriteWhole(const std::string &path, const void *data, size_t size)
{
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	bool ok = fwrite(data, 1, size, f) == size;
	ok &= fclose(f) == 0;
	if (ok)
	{
		remove(path.c_str());
		rename(tmp.c_str(), path.c_str());
	}
	else
		remove(tmp.c_str());
}

// Log files shared by the game thread and this writer. The writer used to
// fflush() them, which holds the stdio lock (and, for rdoom.log, the log
// mutex) during the SD write; any log line the game wrote meanwhile - the
// session report every 600 frames, a pickup message, a perf.log line - then
// waited 90-150 ms for the card (seen on the Switch Lite, 2026-10-13). Now a
// flush of these FILEs only appends to memory, and the writer writes that to
// the file descriptor without holding anything the game thread takes.
struct BufferedFile
{
	FILE *fp = nullptr;
	int fd = -1;
	std::mutex m;	// pending
	std::string pending;
	decltype(((FILE *)nullptr)->_close) close = nullptr;
};
static BufferedFile g_buffered[6];
static std::mutex g_drainMutex;	// writes to the descriptors, registration

static BufferedFile *FindBuffered(void *cookie)
{
	for (auto &b : g_buffered)
		if (b.fp == cookie)
			return &b;
	return nullptr;
}

static void WriteAll(int fd, const std::string &data)
{
	size_t done = 0;
	while (done < data.size())
	{
		ssize_t n = write(fd, data.data() + done, data.size() - done);
		if (n <= 0)
			break;
		done += (size_t)n;
	}
}

static _ssize_t BufferedWrite(struct _reent *, void *cookie, const char *buf, size_t n)
{
	BufferedFile *b = FindBuffered(cookie);
	if (!b)
		return write(fileno((FILE *)cookie), buf, n);
	std::lock_guard<std::mutex> lock(b->m);
	b->pending.append(buf, n);
	return (_ssize_t)n;
}

static void DrainBuffered()
{
	std::lock_guard<std::mutex> drain(g_drainMutex);
	for (auto &b : g_buffered)
	{
		if (!b.fp)
			continue;
		std::string data;
		{
			std::lock_guard<std::mutex> lock(b.m);
			data.swap(b.pending);
		}
		if (!data.empty())
			WriteAll(b.fd, data);
	}
}

static int BufferedClose(struct _reent *r, void *cookie)
{
	std::lock_guard<std::mutex> drain(g_drainMutex);
	BufferedFile *b = FindBuffered(cookie);
	if (!b)
		return close(fileno((FILE *)cookie));
	{
		std::lock_guard<std::mutex> lock(b->m);
		WriteAll(b->fd, b->pending);
		b->pending.clear();
	}
	auto origClose = b->close;
	b->fp = nullptr;
	return origClose ? origClose(r, cookie) : close(b->fd);
}

void RDoom_BufferFile(FILE *f)
{
	if (!f || !g_flushRun)
		return;
	std::lock_guard<std::mutex> drain(g_drainMutex);
	if (FindBuffered(f))
		return;
	for (auto &b : g_buffered)
		if (!b.fp)
		{
			fflush(f);
			b.fd = fileno(f);
			b.close = f->_close;
			b.fp = f;
			f->_write = BufferedWrite;
			f->_close = BufferedClose;
			return;
		}
}

static void FlushOnce()
{
	std::string session;
	bool writeSession = false;
	decltype(g_fileQueue) files;
	{
		std::lock_guard<std::mutex> lock(g_ioMutex);
		if (g_sessionDirty)
		{
			for (auto &kv : g_session)
				session += kv.first + ": " + kv.second + "\n";
			g_sessionDirty = false;
			writeSession = true;
		}
		files.swap(g_fileQueue);
	}
	if (writeSession)
	{
		FILE *f = fopen(RDOOM_SD_ROOT "/logs/engine-session.txt", "w");
		if (f)
		{
			fwrite(session.data(), 1, session.size(), f);
			fclose(f);
		}
	}
	for (auto &file : files)
		WriteWhole(file.first, file.second.data(), file.second.size());
	LogFlush();
	fflush(stdout);
	fflush(stderr);
	NXPerf_FlushFile();
	DrainBuffered();
}

static void FlushThreadMain(void *)
{
	// logs every two seconds; queued files (pipeline cache, warm lists) within
	// a quarter of a second, so quitting with HOME right after they change
	// does not lose them
	for (unsigned tick = 1; g_flushRun; tick++)
	{
		svcSleepThread(250000000ULL);
		bool files;
		{
			std::lock_guard<std::mutex> lock(g_ioMutex);
			files = !g_fileQueue.empty();
		}
		if (files || (tick % 8) == 0)
			FlushOnce();
	}
}

static void StartFlushThread()
{
	if (g_flushRun)
		return;
	// lowest priority on the default core: it sleeps in SD card calls
	if (R_SUCCEEDED(threadCreate(&g_flushThread, FlushThreadMain, nullptr, nullptr, 0x10000, 0x3F, -2)))
	{
		g_flushRun = true;
		if (R_FAILED(threadStart(&g_flushThread)))
		{
			g_flushRun = false;
			threadClose(&g_flushThread);
		}
	}
	LogSetFlushEachLine(!g_flushRun);
	if (g_flushRun)
	{
		RDoom_BufferFile(LogFileHandle());
		RDoom_BufferFile(stdout);
		RDoom_BufferFile(stderr);
		atexit(FlushOnce);	// what is still in memory at a normal exit
	}
}

void RDoom_FlushLogsNow()
{
	FlushOnce();
}

void RDoom_WriteFileAsync(const char *path, const void *data, size_t size)
{
	std::vector<uint8_t> bytes((const uint8_t *)data, (const uint8_t *)data + size);
	if (!g_flushRun)
	{
		WriteWhole(path, bytes.data(), bytes.size());
		return;
	}
	std::lock_guard<std::mutex> lock(g_ioMutex);
	for (auto &file : g_fileQueue)
		if (file.first == path)
		{
			file.second.swap(bytes);
			return;
		}
	g_fileQueue.emplace_back(path, std::move(bytes));
}

void RDoom_SessionSet(const char *key, const char *value)
{
	{
		std::lock_guard<std::mutex> lock(g_ioMutex);
		bool found = false;
		for (auto &kv : g_session)
			if (kv.first == key)
			{
				kv.second = value;
				found = true;
				break;
			}
		if (!found)
			g_session.push_back({ key, value });
		g_sessionDirty = true;
	}
	if (!g_flushRun)
		FlushOnce();
	RLOG_I("session", "%s: %s", key, value);
}

// Called by ZVulkan the first time the GPU is reported lost: leaves a marker
// with everything the session report knows (newest pipeline, frames presented),
// read by the next start-up.
void RDoom_VulkanDeviceLost(const char *what)
{
	RDoom_SessionSet("vulkan_device_lost", what ? what : "?");
	FlushOnce();
	std::string out = std::string("GPU lost: ") + (what ? what : "?") + "\n";
	out += std::string("build: ") + RDOOM_GIT_DESCRIBE + "\n";
	{
		std::lock_guard<std::mutex> lock(g_ioMutex);
		for (auto &kv : g_session)
			out += kv.first + ": " + kv.second + "\n";
	}
	FILE *f = fopen(RDOOM_SD_ROOT "/logs/gpu-lost.txt", "w");
	if (f)
	{
		fwrite(out.data(), 1, out.size(), f);
		fclose(f);
	}
}

void RDoom_SessionLog(const char *category, const char *message)
{
	RLOG_I(category, "%s", message);
}

// ---------------------------------------------------------------------------
// late CVAR overrides (see rdoom_nx.h)
// ---------------------------------------------------------------------------

static std::vector<std::pair<std::string, std::string>> g_lateCvars;

void RDoom_AddLateCvar(const char *name, const char *value)
{
	for (auto &kv : g_lateCvars)
		if (kv.first == name)
		{
			kv.second = value;
			return;
		}
	g_lateCvars.push_back({ name, value });
}

void RDoom_ApplyLateCvars()
{
	extern void RDoom_SetCvarString(const char *name, const char *value);
	for (auto &kv : g_lateCvars)
	{
		RDoom_SetCvarString(kv.first.c_str(), kv.second.c_str());
		RLOG_I("session", "late override %s=%s", kv.first.c_str(), kv.second.c_str());
	}
}
