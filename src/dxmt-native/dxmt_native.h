/*
 * dxmt-native embedder API.
 *
 *  - dxmt_event_*: directx-native's Win32-style events (dxn_event.h),
 *    kqueue-backed, whose dup_fd() view is a pipe fd that survives
 *    SCM_RIGHTS.  create / close / dup_fd are the embedder API
 *    (dxn_embedder.h); wait is for native apps that block on one.
 *  - dxn_shared_texture (dxn_shared.h): the descriptor that
 *    IDXGIResource::GetSharedHandle returns a pointer to and
 *    ID3D11Device::OpenSharedResource accepts (see
 *    src/d3d11/d3d11_texture_shared_native.cpp).
 *  - dxmt_native_abi_version: the DXN_ABI_VERSION this image was built
 *    with (dxn_embedder.h).
 */

#ifndef DXMT_NATIVE_H
#define DXMT_NATIVE_H

#include <stdint.h>

#include <directx-native/dxn_embedder.h>
#include <directx-native/dxn_event.h>
#include <directx-native/dxn_shared.h>

#ifdef __cplusplus
extern "C" {
#endif

/* == Win32-style events ==================================================== */

/* Create a Win32-style event. Returns an opaque handle to pass wherever the
 * D3D API takes an event HANDLE (ID3D11Fence::SetEventOnCompletion). */
void* dxmt_event_create(int manual_reset, int initial_state);
void  dxmt_event_close(void* handle);
/* A NEW poll(2)-able fd mirroring the event's signaled state; survives
 * SCM_RIGHTS to another process. Caller closes it. */
int   dxmt_event_dup_fd(void* handle);
/* WaitForSingleObject on one of these events. */
dxn_wait_status dxmt_event_wait(void* handle, uint64_t timeout_ns);

uint32_t dxmt_native_abi_version(void);

/* == Embedder-shared DYNAMIC buffer storage ================================ */

/* Make the embedder's shared memory fd (its guest's mapping) the storage of
 * one rename allocation of a DYNAMIC ID3D11Buffer, so a guest Map writes
 * the bytes the GPU reads with no host copy.  backing_length must cover the
 * buffer and be page aligned.  The fd is dup'ed; the caller keeps its own.
 * The allocation is tagged with cookie, which the embedder's Map path reads
 * back through dxmt_d3d11_buffer_external_cookie, and queues for a later
 * Map(WRITE_DISCARD) rename.  Once a buffer has one, renames stop reusing
 * its ordinary device-memory allocations; a Map that finds no shared
 * allocation free still succeeds on new device memory, with no cookie. */
int32_t dxmt_d3d11_buffer_bind_external_fd(void *d3d11_buffer, int fd, uint64_t backing_length,
                                           uint32_t cookie);
/* 0 and the cookie if mapped_ptr (from ID3D11DeviceContext::Map) is a shared
 * allocation of that buffer, -1 if it is ordinary device memory. */
int dxmt_d3d11_buffer_external_cookie(void *d3d11_buffer, const void *mapped_ptr, uint32_t *out_cookie);

#ifdef __cplusplus
}
#endif

#endif /* DXMT_NATIVE_H */
