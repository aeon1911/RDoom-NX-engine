/*
** gles_asynctex.cpp
** Background texture preparation for the GLES backend (Switch port).
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <deque>
#include "gles_system.h"
#include "gles_asynctex.h"
#include "gles_hwtexture.h"
#include "gles_renderstate.h"
#include "c_cvars.h"
#include "nx_jobs.h"
#include "nx_perf.h"
#include "i_time.h"
#include "hw_material.h"
#include "texturemanager.h"
#ifdef __SWITCH__
#include "common/platform/posix/nx/nx_glthread.h"
#endif

#ifdef __SWITCH__
CVAR(Bool, gles_async_textures, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Bool, gles_async_textures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif
// GPU upload budget per frame for finished sprites (HUD graphics are not limited)
CVAR(Float, gles_async_upload_ms, 3.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// the pixel processing for these is not thread safe: skip hole detection on workers
extern thread_local bool nx_asyncworker;
// defined with the game code (hw_nxprefetch.cpp)
extern void (*NXPrefetchSpriteHook)(FGameTexture *gtex, int priority);
extern int (*NXSpriteReadyHook)(FGameTexture *gtex);
extern bool (*NXSpriteDrawableHook)(FGameTexture *gtex, int translation, int scaleflags);

namespace OpenGLESRenderer
{

struct AsyncKey
{
	FTexture *tex;
	int flags;
	bool operator==(const AsyncKey &o) const { return tex == o.tex && flags == o.flags; }
};

struct AsyncKeyHash
{
	size_t operator()(const AsyncKey &k) const { return std::hash<void *>()(k.tex) ^ (size_t(k.flags) * 0x9E3779B97F4A7C15ull); }
};

struct AsyncJob
{
	std::atomic<bool> done{ false };
	std::atomic<bool> started{ false };
	std::atomic<bool> bumped{ false };	// somebody is waiting for it: queued again at top priority
	FTextureBuffer result;
	uint64_t lastRequest = 0;
	int64_t bytes = 0;	// decoded size while waiting for the upload
	bool hud = false;	// weapon frame: uploaded as soon as it's decoded
	// prefetched sprite: uploaded ahead of time when the frame has spare time
	FGameTexture *gtex = nullptr;
	int layer = 0;
	int priority = 0;
	bool queued = false;	// in preuploadQueue (protected by asyncMutex)
};

static std::mutex asyncMutex;
static std::unordered_map<AsyncKey, std::shared_ptr<AsyncJob>, AsyncKeyHash> asyncJobs;
static std::atomic<int> generation{ 0 };
static int uploadsThisFrame = 0;
static double uploadMsThisFrame = 0;
static double uploadStart = 0;
// right after a level starts the first couple of frames load everything
// directly, so the first view is complete; after that it's all background
static const int WARMUP_FRAMES = 3;
static int warmup = WARMUP_FRAMES;
static bool in2D = false;
static unsigned placeholder = 0;
static std::atomic<int64_t> pendingBytes{ 0 };
CVAR(Int, gles_async_prefetch_mb, 192, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// time per frame spent sending prefetched pictures to the GPU before they're needed
CVAR(Float, gles_async_preupload_ms, 1.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// GPU memory per level for pictures of monsters/effects sent ahead of time (the weapon is not limited)
CVAR(Int, gles_async_preupload_mb, 256, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static int64_t preuploadLowBytes = 0;
static uint64_t frameCounter = 0;
static std::deque<AsyncKey> preuploadQueue;	// finished prefetches, protected by asyncMutex
static bool preuploading = false;
// GL driver thread on: world sprites are only uploaded at the start of a frame
static bool lateUploads = false;

// Decodes the picture on a worker. The same job may be queued twice (once as
// a prefetch, once more at top priority when it's actually needed); only the
// first one to run does the work.
static void RunJob(const std::shared_ptr<AsyncJob> &job, FTexture *tex, int flags, int gen, bool capped)
{
	if (job->started.exchange(true)) return;
	if (gen != generation.load()) { job->done = true; return; }
	if (capped && !job->bumped && pendingBytes.load() > int64_t(gles_async_prefetch_mb) * 1024 * 1024)
	{
		// too much decoded data is waiting already: forget it, a real request will queue it again
		std::lock_guard<std::mutex> lock(asyncMutex);
		auto it = asyncJobs.find(AsyncKey{ tex, flags });
		if (it != asyncJobs.end() && it->second == job) asyncJobs.erase(it);
		return;
	}
	nx_asyncworker = true;
	FTextureBuffer tb = tex->CreateTexBuffer(0, flags);
	nx_asyncworker = false;
	job->bytes = int64_t(tb.mWidth) * tb.mHeight * 4;
	pendingBytes += job->bytes;
	job->result = std::move(tb);
	job->done = true;
	{
		// gtex may be set by the main thread while this runs: read it under the lock
		std::lock_guard<std::mutex> lock(asyncMutex);
		if (job->gtex != nullptr && !job->queued)
		{
			job->queued = true;
			preuploadQueue.push_back(AsyncKey{ tex, flags });
		}
	}
}

// call with asyncMutex held
static std::shared_ptr<AsyncJob> StartJob(const AsyncKey &key, int priority, bool capped, FGameTexture *gtex = nullptr, int layer = 0)
{
	auto job = std::make_shared<AsyncJob>();
	job->lastRequest = frameCounter;
	job->gtex = gtex;
	job->layer = layer;
	job->priority = priority;
	asyncJobs.emplace(key, job);
	int gen = generation.load();
	FTexture *tex = key.tex;
	int flags = key.flags;
	NXJobs_Push([job, tex, flags, gen, capped]() { RunJob(job, tex, flags, gen, capped); }, priority);
	return job;
}

// a job that's still waiting in the queue is needed now: jump the queue
static void Bump(const AsyncKey &key, const std::shared_ptr<AsyncJob> &job)
{
	if (job->started || job->bumped.exchange(true)) return;
	int gen = generation.load();
	FTexture *tex = key.tex;
	int flags = key.flags;
	NXJobs_Push([job, tex, flags, gen]() { RunJob(job, tex, flags, gen, false); }, NXJOB_NOW);
}

void AsyncTex_Set2D(bool on)
{
	in2D = on;
}

static bool Eligible(FTexture *tex, int translation, int flags)
{
	if (!gles_async_textures || !NXJobs_Enabled()) return false;
	if (translation != 0) return false;	// translation tables may change on the main thread
	if (flags & (CTF_Indexed | CTF_Upscale | CTF_CheckOnly)) return false;
	if (tex->isHardwareCanvas() || tex->GetImage() == nullptr) return false;
	// sprites (including the weapon) and 2D graphics; level textures stay synchronous
	return (flags & CTF_Expand) || in2D;
}

int AsyncTex_Request(FTexture *tex, int translation, int flags, FTextureBuffer &out)
{
	if (warmup > 0 || !Eligible(tex, translation, flags)) return -1;

	AsyncKey key{ tex, flags };
	std::shared_ptr<AsyncJob> job;
	{
		std::lock_guard<std::mutex> lock(asyncMutex);
		auto it = asyncJobs.find(key);
		if (it == asyncJobs.end())
		{
			StartJob(key, NXJOB_NOW, false);
			nxperf.tex_wait++;
			return 0;
		}
		job = it->second;
		job->lastRequest = frameCounter;
		if (!job->done) { Bump(key, job); nxperf.tex_wait++; return 0; }
		if (!in2D && !job->hud && !preuploading && uploadsThisFrame >= 2 && uploadMsThisFrame >= gles_async_upload_ms) { nxperf.tex_budget++; return 0; }
		uploadsThisFrame++;
		nxperf.tex_async++;
		uploadStart = NXPerf_NowMS();
		out = std::move(job->result);
		pendingBytes -= job->bytes;
		job->bytes = 0;
		asyncJobs.erase(it);
	}
	if (out.mBuffer == nullptr) return -1;	// decoding failed, let the normal path deal with it
	return 1;
}

void AsyncTex_BindPlaceholder(int texunit)
{
	if (placeholder == 0)
	{
		static const uint32_t clear = 0;
		glGenTextures(1, &placeholder);
		glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, placeholder);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &clear);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		FHardwareTexture::lastbound[texunit] = placeholder;
		return;
	}
	if (FHardwareTexture::lastbound[texunit] == placeholder) return;
	FHardwareTexture::lastbound[texunit] = placeholder;
	if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, placeholder);
	if (texunit != 0) glActiveTexture(GL_TEXTURE0);
}

void AsyncTex_Uploaded()
{
	if (uploadStart > 0) uploadMsThisFrame += NXPerf_NowMS() - uploadStart;
	uploadStart = 0;
}

// Sends a few finished prefetches to the GPU at the start of a frame, so
// they are ready (and their decoded copy freed) before anybody needs them.
static void PreUpload()
{
	double budget = gles_async_preupload_ms;
	int maxcount = 16;
	if (lateUploads)
	{
		// this is the only place where sprites go to the GPU now
		if (budget < 3.0) budget = 3.0;
		maxcount = 24;
	}
	if (budget <= 0) return;
	double start = NXPerf_NowMS();
	int count = 0;
	while (count < maxcount && NXPerf_NowMS() - start < budget)
	{
		AsyncKey key;
		FGameTexture *gtex;
		int layerindex;
		{
			std::lock_guard<std::mutex> lock(asyncMutex);
			if (preuploadQueue.empty()) break;
			key = preuploadQueue.front();
			preuploadQueue.pop_front();
			auto it = asyncJobs.find(key);
			if (it == asyncJobs.end()) continue;
			it->second->queued = false;	// a sprite that still needs it queues it again
			if (!it->second->done || it->second->result.mBuffer == nullptr || it->second->gtex == nullptr) continue;
			if (it->second->priority == NXJOB_LOW)
			{
				// stays decoded until it's needed (or dropped after a while)
				if (preuploadLowBytes > int64_t(gles_async_preupload_mb) * 1024 * 1024) continue;
				preuploadLowBytes += it->second->bytes;
			}
			gtex = it->second->gtex;
			layerindex = it->second->layer;
		}
		auto mat = FMaterial::ValidateTexture(gtex, CTF_Expand, true);
		if (mat == nullptr || layerindex >= mat->NumLayers()) continue;
		MaterialLayerInfo *layer;
		auto hw = static_cast<FHardwareTexture *>(mat->GetLayer(layerindex, 0, &layer));
		if (hw == nullptr || hw->GetTextureHandle() != 0 || layer->layerTexture != key.tex || (layer->scaleFlags | CTF_ProcessData) != key.flags) continue;
		preuploading = true;
		// No mipmaps in this batch. nx3.7 uploaded the same way. Building the
		// chain here, for every sprite of the pack, stalled on the title picture.
		NX_PushSkipCpuMipmaps();
		hw->BindOrCreate(key.tex, 0, CLAMP_XY_NOMIP, 0, layer->scaleFlags);
		NX_PopSkipCpuMipmaps();
		preuploading = false;
		count++;
	}
	if (count > 0)
	{
		nxperf.tex_preupload += count;
		FHardwareTexture::Unbind(0);
		gl_RenderState.ClearLastMaterial();
	}
}

void AsyncTex_NewFrame()
{
#ifdef __SWITCH__
	lateUploads = NX_GLThreadEnabled();
#endif
	frameCounter++;
	uploadsThisFrame = 0;
	uploadMsThisFrame = 0;
	if (warmup > 0) warmup--;
	else PreUpload();
	// drop finished pictures nobody asked for in a while (about 10 seconds)
	if ((frameCounter & 63) == 0)
	{
		std::lock_guard<std::mutex> lock(asyncMutex);
		for (auto it = asyncJobs.begin(); it != asyncJobs.end();)
		{
			if (it->second->done && frameCounter - it->second->lastRequest > 900)
			{
				pendingBytes -= it->second->bytes;
				it = asyncJobs.erase(it);
			}
			else ++it;
		}
	}
}

// called before level precaching and when the renderer goes away:
// nothing may still be working on textures after this returns.
void AsyncTex_Flush()
{
	warmup = WARMUP_FRAMES;
	generation++;
	NXJobs_WaitAll();
	std::lock_guard<std::mutex> lock(asyncMutex);
	asyncJobs.clear();
	preuploadQueue.clear();
	preuploadLowBytes = 0;
	pendingBytes = 0;
}

//==========================================================================
//
// Prefetching: decode pictures that are likely to be needed soon (the
// sprites of the weapon in hand, of monsters and effects when they first
// appear) before anybody asks for them.
//
//==========================================================================

static void PrefetchTexture(FTexture *tex, int flags, int priority, FGameTexture *gtex, int layer)
{
	if (!Eligible(tex, 0, flags)) return;
	bool capped = priority < NXJOB_NOW;
	if (capped && pendingBytes.load() > int64_t(gles_async_prefetch_mb) * 1024 * 1024) return;
	AsyncKey key{ tex, flags };
	std::lock_guard<std::mutex> lock(asyncMutex);
	auto it = asyncJobs.find(key);
	if (it != asyncJobs.end())
	{
		// about to be shown: move it up the queue
		if (priority >= NXJOB_NOW && !it->second->done)
		{
			it->second->priority = NXJOB_NOW;
			Bump(key, it->second);
		}
		return;
	}
	StartJob(key, priority, capped, gtex, layer);
}

static void PrefetchSprite(FGameTexture *gtex, int priority)
{
	if (!gles_async_textures || gtex == nullptr || !gtex->isValid()) return;
	auto mat = FMaterial::ValidateTexture(gtex, CTF_Expand, true);
	if (mat == nullptr) return;
	MaterialLayerInfo *layer;
	for (int i = 0; i < mat->NumLayers(); i++)
	{
		auto hw = static_cast<FHardwareTexture *>(mat->GetLayer(i, 0, &layer));
		if (hw == nullptr || hw->GetTextureHandle() != 0 || layer->layerTexture == nullptr) continue;
		PrefetchTexture(layer->layerTexture, layer->scaleFlags | CTF_ProcessData, priority, gtex, i);
	}
}

//==========================================================================
//
// Weapon frames: the HUD asks before drawing a new frame whether it can be
// shown right away. If not, it keeps showing the previous frame of that
// layer instead of making the gun disappear, and the new one is decoded
// with top priority.
//
//==========================================================================

static int SpriteReady(FGameTexture *gtex)
{
	if (!gles_async_textures || !NXJobs_Enabled() || warmup > 0 || gtex == nullptr || !gtex->isValid()) return 1;
	auto mat = FMaterial::ValidateTexture(gtex, CTF_Expand, true);
	if (mat == nullptr) return 1;
	int ready = 1;
	MaterialLayerInfo *layer;
	for (int i = 0; i < mat->NumLayers(); i++)
	{
		auto hw = static_cast<FHardwareTexture *>(mat->GetLayer(i, 0, &layer));
		if (hw == nullptr || hw->GetTextureHandle() != 0 || layer->layerTexture == nullptr) continue;
		FTexture *tex = layer->layerTexture;
		int flags = layer->scaleFlags | CTF_ProcessData;
		if (!Eligible(tex, 0, flags)) continue;
		std::lock_guard<std::mutex> lock(asyncMutex);
		AsyncKey key{ tex, flags };
		auto it = asyncJobs.find(key);
		if (it != asyncJobs.end())
		{
			auto &job = it->second;
			job->hud = true;
			job->lastRequest = frameCounter;
			if (!job->done) { Bump(key, job); if (ready > 0) ready = 0; }
			continue;
		}
		StartJob(key, NXJOB_NOW, false)->hud = true;
		ready = -1;
	}
	return ready;
}

//==========================================================================
//
// World sprites: asked right before the draw call. A sprite whose picture
// isn't on the GPU yet would be drawn with the transparent placeholder, which
// shows nothing but still costs a whole draw call - so it's skipped instead.
//
// With the GL driver thread on, pictures are not sent to the GPU in the middle
// of a frame at all (every upload makes the game wait for the driver thread):
// they go up at the start of the next frame, when the driver thread is idle.
//
//==========================================================================

static bool SpriteDrawable(FGameTexture *gtex, int translation, int scaleflags)
{
	if (translation != 0 || !gles_async_textures || warmup > 0 || gtex == nullptr || !(scaleflags & CTF_Expand)) return true;
	auto mat = FMaterial::ValidateTexture(gtex, scaleflags, true);
	if (mat == nullptr) return true;
	MaterialLayerInfo *layer;
	auto hw = static_cast<FHardwareTexture *>(mat->GetLayer(0, 0, &layer));
	if (hw == nullptr || hw->GetTextureHandle() != 0 || layer->layerTexture == nullptr) return true;
	FTexture *tex = layer->layerTexture;
	int flags = layer->scaleFlags | CTF_ProcessData;
	if (!Eligible(tex, 0, flags)) return true;

	std::lock_guard<std::mutex> lock(asyncMutex);
	AsyncKey key{ tex, flags };
	auto it = asyncJobs.find(key);
	if (it == asyncJobs.end())
	{
		// remember the game texture so the finished picture can be uploaded ahead of the draw
		StartJob(key, NXJOB_NOW, false, gtex, 0);
		nxperf.tex_wait++;
		nxperf.spriteskips++;
		return false;
	}
	auto &job = it->second;
	job->lastRequest = frameCounter;
	if (!job->done)
	{
		if (job->gtex == nullptr) { job->gtex = gtex; job->layer = 0; }
		Bump(key, job);
		nxperf.spriteskips++;
		return false;
	}
	if (job->result.mBuffer == nullptr || job->hud) return true;	// failed decode (normal path) / weapon frame (never waits)
	if (lateUploads)
	{
		if (!job->queued)
		{
			if (job->gtex == nullptr) { job->gtex = gtex; job->layer = 0; }
			job->priority = NXJOB_NOW;	// needed now: not subject to the prefetch memory cap
			job->queued = true;
			preuploadQueue.push_back(key);
		}
		nxperf.spriteskips++;
		return false;
	}
	if (uploadsThisFrame >= 2 && uploadMsThisFrame >= gles_async_upload_ms)
	{
		nxperf.tex_budget++;
		nxperf.spriteskips++;
		return false;
	}
	return true;
}

void AsyncTex_Shutdown()
{
	NXPrefetchSpriteHook = nullptr;
	NXSpriteReadyHook = nullptr;
	NXSpriteDrawableHook = nullptr;
	AsyncTex_Flush();
	if (placeholder != 0)
	{
		glDeleteTextures(1, &placeholder);
		placeholder = 0;
	}
}

}

// set from outside the GLES backend (level precaching)
void GLES_AsyncTexFlush()
{
	OpenGLESRenderer::AsyncTex_Flush();
}

namespace OpenGLESRenderer
{
	void AsyncTex_Install()
	{
		NXPrefetchSpriteHook = PrefetchSprite;
		NXSpriteReadyHook = SpriteReady;
		NXSpriteDrawableHook = SpriteDrawable;
	}
}
