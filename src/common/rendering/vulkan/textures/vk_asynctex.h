#pragma once
// Sprite pictures decoded in the background for the Vulkan backend (Switch),
// see vk_asynctex.cpp. Everything else is a no-op on other platforms.

class VulkanRenderDevice;

void VkAsyncTex_Install(VulkanRenderDevice *fb);
// start of a frame: sends finished prefetches to the GPU
void VkAsyncTex_NewFrame();
// level precaching and shutdown: nothing may still be decoding after this returns
void VkAsyncTex_Flush();
void VkAsyncTex_Shutdown();
