/*
 * dxmt-native embedder API.
 *
 *  - dxmt_event_*: kqueue-backed Win32-style events whose dup_fd() view is
 *    a pipe fd that survives SCM_RIGHTS (see
 *    src/winemetal/unix/dxmt_native_event.c).
 *  - dxmt_shared_texture_handle: the POD that
 *    IDXGIResource::GetSharedHandle returns a pointer to and
 *    ID3D11Device::OpenSharedResource accepts (see
 *    src/d3d11/d3d11_texture_shared_native.cpp).
 */

#ifndef DXMT_NATIVE_H
#define DXMT_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* == Win32-style events ==================================================== */

#define DXMT_WAIT_INFINITE UINT64_MAX

typedef enum dxmt_wait_status {
    DXMT_WAIT_SIGNALED = 0,
    DXMT_WAIT_TIMEOUT  = 1,
    DXMT_WAIT_FAILED   = -1, /* not an event handle */
} dxmt_wait_status;

/* Create a Win32-style event. Returns an opaque handle to pass wherever the
 * D3D API takes an event HANDLE (ID3D11Fence::SetEventOnCompletion). */
void* dxmt_event_create(int manual_reset, int initial_state);
void  dxmt_event_signal(void* handle);
void  dxmt_event_clear(void* handle);
void  dxmt_event_close(void* handle);
/* A NEW poll(2)-able fd mirroring the event's signaled state; survives
 * SCM_RIGHTS to another process. Caller closes it. */
int   dxmt_event_dup_fd(void* handle);
dxmt_wait_status dxmt_event_wait(void* handle, uint64_t timeout_ns);

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
int dxmt_d3d11_buffer_external_cookie(void *d3d11_buffer, void *mapped_ptr, uint32_t *out_cookie);

/* == Cross-process shared textures ========================================= */

#define DXMT_SHARED_TEXTURE_MAGIC 0x58544D44u /* 'DMTX' */
#define DXMT_SHARED_HANDLE_VERSION 2u

typedef struct dxmt_shared_texture_handle {
    uint32_t magic;         /* DXMT_SHARED_TEXTURE_MAGIC */
    uint32_t version;       /* DXMT_SHARED_HANDLE_VERSION */
    int32_t  fd;            /* process-local; send via SCM_RIGHTS, then patch */
    uint32_t width, height;
    uint32_t dxgi_format;   /* DXGI_FORMAT */
    uint32_t mip_levels, array_size, sample_count;
    uint32_t bind_flags, misc_flags, cpu_access;
    uint64_t stride;        /* bytesPerRow, byte-exact for the consumer */
    uint64_t size;          /* logical stride*height (NOT page-padded) */
    uint64_t offset;        /* byte offset of the surface within `fd`; 0 for
                               a surface that owns its whole object, nonzero
                               for a surface placed in a shared heap (one
                               object backs the heap). Not necessarily
                               page-aligned. */
} dxmt_shared_texture_handle;

#ifdef __cplusplus
}
#endif

#endif /* DXMT_NATIVE_H */
