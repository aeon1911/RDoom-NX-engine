/*
** vk_commandbuffer.h
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

#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>
#include "zstring.h"

class VulkanRenderDevice;

class VkCommandBufferManager
{
public:
	VkCommandBufferManager(VulkanRenderDevice* fb);
	~VkCommandBufferManager();

	void BeginFrame();

	VulkanCommandBuffer* GetTransferCommands();
	VulkanCommandBuffer* GetDrawCommands();

	void FlushCommands(bool finish, bool lastsubmit = false, bool uploadOnly = false);

	void WaitForCommands(bool finish) { WaitForCommands(finish, false); }
	void WaitForCommands(bool finish, bool uploadOnly);

	// Frame overlap (Switch): the end of a frame submits and presents but does
	// not wait for the GPU; the wait happens here, at the start of the next
	// frame (or before any other submit), so the GPU draws frame N while the
	// CPU runs the game tics and input of frame N+1.
	void WaitForPendingFrame();

	void PushGroup(const FString& name);
	void PopGroup();
	void UpdateGpuStats();

	// GPU timeline of each frame for perf.log (Switch): timestamps at the frame
	// start, the end of the 3D scene, the end of post-processing and the frame
	// end (after the present copy). They are read back without waiting once the
	// frame's fence has passed, so measuring adds no CPU/GPU synchronisation.
	enum GpuMark { GpuFrameStart, GpuSceneEnd, GpuPostEnd, GpuFrameEnd, GpuMarkCount };
	void MarkGpu(GpuMark mark);

	class DeleteList
	{
	public:
		std::vector<std::unique_ptr<VulkanBuffer>> Buffers;
		std::vector<std::unique_ptr<VulkanSampler>> Samplers;
		std::vector<std::unique_ptr<VulkanImage>> Images;
		std::vector<std::unique_ptr<VulkanImageView>> ImageViews;
		std::vector<std::unique_ptr<VulkanFramebuffer>> Framebuffers;
		std::vector<std::unique_ptr<VulkanAccelerationStructure>> AccelStructs;
		std::vector<std::unique_ptr<VulkanDescriptorPool>> DescriptorPools;
		std::vector<std::unique_ptr<VulkanDescriptorSet>> Descriptors;
		std::vector<std::unique_ptr<VulkanShader>> Shaders;
		std::vector<std::unique_ptr<VulkanCommandBuffer>> CommandBuffers;
		size_t TotalSize = 0;

		void Add(std::unique_ptr<VulkanBuffer> obj) { if (obj) { TotalSize += obj->size; Buffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanSampler> obj) { if (obj) { Samplers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImage> obj) { if (obj) { Images.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImageView> obj) { if (obj) { ImageViews.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanFramebuffer> obj) { if (obj) { Framebuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanAccelerationStructure> obj) { if (obj) { AccelStructs.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorPool> obj) { if (obj) { DescriptorPools.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorSet> obj) { if (obj) { Descriptors.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanCommandBuffer> obj) { if (obj) { CommandBuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanShader> obj) { if (obj) { Shaders.push_back(std::move(obj)); } }
	};

	std::unique_ptr<DeleteList> TransferDeleteList = std::make_unique<DeleteList>();
	std::unique_ptr<DeleteList> DrawDeleteList = std::make_unique<DeleteList>();

	void DeleteFrameObjects(bool uploadOnly = false);

private:
	void FlushCommands(VulkanCommandBuffer** commands, size_t count, bool finish, bool lastsubmit);

	VulkanRenderDevice* fb = nullptr;

	std::unique_ptr<VulkanCommandPool> mCommandPool;

	std::unique_ptr<VulkanCommandBuffer> mTransferCommands;
	std::unique_ptr<VulkanCommandBuffer> mDrawCommands;

	enum { maxConcurrentSubmitCount = 8 };
	std::unique_ptr<VulkanSemaphore> mSubmitSemaphore[maxConcurrentSubmitCount];
	std::unique_ptr<VulkanFence> mSubmitFence[maxConcurrentSubmitCount];
	VkFence mSubmitWaitFences[maxConcurrentSubmitCount];
	int mNextSubmit = 0;

	bool mFramePending = false;
	int mPendingFences = 0;
	std::unique_ptr<DeleteList> mPendingTransferDelete;
	std::unique_ptr<DeleteList> mPendingDrawDelete;

	struct TimestampQuery
	{
		FString name;
		uint32_t startIndex;
		uint32_t endIndex;
	};

	enum { GpuMarkSets = 3 };
	std::unique_ptr<VulkanQueryPool> mGpuMarkPool;
	int mGpuMarkSet = 0;
	unsigned mGpuMarksWritten[GpuMarkSets] = {};
	bool mGpuMarkSubmitted[GpuMarkSets] = {};
	void ReadGpuMarks();

	enum { MaxTimestampQueries = 100 };
	std::unique_ptr<VulkanQueryPool> mTimestampQueryPool;
	void CreateTimestampQueries();
	int mNextTimestampQuery = 0;
	std::vector<size_t> mGroupStack;
	std::vector<TimestampQuery> timeElapsedQueries;
};
