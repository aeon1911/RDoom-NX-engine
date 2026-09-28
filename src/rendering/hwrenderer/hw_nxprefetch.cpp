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
EXTERN_CVAR(Int, nx_perflog)

// priority: 0 = prefetch, 1 = likely needed soon, 2 = needed now
void (*NXPrefetchSpriteHook)(FGameTexture *gtex, int priority) = nullptr;
int (*NXSpriteReadyHook)(FGameTexture *gtex) = nullptr;
// false = a world sprite's image isn't on the GPU yet (still decoding or waiting
// for the upload budget): drawing it would only show a transparent placeholder
bool (*NXSpriteDrawableHook)(FGameTexture *gtex, int translation, int scaleflags) = nullptr;
static TMap<PClassActor *, bool> NXPrefetched;

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

void NXPrefetch_NewLevel()
{
	NXPrefetched.Clear();
	NXHeldFrames.Clear();
	NXLookahead.Clear();
}

// If a weapon frame is still being decoded, keep showing the previous
// frame of that layer (instead of making the gun vanish for a moment).
FGameTexture *NXHoldWeaponFrame(DPSprite *psp, FGameTexture *tex, bool &mirror)
{
	if (NXSpriteReadyHook == nullptr || psp == nullptr || tex == nullptr) return tex;
	int state = NXSpriteReadyHook(tex);
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
			NXPrefetchSpriteHook(TexMan.GetGameTexture(id), priority);
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
	if (NXPrefetchSpriteHook == nullptr || cls == nullptr) return;
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
	if (NXPrefetchSpriteHook == nullptr || cls == nullptr) return;
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
	if (NXPrefetchSpriteHook == nullptr || player == nullptr) return;
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
