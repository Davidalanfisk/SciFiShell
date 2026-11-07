#pragma once

#include <stdint.h>

// Native modules are trusted code. This interface is not a security boundary.
#define SF_NATIVE_PLUGIN_ABI 1u

#ifdef __cplusplus
extern "C" {
#endif

// The descriptor and its strings must remain valid until the library is unloaded.
// Callbacks must not throw across the ABI. Failed create/start must undo their
// own work. The host must discard interfaces and stop calling them before stop.
// Host callbacks and their context are borrowed for a synchronous call only.
typedef struct SfNativePluginV1 {
    uint32_t size;
    uint32_t abi;
    const char* id;
    void* (*create)(void);
    int (*start)(void* context);
    void (*stop)(void* context);
    const void* (*get_interface)(void* context, const char* id);
    void (*destroy)(void* context);
} SfNativePluginV1;

// Querying the descriptor does not create an instance. Loading the library itself
// and calling this function still execute trusted, unsandboxed native code.
typedef const SfNativePluginV1* (*SfNativePluginQueryV1)(void);

#ifdef __cplusplus
}
#endif
