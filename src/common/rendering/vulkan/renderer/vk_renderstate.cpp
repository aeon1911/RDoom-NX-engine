/*
** vk_renderstate.cpp
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

#include "vk_renderstate.h"
#include "vulkan/system/vk_renderdevice.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"

#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"

#ifdef __SWITCH__
// The GPU starts on the first draws of a frame while the CPU still records the
// rest (the frame's buffers only grow during a frame, so earlier submits stay valid).
CVAR(Int, vk_submit_size, 300, 0);
#else
CVAR(Int, vk_submit_size, 1000, 0);
#endif
EXTERN_CVAR(Bool, r_skipmats)

VkRenderState::VkRenderState(VulkanRenderDevice* fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb)
{
	Reset();
}

void VkRenderState::ClearScreen()
{
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Apply(DT_TriangleStrip);
	mCommandBuffer->draw(4, 1, FFlatVertexBuffer::FULLSCREEN_INDEX, 0);
}

#ifdef __SWITCH__
// GPU-hang diagnostics: with vk_submit_size 1 and NVK_DEBUG=push_sync every draw
// is submitted and waited for on its own; each recorded draw leaves a line in
// logs/boot.log first, so after a hang the last line names the draw that hung.
CVAR(Int, rd_vk_drawlog, 0, 0)
// experiments for the GPU hang on the first 3D draw (hardware only):
// rd_vk_nofans 1 = triangle fans drawn as indexed triangle lists
// rd_vk_zstate 1 = no depth/stencil writes, 2 = no depth/stencil tests at all
// Hardware results 2026-10-11: triangle fans and depth bias both hang the
// GM20B under NVK (fifo idle timeout); lists and plain states do not.
CVAR(Bool, rd_vk_nofans, true, 0)
CVAR(Int, rd_vk_zstate, 0, 0)
// rd_vk_depthbias 0: depth bias emulated by compressing the viewport depth
// range for biased draws ([0,1-eps] closer, [eps,1] farther); 1: native bias
CVAR(Bool, rd_vk_depthbias, false, 0)
CVAR(Float, rd_vk_depthbias_eps, 1.0f / 8192.0f, 0)
// 2D images with the default material use the "simple2d" effect shader:
// main.fp's 2D path without the 3D material/light code (Project Brutality's
// visor on the console: ~1.9 ms of GPU per screen of 2D with the full shader)
CVAR(Bool, rd_vk_simple2d, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
int rd_vk_drawlog_burst = 0;
int rd_vk_drawlog_started = 0;
extern int nx_levelframes;
void RDoom_Crumb(const char *what);

static void LogDraw(const VkPipelineKey &key, FMaterial *mat, int lightIndex, const char *kind, int index, int count)
{
	static int logged;
	// 3D draws (depth or stencil in use), also during the screen wipe into a level
	if (rd_vk_drawlog <= 0 || (!key.DepthTest && !key.StencilTest && !logged))
		return;
	// the first 3000 draws from the first 3D draw on, then 50 after every new pipeline
	if (logged >= 3000)
	{
		if (rd_vk_drawlog_burst <= 0 || logged >= 20000)
			return;
		rd_vk_drawlog_burst--;
	}
	logged++;
	extern int rd_vk_drawlog_started;
	rd_vk_drawlog_started = 1;
	char buf[300];
	snprintf(buf, sizeof(buf), "draw %d (level frame %d): %s first %d count %d | vfmt %d effect %d special %d alphatest %d drawtype %d layers %d depth %d/%d stencil %d light %d | %s",
		logged, nx_levelframes, kind, index, count, key.VertexFormat, key.EffectState, key.SpecialEffect, key.AlphaTest, key.DrawType,
		key.NumTextureLayers, key.DepthTest, key.DepthWrite, key.StencilTest, lightIndex,
		mat && mat->Source() ? mat->Source()->GetName().GetChars() : "(no texture)");
	RDoom_Crumb(buf);
}
#define RD_LOGDRAW(kind, index, count) LogDraw(mPipelineKey, mMaterial.mMaterial, mLightIndex, kind, index, count)
#else
#define RD_LOGDRAW(kind, index, count)
#endif

void VkRenderState::Draw(int dt, int index, int count, bool apply)
{
#ifdef __SWITCH__
	if (rd_vk_nofans && dt == DT_TriangleFan && count >= 3 && count <= 999)
	{
		IIndexBuffer *oldIndexBuffer = mIndexBuffer;
		mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();
		if (apply || mNeedApply)
			Apply(DT_Triangles);
		else
			ApplyVertexBuffers();
		mCommandBuffer->drawIndexed((count - 2) * 3, 1, 0, index, 0);
		mIndexBuffer = oldIndexBuffer;
		RD_LOGDRAW("fan as triangles", index, count);
		return;
	}
#endif
	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->draw(count, 1, index, 0);
	RD_LOGDRAW("draw", index, count);
}

void VkRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);

	mCommandBuffer->drawIndexed(count, 1, index, 0, 0);
	RD_LOGDRAW("indexed", index, count);
}

bool VkRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mNeedApply = true;
	return lastValue;
}

void VkRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void VkRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void VkRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	int rr = r, gg = g, bb = b, aa = a;
	mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
	mNeedApply = true;
}

void VkRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = screen->stencilValue + offs;
	mStencilRefChanged = true;
	mStencilOp = op;

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
		mDepthWrite = !(flags & SF_DepthMaskOff);
	}

	mNeedApply = true;
}

void VkRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mNeedApply = true;
}

void VkRenderState::EnableClipDistance(int num, bool state)
{
}

void VkRenderState::Clear(int targets)
{
	mClearTargets = targets;
	EndRenderPass();
}

void VkRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void VkRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void VkRenderState::EnableMultisampling(bool on)
{
}

void VkRenderState::EnableLineSmooth(bool on)
{
}

void VkRenderState::Apply(int dt)
{
	drawcalls.Clock();

	mApplyCount++;
	if (mApplyCount >= vk_submit_size)
	{
		fb->GetCommands()->FlushCommands(false);
		mApplyCount = 0;
	}

	ApplyStreamData();
	ApplyMatrices();
	ApplyRenderPass(dt);
	ApplyScissor();
	ApplyViewport();
	ApplyStencilRef();
	ApplyDepthBias();
	ApplyPushConstants();
	ApplyVertexBuffers();
	ApplyHWBufferSet();
	ApplyMaterial();
	mNeedApply = false;

	drawcalls.Unclock();
}

void VkRenderState::ApplyDepthBias()
{
#ifdef __SWITCH__
	if (!rd_vk_depthbias)
	{
		mBias.mChanged = false;  // emulated in ApplyViewport
		return;
	}
#endif
	if (mBias.mChanged)
	{
		mCommandBuffer->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
		mBias.mChanged = false;
	}
}

void VkRenderState::ApplyRenderPass(int dt)
{
	// Find a pipeline that matches our state
	VkPipelineKey pipelineKey;
	pipelineKey.DrawType = dt;
	pipelineKey.VertexFormat = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat;
	pipelineKey.RenderStyle = mRenderStyle;
	pipelineKey.DepthTest = mDepthTest;
	pipelineKey.DepthWrite = mDepthTest && mDepthWrite;
	pipelineKey.DepthFunc = mDepthFunc;
	pipelineKey.DepthClamp = mDepthClamp;
	pipelineKey.DepthBias = !(mBias.mFactor == 0 && mBias.mUnits == 0);
#ifdef __SWITCH__
	if (!rd_vk_depthbias)
		pipelineKey.DepthBias = 0;
#endif
	pipelineKey.StencilTest = mStencilTest;
	pipelineKey.StencilPassOp = mStencilOp;
	pipelineKey.ColorMask = mColorMask;
	if (mNXFxLayer)
	{
		pipelineKey.ColorMask |= NX_PIPELINE_FXLAYER;
		mNXFxDrawn = true;
	}
	pipelineKey.CullMode = mCullMode;
	pipelineKey.NumTextureLayers = mMaterial.mMaterial ? mMaterial.mMaterial->NumLayers() : 0;
	pipelineKey.NumTextureLayers = max(pipelineKey.NumTextureLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS);// Always force minimum 8 textures as the shader requires it
	if (mSpecialEffect > EFF_NONE)
	{
		pipelineKey.SpecialEffect = mSpecialEffect;
		pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = false;
	}
	else
	{
		int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
		pipelineKey.SpecialEffect = EFF_NONE;
		pipelineKey.EffectState = mTextureEnabled ? effectState : SHADER_NoTexture;
		if (r_skipmats && pipelineKey.EffectState >= 3 && pipelineKey.EffectState <= 4)
			pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = mAlphaThreshold >= 0.f;
#ifdef __SWITCH__
		// 2D (fog mode 2) with a texture and the default material, once the
		// effect shaders exist (the loading screen draws while they compile)
		if (rd_vk_simple2d && mFogEnabled == 2 && mTextureEnabled && pipelineKey.EffectState == 0 && !mNXFxLayer &&
			fb->GetShaderManager()->GetEffect(EFF_SIMPLE2D, NORMAL_PASS))
		{
			pipelineKey.SpecialEffect = EFF_SIMPLE2D;
			pipelineKey.AlphaTest = false;
		}
#endif
	}

	// Is this the one we already have?
	bool inRenderPass = mCommandBuffer;
	bool changingPipeline = (!inRenderPass) || (pipelineKey != mPipelineKey);

	if (!inRenderPass)
	{
		mCommandBuffer = fb->GetCommands()->GetDrawCommands();
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;

		BeginRenderPass(mCommandBuffer);
	}

	if (changingPipeline)
	{
		mCommandBuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mPassSetup->GetPipeline(pipelineKey));
		mPipelineKey = pipelineKey;
	}
}

void VkRenderState::ApplyStencilRef()
{
	if (mStencilRefChanged)
	{
		mCommandBuffer->setStencilReference(VK_STENCIL_FRONT_AND_BACK, mStencilRef);
		mStencilRefChanged = false;
	}
}

void VkRenderState::ApplyScissor()
{
	if (mScissorChanged)
	{
		VkRect2D scissor;
		if (mScissorWidth >= 0)
		{
			int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
			int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
			int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
			int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);

			scissor.offset.x = x0;
			scissor.offset.y = y0;
			scissor.extent.width = x1 - x0;
			scissor.extent.height = y1 - y0;
		}
		else
		{
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = mRenderTarget.Width;
			scissor.extent.height = mRenderTarget.Height;
		}
		mCommandBuffer->setScissor(0, 1, &scissor);
		mScissorChanged = false;
	}
}

void VkRenderState::ApplyViewport()
{
#ifdef __SWITCH__
	{
		int sign = 0;
		if (!rd_vk_depthbias && !(mBias.mFactor == 0 && mBias.mUnits == 0))
			sign = (mBias.mUnits < 0 || (mBias.mUnits == 0 && mBias.mFactor < 0)) ? -1 : 1;
		if (sign != mEmuBiasSign)
		{
			mEmuBiasSign = sign;
			mViewportChanged = true;
		}
	}
#endif
	if (mViewportChanged)
	{
		VkViewport viewport;
		if (mViewportWidth >= 0)
		{
			viewport.x = (float)mViewportX;
			viewport.y = (float)mViewportY;
			viewport.width = (float)mViewportWidth;
			viewport.height = (float)mViewportHeight;
		}
		else
		{
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = (float)mRenderTarget.Width;
			viewport.height = (float)mRenderTarget.Height;
		}
		viewport.minDepth = mViewportDepthMin;
		viewport.maxDepth = mViewportDepthMax;
#ifdef __SWITCH__
		if (mEmuBiasSign)
		{
			const float range = mViewportDepthMax - mViewportDepthMin;
			const float eps = clamp<float>(rd_vk_depthbias_eps, 0.0f, 0.01f) * range;
			if (mEmuBiasSign < 0)
				viewport.maxDepth -= eps;	// biased toward the camera (decals, wall sprites)
			else
				viewport.minDepth += eps;	// biased away from it
		}
#endif
		mCommandBuffer->setViewport(0, 1, &viewport);
		mViewportChanged = false;
	}
}

void VkRenderState::ApplyStreamData()
{
	auto passManager = fb->GetRenderPassManager();

	mStreamData.useVertexData = passManager->GetVertexFormat(static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat)->UseVertexData;

	if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
		mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	else
		mStreamData.timer = 0.0f;

	if (!mStreamBufferWriter.Write(mStreamData))
	{
		WaitForStreamBuffers();
		mStreamBufferWriter.Write(mStreamData);
	}
}

void VkRenderState::ApplyPushConstants()
{
	int fogset = 0;
	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((GetFogColor() & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas() && !mMaterial.mMaterial->Source()->GetTranslucency())
		tempTM = TM_OPAQUE;

	mPushConstants.uFogEnabled = fogset;
	mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
	mPushConstants.uLightDist = mLightParms[0];
	mPushConstants.uLightFactor = mLightParms[1];
	mPushConstants.uFogDensity = mLightParms[2];
	mPushConstants.uLightLevel = mLightParms[3];
	mPushConstants.uAlphaThreshold = mAlphaThreshold;
	mPushConstants.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	if (mMaterial.mMaterial)
	{
		auto source = mMaterial.mMaterial->Source();
		mPushConstants.uSpecularMaterial = { source->GetGlossiness(), source->GetSpecularLevel() };
	}

	mPushConstants.uLightIndex = mLightIndex;
	mPushConstants.uBoneIndexBase = mBoneIndexBase;
	mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

	auto passManager = fb->GetRenderPassManager();
	mCommandBuffer->pushConstants(passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants), &mPushConstants);
}

void VkRenderState::ApplyMatrices()
{
	if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled))
	{
		WaitForStreamBuffers();
		mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled);
	}
}

void VkRenderState::ApplyVertexBuffers()
{
	if ((mVertexBuffer != mLastVertexBuffer || mVertexOffsets[0] != mLastVertexOffsets[0] || mVertexOffsets[1] != mLastVertexOffsets[1]) && mVertexBuffer)
	{
		auto vkbuf = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer);
		const VkVertexFormat *format = fb->GetRenderPassManager()->GetVertexFormat(vkbuf->VertexFormat);
		VkBuffer vertexBuffers[2] = { vkbuf->mBuffer->buffer, vkbuf->mBuffer->buffer };
		VkDeviceSize offsets[] = { mVertexOffsets[0] * format->Stride, mVertexOffsets[1] * format->Stride };
		mCommandBuffer->bindVertexBuffers(0, 2, vertexBuffers, offsets);
		mLastVertexBuffer = mVertexBuffer;
		mLastVertexOffsets[0] = mVertexOffsets[0];
		mLastVertexOffsets[1] = mVertexOffsets[1];
	}

	if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer)
	{
		mCommandBuffer->bindIndexBuffer(static_cast<VkHardwareIndexBuffer*>(mIndexBuffer)->mBuffer->buffer, 0, VK_INDEX_TYPE_UINT32);
		mLastIndexBuffer = mIndexBuffer;
	}
}

void VkRenderState::ApplyMaterial()
{
	if (mMaterial.mChanged)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas()) static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();

		VulkanDescriptorSet* descriptorset = mMaterial.mMaterial ? static_cast<VkMaterial*>(mMaterial.mMaterial)->GetDescriptorSet(mMaterial) : descriptors->GetNullTextureDescriptorSet();

		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, fb->GetDescriptorSetManager()->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 2, descriptorset);
		mMaterial.mChanged = false;
	}
}

void VkRenderState::ApplyHWBufferSet()
{
	uint32_t matrixOffset = mMatrixBufferWriter.Offset();
	uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
	if (mViewpointOffset != mLastViewpointOffset || matrixOffset != mLastMatricesOffset || streamDataOffset != mLastStreamDataOffset)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		uint32_t offsets[3] = { mViewpointOffset, matrixOffset, streamDataOffset };
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, fb->GetDescriptorSetManager()->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 1, descriptors->GetHWBufferDescriptorSet(), 3, offsets);

		mLastViewpointOffset = mViewpointOffset;
		mLastMatricesOffset = matrixOffset;
		mLastStreamDataOffset = streamDataOffset;
	}
}

void VkRenderState::WaitForStreamBuffers()
{
	fb->WaitForCommands(false);
	mApplyCount = 0;
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

void VkRenderState::Bind(int bindingpoint, uint32_t offset)
{
	if (bindingpoint == VIEWPOINT_BINDINGPOINT)
	{
		mViewpointOffset = offset;
		mNeedApply = true;
	}
}

void VkRenderState::BeginFrame()
{
	mMaterial.Reset();
	mApplyCount = 0;
	// the frame writes its own region of the stream buffers (DFrameBuffer::PerFrameSlot)
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

void VkRenderState::EndRenderPass()
{
	if (mCommandBuffer)
	{
		mCommandBuffer->endRenderPass();
		mCommandBuffer = nullptr;
		mPipelineKey = {};

		mLastViewpointOffset = 0xffffffff;
		mLastVertexBuffer = nullptr;
		mLastIndexBuffer = nullptr;
		mLastModelMatrixEnabled = true;
		mLastTextureMatrixEnabled = true;
	}
}

void VkRenderState::EndFrame()
{
	mMatrixBufferWriter.Reset();
	mStreamBufferWriter.Reset();
}

void VkRenderState::EnableDrawBuffers(int count, bool apply)
{
	if (mRenderTarget.DrawBuffers != count)
	{
		EndRenderPass();
		mRenderTarget.DrawBuffers = count;
	}
}

void VkRenderState::SetRenderTarget(VkTextureImage *image, VulkanImageView *depthStencilView, int width, int height, VkFormat format, VkSampleCountFlagBits samples)
{
	EndRenderPass();

	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencilView;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
	mRenderTarget.Samples = samples;
}

void VkRenderState::BeginRenderPass(VulkanCommandBuffer *cmdbuffer)
{
	VkRenderPassKey key = {};
	key.DrawBufferFormat = mRenderTarget.Format;
	key.Samples = mRenderTarget.Samples;
	key.DrawBuffers = mRenderTarget.DrawBuffers;
	key.DepthStencil = !!mRenderTarget.DepthStencil;

	mPassSetup = fb->GetRenderPassManager()->GetRenderPass(key);

	auto &framebuffer = mRenderTarget.Image->RSFramebuffers[key];
	if (!framebuffer)
	{
		auto buffers = fb->GetBuffers();
		FramebufferBuilder builder;
		builder.RenderPass(mPassSetup->GetRenderPass(0));
		builder.Size(mRenderTarget.Width, mRenderTarget.Height);
		builder.AddAttachment(mRenderTarget.Image->View.get());
		if (key.DrawBuffers > 1)
			builder.AddAttachment(buffers->SceneFog.View.get());
		if (key.DrawBuffers > 2)
			builder.AddAttachment(buffers->SceneNormal.View.get());
		if (key.DepthStencil)
			builder.AddAttachment(mRenderTarget.DepthStencil);
		builder.DebugName("VkRenderPassSetup.Framebuffer");
		framebuffer = builder.Create(fb->device.get());
	}

	// Only clear depth+stencil if the render target actually has that
	if (!mRenderTarget.DepthStencil)
		mClearTargets &= ~(CT_Depth | CT_Stencil);

	RenderPassBegin beginInfo;
	beginInfo.RenderPass(mPassSetup->GetRenderPass(mClearTargets));
	beginInfo.RenderArea(0, 0, mRenderTarget.Width, mRenderTarget.Height);
	beginInfo.Framebuffer(framebuffer.get());
	beginInfo.AddClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
	if (key.DrawBuffers > 1)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (key.DrawBuffers > 2)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	beginInfo.AddClearDepthStencil(1.0f, 0);
	beginInfo.Execute(cmdbuffer);

	mMaterial.mChanged = true;
	mClearTargets = 0;
}

/////////////////////////////////////////////////////////////////////////////

void VkRenderStateMolten::Draw(int dt, int index, int count, bool apply)
{
	if (dt == DT_TriangleFan)
	{
		IIndexBuffer *oldIndexBuffer = mIndexBuffer;
		mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();

		if (apply || mNeedApply)
			Apply(DT_Triangles);
		else
			ApplyVertexBuffers();

		mCommandBuffer->drawIndexed((count - 2) * 3, 1, 0, index, 0);

		mIndexBuffer = oldIndexBuffer;
	}
	else
	{
		if (apply || mNeedApply)
			Apply(dt);

		mCommandBuffer->draw(count, 1, index, 0);
	}
}
