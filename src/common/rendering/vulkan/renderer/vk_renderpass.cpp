/*
** vk_renderpass.cpp
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

#include "vk_renderpass.h"
#ifdef __SWITCH__
#include "common/platform/posix/nx/rdoom_nx.h"
#endif
#include "vk_renderstate.h"
#include "vk_descriptorset.h"
#include "vk_raytrace.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/shaders/vk_ppshader.h"
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "flatvertices.h"
#include "hw_viewpointuniforms.h"
#include "v_2ddrawer.h"
#include "i_specialpaths.h"
#include "cmdlib.h"
#include "filesystem.h"
#include "hw_material.h"
#include "texturemanager.h"
#include "gamestate.h"
#include "nx_perf.h"
#include "c_cvars.h"

VkRenderPassManager::VkRenderPassManager(VulkanRenderDevice* fb) : fb(fb)
{
	FString path = M_GetCachePath(true);
	CreatePath(path.GetChars());
	CacheFilename = path + "/vulkanpipelinecache";

	PipelineCacheBuilder builder;
	builder.DebugName("PipelineCache");

	try
	{
		FileReader fr;
		if (fr.OpenFile(CacheFilename.GetChars()))
		{
			std::vector<uint8_t> data;
			data.resize(fr.GetLength());
			if (fr.Read(data.data(), data.size()) == (FileReader::Size)data.size())
			{
				builder.InitialData(data.data(), data.size());
#ifdef __SWITCH__
				FString n;
				n.Format("%u KB", (unsigned)(data.size() / 1024));
				RDoom_SessionSet("vulkan_pipeline_cache_loaded", n.GetChars());
#endif
			}
		}
	}
	catch (...)
	{
	}

	PipelineCache = builder.Create(fb->device.get());
}

VkRenderPassManager::~VkRenderPassManager()
{
	// after a lost device the driver may hand back incomplete data; a bad cache
	// would then break every following session
	if (VulkanDeviceWasLost)
		return;
	try
	{
		auto data = PipelineCache->GetCacheData();
		std::unique_ptr<FileWriter> fw(FileWriter::Open(CacheFilename.GetChars()));
		if (fw)
			fw->Write(data.data(), data.size());
	}
	catch (...)
	{
	}
}

void VkRenderPassManager::PipelineCreated(const VkRenderPassKey &passKey, const VkPipelineKey &key)
{
	mUnsavedPipelines++;
	mLastPipelineMs = I_msTime();
#ifdef __SWITCH__
	LoadWarmList();
	const VkVertexFormat *vf = GetVertexFormat(key.VertexFormat);
	if (!vf || vf->Attrs.size() > 12)
		return;
	WarmEntry e;
	memset(&e, 0, sizeof(e));
	e.Pass = passKey;
	e.Key = key;
	e.Key.VertexFormat = 0;
	e.NumBindingPoints = vf->NumBindingPoints;
	e.Stride = (int)vf->Stride;
	e.UseVertexData = vf->UseVertexData;
	e.NumAttrs = (int)vf->Attrs.size();
	for (int i = 0; i < e.NumAttrs; i++)
		e.Attrs[i] = vf->Attrs[i];
	for (auto &w : mWarm)
		if (!memcmp(&w, &e, sizeof(e)))
			return;
	if (mWarm.size() < 400)
	{
		mWarm.push_back(e);
		mWarmDirty = true;
	}
#endif
}

FString VkRenderPassManager::WarmFilename(const char *kind, const char *ext)
{
	// one list per game: hash of the loaded resource files
	uint32_t h = 2166136261u;
	for (int i = 0; i < fileSystem.GetNumWads(); i++)
	{
		const char *name = fileSystem.GetResourceFileName(i);
		for (const char *p = name ? name : ""; *p; p++)
			h = (h ^ (uint8_t)*p) * 16777619u;
	}
	FString path = M_GetCachePath(true);
	path.AppendFormat("/rdoom-%s-%08x.%s", kind, h, ext);
	return path;
}

void VkRenderPassManager::LoadWarmList()
{
	if (mWarmLoaded)
		return;
	mWarmLoaded = true;
	FileReader fr;
	if (!fr.OpenFile(WarmFilename().GetChars()))
		return;
	uint32_t header[3] = {};	// magic, entry size, count
	if (fr.Read(header, sizeof(header)) != (FileReader::Size)sizeof(header) || header[0] != 0x4c504452 || header[1] != sizeof(WarmEntry) || header[2] > 400)
		return;
	mWarm.resize(header[2]);
	if (fr.Read(mWarm.data(), mWarm.size() * sizeof(WarmEntry)) != (FileReader::Size)(mWarm.size() * sizeof(WarmEntry)))
		mWarm.clear();
}

void VkRenderPassManager::WarmupPipelines()
{
#ifdef __SWITCH__
	if (mWarming || !fb->GetShaderManager() || !fb->GetShaderManager()->AllCompiled())
		return;
	LoadWarmList();
	mWarming = true;
	const uint64_t start = I_msTime();
	int created = 0;
	const auto list = mWarm;	// GetPipeline may append
	for (const auto &w : list)
	{
		if (w.NumAttrs <= 0 || w.NumAttrs > 12)
			continue;
		VkPipelineKey key = w.Key;
		if (key.SpecialEffect != EFF_NONE ? !fb->GetShaderManager()->GetEffect(key.SpecialEffect, w.Pass.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS)
			: !fb->GetShaderManager()->Get(key.EffectState, key.AlphaTest, w.Pass.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS))
			continue;	// a shader this game does not have
		key.VertexFormat = GetVertexFormat(w.NumBindingPoints, w.NumAttrs, w.Stride, w.Attrs);
		VkRenderPassSetup *pass = GetRenderPass(w.Pass);
		if (pass->Pipelines.find(key) != pass->Pipelines.end())
			continue;
		pass->GetPipeline(key);
		created++;
	}
	mWarming = false;
	if (created)
	{
		FString n;
		n.Format("%d of %d pipelines in %d ms", created, (int)list.size(), (int)(I_msTime() - start));
		RDoom_SessionSet("vulkan_pipeline_warmup", n.GetChars());
	}
#endif
}

void VkRenderPassManager::LoadTextureList()
{
	if (mTexLoaded)
		return;
	mTexLoaded = true;
	FileReader fr;
	if (!fr.OpenFile(WarmFilename("textures", "txt").GetChars()))
		return;
	auto data = fr.Read();
	FString text((const char *)data.data(), data.size());
	auto lines = text.Split("\n");
	for (auto &line : lines)
	{
		int useType = 0, scaleFlags = 0;
		char name[64] = "";
		if (sscanf(line.GetChars(), "%d %d %63s", &useType, &scaleFlags, name) == 3 && mTexList.size() < 2000)
			mTexList.push_back({ name, useType, scaleFlags });
	}
}

void VkRenderPassManager::TextureFirstUsed(FGameTexture *tex, int scaleFlags)
{
#ifdef __SWITCH__
	if (mTexWarming || !tex || gamestate != GS_LEVEL)
		return;
	// walls and flats are precached with the map already; what shows up by
	// surprise are effect sprites (fire, smoke, gore) and decals
	const ETextureType ut = tex->GetUseType();
	if (ut != ETextureType::Sprite && ut != ETextureType::Decal && ut != ETextureType::SkinSprite)
		return;
	const FString &name = tex->GetName();
	if (name.IsEmpty() || name.Len() > 60 || name.IndexOf(' ') >= 0)
		return;
	LoadTextureList();
	const int useType = (int)tex->GetUseType();
	for (auto &t : mTexList)
		if (t.UseType == useType && t.ScaleFlags == scaleFlags && t.Name.CompareNoCase(name) == 0)
			return;
	if (mTexList.size() < 2000)
	{
		mTexList.push_back({ name, useType, scaleFlags });
		mTexDirty = true;
		mUnsavedPipelines++;	// the saver below also writes this list
		mLastPipelineMs = I_msTime();
	}
#endif
}

void VkRenderPassManager::PrecacheListedTextures()
{
#ifdef __SWITCH__
	LoadTextureList();
	if (mTexList.empty())
		return;
	mTexWarming = true;
	const uint64_t start = I_msTime();
	int done = 0;
	for (auto &t : mTexList)
	{
		FTextureID id = TexMan.CheckForTexture(t.Name.GetChars(), (ETextureType)t.UseType, FTextureManager::TEXMAN_TryAny);
		FGameTexture *tex = id.isValid() ? TexMan.GetGameTexture(id) : nullptr;
		if (!tex || !tex->isValid())
			continue;
		FMaterial *mat = FMaterial::ValidateTexture(tex, t.ScaleFlags, true);
		if (!mat)
			continue;
		fb->PrecacheMaterial(mat, 0);
		done++;
	}
	mTexWarming = false;
	FString n;
	n.Format("%d of %d textures in %d ms", done, (int)mTexList.size(), (int)(I_msTime() - start));
	RDoom_SessionSet("vulkan_texture_warmup", n.GetChars());
#endif
}

void VkRenderPassManager::SaveCacheWhenIdle()
{
#ifdef __SWITCH__
	if (mUnsavedPipelines == 0 || VulkanDeviceWasLost || I_msTime() - mLastPipelineMs < 3000)
		return;
	mUnsavedPipelines = 0;
	try
	{
		auto data = PipelineCache->GetCacheData();
		RDoom_WriteFileAsync(CacheFilename.GetChars(), data.data(), data.size());
		FString n;
		n.Format("%u KB", (unsigned)(data.size() / 1024));
		RDoom_SessionSet("vulkan_pipeline_cache_saved", n.GetChars());
	}
	catch (...)
	{
	}
	if (mTexDirty)
	{
		mTexDirty = false;
		FString out;
		for (auto &t : mTexList)
			out.AppendFormat("%d %d %s\n", t.UseType, t.ScaleFlags, t.Name.GetChars());
		RDoom_WriteFileAsync(WarmFilename("textures", "txt").GetChars(), out.GetChars(), out.Len());
	}
	if (mWarmDirty)
	{
		mWarmDirty = false;
		std::vector<uint8_t> out(12 + mWarm.size() * sizeof(WarmEntry));
		uint32_t header[3] = { 0x4c504452, (uint32_t)sizeof(WarmEntry), (uint32_t)mWarm.size() };
		memcpy(out.data(), header, sizeof(header));
		memcpy(out.data() + 12, mWarm.data(), mWarm.size() * sizeof(WarmEntry));
		RDoom_WriteFileAsync(WarmFilename().GetChars(), out.data(), out.size());
	}
#endif
}

void VkRenderPassManager::RenderBuffersReset()
{
	RenderPassSetup.clear();
	PPRenderPassSetup.clear();
}

VkRenderPassSetup *VkRenderPassManager::GetRenderPass(const VkRenderPassKey &key)
{
	auto &item = RenderPassSetup[key];
	if (!item)
		item.reset(new VkRenderPassSetup(fb, key));
	return item.get();
}

int VkRenderPassManager::GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	for (size_t i = 0; i < VertexFormats.size(); i++)
	{
		const auto &f = VertexFormats[i];
		if (f.Attrs.size() == (size_t)numAttributes && f.NumBindingPoints == numBindingPoints && f.Stride == stride)
		{
			bool matches = true;
			for (int j = 0; j < numAttributes; j++)
			{
				if (memcmp(&f.Attrs[j], &attrs[j], sizeof(FVertexBufferAttribute)) != 0)
				{
					matches = false;
					break;
				}
			}

			if (matches)
				return (int)i;
		}
	}

	VkVertexFormat fmt;
	fmt.NumBindingPoints = numBindingPoints;
	fmt.Stride = stride;
	fmt.UseVertexData = 0;
	for (int j = 0; j < numAttributes; j++)
	{
		if (attrs[j].location == VATTR_COLOR)
			fmt.UseVertexData |= 1;
		else if (attrs[j].location == VATTR_NORMAL)
			fmt.UseVertexData |= 2;
		fmt.Attrs.push_back(attrs[j]);
	}
	VertexFormats.push_back(fmt);
	return (int)VertexFormats.size() - 1;
}

VkVertexFormat *VkRenderPassManager::GetVertexFormat(int index)
{
	return &VertexFormats[index];
}

VulkanPipelineLayout* VkRenderPassManager::GetPipelineLayout(int numLayers)
{
	if (PipelineLayouts.size() <= (size_t)numLayers)
		PipelineLayouts.resize(numLayers + 1);

	auto &layout = PipelineLayouts[numLayers];
	if (layout)
		return layout.get();

	auto descriptors = fb->GetDescriptorSetManager();

	PipelineLayoutBuilder builder;
	builder.AddSetLayout(descriptors->GetFixedSetLayout());
	builder.AddSetLayout(descriptors->GetHWBufferSetLayout());
	if (numLayers != 0)
		builder.AddSetLayout(descriptors->GetTextureSetLayout(numLayers));
	builder.AddPushConstantRange(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants));
	builder.DebugName("VkRenderPassManager.PipelineLayout");
	layout = builder.Create(fb->device.get());
	return layout.get();
}

VkPPRenderPassSetup* VkRenderPassManager::GetPPRenderPass(const VkPPRenderPassKey& key)
{
	auto& passSetup = PPRenderPassSetup[key];
	if (!passSetup)
		passSetup.reset(new VkPPRenderPassSetup(fb, key));
	return passSetup.get();
}

/////////////////////////////////////////////////////////////////////////////

VkRenderPassSetup::VkRenderPassSetup(VulkanRenderDevice* fb, const VkRenderPassKey &key) : PassKey(key), fb(fb)
{
}

std::unique_ptr<VulkanRenderPass> VkRenderPassSetup::CreateRenderPass(int clearTargets)
{
	auto buffers = fb->GetBuffers();

	VkFormat drawBufferFormats[] = { buffers->SceneColorFormat, VK_FORMAT_R8G8B8A8_UNORM, buffers->SceneNormalFormat };

	RenderPassBuilder builder;

	builder.AddAttachment(
		PassKey.DrawBufferFormat, (VkSampleCountFlagBits)PassKey.Samples,
		(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	for (int i = 1; i < PassKey.DrawBuffers; i++)
	{
		builder.AddAttachment(
			drawBufferFormats[i], (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Color) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	if (PassKey.DepthStencil)
	{
		builder.AddDepthStencilAttachment(
			buffers->SceneDepthStencilFormat, (VkSampleCountFlagBits)PassKey.Samples,
			(clearTargets & CT_Depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			(clearTargets & CT_Stencil) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	builder.AddSubpass();
	for (int i = 0; i < PassKey.DrawBuffers; i++)
		builder.AddSubpassColorAttachmentRef(i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (PassKey.DepthStencil)
	{
		builder.AddSubpassDepthStencilAttachmentRef(PassKey.DrawBuffers, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	}
	else
	{
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
	}
	builder.DebugName("VkRenderPassSetup.RenderPass");
	return builder.Create(fb->device.get());
}

VulkanRenderPass *VkRenderPassSetup::GetRenderPass(int clearTargets)
{
	if (!RenderPasses[clearTargets])
		RenderPasses[clearTargets] = CreateRenderPass(clearTargets);
	return RenderPasses[clearTargets].get();
}

VulkanPipeline *VkRenderPassSetup::GetPipeline(const VkPipelineKey &key)
{
	auto &item = Pipelines[key];
	if (!item)
	{
		NXPERF_SCOPE(shader_ms, &nxperf.shaders);	// perf.log "shaders n/ms": pipelines created in that frame
		item = CreatePipeline(key);
		fb->GetRenderPassManager()->PipelineCreated(PassKey, key);
	}
	return item.get();
}

std::unique_ptr<VulkanPipeline> VkRenderPassSetup::CreatePipeline(const VkPipelineKey &key)
{
	GraphicsPipelineBuilder builder;
	builder.Cache(fb->GetRenderPassManager()->GetCache());

	VkShaderProgram *program;
	if (key.SpecialEffect != EFF_NONE)
	{
		program = fb->GetShaderManager()->GetEffect(key.SpecialEffect, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS);
	}
	else
	{
		program = fb->GetShaderManager()->Get(key.EffectState, key.AlphaTest, PassKey.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS);
	}
	builder.AddVertexShader(program->vert.get());
	builder.AddFragmentShader(program->frag.get());

	const VkVertexFormat &vfmt = *fb->GetRenderPassManager()->GetVertexFormat(key.VertexFormat);

	for (int i = 0; i < vfmt.NumBindingPoints; i++)
		builder.AddVertexBufferBinding(i, vfmt.Stride);

	const static VkFormat vkfmts[] = {
		VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_FORMAT_R32G32B32_SFLOAT,
		VK_FORMAT_R32G32_SFLOAT,
		VK_FORMAT_R32_SFLOAT,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_A2B10G10R10_SNORM_PACK32,
		VK_FORMAT_R8G8B8A8_UINT
	};

	bool inputLocations[VATTR_MAX] = {};

	for (size_t i = 0; i < vfmt.Attrs.size(); i++)
	{
		const auto &attr = vfmt.Attrs[i];
		builder.AddVertexAttribute(attr.location, attr.binding, vkfmts[attr.format], attr.offset);
		inputLocations[attr.location] = true;
	}

	// Vulkan requires an attribute binding for each location specified in the shader
	for (int i = 0; i < VATTR_MAX; i++)
	{
		if (!inputLocations[i])
			builder.AddVertexAttribute(i, 0, i != 8 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT, 0);
	}

	builder.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	builder.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	builder.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_BIAS);
	builder.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_REFERENCE);

	// Note: the actual values are ignored since we use dynamic viewport+scissor states
	builder.Viewport(0.0f, 0.0f, 320.0f, 200.0f);
	builder.Scissor(0, 0, 320, 200);

	static const VkPrimitiveTopology vktopology[] = {
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
	};

	static const VkStencilOp op2vk[] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP };
	static const VkCompareOp depthfunc2vk[] = { VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS };

	builder.Topology(vktopology[key.DrawType]);
#ifdef __SWITCH__
	{
		EXTERN_CVAR(Int, rd_vk_zstate)
		if (rd_vk_zstate == 1)
			builder.DepthStencilEnable(key.DepthTest, false, false);
		else if (rd_vk_zstate >= 2)
			builder.DepthStencilEnable(false, false, false);
		else
			builder.DepthStencilEnable(key.DepthTest, key.DepthWrite, key.StencilTest);
	}
#else
	builder.DepthStencilEnable(key.DepthTest, key.DepthWrite, key.StencilTest);
#endif
	builder.DepthFunc(depthfunc2vk[key.DepthFunc]);
	if (fb->device->EnabledFeatures.Features.depthClamp)
		builder.DepthClampEnable(key.DepthClamp);
	builder.DepthBias(key.DepthBias, 0.0f, 0.0f, 0.0f);

	// Note: CCW and CW is intentionally swapped here because the vulkan and opengl coordinate systems differ.
	// main.vp addresses this by patching up gl_Position.z, which has the side effect of flipping the sign of the front face calculations.
	builder.Cull(key.CullMode == Cull_None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT, key.CullMode == Cull_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);

	builder.Stencil(VK_STENCIL_OP_KEEP, op2vk[key.StencilPassOp], VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffffffff, 0xffffffff, 0);

	ColorBlendAttachmentBuilder blendbuilder;
	blendbuilder.ColorWriteMask((VkColorComponentFlags)key.ColorMask);
	BlendMode(blendbuilder, key.RenderStyle);

	for (int i = 0; i < PassKey.DrawBuffers; i++)
		builder.AddColorBlendAttachment(blendbuilder.Create());

	builder.RasterizationSamples((VkSampleCountFlagBits)PassKey.Samples);

	builder.Layout(fb->GetRenderPassManager()->GetPipelineLayout(key.NumTextureLayers));
	builder.RenderPass(GetRenderPass(0));
	builder.DebugName("VkRenderPassSetup.Pipeline");

#ifdef __SWITCH__
	// Evidence for GPU faults on hardware: the session report always names the
	// newest pipeline (a fault right after a new effect/model shows up here).
	{
		static unsigned created;
		FString d, attrs;
		for (auto &a : vfmt.Attrs)
			attrs.AppendFormat(" L%d:b%d:f%d", a.location, a.binding, a.format);
		d.Format("#%u: vertexformat %d (%d bindings, stride %d,%s), effect %d, special %d, alphatest %d, draw %d, layers %d, samples %d, buffers %d, "
			"blend op %d src %d dst %d flags 0x%x, depth test %d write %d func %d clamp %d bias %d, stencil %d op %d, colormask 0x%x, cull %d",
			++created, key.VertexFormat, vfmt.NumBindingPoints, (int)vfmt.Stride, attrs.GetChars(), key.EffectState, key.SpecialEffect,
			key.AlphaTest, key.DrawType, key.NumTextureLayers, PassKey.Samples, PassKey.DrawBuffers,
			key.RenderStyle.BlendOp, key.RenderStyle.SrcAlpha, key.RenderStyle.DestAlpha, key.RenderStyle.Flags,
			key.DepthTest, key.DepthWrite, key.DepthFunc, key.DepthClamp, key.DepthBias, key.StencilTest, key.StencilPassOp, key.ColorMask, key.CullMode);
		RDoom_SessionSet("vulkan_last_new_pipeline", d.GetChars());
		// boot.log is written synchronously: only in the GPU trace mode
		EXTERN_CVAR(Int, rd_vk_drawlog)
		if (rd_vk_drawlog > 0)
		{
			FString c = "new pipeline " + d;
			RDoom_Crumb(c.GetChars());
		}
		extern int rd_vk_drawlog_burst;
		rd_vk_drawlog_burst = 50;
	}
#endif
	return builder.Create(fb->device.get());
}

/////////////////////////////////////////////////////////////////////////////

VkPPRenderPassSetup::VkPPRenderPassSetup(VulkanRenderDevice* fb, const VkPPRenderPassKey& key) : fb(fb)
{
	CreateDescriptorLayout(key);
	CreatePipelineLayout(key);
	CreateRenderPass(key);
	CreatePipeline(key);
}

void VkPPRenderPassSetup::CreateDescriptorLayout(const VkPPRenderPassKey& key)
{
	DescriptorSetLayoutBuilder builder;
	for (int i = 0; i < key.InputTextures; i++)
		builder.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);

	// Automatic uniforms at dedicated binding point (separate from push constants)
	builder.AddBinding(AUTOMATIC_UNIFORMS_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);

	if (key.ShadowMapBuffers)
	{
		builder.AddBinding(LIGHTNODES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		builder.AddBinding(LIGHTLINES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		builder.AddBinding(LIGHTLIST_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	builder.DebugName("VkPPRenderPassSetup.DescriptorLayout");
	DescriptorLayout = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreatePipelineLayout(const VkPPRenderPassKey& key)
{
	PipelineLayoutBuilder builder;
	builder.AddSetLayout(DescriptorLayout.get());
	if (key.Uniforms > 0)
		builder.AddPushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT, 0, key.Uniforms);
	builder.DebugName("VkPPRenderPassSetup.PipelineLayout");
	PipelineLayout = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreatePipeline(const VkPPRenderPassKey& key)
{
	GraphicsPipelineBuilder builder;
	builder.Cache(fb->GetRenderPassManager()->GetCache());
	builder.AddVertexShader(key.Shader->VertexShader.get());
	builder.AddFragmentShader(key.Shader->FragmentShader.get());

	builder.AddVertexBufferBinding(0, sizeof(FFlatVertex));
	builder.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FFlatVertex, x));
	builder.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FFlatVertex, u));
	builder.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	builder.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	// Note: the actual values are ignored since we use dynamic viewport+scissor states
	builder.Viewport(0.0f, 0.0f, 320.0f, 200.0f);
	builder.Scissor(0, 0, 320, 200);
	if (key.StencilTest != WhichDepthStencil::None)
	{
		builder.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		builder.DepthStencilEnable(false, false, true);
		builder.Stencil(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffffffff, 0xffffffff, 0);
	}
	builder.Topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

	ColorBlendAttachmentBuilder blendbuilder;
	BlendMode(blendbuilder, key.BlendMode);
	builder.AddColorBlendAttachment(blendbuilder.Create());

	builder.RasterizationSamples(key.Samples);
	builder.Layout(PipelineLayout.get());
	builder.RenderPass(RenderPass.get());
	builder.DebugName("VkPPRenderPassSetup.Pipeline");
	Pipeline = builder.Create(fb->device.get());
}

void VkPPRenderPassSetup::CreateRenderPass(const VkPPRenderPassKey& key)
{
	RenderPassBuilder builder;
	if (key.SwapChain)
		builder.AddAttachment(key.OutputFormat, key.Samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	else
		builder.AddAttachment(key.OutputFormat, key.Samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (key.StencilTest == WhichDepthStencil::Scene)
	{
		builder.AddDepthStencilAttachment(
			fb->GetBuffers()->SceneDepthStencilFormat, key.Samples,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	if (key.StencilTest == WhichDepthStencil::Pipeline)
	{
		builder.AddDepthStencilAttachment(
			fb->GetBuffers()->PipelineDepthStencilFormat, key.Samples,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	builder.AddSubpass();
	builder.AddSubpassColorAttachmentRef(0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (key.StencilTest != WhichDepthStencil::None)
	{
		builder.AddSubpassDepthStencilAttachmentRef(1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
	}
	else
	{
		builder.AddExternalSubpassDependency(
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
	}

	builder.DebugName("VkPPRenderPassSetup.RenderPass");
	RenderPass = builder.Create(fb->device.get());
}

/////////////////////////////////////////////////////////////////////////////

ColorBlendAttachmentBuilder& BlendMode(ColorBlendAttachmentBuilder& builder, const FRenderStyle& style)
{
	// Just in case Vulkan doesn't do this optimization itself
	if (style.BlendOp == STYLEOP_Add && style.SrcAlpha == STYLEALPHA_One && style.DestAlpha == STYLEALPHA_Zero && style.Flags == 0)
	{
		return builder;
	}

	static const int blendstyles[] = {
		VK_BLEND_FACTOR_ZERO,
		VK_BLEND_FACTOR_ONE,
		VK_BLEND_FACTOR_SRC_ALPHA,
		VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		VK_BLEND_FACTOR_SRC_COLOR,
		VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		VK_BLEND_FACTOR_DST_COLOR,
		VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
		VK_BLEND_FACTOR_DST_ALPHA,
		VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	};

	static const int renderops[] = {
		0, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
	};

	int srcblend = blendstyles[style.SrcAlpha % STYLEALPHA_MAX];
	int dstblend = blendstyles[style.DestAlpha % STYLEALPHA_MAX];
	int blendequation = renderops[style.BlendOp & 15];

	if (blendequation == -1)	// This was a fuzz style.
	{
		srcblend = VK_BLEND_FACTOR_DST_COLOR;
		dstblend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendequation = VK_BLEND_OP_ADD;
	}

	return builder.BlendMode((VkBlendOp)blendequation, (VkBlendFactor)srcblend, (VkBlendFactor)dstblend);
}
