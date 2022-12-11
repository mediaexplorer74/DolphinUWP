// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/FramebufferManager.h"

#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/HW/Memmap.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/Render.h"
#include "VideoBackends/D3D/VertexShaderCache.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
static bool s_integer_efb_render_target = false;

FramebufferManager::Efb FramebufferManager::m_efb;
unsigned int FramebufferManager::m_target_width;
unsigned int FramebufferManager::m_target_height;

D3DTexture2D*& FramebufferManager::GetEFBColorTexture()
{
  return m_efb.color_tex;
}

D3DTexture2D*& FramebufferManager::GetEFBColorReadTexture()
{
  return m_efb.color_read_texture;
}
ID3D11Texture2D*& FramebufferManager::GetEFBColorStagingBuffer()
{
  return m_efb.color_staging_buf;
}

D3DTexture2D*& FramebufferManager::GetEFBDepthTexture()
{
  return m_efb.depth_tex;
}
D3DTexture2D*& FramebufferManager::GetEFBDepthReadTexture()
{
  return m_efb.depth_read_texture;
}
ID3D11Texture2D*& FramebufferManager::GetEFBDepthStagingBuffer()
{
  return m_efb.depth_staging_buf;
}

D3DTexture2D*& FramebufferManager::GetResolvedEFBColorTexture()
{
  if (g_ActiveConfig.iMultisamples > 1)
  {
    for (int i = 0; i < m_efb.slices; i++)
      D3D::context->ResolveSubresource(m_efb.resolved_color_tex->GetTex(),
                                       D3D11CalcSubresource(0, i, 1), m_efb.color_tex->GetTex(),
                                       D3D11CalcSubresource(0, i, 1), DXGI_FORMAT_R8G8B8A8_UNORM);
    return m_efb.resolved_color_tex;
  }
  else
  {
    return m_efb.color_tex;
  }
}

D3DTexture2D*& FramebufferManager::GetResolvedEFBDepthTexture()
{
  if (g_ActiveConfig.iMultisamples > 1)
  {
    // ResolveSubresource does not work with depth textures.
    // Instead, we use a shader that selects the minimum depth from all samples.
    g_renderer->ResetAPIState();

    CD3D11_VIEWPORT viewport(0.f, 0.f, (float)m_target_width, (float)m_target_height);
    D3D::context->RSSetViewports(1, &viewport);
    D3D::context->OMSetRenderTargets(1, &m_efb.resolved_depth_tex->GetRTV(), nullptr);

    const D3D11_RECT source_rect = CD3D11_RECT(0, 0, m_target_width, m_target_height);
    D3D::drawShadedTexQuad(
        m_efb.depth_tex->GetSRV(), &source_rect, m_target_width, m_target_height,
        PixelShaderCache::GetDepthResolveProgram(), VertexShaderCache::GetSimpleVertexShader(),
        VertexShaderCache::GetSimpleInputLayout(), GeometryShaderCache::GetCopyGeometryShader());

    BindEFBRenderTarget();
    g_renderer->RestoreAPIState();

    return m_efb.resolved_depth_tex;
  }
  else
  {
    return m_efb.depth_tex;
  }
}

void FramebufferManager::SwapReinterpretTexture()
{
  std::swap(m_efb.color_tex, m_efb.color_temp_tex);
  std::swap(m_efb.color_int_rtv, m_efb.color_temp_int_rtv);
}

void FramebufferManager::SetIntegerEFBRenderTarget(bool enabled)
{
  if (s_integer_efb_render_target == enabled)
    return;

  // We only use UINT render targets for logic ops, which is only supported with D3D11.1.
  if (!D3D::device1)
    return;

  s_integer_efb_render_target = enabled;
  BindEFBRenderTarget();
}

void FramebufferManager::BindEFBRenderTarget(bool bind_depth)
{
  ID3D11RenderTargetView* rtv =
      s_integer_efb_render_target ? m_efb.color_int_rtv : m_efb.color_tex->GetRTV();
  ID3D11DepthStencilView* dsv = bind_depth ? m_efb.depth_tex->GetDSV() : nullptr;
  D3D::context->OMSetRenderTargets(1, &rtv, dsv);
}

FramebufferManager::FramebufferManager(int target_width, int target_height)
{
  m_target_width = static_cast<unsigned int>(std::max(target_width, 1));
  m_target_height = static_cast<unsigned int>(std::max(target_height, 1));
  DXGI_SAMPLE_DESC sample_desc;
  sample_desc.Count = g_ActiveConfig.iMultisamples;
  sample_desc.Quality = 0;

  ID3D11Texture2D* buf;
  D3D11_TEXTURE2D_DESC texdesc;
  HRESULT hr;

  m_EFBLayers = m_efb.slices = (g_ActiveConfig.stereo_mode != StereoMode::Off) ? 2 : 1;

  // EFB color texture - primary render target
  texdesc =
      CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_TYPELESS, m_target_width, m_target_height,
                            m_efb.slices, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                            D3D11_USAGE_DEFAULT, 0, sample_desc.Count, sample_desc.Quality);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
  CHECK(hr == S_OK, "create EFB color texture (size: %dx%d; hr=%#x)", m_target_width,
        m_target_height, hr);
  m_efb.color_tex = new D3DTexture2D(
      buf, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM,
      (sample_desc.Count > 1));

  SAFE_RELEASE(buf);
  D3D::SetDebugObjectName(m_efb.color_tex->GetTex(), "EFB color texture");
  D3D::SetDebugObjectName(m_efb.color_tex->GetSRV(), "EFB color texture shader resource view");
  D3D::SetDebugObjectName(m_efb.color_tex->GetRTV(), "EFB color texture render target view");

  // Temporary EFB color texture - used in ReinterpretPixelData
  texdesc =
      CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_TYPELESS, m_target_width, m_target_height,
                            m_efb.slices, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                            D3D11_USAGE_DEFAULT, 0, sample_desc.Count, sample_desc.Quality);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
  CHECK(hr == S_OK, "create EFB color temp texture (size: %dx%d; hr=%#x)", m_target_width,
        m_target_height, hr);
  m_efb.color_temp_tex = new D3DTexture2D(
      buf, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM,
      (sample_desc.Count > 1));
  SAFE_RELEASE(buf);
  D3D::SetDebugObjectName(m_efb.color_temp_tex->GetTex(), "EFB color temp texture");
  D3D::SetDebugObjectName(m_efb.color_temp_tex->GetSRV(),
                          "EFB color temp texture shader resource view");
  D3D::SetDebugObjectName(m_efb.color_temp_tex->GetRTV(),
                          "EFB color temp texture render target view");

  // Integer render targets for EFB, used for logic op
  CD3D11_RENDER_TARGET_VIEW_DESC int_rtv_desc(m_efb.color_tex->GetTex(),
                                              g_ActiveConfig.iMultisamples > 1 ?
                                                  D3D11_RTV_DIMENSION_TEXTURE2DMS :
                                                  D3D11_RTV_DIMENSION_TEXTURE2D,
                                              DXGI_FORMAT_R8G8B8A8_UINT);
  hr = D3D::device->CreateRenderTargetView(m_efb.color_tex->GetTex(), &int_rtv_desc,
                                           &m_efb.color_int_rtv);
  CHECK(hr == S_OK, "create EFB integer RTV(hr=%#x)", hr);
  hr = D3D::device->CreateRenderTargetView(m_efb.color_temp_tex->GetTex(), &int_rtv_desc,
                                           &m_efb.color_temp_int_rtv);
  CHECK(hr == S_OK, "create EFB integer RTV(hr=%#x)", hr);

  // Render buffer for AccessEFB (color data)
  texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, D3D11_BIND_RENDER_TARGET);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
  CHECK(hr == S_OK, "create EFB color read texture (hr=%#x)", hr);
  m_efb.color_read_texture = new D3DTexture2D(buf, D3D11_BIND_RENDER_TARGET);
  SAFE_RELEASE(buf);
  D3D::SetDebugObjectName(m_efb.color_read_texture->GetTex(),
                          "EFB color read texture (used in Renderer::AccessEFB)");
  D3D::SetDebugObjectName(
      m_efb.color_read_texture->GetRTV(),
      "EFB color read texture render target view (used in Renderer::AccessEFB)");

  // AccessEFB - Sysmem buffer used to retrieve the pixel data from depth_read_texture
  texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0, D3D11_USAGE_STAGING,
                                  D3D11_CPU_ACCESS_READ);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &m_efb.color_staging_buf);
  CHECK(hr == S_OK, "create EFB color staging buffer (hr=%#x)", hr);
  D3D::SetDebugObjectName(m_efb.color_staging_buf,
                          "EFB color staging texture (used for Renderer::AccessEFB)");

  // EFB depth buffer - primary depth buffer
  texdesc =
      CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32_TYPELESS, m_target_width, m_target_height, m_efb.slices,
                            1, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE,
                            D3D11_USAGE_DEFAULT, 0, sample_desc.Count, sample_desc.Quality);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
  CHECK(hr == S_OK, "create EFB depth texture (size: %dx%d; hr=%#x)", m_target_width,
        m_target_height, hr);
  m_efb.depth_tex = new D3DTexture2D(
      buf, (D3D11_BIND_FLAG)(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE),
      DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_UNKNOWN, (sample_desc.Count > 1));
  SAFE_RELEASE(buf);
  D3D::SetDebugObjectName(m_efb.depth_tex->GetTex(), "EFB depth texture");
  D3D::SetDebugObjectName(m_efb.depth_tex->GetDSV(), "EFB depth texture depth stencil view");
  D3D::SetDebugObjectName(m_efb.depth_tex->GetSRV(), "EFB depth texture shader resource view");

  // Render buffer for AccessEFB (depth data)
  texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 1, D3D11_BIND_RENDER_TARGET);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
  CHECK(hr == S_OK, "create EFB depth read texture (hr=%#x)", hr);
  m_efb.depth_read_texture = new D3DTexture2D(buf, D3D11_BIND_RENDER_TARGET);
  SAFE_RELEASE(buf);
  D3D::SetDebugObjectName(m_efb.depth_read_texture->GetTex(),
                          "EFB depth read texture (used in Renderer::AccessEFB)");
  D3D::SetDebugObjectName(
      m_efb.depth_read_texture->GetRTV(),
      "EFB depth read texture render target view (used in Renderer::AccessEFB)");

  // AccessEFB - Sysmem buffer used to retrieve the pixel data from depth_read_texture
  texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 1, 0, D3D11_USAGE_STAGING,
                                  D3D11_CPU_ACCESS_READ);
  hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &m_efb.depth_staging_buf);
  CHECK(hr == S_OK, "create EFB depth staging buffer (hr=%#x)", hr);
  D3D::SetDebugObjectName(m_efb.depth_staging_buf,
                          "EFB depth staging texture (used for Renderer::AccessEFB)");

  if (g_ActiveConfig.iMultisamples > 1)
  {
    // Framebuffer resolve textures (color+depth)
    texdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, m_target_width, m_target_height,
                                    m_efb.slices, 1, D3D11_BIND_SHADER_RESOURCE,
                                    D3D11_USAGE_DEFAULT, 0, 1);
    hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
    CHECK(hr == S_OK, "create EFB color resolve texture (size: %dx%d; hr=%#x)", m_target_width,
          m_target_height, hr);
    m_efb.resolved_color_tex =
        new D3DTexture2D(buf, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM);
    SAFE_RELEASE(buf);
    D3D::SetDebugObjectName(m_efb.resolved_color_tex->GetTex(), "EFB color resolve texture");
    D3D::SetDebugObjectName(m_efb.resolved_color_tex->GetSRV(),
                            "EFB color resolve texture shader resource view");

    texdesc =
        CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32_FLOAT, m_target_width, m_target_height, m_efb.slices,
                              1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &buf);
    CHECK(hr == S_OK, "create EFB depth resolve texture (size: %dx%d; hr=%#x)", m_target_width,
          m_target_height, hr);
    m_efb.resolved_depth_tex = new D3DTexture2D(
        buf, (D3D11_BIND_FLAG)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET),
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT);
    SAFE_RELEASE(buf);
    D3D::SetDebugObjectName(m_efb.resolved_depth_tex->GetTex(), "EFB depth resolve texture");
    D3D::SetDebugObjectName(m_efb.resolved_depth_tex->GetSRV(),
                            "EFB depth resolve texture shader resource view");
  }
  else
  {
    m_efb.resolved_color_tex = nullptr;
    m_efb.resolved_depth_tex = nullptr;
  }
  s_integer_efb_render_target = false;
}

FramebufferManager::~FramebufferManager()
{
  SAFE_RELEASE(m_efb.color_tex);
  SAFE_RELEASE(m_efb.color_int_rtv);
  SAFE_RELEASE(m_efb.color_temp_tex);
  SAFE_RELEASE(m_efb.color_temp_int_rtv);
  SAFE_RELEASE(m_efb.color_staging_buf);
  SAFE_RELEASE(m_efb.color_read_texture);
  SAFE_RELEASE(m_efb.resolved_color_tex);
  SAFE_RELEASE(m_efb.depth_tex);
  SAFE_RELEASE(m_efb.depth_staging_buf);
  SAFE_RELEASE(m_efb.depth_read_texture);
  SAFE_RELEASE(m_efb.resolved_depth_tex);
}

}  // namespace DX11
