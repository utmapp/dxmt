#ifndef _WIN32

#include "d3d11_texture_shared_native.hpp"
#include "d3d11_resource.hpp"
#include "dxmt_format.hpp"

#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <directx-native/dxn_shm.h>

namespace dxmt {

HRESULT
AllocateShmLinearTexture2D(
    MTLD3D11Device *pDevice, const D3D11_TEXTURE2D_DESC1 &finalDesc, const WMTTextureInfo &info, int import_fd,
    uint64_t import_stride, uint64_t import_size, uint64_t import_offset, Rc<Texture> &out_texture,
    Rc<TextureAllocation> &out_allocation, shm_texture_pod &out_pod
) {
  /* Metal linear (buffer-backed) textures: 2D, single subresource, no
   * compressed/depth formats, no MSAA.  Real shared-texture traffic is
   * BGRA8-family render targets, which all fit. */
  if (finalDesc.MipLevels != 1 || finalDesc.ArraySize != 1 || finalDesc.SampleDesc.Count > 1 ||
      (finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
    ERR("shm texture: unsupported layout (mips=", finalDesc.MipLevels, " array=", finalDesc.ArraySize,
        " samples=", finalDesc.SampleDesc.Count, ")");
    return E_FAIL;
  }

  MTL_DXGI_FORMAT_DESC format_desc;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), finalDesc.Format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid || format_desc.BytesPerTexel == 0 ||
      (format_desc.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    ERR("shm texture: format ", finalDesc.Format, " cannot back a linear texture");
    return E_FAIL;
  }

  uint64_t stride, logical_size;
  if (import_fd >= 0) {
    /* Consumer: the producer's layout is the contract; never recompute.
     * Every descriptor field is untrusted: validate without overflowing
     * (stride * Height can wrap in 64 bits) and within the unsigned range
     * the Texture constructor truncates to. */
    stride = import_stride;
    logical_size = import_size;
    if (!stride || !finalDesc.Height || stride > UINT32_MAX || logical_size > UINT32_MAX ||
        stride < (uint64_t)finalDesc.Width * format_desc.BytesPerTexel ||
        logical_size / stride < finalDesc.Height) {
      ERR("shm texture: import descriptor stride/size inconsistent");
      return E_INVALIDARG;
    }
  } else {
    uint32_t bytes_per_row, bytes_per_image, bytes_per_slice;
    if (FAILED(GetLinearTextureLayout(pDevice, finalDesc, 0, bytes_per_row, bytes_per_image, bytes_per_slice, true))) {
      ERR("shm texture: no linear layout for format ", finalDesc.Format);
      return E_FAIL;
    }
    stride = bytes_per_row;
    logical_size = bytes_per_image;
  }

  /* The surface may start part-way into the object (a texture placed in a
   * shared heap); mmap needs a page-aligned file offset, so map from the page
   * floor and place the texture at the delta inside the buffer. */
  const uint64_t offset = import_fd >= 0 ? import_offset : 0;
  const uint64_t map_floor = offset & ~(dxn_shm_page_align(1) - 1);
  const uint64_t delta = offset - map_floor;
  if (delta > UINT32_MAX || logical_size > UINT64_MAX - delta) {
    ERR("shm texture: import offset out of range");
    return E_INVALIDARG;
  }
  const uint64_t mapped_size = dxn_shm_page_align(delta + logical_size);

  int fd = import_fd >= 0 ? import_fd : dxn_shm_create(mapped_size);
  if (fd < 0) {
    ERR("shm texture: shm_open failed: ", errno);
    return E_FAIL;
  }

  if (import_fd >= 0) {
    /* Accessing pages past the shm object's actual size raises SIGBUS, so
     * the mapping must be backed in full. */
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < map_floor + mapped_size) {
      ERR("shm texture: shm object smaller than descriptor size");
      return E_INVALIDARG;
    }
  }

  void *mapped = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map_floor);
  if (mapped == MAP_FAILED) {
    ERR("shm texture: mmap failed: ", errno);
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  /* Buffer over the mapping; its deallocator munmaps, so the memory lives
   * exactly as long as the last Metal reference (in-flight command buffers
   * included). */
  WMTBufferInfo buffer_info;
  buffer_info.length = mapped_size;
  buffer_info.options = WMTResourceHazardTrackingModeUntracked; /* StorageModeShared */
  buffer_info.memory.set(mapped);
  buffer_info.gpu_address = 0;
  auto buffer = pDevice->GetMTLDevice().newBufferMapped(buffer_info);
  if (!buffer) {
    ERR("shm texture: newBufferWithBytesNoCopy failed");
    munmap(mapped, mapped_size);
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  WMTTextureInfo linear_info = info;
  linear_info.mach_port = 0;
  linear_info.options = WMTResourceHazardTrackingModeUntracked;

  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::Shared);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
    flags.set(TextureAllocationFlag::ShaderReadonly);

  Rc<Texture> texture = new Texture((unsigned)logical_size, (unsigned)stride, linear_info, pDevice->GetMTLDevice());
  Rc<TextureAllocation> allocation =
      texture->allocateFromBuffer(std::move(buffer), (uint8_t *)mapped + delta, (unsigned)delta, flags);
  if (!allocation->texture()) {
    ERR("shm texture: linear texture creation failed (stride=", stride, ")");
    /* buffer reference died with `allocation`; its deallocator munmaps. */
    if (import_fd < 0)
      close(fd);
    return E_FAIL;
  }

  /* The POD owns a fresh fd in both directions so an opened texture can be
   * re-exported and outlive the descriptor it was opened from. */
  int pod_fd = import_fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : fd;
  if (pod_fd < 0) {
    ERR("shm texture: dup failed: ", errno);
    return E_FAIL;
  }

  out_pod = {};
  DXN_SHARED_INIT(&out_pod, DXN_SHARED_TEXTURE_MAGIC);
  out_pod.fd = pod_fd;
  out_pod.meta.width = finalDesc.Width;
  out_pod.meta.height = finalDesc.Height;
  out_pod.meta.mip_levels = finalDesc.MipLevels;
  out_pod.meta.array_size = finalDesc.ArraySize;
  out_pod.meta.dxgi_format = finalDesc.Format;
  out_pod.meta.sample_count = finalDesc.SampleDesc.Count;
  out_pod.meta.sample_quality = finalDesc.SampleDesc.Quality;
  out_pod.meta.usage = finalDesc.Usage;
  out_pod.meta.bind_flags = finalDesc.BindFlags;
  out_pod.meta.cpu_access_flags = finalDesc.CPUAccessFlags;
  out_pod.meta.misc_flags = finalDesc.MiscFlags;
  out_pod.meta.texture_layout = finalDesc.TextureLayout;
  out_pod.plane_count = 1;
  out_pod.planes[0].offset = offset;
  out_pod.planes[0].pitch = stride;
  out_pod.size = logical_size;

  out_texture = std::move(texture);
  out_allocation = std::move(allocation);
  return S_OK;
}

} // namespace dxmt

#endif /* !_WIN32 */
