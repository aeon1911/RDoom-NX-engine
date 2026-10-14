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
** The same pool serves every malloc of 1 KB or less (linker --wrap of
** malloc/free/realloc/calloc/malloc_usable_size): on the console the game
** thread made ~1500 of them per frame in a slaughter map, 2.8 ms of newlib.
** Larger blocks, aligned allocations and what newlib allocates internally
** stay with newlib; its frees see a pointer outside the slabs and pass it on.
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

#include <switch.h>
#include "nx_perf.h"

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void *__real_calloc(size_t, size_t);
extern "C" size_t __real_malloc_usable_size(void *);
extern "C" void *memalign(size_t, size_t);	// not wrapped: newlib's own heap

namespace
{
	constexpr size_t kSlab = 128 * 1024;
	constexpr size_t kStep = 16;
	constexpr size_t kMax = 8192;				// objects up to 8 KB
	constexpr size_t kMallocMax = 1024;			// plain mallocs up to 1 KB
	constexpr size_t kClasses = kMax / kStep;
	constexpr size_t kHeader = 64;				// slab header, keeps blocks 64-byte aligned

	struct FreeBlock { FreeBlock *next; };
	struct Slab { uint32_t magic; uint32_t cls; };
	constexpr uint32_t kMagic = 0x4f42584e;		// "NXBO"

	// The pool must never call the wrapped malloc/free (it would re-enter
	// under its own lock): __real_* and memalign only.
	Mutex lock;	// libnx mutex: no allocation, usable before main
	FreeBlock *freeList[kClasses];
	uint8_t *bump[kClasses], *bumpEnd[kClasses];

	uintptr_t *slabs = nullptr;	// open-addressing set of slab bases (power-of-two capacity)
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
			auto table = (uintptr_t *)__real_calloc(cap, sizeof(uintptr_t));
			if (!table) return false;
			for (size_t i = 0; i < slabCap; i++)
				if (slabs[i])
					for (size_t j = Hash(slabs[i], cap);; j = (j + 1) & (cap - 1))
						if (!table[j]) { table[j] = slabs[i]; break; }
			__real_free(slabs);
			slabs = table;
			slabCap = cap;
		}
		for (size_t i = Hash(base, slabCap);; i = (i + 1) & (slabCap - 1))
			if (!slabs[i]) { slabs[i] = base; slabCount++; return true; }
	}

	// a block of the size class for 'size' (1..kMax), nullptr when out of memory
	void *PoolAlloc(size_t size)
	{
		const size_t cls = (size - 1) / kStep;
		const size_t bsize = (cls + 1) * kStep;
		void *mem = nullptr;
		mutexLock(&lock);
		if (freeList[cls])
		{
			mem = freeList[cls];
			freeList[cls] = freeList[cls]->next;
		}
		else
		{
			if (bump[cls] == nullptr || bump[cls] + bsize > bumpEnd[cls])
			{
				auto slab = (uint8_t *)memalign(kSlab, kSlab);
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
					__real_free(slab);
					bump[cls] = bumpEnd[cls] = nullptr;
				}
			}
			if (bump[cls])
			{
				mem = bump[cls];
				bump[cls] += bsize;
			}
		}
		mutexUnlock(&lock);
		return mem;
	}

	// block size of a pool block, 0 for other pointers
	size_t PoolSize(void *mem)
	{
		const uintptr_t base = (uintptr_t)mem & ~(uintptr_t)(kSlab - 1);
		mutexLock(&lock);
		const size_t bsize = IsSlab(base) ? (((Slab *)base)->cls + 1) * kStep : 0;
		mutexUnlock(&lock);
		return bsize;
	}

	// returns the block size, 0 when the pointer is not the pool's
	size_t PoolFree(void *mem)
	{
		const uintptr_t base = (uintptr_t)mem & ~(uintptr_t)(kSlab - 1);
		size_t bsize = 0;
		mutexLock(&lock);
		if (IsSlab(base))
		{
			const size_t cls = ((Slab *)base)->cls;
			bsize = (cls + 1) * kStep;
			auto b = (FreeBlock *)mem;
			b->next = freeList[cls];
			freeList[cls] = b;
		}
		mutexUnlock(&lock);
		return bsize;
	}

	// game thread (perf.log heap counters)
	Handle gameHandle = INVALID_HANDLE;
	inline bool GameThread() { return gameHandle != INVALID_HANDLE && threadGetCurHandle() == gameHandle; }
}

void NXObj_MarkGameThread() { gameHandle = threadGetCurHandle(); }

void *NXObj_Alloc(size_t size)
{
	void *mem = (size == 0 || size > kMax) ? nullptr : PoolAlloc(size);
	if (!mem)
		return M_Malloc(size);
	GC::ReportAlloc(((size - 1) / kStep + 1) * kStep);
	return mem;
}

void NXObj_Free(void *mem)
{
	if (!mem)
		return;
	if (const size_t bsize = PoolFree(mem))
		GC::ReportDealloc(bsize);
	else
		M_Free(mem);
}

static void *Malloc(size_t size)
{
	if (size > 0 && size <= kMallocMax)
		if (void *p = PoolAlloc(size))
			return p;
	return __real_malloc(size);
}

static void Free(void *p)
{
	if (p && !PoolFree(p))
		__real_free(p);
}

static void *Realloc(void *p, size_t size)
{
	if (!p)
		return Malloc(size);
	const size_t old = PoolSize(p);
	if (!old)
		return __real_realloc(p, size);	// newlib's block stays newlib's
	if (size == 0)
	{
		PoolFree(p);
		return nullptr;
	}
	if (size <= old && size > old / 2)
		return p;
	void *q = Malloc(size);
	if (q)
	{
		memcpy(q, p, size < old ? size : old);
		PoolFree(p);
	}
	return q;
}

extern "C" void *__wrap_malloc(size_t size)
{
	if (!GameThread()) return Malloc(size);
	const double t = NXPerf_NowMS();
	void *p = Malloc(size);
	nxperf.heap_malloc_ms += NXPerf_NowMS() - t;
	nxperf.heap_mallocs++;
	return p;
}

extern "C" void __wrap_free(void *p)
{
	if (!p) return;
	if (!GameThread()) { Free(p); return; }
	const double t = NXPerf_NowMS();
	Free(p);
	nxperf.heap_free_ms += NXPerf_NowMS() - t;
	nxperf.heap_frees++;
}

extern "C" void *__wrap_realloc(void *p, size_t size)
{
	if (!GameThread()) return Realloc(p, size);
	const double t = NXPerf_NowMS();
	void *r = Realloc(p, size);
	nxperf.heap_malloc_ms += NXPerf_NowMS() - t;
	nxperf.heap_mallocs++;
	return r;
}

extern "C" void *__wrap_calloc(size_t n, size_t size)
{
	const size_t total = n * size;
	if (size && total / size != n)
		return nullptr;
	if (total > kMallocMax)
		return __real_calloc(n, size);
	void *p = __wrap_malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}

extern "C" size_t __wrap_malloc_usable_size(void *p)
{
	if (!p) return 0;
	const size_t bsize = PoolSize(p);
	return bsize ? bsize : __real_malloc_usable_size(p);
}

#else

void *NXObj_Alloc(size_t size) { return M_Malloc(size); }
void NXObj_Free(void *mem) { M_Free(mem); }
void NXObj_MarkGameThread() {}

#endif
