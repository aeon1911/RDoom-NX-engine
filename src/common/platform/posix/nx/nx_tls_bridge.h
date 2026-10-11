#pragma once
// See nx_tls_bridge.c. Link with -Wl,--wrap=threadCreate.

#ifdef __cplusplus
extern "C" {
#endif

// Copies the libnx soft thread pointer into TPIDR_EL0 for the calling thread.
void NX_TlsBridgeInit(void);
// Returns 1 if TPIDR_EL0 matches the libnx thread pointer on this thread.
int NX_TlsBridgeCheck(void);

#ifdef __cplusplus
}
#endif
