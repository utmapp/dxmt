/*
 * libdxmt-native — the single native image.
 *
 * The D3D11/DXGI/D3D10 modules and the Metal backend are all linked into
 * this one dylib, so its exported entry points are theirs; nothing is
 * forwarded at runtime.
 *
 * Each module normally defines its own Logger instance, one log file per
 * DLL.  Those definitions are compiled out here (DXMT_NATIVE), leaving this
 * as the single definition for the whole image.
 */

#include "log/log.hpp"
#include "dxmt_native.h"
#include "../d3d11/d3d11_resource.hpp"

#include <directx-native/dxn_log.h>

namespace dxmt {

Logger Logger::s_instance("dxmt.log");

namespace {

/* directx-native's diagnostics (events, shm) land in the same log. */
void
dxn_log_forward(dxn_log_level level, const char *message, void *) {
  switch (level) {
  case DXN_LOG_ERROR: Logger::err(message); break;
  case DXN_LOG_WARN: Logger::warn(message); break;
  case DXN_LOG_INFO: Logger::info(message); break;
  default: Logger::debug(message); break;
  }
}

[[maybe_unused]] const bool dxn_log_installed = (dxn_set_log_callback(dxn_log_forward, nullptr), true);

} // namespace

} // namespace dxmt

/* The embedder API's events are directx-native's, under the dxmt_ prefix. */
extern "C" void *
dxmt_event_create(int manual_reset, int initial_state) {
  return dxn_event_create(manual_reset, initial_state);
}

extern "C" void
dxmt_event_close(void *handle) {
  dxn_event_close(handle);
}

extern "C" int
dxmt_event_dup_fd(void *handle) {
  return dxn_event_dup_fd(handle);
}

extern "C" dxn_wait_status
dxmt_event_wait(void *handle, uint64_t timeout_ns) {
  return dxn_event_wait(handle, timeout_ns);
}

extern "C" DXN_DEFINE_NATIVE_ABI_VERSION(dxmt_)

extern "C" int32_t
dxmt_d3d11_buffer_bind_external_fd(void *d3d11_buffer, int fd, uint64_t backing_length, uint32_t cookie) {
  if (!d3d11_buffer)
    return (int32_t)E_INVALIDARG;
  return (int32_t)dxmt::GetResourceCommon(static_cast<ID3D11Resource *>(d3d11_buffer))
      ->bindDynamicBufferExternalFd(fd, backing_length, cookie);
}

extern "C" int
dxmt_d3d11_buffer_external_cookie(void *d3d11_buffer, const void *mapped_ptr, uint32_t *out_cookie) {
  if (!d3d11_buffer)
    return -1;
  return dxmt::GetResourceCommon(static_cast<ID3D11Resource *>(d3d11_buffer))
                 ->dynamicBufferExternalCookie(const_cast<void *>(mapped_ptr), out_cookie)
             ? 0
             : -1;
}
