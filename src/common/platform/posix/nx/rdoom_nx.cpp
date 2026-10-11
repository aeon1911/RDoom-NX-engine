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
	char buf[256];
	int n = snprintf(buf, sizeof(buf), "[%8llu ms] %s\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000ULL), what);
	write(fd, buf, n > 0 && n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
	close(fd);
}

void RDoom_NXEarlyInit()
{
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
	if (freopen(RDOOM_SD_ROOT "/logs/engine.log", "w", stdout))
		setvbuf(stdout, nullptr, _IOLBF, 4096);
	if (freopen(RDOOM_SD_ROOT "/logs/engine-stderr.log", "w", stderr))
		setvbuf(stderr, nullptr, _IONBF, 0);
	unlink(RDOOM_SD_ROOT "/logs/engine-session.txt");
	RLOG_I("boot", "%s (%s, %s), engine %s", RDOOM_VERSION_STRING, RDOOM_BUILD_DATE, RDOOM_GIT_DESCRIBE, RDOOM_ENGINE_VERSION);
	RLOG_I("boot", "clock policy: stock (RDoom never changes CPU/GPU/memory clocks)");

	// The previous session lost the GPU: start the shader caches from scratch,
	// so a pipeline or shader binary written around the failure is never reused.
	if (FileExists(RDOOM_SD_ROOT "/logs/gpu-lost.txt"))
	{
		RLOG_W("boot", "previous session lost the GPU: clearing shader caches (%d files)", RDoom_ClearShaderCaches());
		RemoveFile(RDOOM_SD_ROOT "/logs/gpu-lost-previous.txt");
		rename(RDOOM_SD_ROOT "/logs/gpu-lost.txt", RDOOM_SD_ROOT "/logs/gpu-lost-previous.txt");
	}
}

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
	LogClose();
}

// ---------------------------------------------------------------------------
// engine session report (shown in the launcher's diagnostics screen)
// ---------------------------------------------------------------------------

static std::vector<std::pair<std::string, std::string>> g_session;

void RDoom_SessionSet(const char *key, const char *value)
{
	for (auto &kv : g_session)
		if (kv.first == key)
		{
			kv.second = value;
			goto write;
		}
	g_session.push_back({ key, value });
write:
	std::string out;
	for (auto &kv : g_session)
		out += kv.first + ": " + kv.second + "\n";
	FILE *f = fopen(RDOOM_SD_ROOT "/logs/engine-session.txt", "w");
	if (f)
	{
		fwrite(out.data(), 1, out.size(), f);
		fclose(f);
	}
	RLOG_I("session", "%s: %s", key, value);
}

// Called by ZVulkan the first time the GPU is reported lost: leaves a marker
// with everything the session report knows (newest pipeline, frames presented),
// read by the next start-up.
void RDoom_VulkanDeviceLost(const char *what)
{
	RDoom_SessionSet("vulkan_device_lost", what ? what : "?");
	std::string out = std::string("GPU lost: ") + (what ? what : "?") + "\n";
	for (auto &kv : g_session)
		out += kv.first + ": " + kv.second + "\n";
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
