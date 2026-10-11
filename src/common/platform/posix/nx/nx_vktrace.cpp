/* Diagnostic: counts the Vulkan commands the engine records and logs them at
 * each vkQueueSubmit (boot.log). Built only with -DRDOOM_VK_TRACE=ON. */
#ifdef __SWITCH__
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
void RDoom_Crumb(const char *what);
extern "C" {
static unsigned counts[17];
static const char *names[17] = { "vkCmdFillBuffer", "vkCmdUpdateBuffer", "vkCmdCopyBuffer", "vkCmdCopyBufferToImage", "vkCmdCopyImageToBuffer", "vkCmdCopyImage", "vkCmdBlitImage", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage", "vkCmdClearAttachments", "vkCmdResetQueryPool", "vkCmdDispatch", "vkCmdResolveImage", "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdBeginRenderPass", "vkCmdPipelineBarrier" };
static unsigned submits;
void __real_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d);
void __wrap_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d) { counts[0]++; __real_vkCmdFillBuffer(c,b,o,s,d); }
void __real_vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void *d);
void __wrap_vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void *d) { counts[1]++; __real_vkCmdUpdateBuffer(c,b,o,s,d); }
void __real_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy *r);
void __wrap_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy *r) { counts[2]++; __real_vkCmdCopyBuffer(c,a,b,n,r); }
void __real_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer a, VkImage b, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r);
void __wrap_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer a, VkImage b, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { counts[3]++; __real_vkCmdCopyBufferToImage(c,a,b,l,n,r); }
void __real_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage a, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r);
void __wrap_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage a, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) { counts[4]++; __real_vkCmdCopyImageToBuffer(c,a,l,b,n,r); }
void __real_vkCmdCopyImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageCopy *r);
void __wrap_vkCmdCopyImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageCopy *r) { counts[5]++; __real_vkCmdCopyImage(c,a,la,b,lb,n,r); }
void __real_vkCmdBlitImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageBlit *r, VkFilter f);
void __wrap_vkCmdBlitImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageBlit *r, VkFilter f) { counts[6]++; __real_vkCmdBlitImage(c,a,la,b,lb,n,r,f); }
void __real_vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r);
void __wrap_vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) { counts[7]++; __real_vkCmdClearColorImage(c,i,l,v,n,r); }
void __real_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r);
void __wrap_vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) { counts[8]++; __real_vkCmdClearDepthStencilImage(c,i,l,v,n,r); }
void __real_vkCmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *a, uint32_t m, const VkClearRect *r);
void __wrap_vkCmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *a, uint32_t m, const VkClearRect *r) { counts[9]++; __real_vkCmdClearAttachments(c,n,a,m,r); }
void __real_vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool p, uint32_t f, uint32_t n);
void __wrap_vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool p, uint32_t f, uint32_t n) { counts[10]++; __real_vkCmdResetQueryPool(c,p,f,n); }
void __real_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z);
void __wrap_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { counts[11]++; __real_vkCmdDispatch(c,x,y,z); }
void __real_vkCmdResolveImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageResolve *r);
void __wrap_vkCmdResolveImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageResolve *r) { counts[12]++; __real_vkCmdResolveImage(c,a,la,b,lb,n,r); }
void __real_vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e);
void __wrap_vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { counts[13]++; __real_vkCmdDraw(c,a,b,d,e); }
void __real_vkCmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f);
void __wrap_vkCmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f) { counts[14]++; __real_vkCmdDrawIndexed(c,a,b,d,e,f); }
void __real_vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *i, VkSubpassContents s);
void __wrap_vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *i, VkSubpassContents s) { counts[15]++; __real_vkCmdBeginRenderPass(c,i,s); }
void __real_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t a, const VkMemoryBarrier *b, uint32_t e, const VkBufferMemoryBarrier *g, uint32_t h, const VkImageMemoryBarrier *i);
void __wrap_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t a, const VkMemoryBarrier *b, uint32_t e, const VkBufferMemoryBarrier *g, uint32_t h, const VkImageMemoryBarrier *i) { counts[16]++; __real_vkCmdPipelineBarrier(c,s,d,f,a,b,e,g,h,i); }
VkResult __real_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f);
VkResult __wrap_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
	char buf[1024];
	int len = snprintf(buf, sizeof(buf), "vk submit #%u:", ++submits);
	for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
		if (counts[i] && len < (int)sizeof(buf) - 64)
			len += snprintf(buf + len, sizeof(buf) - len, " %s=%u", names[i] + 5, counts[i]);
	if (submits <= 40)
		RDoom_Crumb(buf);
	memset(counts, 0, sizeof(counts));
	return __real_vkQueueSubmit(q, n, s, f);
}
}
#endif
