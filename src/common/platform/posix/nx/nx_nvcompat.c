/*
** nx_nvcompat.c
** Emulator compatibility for fixed GPU virtual address reservations.
**
** Mesa's Horizon backend (NVK and Gallium) manages its own GPU VA heap and
** reserves each range with NVGPU_AS_IOCTL_ALLOC_SPACE + FIXED_OFFSET
** (libnx nvAddressSpaceAllocFixed), then maps buffers there with
** MAP_BUFFER_EX + FIXED_OFFSET.
**
** Ryujinx 1.3.x answers a fixed ALLOC_SPACE with the start of the free block
** that contains the requested range instead of the requested offset, and
** records its reservation at that wrong address. The range itself is marked
** used, so the following fixed MAP_BUFFER_EX finds no reservation and fails
** (NvInternalResult.InvalidInput, 0x195c), and vkCreateDevice returns
** VK_ERROR_OUT_OF_DEVICE_MEMORY. A second wrong reservation at an address
** that is already a key throws inside the emulator's nvdrv service.
**
** Real hardware returns the requested offset, so this file does nothing
** there. NX_NvCompatInit() checks the behaviour on a scratch address space
** before the graphics driver starts. Only if the answer is wrong:
**
**   for each address space, on its first fixed reservation:
**     1. reserve the last big page below the big-page region (the reply is
**        wrong, but afterwards the big-page region starts a free block);
**     2. reserve the whole big-page region at its real start (now correct);
**   then fixed reservations/frees inside that region are answered here
**   without an ioctl. Fixed MAP_BUFFER_EX calls still go to the emulator and
**   validate against the single region-wide reservation.
**
** Wrapped with -Wl,--wrap=nvioctlNvhostAsGpu_AllocSpace,
** --wrap=nvioctlNvhostAsGpu_FreeSpace, --wrap=nvioctlNvhostAsGpu_InitializeEx.
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#ifdef __SWITCH__

#include <switch.h>
#include <string.h>
#include <stdio.h>
#include "nx_nvcompat.h"

#define NVCOMPAT_MAX_AS 8
#define NVCOMPAT_BIG_PAGE 0x10000ULL
#define NVCOMPAT_FIXED 1u

typedef struct
{
	u32 fd;
	bool used;
	bool tried;
	bool ok;
	u64 start, end;
} NvCompatAs;

static bool s_active;
static bool s_checked;
static Mutex s_mutex;
static NvCompatAs s_as[NVCOMPAT_MAX_AS];
static char s_status[160] = "not checked";
static unsigned s_emulated;

Result __real_nvioctlNvhostAsGpu_AllocSpace(u32 fd, u32 pages, u32 page_size, u32 flags, u64 align_or_offset, u64 *offset);
Result __real_nvioctlNvhostAsGpu_FreeSpace(u32 fd, u64 offset, u32 pages, u32 page_size);
Result __real_nvioctlNvhostAsGpu_InitializeEx(u32 fd, u32 flags, u32 big_page_size);

static bool NvCompatBigRegion(u32 fd, u64 *start, u64 *end)
{
	nvioctl_va_region regions[2];
	memset(regions, 0, sizeof(regions));
	if (R_FAILED(nvioctlNvhostAsGpu_GetVARegions(fd, regions)))
		return false;
	for (int i = 0; i < 2; i++)
	{
		if (regions[i].page_size == NVCOMPAT_BIG_PAGE && regions[i].pages != 0)
		{
			*start = regions[i].offset;
			*end = regions[i].offset + regions[i].pages * regions[i].page_size;
			return true;
		}
	}
	return false;
}

void NX_NvCompatInit(void)
{
	if (s_checked)
		return;
	s_checked = true;
	mutexInit(&s_mutex);

	Result rc = nvInitialize();
	if (R_FAILED(rc))
	{
		snprintf(s_status, sizeof(s_status), "check skipped: nvInitialize 0x%x", rc);
		return;
	}
	NvAddressSpace as;
	rc = nvAddressSpaceCreate(&as, (u32)NVCOMPAT_BIG_PAGE);
	if (R_FAILED(rc))
	{
		snprintf(s_status, sizeof(s_status), "check skipped: nvAddressSpaceCreate 0x%x", rc);
		nvExit();
		return;
	}
	u64 start = 0, end = 0;
	if (!NvCompatBigRegion(as.fd, &start, &end) || end - start < 0x400000ULL)
	{
		snprintf(s_status, sizeof(s_status), "check skipped: no big-page VA region");
	}
	else
	{
		// one page well inside the region, so it is not the start of a free block
		const u64 want = start + 0x200000ULL;
		u64 got = 0;
		rc = __real_nvioctlNvhostAsGpu_AllocSpace(as.fd, 1, (u32)NVCOMPAT_BIG_PAGE, NVCOMPAT_FIXED, want, &got);
		if (R_SUCCEEDED(rc) && got != want)
		{
			s_active = true;
			snprintf(s_status, sizeof(s_status),
				"ACTIVE: fixed ALLOC_SPACE at 0x%llx answered 0x%llx (emulator quirk)",
				(unsigned long long)want, (unsigned long long)got);
		}
		else if (R_SUCCEEDED(rc))
		{
			snprintf(s_status, sizeof(s_status), "inactive: fixed ALLOC_SPACE behaves like hardware");
			__real_nvioctlNvhostAsGpu_FreeSpace(as.fd, want, 1, (u32)NVCOMPAT_BIG_PAGE);
		}
		else
		{
			snprintf(s_status, sizeof(s_status), "inactive: probe ALLOC_SPACE failed 0x%x", rc);
		}
	}
	// closing the scratch address space drops whatever the probe reserved
	nvAddressSpaceClose(&as);
	nvExit();
}

bool NX_NvCompatActive(void)
{
	return s_active;
}

const char *NX_NvCompatStatus(void)
{
	return s_status;
}

unsigned NX_NvCompatEmulatedCalls(void)
{
	return s_emulated;
}

static NvCompatAs *NvCompatFind(u32 fd, bool create)
{
	for (int i = 0; i < NVCOMPAT_MAX_AS; i++)
		if (s_as[i].used && s_as[i].fd == fd)
			return &s_as[i];
	if (!create)
		return NULL;
	for (int i = 0; i < NVCOMPAT_MAX_AS; i++)
	{
		if (!s_as[i].used)
		{
			memset(&s_as[i], 0, sizeof(s_as[i]));
			s_as[i].used = true;
			s_as[i].fd = fd;
			return &s_as[i];
		}
	}
	return NULL;
}

static void NvCompatPrepare(NvCompatAs *st)
{
	st->tried = true;
	u64 start, end;
	if (!NvCompatBigRegion(st->fd, &start, &end) || start < 2 * NVCOMPAT_BIG_PAGE)
		return;
	u64 got = 0;
	Result rc = __real_nvioctlNvhostAsGpu_AllocSpace(st->fd, 1, (u32)NVCOMPAT_BIG_PAGE, NVCOMPAT_FIXED,
		start - NVCOMPAT_BIG_PAGE, &got);
	if (R_FAILED(rc))
		return;
	const u64 pages = (end - start) / NVCOMPAT_BIG_PAGE;
	rc = __real_nvioctlNvhostAsGpu_AllocSpace(st->fd, (u32)pages, (u32)NVCOMPAT_BIG_PAGE, NVCOMPAT_FIXED, start, &got);
	if (R_SUCCEEDED(rc) && got == start)
	{
		st->ok = true;
		st->start = start;
		st->end = end;
	}
}

Result __wrap_nvioctlNvhostAsGpu_InitializeEx(u32 fd, u32 flags, u32 big_page_size)
{
	Result rc = __real_nvioctlNvhostAsGpu_InitializeEx(fd, flags, big_page_size);
	if (s_active)
	{
		// a new address space on this fd: forget any earlier state
		mutexLock(&s_mutex);
		NvCompatAs *st = NvCompatFind(fd, false);
		if (st)
			st->used = false;
		mutexUnlock(&s_mutex);
	}
	return rc;
}

Result __wrap_nvioctlNvhostAsGpu_AllocSpace(u32 fd, u32 pages, u32 page_size, u32 flags, u64 align_or_offset, u64 *offset)
{
	if (!s_active || !(flags & NVCOMPAT_FIXED))
		return __real_nvioctlNvhostAsGpu_AllocSpace(fd, pages, page_size, flags, align_or_offset, offset);

	mutexLock(&s_mutex);
	NvCompatAs *st = NvCompatFind(fd, true);
	if (st && !st->tried)
		NvCompatPrepare(st);
	const u64 size = (u64)pages * page_size;
	const bool inside = st && st->ok && align_or_offset >= st->start && size <= st->end - align_or_offset;
	if (inside)
		s_emulated++;
	mutexUnlock(&s_mutex);

	if (inside)
	{
		if (offset)
			*offset = align_or_offset;
		return 0;
	}
	// A passthrough fixed reservation would hit the same emulator quirk and
	// may crash its nvdrv service; refuse it instead.
	return MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_BadParameter);
}

Result __wrap_nvioctlNvhostAsGpu_FreeSpace(u32 fd, u64 offset, u32 pages, u32 page_size)
{
	if (s_active)
	{
		mutexLock(&s_mutex);
		NvCompatAs *st = NvCompatFind(fd, false);
		const u64 size = (u64)pages * page_size;
		const bool inside = st && st->ok && offset >= st->start && size <= st->end - offset;
		if (inside)
			s_emulated++;
		mutexUnlock(&s_mutex);
		if (inside)
			return 0;
	}
	return __real_nvioctlNvhostAsGpu_FreeSpace(fd, offset, pages, page_size);
}

#endif
