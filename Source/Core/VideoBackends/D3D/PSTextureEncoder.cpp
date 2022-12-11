// Copyright 2011 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/PSTextureEncoder.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DShader.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/FramebufferManager.h"
#include "VideoBackends/D3D/Render.h"
#include "VideoBackends/D3D/TextureCache.h"
#include "VideoBackends/D3D/VertexShaderCache.h"

#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/VideoCommon.h"

namespace DX11
{
struct EFBEncodeParams
{
  s32 SrcLeft;
  s32 SrcTop;
  u32 DestWidth;
  u32 ScaleFactor;
  float y_scale;
  u32 padding[3];
};

PSTextureEncoder::PSTextureEncoder()
{
}

PSTextureEncoder::~PSTextureEncoder() = default;

void PSTextureEncoder::Init()
{
  // TODO: Move this to a constant somewhere in common.
  TextureConfig encoding_texture_config(EFB_WIDTH * 4, 1024, 1, 1, AbstractTextureFormat::BGRA8,
                                        true);
  m_encoding_render_texture = g_renderer->CreateTexture(encoding_texture_config);
  m_encoding_readback_texture =
      g_renderer->CreateStagingTexture(StagingTextureType::Readback, encoding_texture_config);
  _assert_(m_encoding_render_texture && m_encoding_readback_texture);

  // Create constant buffer for uploading data to shaders
  D3D11_BUFFER_DESC bd = CD3D11_BUFFER_DESC(sizeof(EFBEncodeParams), D3D11_BIND_CONSTANT_BUFFER);
  HRESULT hr = D3D::device->CreateBuffer(&bd, nullptr, &m_encode_params);
  CHECK(SUCCEEDED(hr), "create efb encode params buffer");
  D3D::SetDebugObjectName(m_encode_params, "efb encoder params buffer");
}

void PSTextureEncoder::Shutdown()
{
  for (auto& it : m_encoding_shaders)
    SAFE_RELEASE(it.second);
  m_encoding_shaders.clear();

  SAFE_RELEASE(m_encode_params);
}

void PSTextureEncoder::Encode(u8* dst, const EFBCopyParams& params, u32 native_width,
                              u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
                              const EFBRectangle& src_rect, bool scale_by_half)
{
  // Resolve MSAA targets before copying.
  // FIXME: Instead of resolving EFB, it would be better to pick out a
  // single sample from each pixel. The game may break if it isn't
  // expecting the blurred edges around multisampled shapes.
  ID3D11ShaderResourceView* pEFB = params.depth ?
                                       FramebufferManager::GetResolvedEFBDepthTexture()->GetSRV() :
                                       FramebufferManager::GetResolvedEFBColorTexture()->GetSRV();

  // Reset API
  g_renderer->ResetAPIState();

  // Set up all the state for EFB encoding
  {
    const u32 words_per_row = bytes_per_row / sizeof(u32);

    D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, FLOAT(words_per_row), FLOAT(num_blocks_y));
    D3D::context->RSSetViewports(1, &vp);

    constexpr EFBRectangle fullSrcRect(0, 0, EFB_WIDTH, EFB_HEIGHT);
    TargetRectangle targetRect = g_renderer->ConvertEFBRectangle(fullSrcRect);

    D3D::context->OMSetRenderTargets(
        1,
        &static_cast<DXTexture*>(m_encoding_render_texture.get())->GetRawTexIdentifier()->GetRTV(),
        nullptr);

    EFBEncodeParams encode_params;
    encode_params.SrcLeft = src_rect.left;
    encode_params.SrcTop = src_rect.top;
    encode_params.DestWidth = native_width;
    encode_params.ScaleFactor = scale_by_half ? 2 : 1;
    encode_params.y_scale = params.y_scale;
    D3D::context->UpdateSubresource(m_encode_params, 0, nullptr, &encode_params, 0, 0);
    D3D::stateman->SetPixelConstants(m_encode_params);

    // We also linear filtering for both box filtering and downsampling higher resolutions to 1x
    // TODO: This only produces perfect downsampling for 2x IR, other resolutions will need more
    //       complex down filtering to average all pixels and produce the correct result.
    // Also, box filtering won't be correct for anything other than 1x IR
    if (scale_by_half || g_renderer->GetEFBScale() != 1 || params.y_scale > 1.0f)
      D3D::SetLinearCopySampler();
    else
      D3D::SetPointCopySampler();

    D3D::drawShadedTexQuad(pEFB, targetRect.AsRECT(), g_renderer->GetTargetWidth(),
                           g_renderer->GetTargetHeight(), GetEncodingPixelShader(params),
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout());

    // Copy to staging buffer
    MathUtil::Rectangle<int> copy_rect(0, 0, words_per_row, num_blocks_y);
    m_encoding_readback_texture->CopyFromTexture(m_encoding_render_texture.get(), copy_rect, 0, 0,
                                                 copy_rect);
    m_encoding_readback_texture->Flush();
    if (m_encoding_readback_texture->Map())
    {
      m_encoding_readback_texture->ReadTexels(copy_rect, dst, memory_stride);
      m_encoding_readback_texture->Unmap();
    }
  }

  // Restore API
  FramebufferManager::BindEFBRenderTarget();
  g_renderer->RestoreAPIState();
}

ID3D11PixelShader* PSTextureEncoder::GetEncodingPixelShader(const EFBCopyParams& params)
{
  auto iter = m_encoding_shaders.find(params);
  if (iter != m_encoding_shaders.end())
    return iter->second;

  D3DBlob* bytecode = nullptr;
  const char* shader = TextureConversionShaderTiled::GenerateEncodingShader(params, APIType::D3D);
  if (!D3D::CompilePixelShader(shader, &bytecode))
  {
    PanicAlert("Failed to compile texture encoding shader.");
    m_encoding_shaders[params] = nullptr;
    return nullptr;
  }

  ID3D11PixelShader* newShader;
  HRESULT hr =
      D3D::device->CreatePixelShader(bytecode->Data(), bytecode->Size(), nullptr, &newShader);
  CHECK(SUCCEEDED(hr), "create efb encoder pixel shader");

  m_encoding_shaders.emplace(params, newShader);
  return newShader;
}
}
