/*
** nx_objpool.cpp
** Allocator for script objects (actors, thinkers) on the Switch.
**
** newlib's malloc took ~47 us per actor (~1.6 KB) in a horde of Brutal Doom
** imps, 2.3-2.5 ms of every tic for the ~50 hitbox actors they spawn per tic
** (perf.log "object creation", 2026-10-13). Objects come in a few hundred
** sizes and are created and destroyed all the time: free lists per 16-byte
** size class, carved from 128 KB slabs that one size class owns, make both
** a few dozen nanoseconds. Freed blocks are kept for reuse (the peak number
** of live objects bounds the memory). A table of slab addresses tells this
** allocator's blocks from M_Malloc's.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "nx_objpool.h"
#include "m_alloc.h"
#include "dobjgc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#ifdef __SWITCH__

namespace
{
	constexpr size_t kSlab = 128 * 1024;
	constexpr size_t kStep = 16;
	constexpr size_t kMax = 8192;				// larger objects: M_Malloc
	constexpr size_t kClasses = kMax / kStep;
	constexpr size_t kHeader = 64;				// slab header, keeps blocks 64-byte aligned

	struct FreeBlock { FreeBlock *next; };
	struct Slab { uint32_t magic; uint32_t cls; };
	constexpr uint32_t kMagic = 0x4f42584e;		// "NXBO"

	std::mutex lock;
	FreeBlock *freeList[kClasses];
	uint8_t *bump[kClasses], *bumpEnd[kClasses];

	// open-addressing set of slab base addresses (power-of-two capacity)
	uintptr_t *slabs = nullptr;
	size_t slabCap = 0, slabCount = 0;

	size_t Hash(uintptr_t base, size_t cap) { return (size_t)((base / kSlab) * 0x9E3779B97F4A7C15ull) & (cap - 1); }

	bool IsSlab(uintptr_t base)
	{
		if (!slabCap) return false;
		for (size_t i = Hash(base, slabCap);; i = (i + 1) & (slabCap - 1))
		{
			if (slabs[i] == base) return true;
			if (slabs[i] == 0) return false;
		}
	}

	bool AddSlab(uintptr_t base)
	{
		if ((slabCount + 1) * 2 > slabCap)
		{
			size_t cap = slabCap ? slabCap * 2 : 1024;
			auto table = (uintptr_t *)calloc(cap, sizeof(uintptr_t));
			if (!table) return false;
			for (size_t i = 0; i < slabCap; i++)
				if (slabs[i])
					for (size_t j = Hash(slabs[i], cap);; j = (j + 1) & (cap - 1))
						if (!table[j]) { table[j] = slabs[i]; break; }
			free(slabs);
			slabs = table;
			slabCap = cap;
		}
		for (size_t i = Hash(base, slabCap);; i = (i + 1) & (slabCap - 1))
			if (!slabs[i]) { slabs[i] = base; slabCount++; return true; }
	}
}

void *NXObj_Alloc(size_t size)
{
	if (size == 0 || size > kMax)
		return M_Malloc(size);
	const size_t cls = (size - 1) / kStep;
	const size_t bsize = (cls + 1) * kStep;
	void *mem = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (freeList[cls])
		{
			mem = freeList[cls];
			freeList[cls] = freeList[cls]->next;
		}
		else
		{
			if (bump[cls] == nullptr || bump[cls] + bsize > bumpEnd[cls])
			{
				auto slab = (uint8_t *)aligned_alloc(kSlab, kSlab);
				if (slab && AddSlab((uintptr_t)slab))
				{
					auto h = (Slab *)slab;
					h->magic = kMagic;
					h->cls = (uint32_t)cls;
					bump[cls] = slab + kHeader;
					bumpEnd[cls] = slab + kSlab;
				}
				else
				{
					free(slab);
					bump[cls] = bumpEnd[cls] = nullptr;
				}
			}
			if (bump[cls])
			{
				mem = bump[cls];
				bump[cls] += bsize;
			}
		}
	}
	if (!mem)
		return M_Malloc(size);	// out of slabs: the normal heap
	GC::ReportAlloc(bsize);
	return mem;
}

void NXObj_Free(void *mem)
{
	if (!mem)
		return;
	const uintptr_t base = (uintptr_t)mem & ~(uintptr_t)(kSlab - 1);
	size_t bsize = 0;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (IsSlab(base))
		{
			auto h = (Slab *)base;
			const size_t cls = h->cls;
			bsize = (cls + 1) * kStep;
			auto b = (FreeBlock *)mem;
			b->next = freeList[cls];
			freeList[cls] = b;
		}
	}
	if (bsize)
		GC::ReportDealloc(bsize);
	else
		M_Free(mem);
}

#else

void *NXObj_Alloc(size_t size) { return M_Malloc(size); }
void NXObj_Free(void *mem) { M_Free(mem); }

#endif

#ifdef __SWITCH__
//==========================================================================
//
// Heap diagnostics (perf.log "heap per tic"): malloc/free/realloc of the
// game thread, through the linker's --wrap. The game thread marks itself.
//
//==========================================================================

#include "nx_perf.h"

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void *__real_calloc(size_t, size_t);

#include <switch.h>
// thread handle of the game thread (a thread_local here hung the start-up:
// its first access may allocate, inside malloc)
static Handle nxGameHandle = INVALID_HANDLE;
void NXObj_MarkGameThread() { nxGameHandle = threadGetCurHandle(); }
#define nxGameThread (nxGameHandle != INVALID_HANDLE && threadGetCurHandle() == nxGameHandle)

extern "C" void *__wrap_malloc(size_t size)
{
	if (!nxGameThread) return __real_malloc(size);
	const double t = NXPerf_NowMS();
	void *p = __real_malloc(size);
	nxperf.heap_malloc_ms += NXPerf_NowMS() - t;
	nxperf.heap_mallocs++;
	return p;
}

extern "C" void __wrap_free(void *p)
{
	if (!nxGameThread || !p) { __real_free(p); return; }
	const double t = NXPerf_NowMS();
	__real_free(p);
	nxperf.heap_free_ms += NXPerf_NowMS() - t;
	nxperf.heap_frees++;
}

extern "C" void *__wrap_realloc(void *p, size_t size)
{
	if (!nxGameThread) return __real_realloc(p, size);
	const double t = NXPerf_NowMS();
	void *r = __real_realloc(p, size);
	nxperf.heap_malloc_ms += NXPerf_NowMS() - t;
	nxperf.heap_mallocs++;
	return r;
}

extern "C" void *__wrap_calloc(size_t n, size_t size)
{
	if (!nxGameThread) return __real_calloc(n, size);
	const double t = NXPerf_NowMS();
	void *r = __real_calloc(n, size);
	nxperf.heap_malloc_ms += NXPerf_NowMS() - t;
	nxperf.heap_mallocs++;
	return r;
}
#else
void NXObj_MarkGameThread() {}
#endif
