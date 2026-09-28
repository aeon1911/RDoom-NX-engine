// Switch port: thin out purely visual effect actors (smoke, sparks, debris...)
//
// Heavy gameplay mods spawn dozens of effect actors every tic. On the Switch's
// CPU each of them costs script time to spawn and to animate, which is where
// most of the frame time goes in big fights. This skips a share of the spawns
// of the classes listed in nx_fx_thin (and of everything derived from them),
// and all of those spawned farther away than nx_fx_far (where the renderer
// culls them anyway).
//
//   nx_fx_thin "PB_GunFireSmoke=0.5,PB_AnimatedSmokeThinker=0.3"
//     class name = share that is kept (0 = none, 1 = all)
//
// Only list classes that are pure visuals: a skipped spawn behaves exactly like
// an A_SpawnItemEx whose failchance hit, or like an actor that removes itself
// on its first tic.

#include "actor.h"
#include "d_player.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "vm.h"
#include "nx_perf.h"
#include "nx_fxthin.h"

static bool nxfxDirty = true;
static bool nxfxActive = false;

CUSTOM_CVAR(String, nx_fx_thin, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	nxfxDirty = true;
}
CUSTOM_CVAR(Float, nx_fx_far, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	nxfxDirty = true;
}

struct NXFxRule { FName name; float keep; };
static TArray<NXFxRule> nxfxRules;
static TMap<PClass *, float> nxfxCache;	// resolved share per class (1 = not listed)
static uint32_t nxfxSeed = 0x9E3779B9u;

static inline float NXFxRandom()
{
	// xorshift: must not touch the game's random numbers (demos, saves)
	nxfxSeed ^= nxfxSeed << 13;
	nxfxSeed ^= nxfxSeed >> 17;
	nxfxSeed ^= nxfxSeed << 5;
	return (nxfxSeed >> 8) * (1.0f / 16777216.0f);
}

static void NXFxParse()
{
	nxfxDirty = false;
	nxfxRules.Clear();
	nxfxCache.Clear();
	FString list = *nx_fx_thin;
	auto entries = list.Split(",", FString::TOK_SKIPEMPTY);
	for (auto &e : entries)
	{
		e.StripLeftRight();
		long eq = e.IndexOf('=');
		NXFxRule rule;
		FString name = eq >= 0 ? e.Left(eq) : e;
		name.StripLeftRight();
		if (name.IsEmpty() || !name.CompareNoCase("none")) continue;	// "none" = empty list (a launcher argument can't be empty)
		rule.name = FName(name.GetChars());
		rule.keep = eq >= 0 ? (float)atof(e.GetChars() + eq + 1) : 0.f;
		if (rule.keep < 0) rule.keep = 0;
		if (rule.keep > 1) rule.keep = 1;
		nxfxRules.Push(rule);
	}
	nxfxActive = nxfxRules.Size() > 0;
}

static float NXFxKeep(PClass *cls)
{
	if (auto p = nxfxCache.CheckKey(cls)) return *p;
	float keep = 1.f;
	for (PClass *c = cls; c != nullptr && keep >= 1.f; c = c->ParentClass)
	{
		for (auto &r : nxfxRules)
		{
			if (r.name == c->TypeName) { keep = r.keep; break; }
		}
	}
	nxfxCache[cls] = keep;
	return keep;
}

static bool NXFxFar(const DVector3 &pos)
{
	double far = nx_fx_far;
	if (far <= 0) return false;
	AActor *cam = players[consoleplayer].camera;
	if (cam == nullptr) return false;
	return (pos - cam->Pos()).LengthSquared() > far * far;
}

bool NXFx_SkipSpawn(PClass *cls, const DVector3 &pos)
{
	if (nxfxDirty) NXFxParse();
	if (!nxfxActive || cls == nullptr) return false;
	float keep = NXFxKeep(cls);
	if (keep >= 1.f) return false;
	if (NXFxFar(pos) || keep <= 0.f || NXFxRandom() >= keep)
	{
		nxperf.fxskipped++;
		return true;
	}
	return false;
}

bool NXFx_DropNewThinker(DThinker *thinker)
{
	if (nxfxDirty) NXFxParse();
	if (!nxfxActive) return false;
	PClass *cls = thinker->GetClass();
	float keep = NXFxKeep(cls);
	if (keep >= 1.f) return false;
	bool drop;
	if (cls->IsDescendantOf(RUNTIME_CLASS(AActor)))
	{
		auto actor = static_cast<AActor *>(thinker);
		// never touch anything that takes part in the game (pickups are Inventory;
		// some effects like PB's casings are MF_SPECIAL only to react when touched)
		if (actor->player || actor->tid != 0 || (actor->flags & (MF_SHOOTABLE | MF_SOLID)) || (actor->flags3 & MF3_ISMONSTER) || actor->IsKindOf(NAME_Inventory))
			return false;
		drop = NXFxFar(actor->Pos()) || keep <= 0.f || NXFxRandom() >= keep;
	}
	else
	{
		drop = keep <= 0.f || NXFxRandom() >= keep;
	}
	if (drop) nxperf.fxskipped++;
	return drop;
}

// ZScript side (A_SpawnItem / A_SpawnItemEx): true = behave like a failed spawn
DEFINE_ACTION_FUNCTION(AActor, NXFxSkip)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_CLASS(cls, AActor);
	ACTION_RETURN_BOOL(NXFx_SkipSpawn(cls, self->Pos()));
}

CCMD(nx_fx_status)
{
	if (nxfxDirty) NXFxParse();
	Printf("nx_fx_thin: %d rules, far %.0f\n", nxfxRules.Size(), (double)nx_fx_far);
	for (auto &r : nxfxRules) Printf("  %s keep %.2f\n", r.name.GetChars(), r.keep);
}
