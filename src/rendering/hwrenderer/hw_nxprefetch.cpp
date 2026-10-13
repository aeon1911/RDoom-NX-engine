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
// Sprite border trimming in the background
//
// The first time a sprite is drawn, the renderer decodes its whole picture
// just to find the empty border around it (FTexture::TrimBorders), on the
// game thread, and then decodes it a second time for the upload. An
// explosion in Brutal Doom brings dozens of new pictures at once: on the
// Switch that was 100-150 ms of PNG decoding inside one frame. The trim is
// now computed by a worker (nx_jobs) and the sprite is left out until its
// numbers are ready, usually one or two frames later.
//
//==========================================================================

#ifdef __SWITCH__
CVAR(Bool, rd_async_sprites, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Bool, rd_async_sprites, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif

extern thread_local bool nx_asyncworker;

namespace
{
	struct TrimJob
	{
		std::atomic<bool> started{ false }, done{ false }, bumped{ false };
		uint16_t trim[4] = {};
		bool result = false;
	};
	std::mutex trimMutex;
	std::unordered_map<FGameTexture *, std::shared_ptr<TrimJob>> trimJobs;
	// first frames of a level: everything synchronous, so the first view is complete
	int trimWarmup = 3;

	void RunTrim(const std::shared_ptr<TrimJob> &job, FTexture *tex)
	{
		if (job->started.exchange(true)) return;
		nx_asyncworker = true;
		job->result = tex->TrimBorders(job->trim);
		nx_asyncworker = false;
		job->done = true;
	}

	// call with trimMutex held
	void QueueTrim(FGameTexture *gtex, int priority)
	{
		auto it = trimJobs.find(gtex);
		if (it != trimJobs.end())
		{
			auto job = it->second;
			if (priority >= NXJOB_NOW && !job->started && !job->bumped.exchange(true))
			{
				FTexture *tex = gtex->GetTexture();
				NXJobs_Push([job, tex]() { RunTrim(job, tex); }, NXJOB_NOW);
			}
			return;
		}
		auto job = std::make_shared<TrimJob>();
		trimJobs.emplace(gtex, job);
		FTexture *tex = gtex->GetTexture();
		NXJobs_Push([job, tex]() { RunTrim(job, tex); }, priority);
	}

	bool TrimAsync(FGameTexture *gtex)
	{
		// only the expanded sprite frame (index 1) needs the decoded picture
		return rd_async_sprites && gtex->GetTexture() != nullptr && !gtex->GetTexture()->isHardwareCanvas() && gtex->ShouldExpandSprite() && NXJobs_Enabled();
	}
}

// false: the sprite's positioning is still being computed, leave it out of this frame
bool NXSpriteDataReady(FGameTexture *gtex)
{
	if (gtex == nullptr || gtex->HasSpriteData() || trimWarmup > 0 || !TrimAsync(gtex))
		return true;
	std::shared_ptr<TrimJob> job;
	{
		std::lock_guard<std::mutex> lock(trimMutex);
		auto it = trimJobs.find(gtex);
		if (it == trimJobs.end() || !it->second->done)
		{
			QueueTrim(gtex, NXJOB_NOW);
			nxperf.spriteskips++;
			return false;
		}
		job = it->second;
		trimJobs.erase(it);
	}
	gtex->SetupSpriteData(job->trim, job->result);
	return true;
}

static void PrefetchTrim(FGameTexture *gtex, int priority)
{
	if (gtex == nullptr || !gtex->isValid() || gtex->HasSpriteData() || !TrimAsync(gtex))
		return;
	std::lock_guard<std::mutex> lock(trimMutex);
	QueueTrim(gtex, priority);
}

void NXPrefetch_NewLevel()
{
	NXPrefetched.Clear();
	NXHeldFrames.Clear();
	NXLookahead.Clear();
	// textures may go away with the old level: nothing may still be trimming
	NXPendingHigh.clear();
	NXPendingLow.clear();
	NXJobs_WaitAll();
	std::lock_guard<std::mutex> lock(trimMutex);
	trimJobs.clear();
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

void NXPrefetch_Class(PClassActor *cls, int maxtextures, bool urgent)
{
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
