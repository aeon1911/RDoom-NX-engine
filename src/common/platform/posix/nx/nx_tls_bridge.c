/*
** nx_tls_bridge.c
** Makes hardware-thread-pointer TLS work under libnx.
**
** libnx code is built with -mtp=soft: a thread-local access calls
** __aarch64_read_tp(), which returns ThreadVars->tls_tp (found through
** TPIDRRO_EL0). libnx never writes TPIDR_EL0, so it reads 0 on every thread.
**
** The Mesa 26.2.4 Horizon SDK links Rust code (NAK, the NVK shader compiler,
** and its std) that was built for aarch64-unknown-linux-gnu. Rust has no soft
** thread pointer, so that code reads TPIDR_EL0 directly. With TPIDR_EL0 = 0
** the first Rust thread-local access faults.
**
** Both kinds of code use the local-exec TLS model against the same static
** TLS block, so they agree on every offset once TPIDR_EL0 holds the value
** __aarch64_read_tp() returns. TPIDR_EL0 is part of the per-thread context
** the Horizon kernel saves (see libnx ThreadContext::tpidr), so setting it
** once at the start of each thread is enough:
**   - the main thread: NX_TlsBridgeInit(), called from userAppInit();
**   - every other thread: threadCreate() is wrapped (-Wl,--wrap=threadCreate)
**     so the new thread sets the register before running its entry point.
**     pthread_create() ends up in threadCreate() too.
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#ifdef __SWITCH__

#include <stdlib.h>
#include <switch.h>
#include "nx_tls_bridge.h"

extern void *__aarch64_read_tp(void);

void NX_TlsBridgeInit(void)
{
	void *tp = __aarch64_read_tp();
	__asm__ volatile("msr tpidr_el0, %0" : : "r"(tp) : "memory");
}

int NX_TlsBridgeCheck(void)
{
	void *hw;
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(hw));
	return hw == __aarch64_read_tp();
}

typedef struct
{
	ThreadFunc entry;
	void *arg;
} NXTlsTrampoline;

static void NXTlsThreadEntry(void *p)
{
	NXTlsTrampoline t = *(NXTlsTrampoline *)p;
	free(p);
	NX_TlsBridgeInit();
	t.entry(t.arg);
}

Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid);

Result __wrap_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid)
{
	NXTlsTrampoline *tr = (NXTlsTrampoline *)malloc(sizeof(*tr));
	if (tr == NULL)
		return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
	tr->entry = entry;
	tr->arg = arg;
	Result rc = __real_threadCreate(t, NXTlsThreadEntry, tr, stack_mem, stack_sz, prio, cpuid);
	if (R_FAILED(rc))
		free(tr);
	return rc;
}

#endif
