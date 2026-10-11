/*
** vk_nxupload.cpp
** Texture uploads on the Switch: CPU-written linear image + vkCmdBlitImage.
**
** NVK has one queue family on the GM20B and it has the compute bit, so it
** implements vkCmdCopyBufferToImage with an internal compute shader. Every
** texture upload therefore switches the channel between compute and 3D work.
** On the Tegra X1's unified memory the pixels can instead be written straight
** into a linear image that the 3D engine then blits into the optimally tiled
** texture (NVK's blit is a 3D meta draw); the staging buffer is not needed.
** This is also the path Ryujinx can emulate: it does not run NVK's internal
** compute shaders correctly, and textures uploaded that way stay black
** (vktest/matrix, evidence/vkmatrix).
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#include "vk_nxupload.h"

#ifdef __SWITCH__

#include <string.h>
#include <map>
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "c_cvars.h"

CVAR(Bool, rd_vk_blitupload, true, 0)

static bool FormatSupported(VulkanRenderDevice *fb, VkFormat format)
{
	static std::map<VkFormat, bool> cache;
	auto it = cache.find(format);
	if (it != cache.end())
		return it->second;
	VkFormatProperties props = {};
	vkGetPhysicalDeviceFormatProperties(fb->device->PhysicalDevice.Device, format, &props);
	bool ok = (props.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
		(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
	cache[format] = ok;
	return ok;
}

bool NX_UploadViaBlit(VulkanRenderDevice *fb, VulkanCommandBuffer *cmd, VkImage dst, VkFormat format,
	int w, int h, int pixelsize, const void *pixels, int layer)
{
	if (!rd_vk_blitupload || !pixels || w <= 0 || h <= 0 || !FormatSupported(fb, format))
		return false;

	auto src = ImageBuilder()
		.Format(format)
		.Size(w, h)
		.LinearTiling()
		.InitialLayout(VK_IMAGE_LAYOUT_PREINITIALIZED)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.DebugName("NX_UploadViaBlit.linear")
		.TryCreate(fb->device.get());
	if (!src)
		return false;

	VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
	VkSubresourceLayout layout = {};
	vkGetImageSubresourceLayout(fb->device->device, src->image, &sub, &layout);
	uint8_t *mem = (uint8_t *)src->Map(0, (size_t)(layout.offset + layout.size));
	if (!mem)
		return false;
	const size_t rowBytes = (size_t)w * pixelsize;
	const uint8_t *in = (const uint8_t *)pixels;
	for (int y = 0; y < h; y++)
		memcpy(mem + layout.offset + (size_t)y * layout.rowPitch, in + (size_t)y * rowBytes, rowBytes);
	{
		// The GPU does not snoop the CPU dcache: memory mapped cached must be
		// cleaned before the blit reads it (uncached memory needs nothing).
		if (src->MemoryProperties() & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
			armDCacheClean(mem, (size_t)(layout.offset + layout.size));
	}
	src->Unmap();

	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = src->image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	cmd->pipelineBarrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	VkImageBlit blit = {};
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.srcOffsets[1] = { w, h, 1 };
	blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)layer, 1 };
	blit.dstOffsets[1] = { w, h, 1 };
	cmd->blitImage(src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	auto *list = fb->GetCommands()->TransferDeleteList.get();
	list->TotalSize += (size_t)(layout.offset + layout.size);
	list->Add(std::move(src));
	return true;
}

#else

bool NX_UploadViaBlit(VulkanRenderDevice *, VulkanCommandBuffer *, VkImage, VkFormat, int, int, int, const void *, int)
{
	return false;
}

#endif
