/*
** vk_renderpass.h
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

#pragma once

#include <map>
#include <mutex>
#include <string>

#include "i_time.h"

#include "zvulkan/vulkanobjects.h"
#include "renderstyle.h"
#include "hwrenderer/data/buffers.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_renderstate.h"
#include <string.h>
#include <map>

class VulkanRenderDevice;
class VkPPShader;
class GraphicsPipelineBuilder;
class ColorBlendAttachmentBuilder;

// VkPipelineKey::ColorMask bit above the four channel bits: pipeline for the
// half-resolution effects layer (same size key, so old warm lists stay valid)
enum { NX_PIPELINE_FXLAYER = 0x100 };

class VkPipelineKey
{
public:
	FRenderStyle RenderStyle;
	int SpecialEffect;
	int EffectState;
	int AlphaTest;
	int DepthWrite;
	int DepthTest;
	int DepthFunc;
	int DepthClamp;
	int DepthBias;
	int StencilTest;
	int StencilPassOp;
	int ColorMask;
	int CullMode;
	int VertexFormat;
	int DrawType;
	int NumTextureLayers;

	bool operator<(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) < 0; }
	bool operator==(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) == 0; }
	bool operator!=(const VkPipelineKey &other) const { return memcmp(this, &other, sizeof(VkPipelineKey)) != 0; }
};

class VkRenderPassKey
{
public:
	int DepthStencil;
	int Samples;
	int DrawBuffers;
	VkFormat DrawBufferFormat;

	bool operator<(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) < 0; }
	bool operator==(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) == 0; }
	bool operator!=(const VkRenderPassKey &other) const { return memcmp(this, &other, sizeof(VkRenderPassKey)) != 0; }
};

class VkRenderPassSetup
{
public:
	VkRenderPassSetup(VulkanRenderDevice* fb, const VkRenderPassKey &key);

	VulkanRenderPass *GetRenderPass(int clearTargets);
	VulkanPipeline *GetPipeline(const VkPipelineKey &key);

	VkRenderPassKey PassKey;
	std::unique_ptr<VulkanRenderPass> RenderPasses[8];
	std::map<VkPipelineKey, std::unique_ptr<VulkanPipeline>> Pipelines;

private:
	std::unique_ptr<VulkanRenderPass> CreateRenderPass(int clearTargets);
	std::unique_ptr<VulkanPipeline> CreatePipeline(const VkPipelineKey &key);

	VulkanRenderDevice* fb = nullptr;
};

class VkVertexFormat
{
public:
	int NumBindingPoints;
	size_t Stride;
	std::vector<FVertexBufferAttribute> Attrs;
	int UseVertexData;
};

enum class WhichDepthStencil;

class VkPPRenderPassKey
{
public:
	VkPPShader* Shader;
	int Uniforms;
	int InputTextures;
	PPBlendMode BlendMode;
	VkFormat OutputFormat;
	int SwapChain;
	int ShadowMapBuffers;
	WhichDepthStencil StencilTest;
	VkSampleCountFlagBits Samples;

	bool operator<(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) < 0; }
	bool operator==(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) == 0; }
	bool operator!=(const VkPPRenderPassKey& other) const { return memcmp(this, &other, sizeof(VkPPRenderPassKey)) != 0; }
};

class VkPPRenderPassSetup
{
public:
	VkPPRenderPassSetup(VulkanRenderDevice* fb, const VkPPRenderPassKey& key);

	std::unique_ptr<VulkanDescriptorSetLayout> DescriptorLayout;
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanRenderPass> RenderPass;
	std::unique_ptr<VulkanPipeline> Pipeline;

private:
	void CreateDescriptorLayout(const VkPPRenderPassKey& key);
	void CreatePipelineLayout(const VkPPRenderPassKey& key);
	void CreatePipeline(const VkPPRenderPassKey& key);
	void CreateRenderPass(const VkPPRenderPassKey& key);

	VulkanRenderDevice* fb = nullptr;
};

ColorBlendAttachmentBuilder& BlendMode(ColorBlendAttachmentBuilder& builder, const FRenderStyle& style);

class VkRenderPassManager
{
public:
	VkRenderPassManager(VulkanRenderDevice* fb);
	~VkRenderPassManager();

	void RenderBuffersReset();

	VkRenderPassSetup *GetRenderPass(const VkRenderPassKey &key);
	int GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs);
	VkVertexFormat *GetVertexFormat(int index);
	VulkanPipelineLayout* GetPipelineLayout(int numLayers);

	VkPPRenderPassSetup* GetPPRenderPass(const VkPPRenderPassKey& key);

	VulkanPipelineCache* GetCache() { return PipelineCache.get(); }
	// held while a pipeline is created with the cache and while its data is read (on a worker)
	std::mutex &CacheMutex() { return mCacheMutex; }

	// Switch: the cache used to be written only by a clean exit, and closing
	// with HOME skipped it, so every session compiled its pipelines again
	// (40-90 ms each on the console). It is now handed to the background writer
	// a few seconds after the last new pipeline.
	void PipelineCreated(const VkRenderPassKey &passKey, const VkPipelineKey &key);
	void SaveCacheWhenIdle();

	// Pipeline warm-up (Switch): even with the cache, creating a pipeline costs
	// ~20 ms on the console (the driver uploads the shader to GPU memory and
	// waits), which showed up as hitches the first time a door, shot or
	// explosion appeared. Every pipeline a game (IWAD + mods) used is listed in
	// cache/uzdoom/rdoom-pipelines-<hash>.bin and created again while a level
	// loads, behind the loading screen.
	void WarmupPipelines();

	// The same for textures: every untranslated material first drawn during a
	// level is listed (rdoom-textures-<hash>.txt) and uploaded again at the
	// end of the next level's precache.
	void TextureFirstUsed(FGameTexture *tex, int scaleFlags);
	void PrecacheListedTextures();

private:
	int mUnsavedPipelines = 0;
	uint64_t mLastPipelineMs = 0;

	struct WarmEntry
	{
		VkRenderPassKey Pass;
		VkPipelineKey Key;	// VertexFormat zeroed: the format is stored below
		int NumBindingPoints, Stride, UseVertexData, NumAttrs;
		FVertexBufferAttribute Attrs[12];
	};
	std::vector<WarmEntry> mWarm;
	bool mWarmLoaded = false, mWarmDirty = false, mWarming = false;
	FString WarmFilename(const char *kind = "pipelines", const char *ext = "bin");
	FString GlobalWarmFilename();
	std::map<std::string, FString> mWarmPaths;	// computed once: the loaded files don't change
	std::mutex mCacheMutex;
	void LoadWarmList();
	struct TextureUse { FString Name; int UseType, ScaleFlags; };
	std::vector<TextureUse> mTexList;
	bool mTexLoaded = false, mTexDirty = false, mTexWarming = false;
	uint64_t mLastTexMs = 0;
	uint64_t mLastTexSaveMs = 0;	// 0 = not saved yet this session
	void LoadTextureList();

	VulkanRenderDevice* fb = nullptr;

	std::map<VkRenderPassKey, std::unique_ptr<VkRenderPassSetup>> RenderPassSetup;
	std::vector<std::unique_ptr<VulkanPipelineLayout>> PipelineLayouts;
	std::vector<VkVertexFormat> VertexFormats;

	std::map<VkPPRenderPassKey, std::unique_ptr<VkPPRenderPassSetup>> PPRenderPassSetup;

	FString CacheFilename;
	std::unique_ptr<VulkanPipelineCache> PipelineCache;
};
