/*
** vk_asynctex.cpp
** Sprite pictures decoded in the background for the Vulkan backend (Switch).
**
** A sprite picture that is not on the GPU yet used to be read from the
** archive, decoded and uploaded in the middle of the frame that first shows
** it. The first explosion of a session in Brutal Doom brings dozens of them
** (fire, smoke, debris, gore), which cost 100-200 ms of a single frame on the
** Switch. Here the decoding runs on the nx_jobs worker (core 2); the game
** thread only copies finished pictures to the GPU, a few per frame, and
** leaves a sprite out of the frame until its picture is there (one or two
** frames for something that was not prefetched).
**
** Pictures of actor classes are queued ahead of time when the class first
** shows up in a level (hw_nxprefetch.cpp), so most are ready before they
** are needed. Translated sprites, indexed and upscaled textures and HUD
** graphics keep the normal synchronous path.
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#include "vk_asynctex.h"

#ifdef __SWITCH__

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "vk_hwtexture.h"
#include "vulkan/system/vk_renderdevice.h"
#include "c_cvars.h"
#include "hw_material.h"
#include "nx_jobs.h"
#include "nx_perf.h"
#include "textures.h"

CVAR(Bool, rd_vk_async_textures, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// game thread time per frame for copying finished pictures to the GPU
CVAR(Float, rd_vk_async_upload_ms, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

extern thread_local bool nx_asyncworker;
extern void (*NXPrefetchSpriteHook)(FGameTexture *gtex, int priority);
extern int (*NXSpriteReadyHook)(FGameTexture *gtex);
extern bool (*NXSpriteDrawableHook)(FGameTexture *gtex, int translation, int scaleflags);
extern bool (*NXSpriteLoadedHook)(FGameTexture *gtex, int scaleflags);
extern void (*NXSpriteRequestHook)(FGameTexture *gtex, int scaleflags);
extern bool NXSpriteDecoderActive;
void NXSpriteTrimFromBuffer(FTexture *tex, const FTextureBuffer &tb);
void NXSprite_ServiceWanted();

namespace
{
	struct Key
	{
		FTexture *tex;
		int flags;
		bool operator==(const Key &o) const { return tex == o.tex && flags == o.flags; }
	};

	struct KeyHash
	{
		size_t operator()(const Key &k) const { return std::hash<void *>()(k.tex) ^ (size_t(k.flags) * 0x9E3779B97F4A7C15ull); }
	};

	struct Job
	{
		std::atomic<bool> started{ false }, done{ false }, bumped{ false };
		FTextureBuffer result;
		int64_t bytes = 0;
		uint64_t lastRequest = 0;
		int priority = NXJOB_LOW;
		// prefetched sprite: uploaded at the start of a frame, before it is needed
		FGameTexture *gtex = nullptr;
		int scaleFlags = 0;
		bool queued = false;	// in readyNow or readyLater
	};

	VulkanRenderDevice *device = nullptr;
	std::mutex jobMutex;	// jobs, ready queues, Job::gtex/queued/priority
	std::unordered_map<Key, std::shared_ptr<Job>, KeyHash> jobs;
	// decoded, waiting for the upload: what is on screen first, then prefetches
	std::deque<Key> readyNow, readyLater;
	std::atomic<int> generation{ 0 };
	std::atomic<int64_t> pendingBytes{ 0 };	// decoded, waiting for the upload
	const int64_t PrefetchCap = 96ll << 20;	// prefetches stop above this
	const int64_t PreuploadCap = 192ll << 20;	// prefetched pictures sent ahead per level
	int64_t preuploaded = 0;
	uint64_t frame = 0;
	int uploadsThisFrame = 0;
	double uploadMsThisFrame = 0;
	// first frames of a level: everything synchronous, so the first view is complete
	const int WarmupFrames = 3;
	int warmup = WarmupFrames;

	bool Active()
	{
		return device != nullptr && rd_vk_async_textures && NXJobs_Enabled();
	}

	bool Eligible(FTexture *tex, int flags)
	{
		if (flags & (CTF_Indexed | CTF_Upscale | CTF_CheckOnly))
			return false;
		return tex != nullptr && !tex->isHardwareCanvas() && tex->GetImage() != nullptr;
	}

	void Run(const std::shared_ptr<Job> &job, FTexture *tex, int flags, int gen, bool capped)
	{
		if (job->started.exchange(true))
			return;
		if (gen != generation.load() || (capped && !job->bumped && pendingBytes.load() > PrefetchCap))
		{
			// level changed, or too much is waiting already: a real request queues it again
			std::lock_guard<std::mutex> lock(jobMutex);
			auto it = jobs.find(Key{ tex, flags });
			if (it != jobs.end() && it->second == job)
				jobs.erase(it);
			return;
		}
		nx_asyncworker = true;
		FTextureBuffer tb = tex->CreateTexBuffer(0, flags);
		nx_asyncworker = false;
		NXSpriteTrimFromBuffer(tex, tb);	// the sprite's border trim comes with the same decode
		job->bytes = int64_t(tb.mWidth) * tb.mHeight * 4;
		pendingBytes += job->bytes;
		job->result = std::move(tb);
		std::lock_guard<std::mutex> lock(jobMutex);
		job->done = true;
		if (job->gtex != nullptr && !job->queued)
		{
			job->queued = true;
			(job->priority >= NXJOB_NOW ? readyNow : readyLater).push_back(Key{ tex, flags });
		}
	}

	// call with jobMutex held
	std::shared_ptr<Job> Start(const Key &key, int priority, FGameTexture *gtex, int scaleFlags)
	{
		auto job = std::make_shared<Job>();
		job->lastRequest = frame;
		job->priority = priority;
		job->gtex = gtex;
		job->scaleFlags = scaleFlags;
		jobs.emplace(key, job);
		const int gen = generation.load();
		FTexture *tex = key.tex;
		const int flags = key.flags;
		const bool capped = priority < NXJOB_NOW;
		NXJobs_Push([job, tex, flags, gen, capped]() { Run(job, tex, flags, gen, capped); }, priority);
		return job;
	}

	// call with jobMutex held: a queued job is needed now, push it again at the top
	void Bump(const Key &key, const std::shared_ptr<Job> &job)
	{
		if (job->started || job->bumped.exchange(true))
			return;
		job->priority = NXJOB_NOW;
		const int gen = generation.load();
		FTexture *tex = key.tex;
		const int flags = key.flags;
		NXJobs_Push([job, tex, flags, gen]() { Run(job, tex, flags, gen, false); }, NXJOB_NOW);
	}

	void Upload(VkHardwareTexture *hw, FTextureBuffer &tb)
	{
		const double start = NXPerf_NowMS();
		hw->CreateFromBuffer(tb);
		uploadMsThisFrame += NXPerf_NowMS() - start;
		uploadsThisFrame++;
		nxperf.tex_async++;
	}

	// 1 = on the GPU (uploaded now if it was decoded), 0 = not yet
	int Request(VkHardwareTexture *hw, FTexture *tex, int flags, FGameTexture *gtex, int scaleFlags, bool force)
	{
		FTextureBuffer tb;
		{
			std::lock_guard<std::mutex> lock(jobMutex);
			const Key key{ tex, flags };
			auto it = jobs.find(key);
			if (it == jobs.end())
			{
				Start(key, NXJOB_NOW, gtex, scaleFlags);
				nxperf.tex_wait++;
				return 0;
			}
			auto job = it->second;
			job->lastRequest = frame;
			if (!job->done)
			{
				if (job->gtex == nullptr)
				{
					job->gtex = gtex;
					job->scaleFlags = scaleFlags;
				}
				Bump(key, job);
				nxperf.tex_wait++;
				return 0;
			}
			if (job->result.mBuffer != nullptr && !force && uploadsThisFrame >= 2 && uploadMsThisFrame >= rd_vk_async_upload_ms)
			{
				nxperf.tex_budget++;
				return 0;
			}
			tb = std::move(job->result);
			pendingBytes -= job->bytes;
			jobs.erase(it);
		}
		if (tb.mBuffer == nullptr)
			return 1;	// decoding failed: the normal path deals with it
		Upload(hw, tb);
		return 1;
	}

	// 1 = every layer is on the GPU, 0 = still decoding, -1 = just queued
	int Layers(FGameTexture *gtex, int scaleFlags, bool force)
	{
		FMaterial *mat = FMaterial::ValidateTexture(gtex, scaleFlags, true);
		if (mat == nullptr)
			return 1;
		int ready = 1;
		MaterialLayerInfo *layer;
		for (int i = 0; i < mat->NumLayers(); i++)
		{
			auto hw = static_cast<VkHardwareTexture *>(mat->GetLayer(i, 0, &layer));
			if (hw == nullptr || hw->HasImage() || layer->layerTexture == nullptr)
				continue;
			const int flags = layer->scaleFlags | CTF_ProcessData;
			if (!Eligible(layer->layerTexture, flags))
				continue;
			if (Request(hw, layer->layerTexture, flags, gtex, scaleFlags, force) == 0)
				ready = 0;
		}
		return ready;
	}

	bool SpriteDrawable(FGameTexture *gtex, int translation, int scaleflags)
	{
		if (translation != 0 || warmup > 0 || gtex == nullptr || !gtex->isValid() || !Active())
			return true;
		if (Layers(gtex, scaleflags, false) > 0)
			return true;
		nxperf.spriteskips++;
		return false;
	}

	// weapon frames: never wait for the upload budget, the HUD holds the previous frame meanwhile
	int SpriteReady(FGameTexture *gtex)
	{
		if (warmup > 0 || gtex == nullptr || !gtex->isValid() || !Active())
			return 1;
		return Layers(gtex, CTF_Expand, true);
	}

	void Prefetch(FGameTexture *gtex, int priority, int scaleFlags)
	{
		if (gtex == nullptr || !gtex->isValid() || !Active())
			return;
		if (priority < NXJOB_NOW && pendingBytes.load() > PrefetchCap)
			return;
		FMaterial *mat = FMaterial::ValidateTexture(gtex, scaleFlags, true);
		if (mat == nullptr)
			return;
		MaterialLayerInfo *layer;
		for (int i = 0; i < mat->NumLayers(); i++)
		{
			auto hw = static_cast<VkHardwareTexture *>(mat->GetLayer(i, 0, &layer));
			if (hw == nullptr || hw->HasImage() || layer->layerTexture == nullptr)
				continue;
			const int flags = layer->scaleFlags | CTF_ProcessData;
			if (!Eligible(layer->layerTexture, flags))
				continue;
			std::lock_guard<std::mutex> lock(jobMutex);
			const Key key{ layer->layerTexture, flags };
			auto it = jobs.find(key);
			if (it == jobs.end())
				Start(key, priority, gtex, scaleFlags);
			else if (priority >= NXJOB_NOW)
			{
				auto &job = it->second;
				job->lastRequest = frame;
				if (job->gtex == nullptr)
				{
					job->gtex = gtex;
					job->scaleFlags = scaleFlags;
				}
				if (!job->done)
					Bump(key, job);
				else if (job->priority < NXJOB_NOW && job->queued)
				{
					// decoded as a prefetch and still waiting: upload it first
					job->priority = NXJOB_NOW;
					readyNow.push_back(key);
				}
				else if (!job->queued && job->result.mBuffer != nullptr)
				{
					job->queued = true;
					job->priority = NXJOB_NOW;
					readyNow.push_back(key);
				}
			}
		}
	}

	void PrefetchSprite(FGameTexture *gtex, int priority)
	{
		Prefetch(gtex, priority, CTF_Expand);
	}

	// the render thread missed this picture: decode and upload it first
	void RequestSprite(FGameTexture *gtex, int scaleFlags)
	{
		Prefetch(gtex, NXJOB_NOW, scaleFlags);
	}

	// any thread: every layer that goes through this path is on the GPU (creates nothing)
	bool SpriteLoaded(FGameTexture *gtex, int scaleFlags)
	{
		if (!Active() || gtex == nullptr)
			return true;
		FMaterial *mat = FMaterial::ValidateTexture(gtex, scaleFlags, false);
		if (mat == nullptr)
			return false;
		for (auto &layer : mat->GetLayerArray())
		{
			if (layer.layerTexture == nullptr || !Eligible(layer.layerTexture, layer.scaleFlags | CTF_ProcessData))
				continue;
			auto hw = static_cast<VkHardwareTexture *>(layer.layerTexture->FindHardwareTexture(layer.scaleFlags));
			if (hw == nullptr || !hw->HasImage())
				return false;
		}
		return true;
	}

	// Decoded pictures go to the GPU at the start of a frame: what the render
	// thread is waiting for first, then prefetches. This is the only place
	// world sprites are uploaded (the render thread holds them out until then).
	void PreUpload()
	{
		const double start = NXPerf_NowMS();
		const double budget = rd_vk_async_upload_ms;
		int count = 0;
		while (count < 24 && NXPerf_NowMS() - start < budget)
		{
			Key key;
			FGameTexture *gtex;
			int scaleFlags;
			FTextureBuffer tb;
			{
				std::lock_guard<std::mutex> lock(jobMutex);
				auto &q = !readyNow.empty() ? readyNow : readyLater;
				if (q.empty())
					break;
				key = q.front();
				q.pop_front();
				auto it = jobs.find(key);
				if (it == jobs.end())
					continue;
				auto &job = it->second;
				if (!job->queued || !job->done || job->result.mBuffer == nullptr || job->gtex == nullptr)
					continue;
				job->queued = false;
				if (job->priority < NXJOB_NOW)
				{
					// level prefetch: the rest stays decoded until it is needed
					if (preuploaded > PreuploadCap)
						continue;
					preuploaded += job->bytes;
				}
				gtex = job->gtex;
				scaleFlags = job->scaleFlags;
				tb = std::move(job->result);
				pendingBytes -= job->bytes;
				jobs.erase(it);
			}
			FMaterial *mat = FMaterial::ValidateTexture(gtex, scaleFlags, true);
			if (mat == nullptr)
				continue;
			MaterialLayerInfo *layer;
			for (int i = 0; i < mat->NumLayers(); i++)
			{
				auto hw = static_cast<VkHardwareTexture *>(mat->GetLayer(i, 0, &layer));
				if (hw != nullptr && !hw->HasImage() && layer->layerTexture == key.tex && (layer->scaleFlags | CTF_ProcessData) == key.flags)
				{
					Upload(hw, tb);
					nxperf.tex_preupload++;
					count++;
					break;
				}
			}
		}
	}
}

void VkAsyncTex_Install(VulkanRenderDevice *fb)
{
	device = fb;
	NXPrefetchSpriteHook = PrefetchSprite;
	NXSpriteReadyHook = SpriteReady;
	NXSpriteDrawableHook = SpriteDrawable;
	NXSpriteLoadedHook = SpriteLoaded;
	NXSpriteRequestHook = RequestSprite;
}

void VkAsyncTex_NewFrame()
{
	if (device == nullptr)
		return;
	frame++;
	uploadsThisFrame = 0;
	uploadMsThisFrame = 0;
	NXSpriteDecoderActive = Active();
	if (warmup > 0)
		warmup--;
	else if (NXSpriteDecoderActive)
	{
		NXSprite_ServiceWanted();
		PreUpload();
	}
	// drop pictures nobody asked for in about 15 seconds
	if ((frame & 63) == 0)
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		for (auto it = jobs.begin(); it != jobs.end();)
		{
			if (it->second->done && frame - it->second->lastRequest > 900)
			{
				pendingBytes -= it->second->bytes;
				it = jobs.erase(it);
			}
			else
				++it;
		}
	}
}

void VkAsyncTex_Flush()
{
	warmup = WarmupFrames;
	generation++;
	NXJobs_WaitAll();
	std::lock_guard<std::mutex> lock(jobMutex);
	jobs.clear();
	readyNow.clear();
	readyLater.clear();
	preuploaded = 0;
	pendingBytes = 0;
}

void VkAsyncTex_Shutdown()
{
	if (device == nullptr)
		return;
	NXPrefetchSpriteHook = nullptr;
	NXSpriteReadyHook = nullptr;
	NXSpriteDrawableHook = nullptr;
	NXSpriteLoadedHook = nullptr;
	NXSpriteRequestHook = nullptr;
	NXSpriteDecoderActive = false;
	VkAsyncTex_Flush();
	device = nullptr;
}

#else

void VkAsyncTex_Install(VulkanRenderDevice *) {}
void VkAsyncTex_NewFrame() {}
void VkAsyncTex_Flush() {}
void VkAsyncTex_Shutdown() {}

#endif
