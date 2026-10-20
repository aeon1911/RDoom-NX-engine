/*
** vk_descriptorset.h
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

#include <unordered_map>
#include <unordered_set>

#include "zvulkan/vulkanobjects.h"
#include <list>
#include "tarray.h"

class VulkanRenderDevice;
class VkMaterial;
class FTexture;
class FGameTexture;
class PPTextureInput;
class VkPPRenderPassSetup;

class VkDescriptorSetManager
{
public:
	VkDescriptorSetManager(VulkanRenderDevice* fb);
	~VkDescriptorSetManager();

	void Init();
	void Deinit();
	void BeginFrame();
	void UpdateFixedSet();
	void UpdateHWBufferSet();
	void ResetHWTextureSets();

	VulkanDescriptorSetLayout* GetHWBufferSetLayout() { return HWBufferSetLayout.get(); }
	VulkanDescriptorSetLayout* GetFixedSetLayout() { return FixedSetLayout.get(); }
	VulkanDescriptorSetLayout* GetTextureSetLayout(int numLayers);

	VulkanDescriptorSet* GetHWBufferDescriptorSet() { return HWBufferSet.get(); }
	VulkanDescriptorSet* GetFixedDescriptorSet() { return FixedSet.get(); }
	VulkanDescriptorSet* GetNullTextureDescriptorSet();

	std::unique_ptr<VulkanDescriptorSet> AllocateTextureDescriptorSet(int numLayers);

	VulkanDescriptorSet* GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers);

	void AddMaterial(VkMaterial* texture);
	void RemoveMaterial(VkMaterial* texture);
	const std::list<VkMaterial*> &GetMaterials() const { return Materials; }
#ifdef __SWITCH__
	// pictures sampled by materials of more than one texture, or as a
	// brightmap/extra layer: texture eviction leaves their GPU copy alone,
	// another material's descriptor set may still point at it
	bool IsSharedPicture(FTexture *pic) const { return SharedPictures.count(pic) != 0; }
#endif

private:
	void CreateHWBufferSetLayout();
	void CreateFixedSetLayout();
	void CreateHWBufferPool();
	void CreateFixedSetPool();

	std::unique_ptr<VulkanDescriptorSet> AllocatePPDescriptorSet(VulkanDescriptorSetLayout* layout);

	VulkanRenderDevice* fb = nullptr;
#ifdef __SWITCH__
	std::unordered_map<FTexture*, FGameTexture*> PictureOwner;
	std::unordered_set<FTexture*> SharedPictures;
#endif

	std::unique_ptr<VulkanDescriptorSetLayout> HWBufferSetLayout;
	std::unique_ptr<VulkanDescriptorSetLayout> FixedSetLayout;
	std::vector<std::unique_ptr<VulkanDescriptorSetLayout>> TextureSetLayouts;

	std::unique_ptr<VulkanDescriptorPool> HWBufferDescriptorPool;
	std::unique_ptr<VulkanDescriptorPool> FixedDescriptorPool;

	std::unique_ptr<VulkanDescriptorPool> PPDescriptorPool;

	int TextureDescriptorSetsLeft = 0;
	int TextureDescriptorsLeft = 0;
	std::vector<std::unique_ptr<VulkanDescriptorPool>> TextureDescriptorPools;

	std::unique_ptr<VulkanDescriptorSet> HWBufferSet;
	std::unique_ptr<VulkanDescriptorSet> FixedSet;
	std::unique_ptr<VulkanDescriptorSet> NullTextureDescriptorSet;

	std::list<VkMaterial*> Materials;

	static const int maxSets = 10;
};
