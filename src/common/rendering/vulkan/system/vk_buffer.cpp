/*
** vk_buffer.cpp
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

#include "vk_buffer.h"
#include "v_video.h"
#include "vk_hwbuffer.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif

VkBufferManager::VkBufferManager(VulkanRenderDevice* fb) : fb(fb)
{
}

VkBufferManager::~VkBufferManager()
{
}

void VkBufferManager::Init()
{
	// one region per frame in flight (DFrameBuffer::PerFrameSlots)
	MatrixBuffer.reset(new VkStreamBuffer(this, sizeof(MatricesUBO), 50000 * screen->PerFrameSlots()));
	StreamBuffer.reset(new VkStreamBuffer(this, sizeof(StreamUBO), 300 * screen->PerFrameSlots()));

	CreateFanToTrisIndexBuffer();
}

void VkBufferManager::Deinit()
{
	while (!Buffers.empty())
		RemoveBuffer(Buffers.back());
}

void VkBufferManager::AddBuffer(VkHardwareBuffer* buffer)
{
	buffer->it = Buffers.insert(Buffers.end(), buffer);
}

void VkBufferManager::FlushPersistentBuffers()
{
#if defined(__SWITCH__)
	for (auto buf : Buffers)
	{
		if (buf && buf->mPersistent && buf->Memory() && buf->Size() > 0 && buf->NeedsCacheClean())
			armDCacheClean(buf->Memory(), buf->Size());
	}
#endif
}

void VkBufferManager::RemoveBuffer(VkHardwareBuffer* buffer)
{
	buffer->Reset();
	buffer->fb = nullptr;
	Buffers.erase(buffer->it);

	for (VkHardwareDataBuffer** knownbuf : { &ViewpointUBO, &LightBufferSSO, &LightNodes, &LightLines, &LightList, &BoneBufferSSO })
	{
		if (buffer == *knownbuf) *knownbuf = nullptr;
	}
}

IVertexBuffer* VkBufferManager::CreateVertexBuffer()
{
	return new VkHardwareVertexBuffer(fb);
}

IIndexBuffer* VkBufferManager::CreateIndexBuffer()
{
	return new VkHardwareIndexBuffer(fb);
}

IDataBuffer* VkBufferManager::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	auto buffer = new VkHardwareDataBuffer(fb, bindingpoint, ssbo, needsresize);

	switch (bindingpoint)
	{
	case LIGHTBUF_BINDINGPOINT: LightBufferSSO = buffer; break;
	case VIEWPOINT_BINDINGPOINT: ViewpointUBO = buffer; break;
	case LIGHTNODES_BINDINGPOINT: LightNodes = buffer; break;
	case LIGHTLINES_BINDINGPOINT: LightLines = buffer; break;
	case LIGHTLIST_BINDINGPOINT: LightList = buffer; break;
	case BONEBUF_BINDINGPOINT: BoneBufferSSO = buffer; break;
	case POSTPROCESS_BINDINGPOINT: break;
	default: break;
	}

	return buffer;
}

void VkBufferManager::CreateFanToTrisIndexBuffer()
{
	TArray<uint32_t> data;
	for (int i = 2; i < 1000; i++)
	{
		data.Push(0);
		data.Push(i - 1);
		data.Push(i);
	}

	FanToTrisIndexBuffer.reset(CreateIndexBuffer());
	FanToTrisIndexBuffer->SetData(sizeof(uint32_t) * data.Size(), data.Data(), BufferUsageType::Static);
}

/////////////////////////////////////////////////////////////////////////////

VkStreamBuffer::VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count)
{
	mBlockSize = static_cast<uint32_t>((structSize + screen->uniformblockalignment - 1) / screen->uniformblockalignment * screen->uniformblockalignment);

	UniformBuffer = (VkHardwareDataBuffer*)buffers->CreateDataBuffer(-1, false, false);
	UniformBuffer->SetData(mBlockSize * count, nullptr, BufferUsageType::Persistent);
	mRegionSize = mBlockSize * (uint32_t)(count / screen->PerFrameSlots());
	Reset();
}

void VkStreamBuffer::Reset()
{
	mStreamDataOffset = mRegionSize * screen->PerFrameSlot();
	mRegionEnd = mStreamDataOffset + mRegionSize;
}

VkStreamBuffer::~VkStreamBuffer()
{
	delete UniformBuffer;
}

uint32_t VkStreamBuffer::NextStreamDataBlock()
{
	mStreamDataOffset += mBlockSize;
	if (mStreamDataOffset + (size_t)mBlockSize >= mRegionEnd)
	{
		Reset();
		return 0xffffffff;
	}
	return mStreamDataOffset;
}
