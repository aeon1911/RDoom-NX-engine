#pragma once
// Switch texture upload through the 3D engine (see vk_nxupload.cpp).

#include <zvulkan/vulkanobjects.h>

class VulkanRenderDevice;

// Copies w x h tightly packed pixels into mip 0 of one array layer of dst.
// dst must be in TRANSFER_DST_OPTIMAL and stays there. Returns false (and
// records nothing) when the format cannot take this path; the caller then
// uses vkCmdCopyBufferToImage.
bool NX_UploadViaBlit(VulkanRenderDevice *fb, VulkanCommandBuffer *cmd, VkImage dst, VkFormat format,
	int w, int h, int pixelsize, const void *pixels, int layer = 0);
