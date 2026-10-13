#pragma once
// Switch port: actors that are purely visual, which the frame rate stabilizer
// may leave out (hw_bsp.cpp, hw_sprites.cpp). Players, monsters, pickups,
// shootable things and projectiles that hurt are never trimmed. Brutal Doom
// spawns its sparks, flames and debris as zero-damage projectiles: those are
// effects (an explosion had ~300 translucent sprites while the area budget,
// which skipped every projectile, could only leave out 17 of them).

#include "actor.h"

inline bool NXIsPureEffect(AActor *thing)
{
	if (thing->player || (thing->flags3 & MF3_ISMONSTER) || (thing->flags & (MF_SPECIAL | MF_SHOOTABLE)))
		return false;
	if ((thing->flags & MF_MISSILE) && !thing->IsZeroDamage())
		return false;
	return true;
}
