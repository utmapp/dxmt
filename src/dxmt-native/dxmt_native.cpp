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

namespace dxmt {

Logger Logger::s_instance("dxmt.log");

} // namespace dxmt

extern "C" int32_t
dxmt_d3d11_buffer_bind_external_fd(void *d3d11_buffer, int fd, uint64_t backing_length, uint32_t cookie) {
  if (!d3d11_buffer)
    return (int32_t)E_INVALIDARG;
  return (int32_t)dxmt::GetResourceCommon(static_cast<ID3D11Resource *>(d3d11_buffer))
      ->bindDynamicBufferExternalFd(fd, backing_length, cookie);
}

extern "C" int
dxmt_d3d11_buffer_external_cookie(void *d3d11_buffer, void *mapped_ptr, uint32_t *out_cookie) {
  if (!d3d11_buffer)
    return -1;
  return dxmt::GetResourceCommon(static_cast<ID3D11Resource *>(d3d11_buffer))
                 ->dynamicBufferExternalCookie(mapped_ptr, out_cookie)
             ? 0
             : -1;
}
