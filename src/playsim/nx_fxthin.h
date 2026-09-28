#pragma once
// Switch port: thinning of purely visual effect actors (see nx_fxthin.cpp)
class PClass;
class DThinker;
#include "vectors.h"

bool NXFx_SkipSpawn(PClass *cls, const DVector3 &pos);	// before spawning: true = don't spawn
bool NXFx_DropNewThinker(DThinker *thinker);				// on a thinker's first tic: true = destroy it
