#pragma once
// See nx_nvcompat.c. Link with
//   -Wl,--wrap=nvioctlNvhostAsGpu_AllocSpace -Wl,--wrap=nvioctlNvhostAsGpu_FreeSpace
//   -Wl,--wrap=nvioctlNvhostAsGpu_InitializeEx

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Detects the emulator's fixed ALLOC_SPACE behaviour. Call once, before the
// first Vulkan/EGL call.
void NX_NvCompatInit(void);
bool NX_NvCompatActive(void);
// Human-readable result of the check, for logs.
const char *NX_NvCompatStatus(void);
// Number of fixed reservations/frees answered locally so far.
unsigned NX_NvCompatEmulatedCalls(void);

#ifdef __cplusplus
}
#endif
