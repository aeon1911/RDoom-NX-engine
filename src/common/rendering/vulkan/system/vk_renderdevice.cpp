/*
** vk_renderdevice.cpp
**
** Vulkan backend
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include <cinttypes>
#include "gamestate.h"
#include "vulkan/renderer/vk_pprenderstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkansurface.h>

#include "c_dispatch.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_clock.h"
#include "nx_perf.h"
#include "i_time.h"
#include "filesystem.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "m_png.h"
#include "printf.h"
#include "v_draw.h"
#include "v_video.h"
#include "version.h"
#include "vk_renderdevice.h"
#ifdef __SWITCH__
#include "common/platform/posix/nx/rdoom_nx.h"
#endif
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_raytrace.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_asynctex.h"

#ifdef __SWITCH__
// half-resolution effects layer: pass writing its depth (NXFxDepthDownsample)
struct NXFxDepthPass
{
	std::unique_ptr<VulkanShader> VS, FS;
	std::unique_ptr<VulkanDescriptorSetLayout> SetLayout;
	std::unique_ptr<VulkanPipelineLayout> Layout;
	std::unique_ptr<VulkanRenderPass> Pass;
	std::unique_ptr<VulkanPipeline> Pipeline;
	std::unique_ptr<VulkanDescriptorPool> Pool;
	VkFormat Format = VK_FORMAT_UNDEFINED;
};
#endif

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, vk_raytrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) :
	Super(hMonitor, fullscreen)
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	SupportedDevices = builder.FindDevices(surface->Instance);
	device = builder.Create(surface->Instance);
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	VkAsyncTex_Shutdown();	// no worker may still decode into textures that go away below
	// make sure the GPU is no longer using any objects before RAII tears them down
	if (vkDeviceWaitIdle(device->device) == VK_ERROR_DEVICE_LOST && !VulkanDeviceWasLost)
	{
		VulkanDeviceWasLost = true;  // no exception here: this is a destructor
		if (VulkanDeviceLostCallback)
			VulkanDeviceLostCallback("vkDeviceWaitIdle at shutdown");
	}

#ifdef __SWITCH__
	mNXFxDepthPass.reset();
	mNXFxProbe[0].reset();
	mNXFxProbe[1].reset();
#endif
	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (device->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten(this));
#else
	mRenderState.reset(new VkRenderState(this));
#endif
	VkAsyncTex_Install(this);
}

void VulkanRenderDevice::Update()
{
	// wipes and the start screen present frames without BeginFrame: they
	// write the per-frame buffers without a region of their own
	if (!mFrameBegun)
		mCommands->WaitForPendingFrame();
	mFrameBegun = false;
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	mCommands->MarkGpu(VkCommandBufferManager::GpuPostEnd);
	{
		NXPERF_SCOPE(draw2d_ms, nullptr);
		Draw2D();
		twod->Clear();
	}

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	mCommands->WaitForCommands(true);
	mCommands->UpdateGpuStats();

	Super::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->View.get(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), GetBuffers()->SceneColorFormat, GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

#ifdef __SWITCH__
CVAR(Bool, rd_vk_earlysubmit, true, 0)
// Dynamic resolution of the 3D view: the scene is drawn into a smaller part of
// the render target when the GPU needs more than rd_dynres_target ms per frame
// (Brutal Doom explosions: dozens of large translucent smoke sprites), then
// stretched back before the weapon and HUD, which stay at full resolution.
CVAR(Bool, rd_dynres, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, rd_dynres_target, 12.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, rd_dynres_min, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, rd_dynres_force, 0.0f, 0)	// tests: fixed 3D scale (0 = automatic)
CVAR(Bool, rd_dynres_blit, false, 0)	// tests: stretch with vkCmdBlitImage instead of a draw
EXTERN_CVAR(Int, rd_gpubench)

void VulkanRenderDevice::UpdateDynamicResolution()
{
	if (rd_dynres_force > 0)
	{
		mDynScale = clamp((float)rd_dynres_force, 0.25f, 1.0f);
		return;
	}
	const float minScale = clamp((float)rd_dynres_min, 0.4f, 1.0f);
	if (!rd_dynres || rd_gpubench > 0 || gamestate != GS_LEVEL)
	{
		mDynScale = 1.0f;
		mDynCalmFrames = 0;
		mDynGpuSamples = 0;	// a new map starts its own average (the Lexicon hub's must not carry over)
		mDynAvgFrame = 1000.0 / 60.0;
		mDynAvgWait = 0;
		return;
	}
	// Drop: the previous frame missed vsync and this one had to wait for the
	// GPU (frame N-2) before it could start. (The GPU timestamps include the
	// gaps between a frame's submits and overstate the load.)
	// Rise: 45 frames without a missed vsync. The wait for frame N-2 is no
	// sign of GPU load here: with vsync it also contains the display queue
	// (1-2 ms per frame at 60% scale and 10 ms of GPU on the Switch Lite,
	// which kept the scale at 60% for whole sessions). A scale that misses
	// vsync right after a rise is not tried again for 20 seconds.
	const double budget = 1000.0 / 60.0;
	// a frame held back by the tic rate (menu, pause) says nothing about the GPU
	const double frameMs = NXPerf_LastTicWaitMS() > 8.0 ? budget : NXPerf_LastFrameMS();
	const double waitMs = nxperf.vkpending_ms;	// this frame's wait at BeginFrame
	mDynFramesSinceRise++;
	if (mDynCeilingFrames > 0 && --mDynCeilingFrames == 0)
		mDynCeiling = 1.0f;

	// GPU timestamps (two frames old, averaged). The frame time alone kept the
	// view at 60% for whole maps on the console (2026-10-13 test): with vsync
	// a frame swings between 16 and 18 ms, so 45 frames in a row under 17.3 ms
	// never came and any 18.2 ms frame dropped the scale again, while the GPU
	// needed ~10 of the 16.7 ms. The timestamps include the gaps between a
	// frame's submits (they overstate the load), which only makes rises later.
	double gpuFrame, gpuScene;
	if (mCommands->TakeGpuFrameTime(gpuFrame, gpuScene) && gpuFrame > 0.5)
	{
		// plain mean of the first samples after a change, then a running average
		const double k = mDynGpuSamples < 10 ? 1.0 / (mDynGpuSamples + 1) : 0.1;
		mDynGpuFrame += (gpuFrame - mDynGpuFrame) * k;
		mDynGpuScene += (gpuScene - mDynGpuScene) * k;
		mDynGpuSamples = min(mDynGpuSamples + 1, 1000);
		mDynGpuSeen = true;
	}
	// Once the console has delivered timestamps the frame-time rules are never
	// used again: they ran while the average settled after each change (20
	// frames) and, with the CPU over budget (slaughter map, 1000+ monsters,
	// 20 ms frames), dropped every rise right back to 60% (2026-10-13 test).
	// 8 samples at the new scale: with 30 the Lexicon hub (GPU 18-23 ms at
	// full scale) spent its first seconds at 22 fps stepping down from 100%
	// Sustained averages (~20 frames). Ashes 2063 on the console ran at 46-53
	// fps with the CPU waiting 13-15 ms per frame for the GPU while the
	// timestamps said 12-13 ms of GPU: they do not see all of its load, so a
	// frame rate that stays below 60 with long waits for the GPU also drops
	// the scale, and the scale only rises while the game holds 60 fps.
	// Freedoom 2 MAP01 on the console (1.0.0) sat at 54-57 fps for whole
	// minutes, CPU waiting 13-14 ms for the GPU, timestamps 11 ms: between the
	// old drop (18.7 ms) and rise (17.5 ms) averages nothing moved the scale.
	// Now under ~57.7 fps with long waits drops it; a rise needs ~58.8 fps.
	// The sustained wait also counts the last frame's acquire (no swapchain
	// image free yet: the GPU is behind too): the Lexicon slaughter pack ran
	// at 52 fps with 4.9 ms at frame start and the old 6 ms limit (console,
	// engine 6926966ab); CPU-bound maps wait ~1 ms in all.
	// one loading hitch (0.5 s for a new texture) must not look like 20 slow frames
	mDynAvgFrame += (min(frameMs, budget * 2.0) - mDynAvgFrame) * 0.05;
	mDynAvgWait += (waitMs + NXPerf_LastAcquireMS() - mDynAvgWait) * 0.05;
	if (mDynGpuSeen && mDynGpuSamples < 8)
		return;
	if (mDynGpuSamples >= 8)
	{
		// the 3D view grows with its pixel count; the rest of the frame does not
		auto predict = [&](float scale) { return mDynGpuFrame + mDynGpuScene * ((scale * scale) / (mDynScale * mDynScale) - 1.0); };
		const bool gpuBound = (frameMs > budget + 1.5 && waitMs > 2.5 && mDynGpuFrame > budget * 0.9) ||
			(mDynAvgFrame > budget * 1.04 && mDynAvgWait > 4.0);
		if (gpuBound && mDynScale > minScale + 0.001f)
		{
			if (mDynFramesSinceRise < 120)
			{
				mDynCeiling = mDynScale - 0.05f;
				mDynCeilingFrames = 1200;
			}
			// what the 3D view may cost to fit 85% of the frame
			const double sceneBudget = budget * 0.85 - (mDynGpuFrame - mDynGpuScene);
			float s = sceneBudget > 0.5 ? mDynScale * (float)sqrt(sceneBudget / max(mDynGpuScene, 0.5)) : minScale;
			s = max(s, mDynScale * 0.6f);
			mDynScale = clamp(floorf(s * 20.0f) / 20.0f, minScale, mDynScale - 0.05f);
			mDynCalmFrames = 0;
			mDynGpuSamples = 0;	// the average restarts at the new scale
			mDynAvgFrame = budget;	// and so do the frame and wait averages
			mDynAvgWait = 0;
		}
		else if (mDynScale < 1.0f && mDynAvgFrame < budget * 1.02 && predict(min(1.0f, mDynScale + 0.05f)) < budget * 0.85)
		{
			if (++mDynCalmFrames >= 45)
			{
				mDynCalmFrames = 0;
				const float next = min(1.0f, mDynScale + 0.1f);
				if (next <= mDynCeiling + 0.001f && predict(next) < budget * 0.85)
					mDynScale = next;
				else if (mDynScale + 0.05f <= mDynCeiling + 0.001f)
					mDynScale += 0.05f;
				else
					return;
				mDynFramesSinceRise = 0;
				mDynGpuSamples = 0;
			}
		}
		else
			mDynCalmFrames = 0;
		return;
	}

	// no timestamps (Ryujinx reads 0): frame time only
	if (frameMs > budget + 1.5 && waitMs > 2.5)
	{
		if (mDynFramesSinceRise < 120)
		{
			mDynCeiling = mDynScale - 0.05f;
			mDynCeilingFrames = 1200;
		}
		float s = mDynScale * (float)sqrt(budget / frameMs);
		s = max(s, mDynScale * 0.8f);	// at most 20% per step
		mDynScale = clamp(floorf(s * 20.0f) / 20.0f, minScale, 1.0f);
		mDynCalmFrames = 0;
	}
	else if (frameMs < budget + 0.6)
	{
		if (mDynScale < 1.0f && ++mDynCalmFrames >= 45)
		{
			mDynCalmFrames = 0;
			const float next = min(1.0f, mDynScale + 0.1f);
			if (next <= mDynCeiling + 0.001f)
			{
				mDynScale = next;
				mDynFramesSinceRise = 0;
			}
			else if (mDynScale + 0.05f <= mDynCeiling + 0.001f)
			{
				mDynScale += 0.05f;
				mDynFramesSinceRise = 0;
			}
		}
	}
	else
		mDynCalmFrames = 0;
}

void VulkanRenderDevice::PrecacheUsedTextures()
{
	if (mRenderPassManager)
		mRenderPassManager->PrecacheListedTextures();
}

void VulkanRenderDevice::ScaleSceneViewport()
{
	mSceneScaled = false;
	nxperf.scene_scale = mDynScale;
	if (mDynScale >= 0.999f || mSceneViewport.width < 64 || mSceneViewport.height < 64)
		return;
	mSceneFull = mSceneViewport;
	mSceneSmall = mSceneViewport;
	mSceneSmall.width = max(32, (int)(mSceneFull.width * mDynScale));
	mSceneSmall.height = max(32, (int)(mSceneFull.height * mDynScale));
	mSceneViewport = mSceneSmall;
	mSceneScaled = true;
}
#else
void VulkanRenderDevice::UpdateDynamicResolution() {}
void VulkanRenderDevice::ScaleSceneViewport() {}
void VulkanRenderDevice::PrecacheUsedTextures() {}
#endif

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	mCommands->MarkGpu(VkCommandBufferManager::GpuSceneEnd);
#ifdef __SWITCH__
	// Hand the finished 3D scene to the GPU now. Only the end-of-frame submit
	// waits for a free swapchain image; with the whole frame in that submit the
	// scene itself waited for the display (vsync) and the GPU sat idle while
	// the CPU recorded the frame (console perf.log: GPU 9-13 ms per frame, yet
	// 24-31 ms frames).
	if (rd_vk_earlysubmit)
		mCommands->FlushCommands(false);
#endif
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
#ifdef __SWITCH__
	if (mSceneScaled)
	{
		mSceneScaled = false;
		if (rd_dynres_blit)
			mPostprocess->UpscaleScene(mSceneSmall, mSceneFull);
		else
		{
			VkPPRenderState renderstate(this);
			hw_postprocess.upscale.Render(&renderstate, mSceneFull);	// reads mSceneViewport (still the small one)
		}
		mSceneViewport = mSceneFull;
	}
#endif
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

const char* VulkanRenderDevice::DeviceName() const
{
	return device->PhysicalDevice.Properties.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IVertexBuffer *VulkanRenderDevice::CreateVertexBuffer()
{
	return GetBufferManager()->CreateVertexBuffer();
}

IIndexBuffer *VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

IDataBuffer *VulkanRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return GetBufferManager()->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	// perf.log "begin" breakdown: wait / cache save / buffers / sprites
	double t = NXPerf_NowMS(), t2;
	auto lap = [&](double &acc) { t2 = NXPerf_NowMS(); acc += t2 - t; t = t2; };

	// the frame's region of the per-frame buffers is rewritten from the start
	// below; only the frame that used the same region (two frames ago) must be done
	mCommands->WaitForFrameSlot();
	mFrameBegun = true;
	lap(nxperf.begin_wait_ms);
	mRenderPassManager->SaveCacheWhenIdle();
	lap(nxperf.begin_save_ms);
#ifdef __SWITCH__
	UpdateDynamicResolution();
#endif
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
	lap(nxperf.begin_buffers_ms);
	VkAsyncTex_NewFrame();
	lap(nxperf.begin_sprites_ms);
}

void VulkanRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GetTextureManager()->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void VulkanRenderDevice::Draw2D()
{
	::Draw2D(twod, *mRenderState);
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

int VulkanRenderDevice::PerFrameSlots() const
{
#ifdef __SWITCH__
	return 2;
#else
	return 1;
#endif
}

int VulkanRenderDevice::PerFrameSlot() const
{
	return mCommands ? mCommands->FrameSlot() : 0;
}

void VulkanRenderDevice::SyncNextFrame()
{
	if (mCommands)
		mCommands->SyncNextFrame();
}

void VulkanRenderDevice::WaitForPendingFrame()
{
	mCommands->WaitForPendingFrame();
	// called while a level loads: create the pipelines this game used before
	if (mRenderPassManager)
		mRenderPassManager->WarmupPipelines();
	NXWarmHalfEffects();
}

#ifdef __SWITCH__
EXTERN_CVAR(Bool, rd_vk_zcull)
bool VulkanRenderDevice::AllowSSAO()
{
	return rd_vk_zcull;  // depth buffers are attachment-only (no ZCULL) otherwise
}

//==========================================================================
//
// Half-resolution effects layer (HWDrawInfo::NXRenderHalfEffects)
//
// Begin: the scene depth (its dynamic-resolution viewport) is drawn at half
// size into the layer's depth buffer, the layer is cleared to (0,0,0,1) and
// becomes the render target with the FXLAYER pipelines. End: the layer is
// blended over the scene colour with a post-processing draw, as SSAO does,
// before the weapon and HUD.
//
//==========================================================================

EXTERN_CVAR(Bool, rd_vk_halfeffects)
// tests: 1 composite without blending (shows the layer), 2 no depth test in
// the layer, 3 layer cleared to red (checks the composite), 4 layer depth
// cleared to far, 5 depth pass writes far, 9 shows the depth copy, 10 the
// scene colour through the depth copy (checks the mapping in Ryujinx, which
// cannot sample NVK's depth views: "Invalid texture format 0x2512B")
CVAR(Int, rd_halfeffects_debug, 0, 0)

bool VulkanRenderDevice::NXHalfEffectsAvailable()
{
	// its shaders come with this build's uzdoom.pk3: an SD card with only the
	// new RDoom.nro copied over must not end in a fatal error at map start
	if (mNXFxDepthState == 0 && !mNXFxShadersChecked)
	{
		mNXFxShadersChecked = true;
		for (const char *lump : { "shaders/pp/nxdepth.fp", "shaders/pp/upscale.fp" })
		{
			if (fileSystem.CheckNumForFullName(lump) < 0)
			{
				mNXFxDepthState = -1;
				FString n;
				n.Format("off (%s missing: uzdoom.pk3 older than RDoom.nro, copy the whole package)", lump);
				RDoom_SessionSet("vulkan_half_effects", n.GetChars());
				break;
			}
		}
	}
	return rd_vk_halfeffects && mNXFxDepthState >= 0 && mScreenBuffers && GetBuffers() == mScreenBuffers.get() && mScreenBuffers->NXFxSupported();
}

// The layer's depth: the scene depth copied at half size into an R32F texture
// by a post-processing draw (read as SSAO reads it), then written into the
// layer's depth buffer by a full-screen triangle through gl_FragDepth.
// vkCmdBlitImage of the depth, or sampling the depth texture in this
// depth-only pass, left Ryujinx with zero depth (every effect hidden), so
// neither could be checked off the console; both steps here can.

bool VulkanRenderDevice::NXFxDepthPassReady()
{
	const VkFormat format = GetBuffers()->SceneDepthStencilFormat;
	if (mNXFxDepthPass && mNXFxDepthPass->Format == format)
		return true;
	if (mNXFxDepthPass)
		mCommands->WaitForCommands(false);
	try
	{
		auto p = std::make_unique<NXFxDepthPass>();
		p->Format = format;
		p->VS = ShaderBuilder()
			.Type(ShaderType::Vertex)
			.AddSource("nxfxdepth.vp",
				"#version 450\n"
				"layout(push_constant) uniform NXRect { vec4 rect; vec4 mode; };\n"	// uv scale xy, offset zw; mode.x: tests
				"layout(location=0) out vec2 uv;\n"
				"layout(location=1) flat out int dbg;\n"
				"void main() {\n"
				"	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
				"	uv = rect.zw + p * rect.xy;\n"
				"	dbg = int(mode.x);\n"
				"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
				"}\n")
			.DebugName("nxfxdepth.vp")
			.Create("nxfxdepth.vp", device.get());
		p->FS = ShaderBuilder()
			.Type(ShaderType::Fragment)
			.AddSource("nxfxdepth.fp",
				"#version 450\n"
				"layout(binding=0) uniform sampler2D DepthCopy;\n"
				"layout(location=0) in vec2 uv;\n"
				"layout(location=1) flat in int dbg;\n"
				"void main() {\n"
				"	ivec2 ipos = ivec2(max(uv * vec2(textureSize(DepthCopy, 0)), vec2(0.0)));\n"
				"	float d = texelFetch(DepthCopy, ipos, 0).r;\n"
				"	if (dbg == 5) d = 1.0;\n"

				"	gl_FragDepth = d;\n"
				"}\n")
			.DebugName("nxfxdepth.fp")
			.Create("nxfxdepth.fp", device.get());
		p->SetLayout = DescriptorSetLayoutBuilder()
			.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
			.DebugName("NXFxDepth.SetLayout")
			.Create(device.get());
		p->Layout = PipelineLayoutBuilder()
			.AddSetLayout(p->SetLayout.get())
			.AddPushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 8)
			.DebugName("NXFxDepth.Layout")
			.Create(device.get());
		// depth fully rewritten, stencil cleared to 0 for the layer's draws;
		// the depth load op is not CLEAR, so NVK enables no ZCULL here
		p->Pass = RenderPassBuilder()
			.AddDepthStencilAttachment(format, VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
				VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
			.AddSubpass()
			.AddSubpassDepthStencilAttachmentRef(0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
			.AddExternalSubpassDependency(
				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
			.DebugName("NXFxDepth.Pass")
			.Create(device.get());
		{
			std::lock_guard<std::mutex> cacheLock(mRenderPassManager->CacheMutex());
			p->Pipeline = GraphicsPipelineBuilder()
				.Cache(mRenderPassManager->GetCache())
				.AddVertexShader(p->VS.get())
				.AddFragmentShader(p->FS.get())
				.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
				.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT)
				.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR)
				.Viewport(0.0f, 0.0f, 320.0f, 200.0f)
				.Scissor(0, 0, 320, 200)
				.Cull(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
				.DepthStencilEnable(true, true, false)
				.DepthFunc(VK_COMPARE_OP_ALWAYS)
				.RasterizationSamples(VK_SAMPLE_COUNT_1_BIT)
				.Layout(p->Layout.get())
				.RenderPass(p->Pass.get())
				.DebugName("NXFxDepth.Pipeline")
				.Create(device.get());
		}
		// a set per frame (the scene depth view changes with the buffers)
		p->Pool = DescriptorPoolBuilder()
			.Flags(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
			.MaxSets(16)
			.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16)
			.DebugName("NXFxDepth.Pool")
			.Create(device.get());
		mNXFxDepthPass = std::move(p);
	}
	catch (const std::exception &e)
	{
		mNXFxDepthPass.reset();
		FString n;
		n.Format("off (depth pass: %s)", e.what());
		RDoom_SessionSet("vulkan_half_effects", n.GetChars());
		return false;
	}
	return true;
}

bool VulkanRenderDevice::NXFxDepthDownsample(VkTextureImage *depth, const IntRect &sv)
{
	if (!NXFxDepthPassReady())
		return false;
	auto &p = *mNXFxDepthPass;
	auto buffers = GetBuffers();
	auto set = p.Pool->tryAllocate(p.SetLayout.get());
	if (!set)
		return false;

	// 1. scene depth -> R32F copy at the layer's size (a post-processing draw
	//    reading the depth texture as SSAO does)
	{
		VkPPRenderState renderstate(this);
		hw_postprocess.nxeffects.CopyDepth(&renderstate, mNXFxRect, rd_halfeffects_debug == 10);
	}
	VkTextureImage *copy = mTextureManager->GetTexture(PPTextureType::PPTexture, hw_postprocess.nxeffects.DepthCopy());
	if (mNXFxDepthState == 0)
		NXFxCheckDepth(copy);

	// 2. copy -> layer depth buffer through gl_FragDepth
	WriteDescriptors()
		.AddCombinedImageSampler(set.get(), 0, copy->View.get(), mSamplerManager->Get(PPFilterMode::Nearest, PPWrapMode::Clamp), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		.Execute(device.get());
	const int w = depth->Image->width, h = depth->Image->height;
	if (!depth->PPFramebuffer)	// freed with the image (VkTextureImage::Reset)
	{
		depth->PPFramebuffer = FramebufferBuilder()
			.RenderPass(p.Pass.get())
			.AddAttachment(depth->View.get())
			.Size(w, h)
			.DebugName("NXFxDepth.Framebuffer")
			.Create(device.get());
	}

	auto cmd = mCommands->GetDrawCommands();
	VkImageTransition()
		.AddImage(&buffers->SceneDepthStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false)
		.AddImage(copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.AddImage(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true)
		.Execute(cmd);
	RenderPassBegin()
		.RenderPass(p.Pass.get())
		.RenderArea(0, 0, w, h)
		.Framebuffer(depth->PPFramebuffer.get())
		.AddClearDepthStencil(1.0f, 0)
		.Execute(cmd);
	cmd->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.Pipeline.get());
	cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, p.Layout.get(), 0, set.get());
	VkViewport viewport = { (float)mNXFxRect.left, (float)mNXFxRect.top, (float)mNXFxRect.width, (float)mNXFxRect.height, 0.0f, 1.0f };
	VkRect2D scissor = { { mNXFxRect.left, mNXFxRect.top }, { (uint32_t)mNXFxRect.width, (uint32_t)mNXFxRect.height } };
	cmd->setViewport(0, 1, &viewport);
	cmd->setScissor(0, 1, &scissor);
	const float rect[8] = { mNXFxRect.width / (float)w, mNXFxRect.height / (float)h,
		mNXFxRect.left / (float)w, mNXFxRect.top / (float)h, (float)rd_halfeffects_debug, 0.0f, 0.0f, 0.0f };
	cmd->pushConstants(p.Layout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
	cmd->draw(3, 1, 0, 0);
	cmd->endRenderPass();
	mCommands->DrawDeleteList->Add(std::move(set));
	return true;
}

void VulkanRenderDevice::NXFxCheckDepth(VkTextureImage *copy)
{
	// this slot's previous frame is finished (BeginFrame waited for it)
	const int slot = mCommands->FrameSlot() & 1;
	if (mNXFxProbePending[slot])
	{
		mNXFxProbePending[slot] = false;
		const float *v = (const float *)mNXFxProbe[slot]->Map(0, 48 * sizeof(float));
		bool depth = false;
		for (int i = 0; i < 48; i++)
			if (v[i] > 0.0f && v[i] <= 1.0f)
				depth = true;
		mNXFxProbe[slot]->Unmap();
		if (depth)
		{
			mNXFxDepthState = 1;
			RDoom_SessionSet("vulkan_half_effects_depth", "ok (scene depth read back)");
			return;
		}
		// real geometry is never at depth 0 (the near plane)
		if (++mNXFxZeroChecks >= 3)
		{
			mNXFxDepthState = -1;
			RDoom_SessionSet("vulkan_half_effects", "off (the scene depth reads back as zero here; effects drawn at full resolution)");
			RDoom_SessionSet("vulkan_half_effects_depth", "zero in 3 frames");
			return;
		}
	}
	if (!mNXFxProbe[slot])
	{
		mNXFxProbe[slot] = BufferBuilder()
			.Size(48 * sizeof(float))
			.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
			.DebugName("NXFxProbe")
			.Create(device.get());
	}
	// three rows of 16 texels across the middle of the view
	VkBufferImageCopy regions[3] = {};
	for (int i = 0; i < 3; i++)
	{
		regions[i].bufferOffset = i * 16 * sizeof(float);
		regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		regions[i].imageSubresource.layerCount = 1;
		regions[i].imageOffset = { mNXFxRect.left + max(0, mNXFxRect.width / 2 - 8), mNXFxRect.top + mNXFxRect.height * (i + 1) / 4, 0 };
		regions[i].imageExtent = { (uint32_t)min(16, mNXFxRect.width), 1, 1 };
	}
	auto cmd = mCommands->GetDrawCommands();
	VkImageTransition().AddImage(copy, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false).Execute(cmd);
	cmd->copyImageToBuffer(copy->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mNXFxProbe[slot]->buffer, 3, regions);
	mNXFxProbePending[slot] = true;
}

bool VulkanRenderDevice::NXBeginHalfEffects(FRenderState &state)
{
	if (!NXHalfEffectsAvailable() || state.GetPassType() != NORMAL_PASS)
		return false;
	auto buffers = GetBuffers();
	const int lw = (buffers->GetWidth() + 1) / 2, lh = (buffers->GetHeight() + 1) / 2;
	PPTexture *layer = hw_postprocess.nxeffects.Layer(lw, lh);
	VkTextureImage *depth = buffers->GetNXFxDepth(lw, lh);
	if (!depth)
		return false;
	VkTextureImage *color = mTextureManager->GetTexture(PPTextureType::PPTexture, layer);
	const VkFormat colorFormat = mTextureManager->GetTextureFormat(layer);

	IntRect sv = mSceneViewport;
	sv.left = clamp(sv.left, 0, buffers->GetWidth() - 1);
	sv.top = clamp(sv.top, 0, buffers->GetHeight() - 1);
	sv.width = clamp(sv.width, 1, buffers->GetWidth() - sv.left);
	sv.height = clamp(sv.height, 1, buffers->GetHeight() - sv.top);
	mNXFxRect.left = sv.left / 2;
	mNXFxRect.top = sv.top / 2;
	mNXFxRect.width = clamp((sv.width + 1) / 2, 1, lw - mNXFxRect.left);
	mNXFxRect.height = clamp((sv.height + 1) / 2, 1, lh - mNXFxRect.top);

	mRenderState->EndRenderPass();
	if (rd_halfeffects_debug == 4)
	{
		VkImageTransition().AddImage(depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true).Execute(mCommands->GetDrawCommands());
		VkClearDepthStencilValue cv = { 1.0f, 0 };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
		mCommands->GetDrawCommands()->clearDepthStencilImage(depth->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
		VkImageTransition().AddImage(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false).Execute(mCommands->GetDrawCommands());
	}
	else if (!NXFxDepthDownsample(depth, sv))
		return false;
	VkImageTransition()
		.AddImage(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true)
		.Execute(mCommands->GetDrawCommands());

	// colour cleared when the first effect is drawn (stencil: 0 from the depth pass)
	memcpy(mNXFxClearSave, mSceneClearColor, sizeof(mNXFxClearSave));
	mSceneClearColor[0] = mSceneClearColor[1] = mSceneClearColor[2] = 0.0f;
	mSceneClearColor[3] = 1.0f;
	if (rd_halfeffects_debug == 3)
	{
		mSceneClearColor[0] = 1.0f;
		mSceneClearColor[3] = 0.5f;
	}
	mRenderState->SetRenderTarget(color, depth->View.get(), lw, lh, colorFormat, VK_SAMPLE_COUNT_1_BIT);
	mRenderState->Clear(CT_Color | CT_Stencil);
	mRenderState->NXSetFxLayer(true);
	mRenderState->SetViewport(mNXFxRect.left, mNXFxRect.top, mNXFxRect.width, mNXFxRect.height);
	mRenderState->SetScissor(mNXFxRect.left, mNXFxRect.top, mNXFxRect.width, mNXFxRect.height);
	if (rd_halfeffects_debug == 2)
		mRenderState->EnableDepthTest(false);
	return true;
}

void VulkanRenderDevice::NXEndHalfEffects(FRenderState &state)
{
	mRenderState->EndRenderPass();
	const bool drawn = mRenderState->NXFxDrawn();
	mRenderState->NXSetFxLayer(false);
	mRenderState->Clear(0);
	if (rd_halfeffects_debug == 2)
		mRenderState->EnableDepthTest(true);
	memcpy(mSceneClearColor, mNXFxClearSave, sizeof(mNXFxClearSave));
	SetSceneRenderTarget(false);
	if (drawn || rd_halfeffects_debug >= 9)
	{
		VkPPRenderState renderstate(this);
		hw_postprocess.nxeffects.Composite(&renderstate, mNXFxRect, rd_halfeffects_debug);
	}
	const IntRect &sv = mSceneViewport;
	mRenderState->SetViewport(sv.left, sv.top, sv.width, sv.height);
	mRenderState->SetScissor(sv.left, sv.top, sv.width, sv.height);
}

// At level load: create what the first frame with smoke would otherwise
// create in the middle of a fight (each a hitch of 20+ ms on the Switch):
// the layer images, the composite shader and pipeline, the depth pass.
void VulkanRenderDevice::NXWarmHalfEffects()
{
	if (mNXFxWarmed || !NXHalfEffectsAvailable() || !mShaderManager || !mShaderManager->AllCompiled())
		return;
	mNXFxWarmed = true;
	const uint64_t start = I_msTime();
	auto buffers = mScreenBuffers.get();
	const int lw = (buffers->GetWidth() + 1) / 2, lh = (buffers->GetHeight() + 1) / 2;
	PPTexture *layer = hw_postprocess.nxeffects.Layer(lw, lh);
	mTextureManager->GetTexture(PPTextureType::PPTexture, layer);
	if (!buffers->GetNXFxDepth(lw, lh))
		return;

	VkPPRenderPassKey key;
	key.Shader = mShaderManager->GetVkShader(&hw_postprocess.nxeffects.Shader);
	key.Uniforms = sizeof(ExtractUniforms);
	key.InputTextures = 1;
	key.BlendMode = PPBlendMode();
	PPNXEffects::Blend(key.BlendMode);
	key.OutputFormat = buffers->SceneColorFormat;
	key.SwapChain = 0;
	key.ShadowMapBuffers = 0;
	key.StencilTest = WhichDepthStencil::Scene;
	key.Samples = buffers->GetSceneSamples();
	mRenderPassManager->GetPPRenderPass(key);
	key.Shader = mShaderManager->GetVkShader(&hw_postprocess.nxeffects.DepthShader);
	key.BlendMode = PPBlendMode();
	key.BlendMode.BlendOp = STYLEOP_Add;
	key.BlendMode.SrcAlpha = STYLEALPHA_One;
	key.BlendMode.DestAlpha = STYLEALPHA_Zero;
	key.BlendMode.Flags = 0;
	key.OutputFormat = VK_FORMAT_R32_SFLOAT;
	key.StencilTest = WhichDepthStencil::None;
	key.Samples = VK_SAMPLE_COUNT_1_BIT;
	mRenderPassManager->GetPPRenderPass(key);
	mTextureManager->GetTexture(PPTextureType::PPTexture, hw_postprocess.nxeffects.DepthCopy());

	NXFxDepthPassReady();

	FString n;
	n.Format("layer %dx%d, composite and depth pass prepared in %d ms", lw, lh, (int)(I_msTime() - start));
	RDoom_SessionSet("vulkan_half_effects_warmup", n.GetChars());
}
#else
void VulkanRenderDevice::NXWarmHalfEffects() {}
#endif

unsigned int VulkanRenderDevice::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

#ifdef __SWITCH__
EXTERN_CVAR(Bool, rd_vk_frameoverlap)
#endif

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = device->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
#ifdef __SWITCH__
	{
		FString d;
		d.Format("%s (api %s, driver %s)", props.deviceName, apiVersion.GetChars(), driverVersion.GetChars());
		RDoom_SessionSet("vulkan_device", d.GetChars());
		RDoom_SessionSet("renderer_active", "vulkan");

		// memory types decide whether CPU writes need dcache cleaning (HOST_CACHED)
		FString mt;
		const auto &mem = device->PhysicalDevice.Properties.Memory;
		for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
		{
			VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
			mt.AppendFormat("%s%u:%s%s%s%s", i ? ", " : "", i,
				(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "L" : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "V" : "",
				(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "Co" : "", (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "Ca" : "");
		}
		mt += " (L=device local, V=host visible, Co=coherent, Ca=cached)";
		RDoom_SessionSet("vulkan_memory_types", mt.GetChars());
		RDoom_SessionSet("vulkan_frame_overlap", rd_vk_frameoverlap ? "on" : "off");

		// vertex formats the renderer uses (models: packed normals, bone ids)
		static const struct { VkFormat f; const char *n; } vf[] = {
			{ VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F" }, { VK_FORMAT_R32G32B32_SFLOAT, "RGB32F" },
			{ VK_FORMAT_R32G32_SFLOAT, "RG32F" }, { VK_FORMAT_R32_SFLOAT, "R32F" }, { VK_FORMAT_R8G8B8A8_UNORM, "RGBA8" },
			{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, "A2B10G10R10_SNORM" }, { VK_FORMAT_R8G8B8A8_UINT, "RGBA8UI" } };
		FString fs;
		for (auto &v : vf)
		{
			VkFormatProperties fp = {};
			vkGetPhysicalDeviceFormatProperties(device->PhysicalDevice.Device, v.f, &fp);
			fs.AppendFormat("%s%s=%s", fs.IsEmpty() ? "" : ", ", v.n, (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "ok" : "NO");
		}
		RDoom_SessionSet("vulkan_vertex_formats", fs.GetChars());
	}
#endif
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh)
{
	mRaytrace->SetLevelMesh(mesh);
}

void VulkanRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), GetBuffers()->SceneColorFormat, GetBuffers()->GetSceneSamples());
}

bool VulkanRenderDevice::RaytracingEnabled()
{
	return vk_raytrace && device->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}
