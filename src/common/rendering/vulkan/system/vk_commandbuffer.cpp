/*
** vk_commandbuffer.cpp
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

#include "vk_commandbuffer.h"
#include "vk_renderdevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_clock.h"
#include "v_video.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "nx_perf.h"
#include "c_cvars.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

extern int rendered_commandbuffers;
int current_rendered_commandbuffers;

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

VkCommandBufferManager::VkCommandBufferManager(VulkanRenderDevice* fb) : fb(fb)
{
	mCommandPool = CommandPoolBuilder()
		.QueueFamily(fb->device->GraphicsFamily)
		.DebugName("mCommandPool")
		.Create(fb->device.get());

	for (auto& semaphore : mSubmitSemaphore)
		semaphore.reset(new VulkanSemaphore(fb->device.get()));

	for (auto& fence : mSubmitFence)
		fence.reset(new VulkanFence(fb->device.get()));

	for (int i = 0; i < maxConcurrentSubmitCount; i++)
		mSubmitWaitFences[i] = mSubmitFence[i]->fence;

#ifndef __SWITCH__
	CreateTimestampQueries();
#endif
	// Switch: created the first time "stat gpu" asks for GPU timings. NVK
	// resets query pools with an internal compute dispatch, which costs a
	// submit at start-up for a profiling feature most sessions never use.
}

void VkCommandBufferManager::CreateTimestampQueries()
{
	if (mTimestampQueryPool || !fb->device->GraphicsTimeQueries)
		return;
	mTimestampQueryPool = QueryPoolBuilder()
		.QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxTimestampQueries)
		.Create(fb->device.get());
	GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, MaxTimestampQueries);
}

VkCommandBufferManager::~VkCommandBufferManager()
{
}

VulkanCommandBuffer* VkCommandBufferManager::GetTransferCommands()
{
	if (!mTransferCommands)
	{
		mTransferCommands = mCommandPool->createBuffer();
		mTransferCommands->SetDebugName("VulkanRenderDevice.mTransferCommands");
		mTransferCommands->begin();
	}
	return mTransferCommands.get();
}

VulkanCommandBuffer* VkCommandBufferManager::GetDrawCommands()
{
	if (!mDrawCommands)
	{
		mDrawCommands = mCommandPool->createBuffer();
		mDrawCommands->SetDebugName("VulkanRenderDevice.mDrawCommands");
		mDrawCommands->begin();
	}
	return mDrawCommands.get();
}

void VkCommandBufferManager::BeginFrame()
{
	if (mNextTimestampQuery > 0)
	{
		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, mNextTimestampQuery);
		mNextTimestampQuery = 0;
	}
}

#if defined(__SWITCH__)
CVAR(Bool, rd_vk_frameoverlap, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif

void VkCommandBufferManager::WaitForPendingFrame()
{
	if (!mFramePending)
		return;
	NXPERF_SCOPE(vkpending_ms, nullptr);
	vkWaitForFences(fb->device->device, mPendingFences, mSubmitWaitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(fb->device->device, mPendingFences, mSubmitWaitFences);
	mPendingTransferDelete.reset();
	mPendingDrawDelete.reset();
	mFramePending = false;
	mPendingFences = 0;
}

void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, size_t count, bool finish, bool lastsubmit)
{
	// the fences and command buffers of the previous frame are reused from index 0
	WaitForPendingFrame();

	int currentIndex = mNextSubmit % maxConcurrentSubmitCount;

	if (mNextSubmit >= maxConcurrentSubmitCount)
	{
		NXPERF_SCOPE(gpuwait_ms, nullptr);
		vkWaitForFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence);
	}

	QueueSubmit submit;

	for (size_t i = 0; i < count; i++)
		submit.AddCommandBuffer(commands[i]);

	if (mNextSubmit > 0)
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[(mNextSubmit - 1) % maxConcurrentSubmitCount].get());

	auto framebuffers = fb->GetFramebufferManager();
	if (finish && framebuffers->PresentImageIndex != -1)
	{
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, framebuffers->SwapChainImageAvailableSemaphore.get());
		submit.AddSignal(framebuffers->RenderFinishedSemaphores[framebuffers->PresentImageIndex].get());
	}

	if (!lastsubmit)
		submit.AddSignal(mSubmitSemaphore[currentIndex].get());

#if defined(__SWITCH__)
	if (fb && fb->GetBufferManager())
	{
		NXPERF_SCOPE(vkclean_ms, nullptr);
		fb->GetBufferManager()->FlushPersistentBuffers();
	}
#endif

#if defined(__SWITCH__)
	{
		extern int nx_levelframes;
		static int logged;
		EXTERN_CVAR(Int, rd_vk_drawlog)
		extern int rd_vk_drawlog_started;
		if (rd_vk_drawlog > 0 && rd_vk_drawlog_started && logged < 3000)
		{
			void RDoom_Crumb(const char *what);
			char buf[96];
			snprintf(buf, sizeof(buf), "submit %d: %d command buffer(s)%s", ++logged, (int)count, finish ? ", frame end (post-processing + present)" : "");
			RDoom_Crumb(buf);
		}
	}
#endif
	{
		NXPERF_SCOPE(vksubmit_ms, &nxperf.vksubmits);
		submit.Execute(fb->device.get(), fb->device->GraphicsQueue, mSubmitFence[currentIndex].get());
	}
	mNextSubmit++;
}

void VkCommandBufferManager::FlushCommands(bool finish, bool lastsubmit, bool uploadOnly)
{
	if (!uploadOnly)
		fb->GetRenderState()->EndRenderPass();

	if ((!uploadOnly && mDrawCommands) || mTransferCommands)
	{
		VulkanCommandBuffer* commands[2];
		size_t count = 0;

		if (mTransferCommands)
		{
			mTransferCommands->end();
			commands[count++] = mTransferCommands.get();
			TransferDeleteList->Add(std::move(mTransferCommands));
		}

		if (!uploadOnly && mDrawCommands)
		{
			mDrawCommands->end();
			commands[count++] = mDrawCommands.get();
			DrawDeleteList->Add(std::move(mDrawCommands));
		}

		FlushCommands(commands, count, finish, lastsubmit);

		current_rendered_commandbuffers += (int)count;
	}
}

void VkCommandBufferManager::WaitForCommands(bool finish, bool uploadOnly)
{
	WaitForPendingFrame();

	if (finish)
	{
		Finish.Reset();
		Finish.Clock();

		fb->GetFramebufferManager()->AcquireImage();
	}

	FlushCommands(finish, true, uploadOnly);

	if (finish)
	{
		if (!fb->GetVSync())
			fb->FPSLimit();
		fb->GetFramebufferManager()->QueuePresent();
	}

	int numWaitFences = min(mNextSubmit, (int)maxConcurrentSubmitCount);

#if defined(__SWITCH__)
	// End of a presented frame: leave the GPU work in flight. Everything the
	// frame used (command buffers, staging memory, deleted objects) stays
	// alive until WaitForPendingFrame. GPU timestamp queries need the wait.
	if (finish && !uploadOnly && rd_vk_frameoverlap && !gpuStatActive && numWaitFences > 0)
	{
		mFramePending = true;
		mPendingFences = numWaitFences;
		mPendingTransferDelete = std::move(TransferDeleteList);
		mPendingDrawDelete = std::move(DrawDeleteList);
		DeleteFrameObjects(false);
		mNextSubmit = 0;
		Finish.Unclock();
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
		return;
	}
#endif

	if (numWaitFences > 0)
	{
		NXPERF_SCOPE(gpuwait_ms, nullptr);
		vkWaitForFences(fb->device->device, numWaitFences, mSubmitWaitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, numWaitFences, mSubmitWaitFences);
	}

	DeleteFrameObjects(uploadOnly);
	mNextSubmit = 0;

	if (finish)
	{
		Finish.Unclock();
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
	}
}

void VkCommandBufferManager::DeleteFrameObjects(bool uploadOnly)
{
	TransferDeleteList = std::make_unique<DeleteList>();
	if (!uploadOnly)
		DrawDeleteList = std::make_unique<DeleteList>();
}

void VkCommandBufferManager::PushGroup(const FString& name)
{
	if (!gpuStatActive)
		return;

	CreateTimestampQueries();
	if (mTimestampQueryPool && mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		TimestampQuery q;
		q.name = name;
		q.startIndex = mNextTimestampQuery++;
		q.endIndex = 0;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.startIndex);
		mGroupStack.push_back(timeElapsedQueries.size());
		timeElapsedQueries.push_back(q);
	}
}

void VkCommandBufferManager::PopGroup()
{
	if (!gpuStatActive || mGroupStack.empty())
		return;

	TimestampQuery& q = timeElapsedQueries[mGroupStack.back()];
	mGroupStack.pop_back();

	if (mTimestampQueryPool && mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		q.endIndex = mNextTimestampQuery++;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.endIndex);
	}
}

void VkCommandBufferManager::UpdateGpuStats()
{
	uint64_t timestamps[MaxTimestampQueries];
	if (mNextTimestampQuery > 0 && mTimestampQueryPool)
		mTimestampQueryPool->getResults(0, mNextTimestampQuery, sizeof(uint64_t) * mNextTimestampQuery, timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	double timestampPeriod = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;

	gpuStatOutput = "";
	for (auto& q : timeElapsedQueries)
	{
		if (q.endIndex <= q.startIndex)
			continue;

		int64_t timeElapsed = max(static_cast<int64_t>(timestamps[q.endIndex] - timestamps[q.startIndex]), (int64_t)0);
		double timeNS = timeElapsed * timestampPeriod;

		FString out;
		out.Format("%s=%04.2f ms\n", q.name.GetChars(), timeNS / 1000000.0f);
		gpuStatOutput += out;
	}
	timeElapsedQueries.clear();
	mGroupStack.clear();

	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;
}
