// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Logging/Log.h"

#include "VideoBackends/Null/NullTexture.h"
#include "VideoBackends/Null/Render.h"

#include "VideoCommon/VideoConfig.h"

namespace Null
{
// Init functions
Renderer::Renderer() : ::Renderer(1, 1)
{
  UpdateActiveConfig();
}

Renderer::~Renderer()
{
  UpdateActiveConfig();
}

std::unique_ptr<AbstractTexture> Renderer::CreateTexture(const TextureConfig& config)
{
  return std::make_unique<NullTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> Renderer::CreateStagingTexture(StagingTextureType type,
                                                                       const TextureConfig& config)
{
  return std::make_unique<NullStagingTexture>(type, config);
}

void Renderer::RenderText(const std::string& text, int left, int top, u32 color)
{
  NOTICE_LOG(VIDEO, "RenderText: %s", text.c_str());
}

TargetRectangle Renderer::ConvertEFBRectangle(const EFBRectangle& rc)
{
  TargetRectangle result;
  result.left = rc.left;
  result.top = rc.top;
  result.right = rc.right;
  result.bottom = rc.bottom;
  return result;
}

void Renderer::SwapImpl(AbstractTexture*, const EFBRectangle&, u64, float)
{
  UpdateActiveConfig();
}

}  // namespace Null
