/*
** sdlglvideo.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Marisa Heit
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

// HEADER FILES ------------------------------------------------------------

#include <SDL2/SDL.h>
#include "nx_perf.h"
#ifdef __SWITCH__
#include "common/platform/posix/nx/nx_glthread.h"
#include "common/platform/posix/nx/nx_loadscreen.h"
#include "common/platform/posix/nx/rdoom_nx.h"
#include "common/platform/posix/nx/nx_nvcompat.h"
#include "gamestate.h"
#include <switch.h>
#endif

#ifdef HAVE_VULKAN
#if !defined(__SWITCH__)
#include <SDL2/SDL_vulkan.h>
#endif
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <ctype.h>
#include <unistd.h>
#include <zvulkan/vulkansurface.h>
#endif

#include "basics.h"
#include "c_dispatch.h"
#include "gl_framebuffer.h"
#include "gl_sysfb.h"
#include "i_soundinternal.h"
#include "i_video.h"
#include "m_argv.h"
#include "printf.h"
#include "v_video.h"
#include "version.h"

#ifdef HAVE_GLES2
#include "gles_framebuffer.h"
#endif

#ifdef HAVE_VULKAN
#include "vulkan/system/vk_renderdevice.h"
#endif

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

extern IVideo *Video;

EXTERN_CVAR (Int, vid_adapter)
EXTERN_CVAR (Int, vid_displaybits)
EXTERN_CVAR (Int, vid_defwidth)
EXTERN_CVAR (Int, vid_defheight)
EXTERN_CVAR (Bool, cl_capfps)
#ifdef __SWITCH__
EXTERN_CVAR(Bool, vid_vsync)
// Switch: vsync interval (1 = 60 Hz, 2 = locked 30 fps)
CUSTOM_CVAR(Int, nx_swapinterval, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	else if (self > 4) self = 4;
	else if (screen) screen->SetVSync(vid_vsync);
}
#endif
EXTERN_CVAR(Bool, vk_debug)
EXTERN_FARG(glversion);

// PUBLIC DATA DEFINITIONS -------------------------------------------------

CUSTOM_CVAR(Bool, gl_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}
CUSTOM_CVAR(Bool, gl_es, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(String, vid_sdl_render_driver, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vid_list_sdl_render_drivers)
{
	for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i)
	{
		SDL_RendererInfo info;
		if (SDL_GetRenderDriverInfo(i, &info) == 0)
			Printf("%s\n", info.name);
	}
}

// PRIVATE DATA DEFINITIONS ------------------------------------------------

namespace Priv
{
	SDL_Window *window;
	bool vulkanEnabled;
	bool softpolyEnabled;
	bool fullscreenSwitch;
	int numberOfDisplays;
	SDL_Rect* displayBounds = nullptr;

	void updateDisplayInfo()
	{
		Priv::numberOfDisplays = SDL_GetNumVideoDisplays();
		if (Priv::numberOfDisplays <= 0) {
			Printf("%sWrong number of displays detected.\n", TEXTCOLOR_BOLD);
			return;
		}
		Printf("Number of detected displays %d .\n", Priv::numberOfDisplays);

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
		}
		Priv::displayBounds = (SDL_Rect*) calloc(Priv::numberOfDisplays, sizeof(SDL_Rect));

		for (int i=0; i < Priv::numberOfDisplays; i++) {
			if (0 != SDL_GetDisplayBounds(i, &Priv::displayBounds[i])) {
				Printf("%sError getting display %d size: %s\n", TEXTCOLOR_BOLD, i, SDL_GetError());
				if (i == 0) {
					free(Priv::displayBounds);
					displayBounds = nullptr;
				}
				Priv::numberOfDisplays = i;
				return;
			}
		}
	}

	void CreateWindow(uint32_t extraFlags)
	{
		assert(Priv::window == nullptr);

		// Get displays and default display size
		updateDisplayInfo();

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &displayBounds[vid_adapter % numberOfDisplays];

#ifdef __SWITCH__
		// the Switch reports the docked 1080p mode; the game renders at 720p
		win_w = 1280;
		win_h = 720;
#endif
		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}

		int xWindowPos = (win_x <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_x;
		int yWindowPos = (win_y <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_y;
		Printf("Creating window [%dx%d] on adapter %d\n", (*win_w), (*win_h), (*vid_adapter));

		FString caption;
		caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());

		const uint32_t windowFlags = (win_maximized ? SDL_WINDOW_MAXIMIZED : 0) | SDL_WINDOW_RESIZABLE | extraFlags;
		Priv::window = SDL_CreateWindow(caption.GetChars(), xWindowPos, yWindowPos, win_w, win_h, windowFlags);

		if (Priv::window != nullptr)
		{
			SDL_version sdlver;
			SDL_GetVersion(&sdlver);
			// Enforce minimum size limit
			SDL_SetWindowMinimumSize(Priv::window, VID_MIN_WIDTH, VID_MIN_HEIGHT);
			// Tell SDL to start sending text input on Wayland if it's on affected versions.
			if (strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) == 0 && sdlver.major == 2 && sdlver.minor == 0 && sdlver.patch < 18)
				SDL_StartTextInput();
		}
	}

	void DestroyWindow()
	{
		assert(Priv::window != nullptr);

		SDL_DestroyWindow(Priv::window);
		Priv::window = nullptr;

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
			Priv::displayBounds = nullptr;
		}
	}

	void SetupPixelFormat(int multisample, const int *glver)
	{
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		if (multisample > 0) {
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisample);
		}
		if (gl_debug)
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

		if (gl_es)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		}
		else if (glver[0] > 2)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, glver[0]);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, glver[1]);
		}
		else
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		}
	}
}

CUSTOM_CVAR(Int, vid_adapter, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
  if (Priv::window != nullptr) {
		// Get displays and default display size
		Priv::updateDisplayInfo();

	int display = (*self) % Priv::numberOfDisplays;

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &Priv::displayBounds[vid_adapter % Priv::numberOfDisplays];

#ifdef __SWITCH__
		// the Switch reports the docked 1080p mode; the game renders at 720p
		win_w = 1280;
		win_h = 720;
#endif
		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}
		// Forces to set to the ini this vars to -1, so +vid_adapter keeps working the next time that the game it's launched
		win_x = -1;
		win_y = -1;

		if ((SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {

			// TODO This not works. For some reason keeps stuck on the previus screen
			/*
			SDL_DisplayMode currentDisplayMode;
			SDL_GetWindowDisplayMode(Priv::window, &currentDisplayMode);
			currentDisplayMode.w = win_w;
			currentDisplayMode.h = win_h;
			if ( 0 != SDL_SetWindowDisplayMode(Priv::window, &currentDisplayMode)) {
				Printf("A problem occured trying to change of display %s\n", SDL_GetError());
			}
			*/

			// TODO This workaround also isn't working
			/*
			SDL_SetWindowFullscreen(Priv::window, 0);
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, bounds->x , bounds->y);
			SDL_SetWindowFullscreen(Priv::window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			*/
			Printf("Changing adapter on fullscreen, isn't full supported by SDL. Instead try to switch to windowed mode, change the adapter and then switch again to fullscreen.\n");

		} else {
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		}

	display = SDL_GetWindowDisplayIndex(Priv::window);
	if (display >= 0) {
			Printf("New display is %d\n", display );
		} else {
			Printf("A problem occured trying to change of display %s\n", SDL_GetError());
		}
  }
}

class SDLVideo : public IVideo
{
public:
	SDLVideo ();
	~SDLVideo ();

	void DumpAdapters();

	DFrameBuffer *CreateFrameBuffer ();

private:
#ifdef HAVE_VULKAN
	std::shared_ptr<VulkanSurface> surface;
#endif
};

// CODE --------------------------------------------------------------------

#ifdef HAVE_VULKAN
void I_GetVulkanDrawableSize(int *width, int *height)
{
	assert(Priv::vulkanEnabled);
#if defined(__SWITCH__)
	if (width) *width = 1280;
	if (height) *height = 720;
#else
	assert(Priv::window != nullptr);
	SDL_Vulkan_GetDrawableSize(Priv::window, width, height);
#endif
}

bool I_GetVulkanPlatformExtensions(unsigned int *count, const char **names)
{
	assert(Priv::vulkanEnabled);
#if defined(__SWITCH__)
	static const char *exts[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_NN_VI_SURFACE_EXTENSION_NAME
	};
	if (names)
	{
		names[0] = exts[0];
		names[1] = exts[1];
	}
	*count = 2;
	return true;
#else
	assert(Priv::window != nullptr);
	return SDL_Vulkan_GetInstanceExtensions(Priv::window, count, names) == SDL_TRUE;
#endif
}

bool I_CreateVulkanSurface(VkInstance instance, VkSurfaceKHR *surface)
{
	assert(Priv::vulkanEnabled);
#if defined(__SWITCH__)
	NWindow *win = nwindowGetDefault();
	// The loading screen registered its own buffers, then framebufferClose
	// disconnects the window and stores format 0. nwindowConfigureBuffer
	// only copies the scanout format when this field is still the creation
	// sentinel. With format 0 the compositor never shows the Vulkan image.
	// Do this only when that screen owned the window. Releasing buffers on
	// a window we never claimed drops the layer and the present never shows.
	if (NXLS_ClaimedDisplay() || RDoom_NXLauncherUsedDisplay())
	{
		nwindowReleaseBuffers(win);
		win->format = ~0u;
		win->usage = 0;
		nwindowSetDimensions(win, 1280, 720);
	}
	VkViSurfaceCreateInfoNN createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
	createInfo.window = (void*)win;
	VkResult res = vkCreateViSurfaceNN(instance, &createInfo, nullptr, surface);
	RDoom_SessionSet("vulkan_surface", res == VK_SUCCESS ? "VK_NN_vi_surface on the default NWindow: ok" : "vkCreateViSurfaceNN FAILED");
	return res == VK_SUCCESS;
#else
	assert(Priv::window != nullptr);
	return SDL_Vulkan_CreateSurface(Priv::window, instance, surface) == SDL_TRUE;
#endif
}
#endif


SDLVideo::SDLVideo ()
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
		return;
	}

	// Fail gracefully if we somehow reach here after linking against a SDL2 library older than 2.0.6.
	if (!SDL_VERSION_ATLEAST(2, 0, 6))
	{
		I_FatalError("Only SDL 2.0.6 or later is supported.");
	}

#ifdef HAVE_VULKAN
	Priv::vulkanEnabled = vid_preferbackend == BACKEND_VULKAN;

	if (Priv::vulkanEnabled)
	{
#if defined(__SWITCH__)
		// SDL2 on Horizon only creates EGL windows. Vulkan draws on the
		// default NWindow, so this path does not open an SDL window.
		Priv::updateDisplayInfo();
#else
		Priv::CreateWindow(SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | (vid_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));

		if (Priv::window == nullptr)
		{
			Priv::vulkanEnabled = false;
		}
#endif
	}
#endif
}

SDLVideo::~SDLVideo ()
{
#ifdef HAVE_VULKAN
	surface.reset();
#endif
}

void SDLVideo::DumpAdapters()
{
	Priv::updateDisplayInfo();
  for (int i=0; i < Priv::numberOfDisplays; i++) {
	Printf("%s%d. [%dx%d @ (%d,%d)]\n",
		vid_adapter == i ? TEXTCOLOR_BOLD : "",
		i,
		Priv::displayBounds[i].w,
		Priv::displayBounds[i].h,
		Priv::displayBounds[i].x,
		Priv::displayBounds[i].y
	  );
  }
}


DFrameBuffer *SDLVideo::CreateFrameBuffer ()
{
	SystemBaseFrameBuffer *fb = nullptr;

	// first try Vulkan, if that fails OpenGL
#ifdef HAVE_VULKAN
#ifdef __SWITCH__
	RDoom_SessionSet("renderer_requested", vid_preferbackend == BACKEND_VULKAN ? "vulkan" : vid_preferbackend == BACKEND_OPENGL ? "opengl" : "gles");
	RDoom_SessionSet("nv_compat", NX_NvCompatStatus());
	// Diagnostics without a new build: switch/rdoom/rdoom_debug.txt with one
	// NAME=value per line. UPPER-CASE names are driver environment variables
	// (NVK_DEBUG=push_sync, NVK_SWITCH_NO_UBO_CBUF=1, ...), applied before the
	// driver starts; lower-case names are engine CVARs (rd_vk_frameoverlap=0,
	// r_dynlights=0, ...). Nothing changes when the file is absent.
	// Automatic workarounds after a lost GPU (see rdoom_nx.h); a manual
	// rdoom_debug.txt replaces them.
	if (vid_preferbackend == BACKEND_VULKAN && RDoom_GpuWorkaroundStep() > 0 && access("rdoom_debug.txt", F_OK) != 0)
	{
		const int step = RDoom_GpuWorkaroundStep();
		FString applied;
		setenv("NVK_SWITCH_PERF_LOG", "1", 1);  // the driver then reports channel errors in detail
		applied += "NVK_SWITCH_PERF_LOG=1";
		auto setcvar = [&](const char *name, const char *value) {
			if (FBaseCVar *cv = FindCVar(name, nullptr)) { UCVarValue v; v.String = value; cv->SetGenericRep(v, CVAR_String); }
			applied.AppendFormat(" %s=%s", name, value);
		};
		// one hypothesis per step for the hang on the first 3D draw (ZCULL is
		// already off by default; driver workarounds did not help, 2026-10-11)
		if (step >= 1 && step <= 3)
			setcvar("rd_vk_nofans", "1");
		if (step == 2)
			setcvar("rd_vk_zstate", "1");
		if (step == 3)
			setcvar("rd_vk_zstate", "2");
		if (step >= 4)
		{
			// trace mode: every draw submitted and waited for, one log line per 3D draw
			setenv("NVK_DEBUG", "push_sync", 1);
			applied += " NVK_DEBUG=push_sync (trace mode)";
			setcvar("vk_submit_size", "1");
			setcvar("rd_vk_drawlog", "1");
			setcvar("rd_vk_frameoverlap", "0");
		}
		FString d;
		d.Format("step %d of %d%s: %s", step, RDOOM_GPU_STEPS, RDoom_PreviousSessionLostGpu() ? " (previous session lost the GPU)" : " (kept: no GPU loss since)", applied.GetChars());
		RDoom_SessionSet("gpu_workaround", d.GetChars());
	}
	if (FILE *ef = fopen("rdoom_debug.txt", "r"))
	{
		char line[256];
		FString applied;
		while (fgets(line, sizeof(line), ef))
		{
			char *nl = strpbrk(line, "\r\n");
			if (nl) *nl = 0;
			char *eq = strchr(line, '=');
			if (!eq || eq == line || line[0] == '#') continue;
			*eq = 0;
			const char *value = eq + 1;
			if (isupper((unsigned char)line[0]))
			{
				setenv(line, value, 1);
				applied.AppendFormat("%s%s=%s", applied.IsEmpty() ? "" : " ", line, value);
			}
			else if (FBaseCVar *cv = FindCVar(line, nullptr))
			{
				UCVarValue v;
				v.String = value;
				cv->SetGenericRep(v, CVAR_String);
				applied.AppendFormat("%s%s=%s", applied.IsEmpty() ? "" : " ", line, value);
			}
			else
				applied.AppendFormat("%s%s=?(unknown cvar)", applied.IsEmpty() ? "" : " ", line);
		}
		fclose(ef);
		RDoom_SessionSet("debug_file", applied.GetChars());
	}
	VulkanDeviceLostCallback = RDoom_VulkanDeviceLost;
#endif
	if (Priv::vulkanEnabled)
	{
		try
		{
			unsigned int count = 64;
			const char* names[64];
			if (!I_GetVulkanPlatformExtensions(&count, names))
				VulkanError("I_GetVulkanPlatformExtensions failed");

			VulkanInstanceBuilder builder;
			builder.DebugLayer(vk_debug);
			for (unsigned int i = 0; i < count; i++)
				builder.RequireExtension(names[i]);
			auto instance = builder.Create();
#ifdef __SWITCH__
			{
				FString iv;
				iv.Format("ok (api %d.%d, loaderless Mesa NVK)", VK_VERSION_MAJOR(instance->ApiVersion), VK_VERSION_MINOR(instance->ApiVersion));
				RDoom_SessionSet("vulkan_instance", iv.GetChars());
			}
#endif

			VkSurfaceKHR surfacehandle = nullptr;
			if (!I_CreateVulkanSurface(instance->Instance, &surfacehandle))
				VulkanError("I_CreateVulkanSurface failed");

			surface = std::make_shared<VulkanSurface>(instance, surfacehandle);

			fb = new VulkanRenderDevice(nullptr, vid_fullscreen, surface);
		}
		catch (CVulkanError const &error)
		{
#ifdef __SWITCH__
			// No silent switch to OpenGL: the profile asked for Vulkan.
			RDoom_SessionSet("renderer_active", "none");
			RDoom_SessionSet("vulkan_error", error.what());
			I_FatalError("O Vulkan n\xc3\xa3o p\xc3\xb4" "de ser iniciado / Vulkan could not be started:\n%s\n\n"
				"Detalhes em switch/rdoom/logs/engine-session.txt.\n"
				"Para usar outro renderizador, escolha OpenGL ou OpenGL ES no perfil (launcher > Editar perfil > Renderizador).", error.what());
#else
			if (Priv::window != nullptr)
			{
				Priv::DestroyWindow();
			}

			Printf(TEXTCOLOR_RED "Initialization of Vulkan failed: %s\n", error.what());
			Priv::vulkanEnabled = false;
#endif
		}
	}
#endif

	if (fb == nullptr)
	{
#ifdef HAVE_GLES2
		if (vid_preferbackend != BACKEND_OPENGL)
			fb = new OpenGLESRenderer::OpenGLFrameBuffer(0, vid_fullscreen);
		else
#endif
			fb = new OpenGLRenderer::OpenGLFrameBuffer(0, vid_fullscreen);
	}

	return fb;
}


IVideo *gl_CreateVideo()
{
	return new SDLVideo();
}


// FrameBuffer Implementation -----------------------------------------------

SystemBaseFrameBuffer::SystemBaseFrameBuffer (void *, bool fullscreen)
: DFrameBuffer (vid_defwidth, vid_defheight)
{
}

int SystemBaseFrameBuffer::GetClientWidth()
{
	int width = 0;


#ifdef HAVE_VULKAN
	assert(Priv::vulkanEnabled);
#if defined(__SWITCH__)
	width = 1280;
#else
	SDL_Vulkan_GetDrawableSize(Priv::window, &width, nullptr);
#endif
#endif

	return width;
}

int SystemBaseFrameBuffer::GetClientHeight()
{
	int height = 0;

#ifdef HAVE_VULKAN
	assert(Priv::vulkanEnabled);
#if defined(__SWITCH__)
	height = 720;
#else
	SDL_Vulkan_GetDrawableSize(Priv::window, nullptr, &height);
#endif
#endif

	return height;
}

bool SystemBaseFrameBuffer::IsFullscreen ()
{
#if defined(__SWITCH__)
	if (Priv::window == nullptr) return true;
#endif
	return (SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void SystemBaseFrameBuffer::ToggleFullscreen(bool yes)
{
	if (Priv::window == nullptr) return;
	SDL_ShowWindow(Priv::window);
	SDL_SetWindowFullscreen(Priv::window, yes ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if ( !yes )
	{
		if ( !Priv::fullscreenSwitch )
		{
			Priv::fullscreenSwitch = true;
			vid_fullscreen = false;
		}
		else
		{
			Priv::fullscreenSwitch = false;
			SetWindowSize(win_w, win_h);
		}
	}
}

void SystemBaseFrameBuffer::SetWindowSize(int w, int h)
{
	if (w < VID_MIN_WIDTH || h < VID_MIN_HEIGHT)
	{
		w = VID_MIN_WIDTH;
		h = VID_MIN_HEIGHT;
	}
	win_w = w;
	win_h = h;
	if (vid_fullscreen)
	{
		vid_fullscreen = false;
	}
	else if (Priv::window != nullptr)
	{
		win_maximized = false;
		SDL_SetWindowSize(Priv::window, w, h);
		SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter), SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter));
		SetSize(GetClientWidth(), GetClientHeight());
		int x, y;
		SDL_GetWindowPosition(Priv::window, &x, &y);
		win_x = x;
		win_y = y;

	}
}


SystemGLFrameBuffer::SystemGLFrameBuffer(void *hMonitor, bool fullscreen)
: SystemBaseFrameBuffer(hMonitor, fullscreen)
{
	// NOTE: Core profiles were added with GL 3.2, so there's no sense trying
	// to set core 3.1 or 3.0. We could try a forward-compatible context
	// instead, but that would be too restrictive (w.r.t. shaders).
	static const int glvers[][2] = {
		{ 4, 6 }, { 4, 5 }, { 4, 4 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 },
		{ 3, 3 }, { 3, 2 }, { 2, 0 },
		{ 0, 0 },
	};
	int glveridx = 0;
	int i;

	const char *version = Args->CheckValue(FArg_glversion);
	if (version != NULL)
	{
		double gl_version = strtod(version, NULL) + 0.01;
		int vermaj = (int)gl_version;
		int vermin = (int)(gl_version*10.0) % 10;

		while (glvers[glveridx][0] > vermaj || (glvers[glveridx][0] == vermaj &&
				glvers[glveridx][1] > vermin))
		{
			glveridx++;
			if (glvers[glveridx][0] == 0)
			{
				glveridx = 0;
				break;
			}
		}
	}

#ifdef __SWITCH__
	// Mesa 26 Horizon SDK: glthread is chosen before the EGL context exists
	EXTERN_CVAR(Bool, nx_glthread)
	setenv("MESA_SWITCH_GLTHREAD", nx_glthread ? "1" : "0", 1);
	RDoom_SessionSet("renderer_active", vid_preferbackend == BACKEND_OPENGL ? "opengl (Mesa NVC0)" : "gles (Mesa NVC0)");
#endif
	for ( ; glvers[glveridx][0] > 0; ++glveridx)
	{
		Priv::SetupPixelFormat(0, glvers[glveridx]);
#ifdef __SWITCH__
		nwindowReleaseBuffers(nwindowGetDefault());
#endif
		Priv::CreateWindow(SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));

		if (Priv::window == nullptr)
		{
			continue;
		}

		GLContext = SDL_GL_CreateContext(Priv::window);
		if (GLContext == nullptr)
		{
			Priv::DestroyWindow();
		}
		else
		{
			break;
		}
	}
	if (Priv::window == nullptr)
	{
		I_FatalError("Could not create OpenGL window:\n%s\n",SDL_GetError());
	}
}

SystemGLFrameBuffer::~SystemGLFrameBuffer ()
{
	if (Priv::window)
	{
		if (GLContext)
		{
			SDL_GL_DeleteContext(GLContext);
		}

		Priv::DestroyWindow();
	}
}

int SystemGLFrameBuffer::GetClientWidth()
{
	int width = 0;
	SDL_GL_GetDrawableSize(Priv::window, &width, nullptr);
	return width;
}

int SystemGLFrameBuffer::GetClientHeight()
{
	int height = 0;
	SDL_GL_GetDrawableSize(Priv::window, nullptr, &height);
	return height;
}

#ifdef __SWITCH__
// Brutal's opening is a title map, not a level. Interval 2 waits inside the
// Switch display driver and never returns, so the controller is never read.
// A real level keeps the interval chosen in the launcher.
static int NXPresentInterval(bool vsync)
{
	if (!vsync) return 0;
	if (gamestate != GS_LEVEL) return 1;
	return clamp<int>(nx_swapinterval, 1, 4);
}
#endif

void SystemGLFrameBuffer::SetVSync( bool vsync )
{
#if defined (__APPLE__)
	const GLint value = vsync ? 1 : 0;
	CGLSetParameter( CGLGetCurrentContext(), kCGLCPSwapInterval, &value );
#else
#ifdef __SWITCH__
	// the panel is 60 Hz: interval 2 presents every other refresh = a real 30 fps lock
	NX_GLThreadFinish();
	SDL_GL_SetSwapInterval(NXPresentInterval(vsync));
#else
	if (vsync)
	{
		if (SDL_GL_SetSwapInterval(-1) == -1)
			SDL_GL_SetSwapInterval(1);
	}
	else
	{
		SDL_GL_SetSwapInterval(0);
	}
#endif
#endif
}

void SystemGLFrameBuffer::SwapBuffers()
{
#ifdef __SWITCH__
	// everything recorded for this frame must reach the driver before the swap
	NX_GLThreadFinish();
	static int applied = -1;
	int interval = NXPresentInterval(vid_vsync);
	if (interval != applied)
	{
		SDL_GL_SetSwapInterval(interval);
		applied = interval;
	}
#endif
	NXPERF_SCOPE(swap_ms, nullptr);
	SDL_GL_SwapWindow(Priv::window);
}


void ProcessSDLWindowEvent(const SDL_WindowEvent &event)
{
	switch (event.event)
	{
	extern bool AppActive;

	case SDL_WINDOWEVENT_FOCUS_GAINED:
		S_SetSoundPaused(1);
		AppActive = true;
		break;

	case SDL_WINDOWEVENT_FOCUS_LOST:
		S_SetSoundPaused(0);
		AppActive = false;
		break;

	case SDL_WINDOWEVENT_MOVED:
		if (!vid_fullscreen)
		{
			int top = 0, left = 0;
			SDL_GetWindowBordersSize(Priv::window, &top, &left, nullptr, nullptr);
			win_x = event.data1-left;
			win_y = event.data2-top;
		}
		break;

	case SDL_WINDOWEVENT_RESIZED:
		if (!vid_fullscreen && !Priv::fullscreenSwitch)
		{
			win_w = event.data1;
			win_h = event.data2;
		}
		break;

	case SDL_WINDOWEVENT_MAXIMIZED:
		win_maximized = true;
		break;

	case SDL_WINDOWEVENT_RESTORED:
		win_maximized = false;
		break;
	}
}


// each platform has its own specific version of this function.
void I_SetWindowTitle(const char* caption)
{
	if (caption)
	{
		SDL_SetWindowTitle(Priv::window, caption);
	}
	else
	{
		FString default_caption;
		default_caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());
		SDL_SetWindowTitle(Priv::window, default_caption.GetChars());
	}
}
