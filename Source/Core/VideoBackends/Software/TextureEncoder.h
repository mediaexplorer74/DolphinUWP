// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/VideoCommon.h"

struct EFBCopyParams;

namespace TextureEncoder
{
void Encode(u8* dst, const EFBCopyParams& params, u32 native_width, u32 bytes_per_row,
            u32 num_blocks_y, u32 memory_stride, const EFBRectangle& src_rect, bool scale_by_half);
}
