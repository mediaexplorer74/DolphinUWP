#pragma once

#include <memory>
#include "VideoBackends/Software/SWTexture.h"
#include "VideoBackends/Software/TextureEncoder.h"
#include "VideoCommon/TextureCacheBase.h"

namespace SW
{
class TextureCache : public TextureCacheBase
{
public:
  bool CompileShaders() override { return true; }
  void DeleteShaders() override {}
  void ConvertTexture(TCacheEntry* entry, TCacheEntry* unconverted, const void* palette,
                      TLUTFormat format) override
  {
  }
  void CopyEFB(u8* dst, const EFBCopyParams& params, u32 native_width, u32 bytes_per_row,
               u32 num_blocks_y, u32 memory_stride, const EFBRectangle& src_rect,
               bool scale_by_half) override
  {
    TextureEncoder::Encode(dst, params, native_width, bytes_per_row, num_blocks_y, memory_stride,
                           src_rect, scale_by_half);
  }

private:
  void CopyEFBToCacheEntry(TCacheEntry* entry, bool is_depth_copy, const EFBRectangle& src_rect,
                           bool scale_by_half, unsigned int cbuf_id, const float* colmat,
                           EFBCopyFormat dst_format, bool is_intensity) override
  {
    // TODO: If we ever want to "fake" vram textures, we would need to implement this
  }
};

}  // namespace SW
