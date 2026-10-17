#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define MT2FW_EXPORT __declspec(dllexport)
#define MT2FW_CALL __cdecl
#else
#define MT2FW_EXPORT __attribute__((visibility("default")))
#define MT2FW_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define MT2FW_ABI_VERSION 1u
typedef void (MT2FW_CALL *MT2FW_EventCallback)(void* user, const char* json, size_t length);
typedef struct MT2FW_Host {
    uint32_t size;
    uint32_t abi_version;
    void* context;
    /* UTF-8 JSON. The returned required size includes the terminator. A null output only measures;
       it never executes a mutation. Call once with a sufficiently sized buffer. */
    size_t (MT2FW_CALL *request)(void* context, const char* operation, const char* arguments,
                               char* output, size_t capacity);
    uint64_t (MT2FW_CALL *subscribe)(void* context, const char* event, MT2FW_EventCallback callback, void* user);
    void (MT2FW_CALL *unsubscribe)(void* context, uint64_t subscription);
} MT2FW_Host;

typedef struct MT2FW_PluginInfo {
    uint32_t size;
    uint32_t abi_version;
    /* Borrowed UTF-8 strings; keep them valid until PluginShutdown returns. */
    const char* id;
    const char* version;
} MT2FW_PluginInfo;

typedef int (MT2FW_CALL *MT2FW_QueryFn)(MT2FW_PluginInfo* info);
typedef int (MT2FW_CALL *MT2FW_LoadFn)(const MT2FW_Host* host);
typedef void (MT2FW_CALL *MT2FW_ShutdownFn)(void);
/* Export MT2FW_PluginQuery, MT2FW_PluginLoad, MT2FW_PluginShutdown with these signatures.
   Strings passed to the host are copied during the call. Callback JSON is borrowed until return.
   The host remains valid until Shutdown returns. No exception may cross this boundary. */
#ifdef __cplusplus
}
#endif
