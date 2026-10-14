/*
** nx_objpool.h
** Allocator for script objects (actors, thinkers) on the Switch.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include <stddef.h>

// PClass::CreateNew and the GC sweep use these instead of M_Malloc/M_Free.
// Anything this allocator did not hand out goes to M_Free.
void *NXObj_Alloc(size_t size);
void NXObj_Free(void *mem);

// perf.log heap counters count the calling thread's malloc/free (the game thread)
void NXObj_MarkGameThread();
