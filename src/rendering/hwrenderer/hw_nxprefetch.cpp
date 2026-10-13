/*
** hw_nxprefetch.cpp
** Sprite prefetching (Switch port)
**
** When an actor class shows up for the first time in a level (or a weapon
** is taken out), the pictures of all its animation frames are decoded in
** the background, so they are ready when the animation gets there instead
** of being loaded in the middle of a fight.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "actor.h"
#include "info.h"
#include "r_data/sprites.h"
#include "r_state.h"
#include "texturemanager.h"
#include "d_player.h"
#include "tarray.h"
#include "p_pspr.h"
#include "c_cvars.h"
#include "printf.h"
#include "nx_jobs.h"
#include "nx_perf.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string.h>
EXTERN_CVAR(Int, nx_perflog)

// priority: 0 = prefetch, 1 = likely needed soon, 2 = needed now
void (*NXPrefetchSpriteHook)(FGameTexture *gtex, int priority) = nullptr;
int (*NXSpriteReadyHook)(FGameTexture *gtex) = nullptr;
// false = a world sprite's image isn't on the GPU yet (still decoding or waiting
// for the upload budget): drawing it would only show a transparent placeholder
bool (*NXSpriteDrawableHook)(FGameTexture *gtex, int translation, int scaleflags) = nullptr;
static TMap<PClassActor *, bool> NXPrefetched;
static std::deque<FGameTexture *> NXPendingHigh, NXPendingLow;	// see NXPrefetch_Pump

// last frame of each weapon layer that could be shown
struct NXHeldFrame
{
	AActor *caller;
	FGameTexture *tex;
	bool mirror;
};
static TMap<DPSprite *, NXHeldFrame> NXHeldFrames;
// state each weapon layer was in when its upcoming frames were last queued
static TMap<DPSprite *, FState *> NXLookahead;

//==========================================================================
//
// Sprites that are not ready yet
//
// The first time a sprite picture is drawn, the renderer used to decode it
// twice on the game/render thread: once just to find the empty border
// around it (FTexture::TrimBorders) and once for the upload. An explosion
// in Brutal Doom brings dozens of new pictures at once: on the Switch that
// was 100-150 ms of PNG decoding inside one frame.
//
// Now the render thread only checks whether a picture is ready (border
// trim known, layers on the GPU - nothing is created there). If it is not,
// the game thread asks for it at the start of the next frame and the
// worker decodes it once; that decode also delivers the trim
// (NXSpriteTrimFromBuffer). Meanwhile an actor keeps showing the last
// picture it had (an animation holds its previous frame instead of
// flickering); an actor that has nothing to show yet is left out.
//
//==========================================================================

#ifdef __SWITCH__
CVAR(Bool, rd_async_sprites, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Bool, rd_async_sprites, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif

extern thread_local bool nx_asyncworker;

// set by a backend that decodes sprite pictures in the background (vk_asynctex.cpp)
bool NXSpriteDecoderActive = false;
// the picture's layers are on the GPU: never creates anything, callable from any thread
bool (*NXSpriteLoadedHook)(FGameTexture *gtex, int scaleflags) = nullptr;
// game thread: decode and upload this picture as soon as possible
void (*NXSpriteRequestHook)(FGameTexture *gtex, int scaleflags) = nullptr;

namespace
{
	struct TrimJob
	{
		std::atomic<bool> started{ false }, done{ false }, bumped{ false };
		uint16_t trim[4] = {};
		bool result = false;
	};
	std::mutex trimMutex;	// trimJobs, wanted
	// finished results stay: the textures live as long as the session
	std::unordered_map<FTexture *, std::shared_ptr<TrimJob>> trimJobs;
	// first frames of a level: everything synchronous, so the first view is complete
	std::atomic<int> trimWarmup{ 3 };
	// pictures the render thread found missing (picture, scale flags)
	std::vector<std::pair<FGameTexture *, int>> wanted;

	// last picture each actor showed, with its sprite (so a new actor that got the
	// address of a destroyed one cannot show the old actor's picture)
	struct Held { FGameTexture *tex; int sprite; };
	std::mutex holdMutex;
	std::unordered_map<AActor *, Held> held;

	void RunTrim(const std::shared_ptr<TrimJob> &job, FTexture *tex)
	{
		if (job->started.exchange(true)) return;
		uint16_t trim[4];
		nx_asyncworker = true;
		const bool result = tex->TrimBorders(trim);
		nx_asyncworker = false;
		std::lock_guard<std::mutex> lock(trimMutex);
		if (!job->done)
		{
			memcpy(job->trim, trim, sizeof(trim));
			job->result = result;
			job->done = true;
		}
	}

	// call with trimMutex held
	void QueueTrim(FTexture *tex, int priority)
	{
		auto &job = trimJobs[tex];
		if (job)
		{
			if (priority >= NXJOB_NOW && !job->started && !job->done && !job->bumped.exchange(true))
			{
				auto j = job;
				NXJobs_Push([j, tex]() { RunTrim(j, tex); }, NXJOB_NOW);
			}
			return;
		}
		job = std::make_shared<TrimJob>();
		auto j = job;
		NXJobs_Push([j, tex]() { RunTrim(j, tex); }, priority);
	}

	// call with trimMutex held
	void Want(FGameTexture *gtex, int scaleflags)
	{
		if (wanted.size() < 512)
			wanted.emplace_back(gtex, scaleflags);
	}

	bool TrimNeeded(FGameTexture *gtex)
	{
		// only the expanded sprite frame (index 1) needs the decoded picture
		return rd_async_sprites && gtex->GetTexture() != nullptr && !gtex->GetTexture()->isHardwareCanvas() && gtex->ShouldExpandSprite() && NXJobs_Enabled();
	}

	bool Loaded(FGameTexture *gtex, int scaleflags)
	{
		return NXSpriteLoadedHook == nullptr || NXSpriteLoadedHook(gtex, scaleflags);
	}
}

// A decoded picture (any thread): its border trim, without decoding it again.
// 'tb' is the picture as uploaded, w x h or w+2 x h+2 (CTF_Expand).
void NXSpriteTrimFromBuffer(FTexture *tex, const FTextureBuffer &tb)
{
	if (tex == nullptr || tb.mBuffer == nullptr)
		return;
	const int w = tex->GetWidth(), h = tex->GetHeight();
	int border;
	if (tb.mWidth == w && tb.mHeight == h)
		border = 0;
	else if (tb.mWidth == w + 2 && tb.mHeight == h + 2)
		border = 1;
	else
		return;	// replaced or scaled picture: TrimBorders decides
	uint16_t trim[4];
	const bool result = FTexture::TrimBuffer(tb.mBuffer + (border * tb.mWidth + border) * 4, w, h, tb.mWidth, trim);
	std::lock_guard<std::mutex> lock(trimMutex);
	auto &job = trimJobs[tex];
	if (!job)
		job = std::make_shared<TrimJob>();
	if (!job->done)
	{
		memcpy(job->trim, trim, sizeof(trim));
		job->result = result;
		job->started = true;
		job->done = true;
	}
}

// false: the sprite's positioning is still being computed (asked for)
bool NXSpriteDataReady(FGameTexture *gtex)
{
	if (gtex == nullptr || gtex->HasSpriteData() || trimWarmup > 0 || !TrimNeeded(gtex))
		return true;
	FTexture *tex = gtex->GetTexture();
	std::shared_ptr<TrimJob> job;
	{
		std::lock_guard<std::mutex> lock(trimMutex);
		auto it = trimJobs.find(tex);
		if (it != trimJobs.end() && it->second->done)
			job = it->second;
		else if (!NXSpriteDecoderActive)
			QueueTrim(tex, NXJOB_NOW);
		else
			Want(gtex, CTF_Expand);	// the decode for the upload brings the trim
	}
	if (!job)
		return false;
	gtex->SetupSpriteData(job->trim, job->result);
	return true;
}

// Render thread: the picture to draw for this actor now, or nullptr to leave it out.
FGameTexture *NXSpriteChoose(AActor *thing, FGameTexture *tex, int scaleflags, bool translated)
{
	bool ready = NXSpriteDataReady(tex);
	if (ready && !translated && trimWarmup == 0 && rd_async_sprites && !Loaded(tex, scaleflags))
	{
		ready = false;
		std::lock_guard<std::mutex> lock(trimMutex);
		Want(tex, scaleflags);
	}
	if (thing == nullptr || !rd_async_sprites)
		return ready ? tex : nullptr;
	std::lock_guard<std::mutex> lock(holdMutex);
	if (ready)
	{
		if (held.size() > 8192)
			held.clear();
		held[thing] = Held{ tex, thing->sprite };
		return tex;
	}
	auto it = held.find(thing);
	if (it != held.end() && it->second.sprite == thing->sprite && it->second.tex->HasSpriteData() && (translated || Loaded(it->second.tex, scaleflags)))
		return it->second.tex;
	nxperf.spriteskips++;
	return nullptr;
}

static void PrefetchTrim(FGameTexture *gtex, int priority)
{
	// with a background decoder the trim comes with the picture
	if (NXSpriteDecoderActive || gtex == nullptr || !gtex->isValid() || gtex->HasSpriteData() || !TrimNeeded(gtex))
		return;
	std::lock_guard<std::mutex> lock(trimMutex);
	QueueTrim(gtex->GetTexture(), priority);
}

// Game thread, once per frame: asks the backend for the pictures the render
// thread found missing.
void NXSprite_ServiceWanted()
{
	decltype(wanted) list;
	{
		std::lock_guard<std::mutex> lock(trimMutex);
		list.swap(wanted);
	}
	for (size_t i = 0; i < list.size(); i++)
	{
		FGameTexture *gtex = list[i].first;
		const int flags = list[i].second;
		bool dup = false;
		for (size_t j = 0; j < i && !dup; j++)
			dup = list[j] == list[i];
		if (dup || gtex == nullptr || !gtex->isValid())
			continue;
		if (!Loaded(gtex, flags))
		{
			if (NXSpriteRequestHook)
				NXSpriteRequestHook(gtex, flags);
		}
		else if (!gtex->HasSpriteData() && TrimNeeded(gtex))
		{
			// on the GPU already (loaded synchronously): only the trim is missing
			std::lock_guard<std::mutex> lock(trimMutex);
			auto it = trimJobs.find(gtex->GetTexture());
			if (it == trimJobs.end() || !it->second->done)
				QueueTrim(gtex->GetTexture(), NXJOB_NOW);
		}
	}
}

void NXPrefetch_NewLevel()
{
	NXPrefetched.Clear();
	NXHeldFrames.Clear();
	NXLookahead.Clear();
	NXPendingHigh.clear();
	NXPendingLow.clear();
	// nothing may still be working on textures while the level changes
	NXJobs_WaitAll();
	{
		std::lock_guard<std::mutex> lock(trimMutex);
		for (auto it = trimJobs.begin(); it != trimJobs.end();)
			it = it->second->done ? std::next(it) : trimJobs.erase(it);
		wanted.clear();
	}
	{
		std::lock_guard<std::mutex> lock(holdMutex);
		held.clear();
	}
	trimWarmup = 3;
}

// If a weapon frame is still being decoded, keep showing the previous
// frame of that layer (instead of making the gun vanish for a moment).
FGameTexture *NXHoldWeaponFrame(DPSprite *psp, FGameTexture *tex, bool &mirror)
{
	if (psp == nullptr || tex == nullptr) return tex;
	int state = NXSpriteReadyHook ? NXSpriteReadyHook(tex) : 1;
	if (!NXSpriteDataReady(tex) && state > 0) state = 0;
	if (state > 0)
	{
		if (NXHeldFrames.CountUsed() > 256) NXHeldFrames.Clear();
		NXHeldFrames[psp] = NXHeldFrame{ psp->GetCaller(), tex, mirror };
		return tex;
	}
	auto held = NXHeldFrames.CheckKey(psp);
	if (nx_perflog >= 3)
		Printf("NXHOLD %d layer %d %s -> %s\n", state, psp->GetID(), tex->GetName().GetChars(), held && held->caller == psp->GetCaller() && held->tex ? held->tex->GetName().GetChars() : "(none)");
	if (held != nullptr && held->caller == psp->GetCaller() && held->tex != nullptr)
	{
		mirror = held->mirror;
		return held->tex;
	}
	return tex;
}

// Pictures to prefetch. Preparing one (material, queueing the decode) costs
// tens of microseconds on the game thread, and a class can bring hundreds of
// them - Brutal Doom spawning its casings, smoke and gibs for the first time
// meant 50+ ms inside one tic. Only what the HUD needs right now is queued
// at once; the rest goes through these lists at most PumpMs per frame.
static const double PumpMs = 0.75;

static void NXPrefetchNow(FGameTexture *gtex, int priority)
{
	PrefetchTrim(gtex, priority);
	if (NXPrefetchSpriteHook) NXPrefetchSpriteHook(gtex, priority);
}

static void NXPrefetchTexture(FGameTexture *gtex, int priority)
{
	if (gtex == nullptr) return;
	if (priority >= 2) NXPrefetchNow(gtex, priority);
	else (priority == 1 ? NXPendingHigh : NXPendingLow).push_back(gtex);
}

static void NXPrefetch_Pump()
{
	const double start = NXPerf_NowMS();
	while (!NXPendingHigh.empty() || !NXPendingLow.empty())
	{
		auto &q = !NXPendingHigh.empty() ? NXPendingHigh : NXPendingLow;
		const int priority = &q == &NXPendingHigh ? 1 : 0;
		FGameTexture *gtex = q.front();
		q.pop_front();
		NXPrefetchNow(gtex, priority);
		if (NXPerf_NowMS() - start >= PumpMs) break;
	}
}

struct NXPrefetchList
{
	TMap<int, bool> seen;
	int count = 0;
	int max;
	int priority;
	bool allrotations;

	bool Full() const { return count >= max; }

	void AddState(const FState *st)
	{
		if (st == nullptr || st->sprite < 0 || (unsigned)st->sprite >= sprites.Size()) return;
		const spritedef_t &sd = sprites[st->sprite];
		if (st->Frame >= sd.numframes) return;
		unsigned fi = sd.spriteframes + st->Frame;
		if (fi >= SpriteFrames.Size()) return;
		const spriteframe_t &fr = SpriteFrames[fi];
		int rots = allrotations ? 16 : 1;
		for (int r = 0; r < rots && count < max; r++)
		{
			FTextureID id = fr.Texture[r];
			if (!id.isValid()) continue;
			if (seen.CheckKey(id.GetIndex())) continue;
			seen.Insert(id.GetIndex(), true);
			NXPrefetchTexture(TexMan.GetGameTexture(id), priority);
			count++;
		}
	}

	// follows an animation sequence (as far as it goes without script decisions)
	void Walk(const FState *st, int steps)
	{
		for (int i = 0; i < steps && st != nullptr && count < max; i++)
		{
			AddState(st);
			const FState *next = st->NextState;
			if (next == st) break;
			st = next;
		}
	}
};

// prefetch the pictures of every actor class when it first appears (off: only what is drawn)
CVAR(Bool, rd_sprite_prefetch, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void NXPrefetch_Class(PClassActor *cls, int maxtextures, bool urgent)
{
	if (!rd_sprite_prefetch) return;
	if ((NXPrefetchSpriteHook == nullptr && !rd_async_sprites) || cls == nullptr) return;
	if (NXPrefetched.CheckKey(cls)) return;
	NXPrefetched.Insert(cls, true);

	NXPrefetchList list;
	list.max = maxtextures;
	list.priority = urgent ? 1 : 0;
	list.allrotations = true;
	// the class itself and a few of its parents (base classes often define shared animations)
	PClassActor *c = cls;
	for (int depth = 0; depth < 4 && c != nullptr && !list.Full(); depth++)
	{
		if (c == RUNTIME_CLASS(AActor)) break;
		for (unsigned i = 0; i < c->GetStateCount() && !list.Full(); i++)
		{
			list.AddState(&c->GetStates()[i]);
		}
		auto parent = c->ParentClass;
		c = (parent && parent->IsDescendantOf(RUNTIME_CLASS(AActor))) ? static_cast<PClassActor *>(parent) : nullptr;
	}
}

// A weapon: the animations for shooting, aiming, reloading and switching,
// in the order they're most likely to be needed.
static void NXPrefetch_Weapon(PClassActor *cls)
{
	if ((NXPrefetchSpriteHook == nullptr && !rd_async_sprites) || cls == nullptr) return;
	if (NXPrefetched.CheckKey(cls)) return;
	NXPrefetched.Insert(cls, true);

	static const char *labels[] = { "Select", "Ready", "Fire", "Flash", "Hold", "AltFire", "AltFlash", "AltHold",
		"Zoom", "Reload", "Deselect", "User1", "User2", "User3", "User4" };
	NXPrefetchList list;
	list.max = 240;
	list.priority = 1;
	list.allrotations = false;	// the weapon is always seen from the front
	for (auto label : labels)
	{
		if (list.Full()) break;
		FName name(label, true);
		if (name == NAME_None) continue;
		list.Walk(cls->FindState(name), 40);
	}
}

void NXPrefetch_Player(player_t *player)
{
	// called once per rendered frame
	if (trimWarmup > 0) trimWarmup--;
	else NXPrefetch_Pump();
	NXSprite_ServiceWanted();
	if ((NXPrefetchSpriteHook == nullptr && !rd_async_sprites) || player == nullptr) return;
	if (player->ReadyWeapon) NXPrefetch_Weapon(player->ReadyWeapon->GetClass());
	if (player->PendingWeapon && player->PendingWeapon != WP_NOCHANGE)
		NXPrefetch_Weapon(static_cast<AActor *>(player->PendingWeapon)->GetClass());

	// the next frames of every animation playing on the HUD right now
	for (DPSprite *psp = player->psprites; psp != nullptr; psp = psp->GetNext())
	{
		FState *st = psp->GetState();
		if (st == nullptr) continue;
		auto last = NXLookahead.CheckKey(psp);
		if (last != nullptr && *last == st) continue;
		if (NXLookahead.CountUsed() > 256) NXLookahead.Clear();
		NXLookahead[psp] = st;
		NXPrefetchList list;
		list.max = 24;
		list.priority = 2;
		list.allrotations = false;
		list.Walk(st, 24);
	}
}
