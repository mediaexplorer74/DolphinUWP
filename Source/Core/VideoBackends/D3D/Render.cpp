// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/Render.h"

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <strsafe.h>
#include <tuple>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"

#include "VideoBackends/D3D/BoundingBox.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/FramebufferManager.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/TextureCache.h"
#include "VideoBackends/D3D/VertexShaderCache.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace DX11
{
// Nvidia stereo blitting struct defined in "nvstereo.h" from the Nvidia SDK
typedef struct _Nv_Stereo_Image_Header
{
  unsigned int dwSignature;
  unsigned int dwWidth;
  unsigned int dwHeight;
  unsigned int dwBPP;
  unsigned int dwFlags;
} NVSTEREOIMAGEHEADER, *LPNVSTEREOIMAGEHEADER;

#define NVSTEREO_IMAGE_SIGNATURE 0x4433564e

Renderer::Renderer() : ::Renderer(D3D::GetBackBufferWidth(), D3D::GetBackBufferHeight())
{
  m_last_multisamples = g_ActiveConfig.iMultisamples;
  m_last_stereo_mode = g_ActiveConfig.stereo_mode != StereoMode::Off;
  m_last_fullscreen_mode = D3D::GetFullscreenState();

  g_framebuffer_manager = std::make_unique<FramebufferManager>(m_target_width, m_target_height);
  SetupDeviceObjects();

  // Setup GX pipeline state
  for (auto& sampler : m_gx_state.samplers)
    sampler.hex = RenderState::GetPointSamplerState().hex;

  m_gx_state.zmode.testenable = false;
  m_gx_state.zmode.updateenable = false;
  m_gx_state.zmode.func = ZMode::NEVER;
  m_gx_state.raster.cullmode = GenMode::CULL_NONE;

  // Clear EFB textures
  constexpr std::array<float, 4> clear_color{{0.f, 0.f, 0.f, 1.f}};
  D3D::context->ClearRenderTargetView(FramebufferManager::GetEFBColorTexture()->GetRTV(),
                                      clear_color.data());
  D3D::context->ClearDepthStencilView(FramebufferManager::GetEFBDepthTexture()->GetDSV(),
                                      D3D11_CLEAR_DEPTH, 0.f, 0);

  D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, (float)m_target_width, (float)m_target_height);
  D3D::context->RSSetViewports(1, &vp);
  FramebufferManager::BindEFBRenderTarget();
}

Renderer::~Renderer()
{
  TeardownDeviceObjects();
}

void Renderer::SetupDeviceObjects()
{
  HRESULT hr;

  D3D11_DEPTH_STENCIL_DESC ddesc;
  ddesc.DepthEnable = FALSE;
  ddesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ddesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ddesc.StencilEnable = FALSE;
  ddesc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
  ddesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
  hr = D3D::device->CreateDepthStencilState(&ddesc, &m_clear_depth_states[0]);
  CHECK(hr == S_OK, "Create depth state for Renderer::ClearScreen");
  ddesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  ddesc.DepthEnable = TRUE;
  hr = D3D::device->CreateDepthStencilState(&ddesc, &m_clear_depth_states[1]);
  CHECK(hr == S_OK, "Create depth state for Renderer::ClearScreen");
  ddesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  hr = D3D::device->CreateDepthStencilState(&ddesc, &m_clear_depth_states[2]);
  CHECK(hr == S_OK, "Create depth state for Renderer::ClearScreen");
  D3D::SetDebugObjectName(m_clear_depth_states[0],
                          "depth state for Renderer::ClearScreen (depth buffer disabled)");
  D3D::SetDebugObjectName(
      m_clear_depth_states[1],
      "depth state for Renderer::ClearScreen (depth buffer enabled, writing enabled)");
  D3D::SetDebugObjectName(
      m_clear_depth_states[2],
      "depth state for Renderer::ClearScreen (depth buffer enabled, writing disabled)");

  D3D11_BLEND_DESC blenddesc;
  blenddesc.AlphaToCoverageEnable = FALSE;
  blenddesc.IndependentBlendEnable = FALSE;
  blenddesc.RenderTarget[0].BlendEnable = FALSE;
  blenddesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  blenddesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blenddesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
  blenddesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blenddesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blenddesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  blenddesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  hr = D3D::device->CreateBlendState(&blenddesc, &m_reset_blend_state);
  CHECK(hr == S_OK, "Create blend state for Renderer::ResetAPIState");
  D3D::SetDebugObjectName(m_reset_blend_state, "blend state for Renderer::ResetAPIState");

  m_clear_blend_states[0] = m_reset_blend_state;
  m_reset_blend_state->AddRef();

  blenddesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
  hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_blend_states[1]);
  CHECK(hr == S_OK, "Create blend state for Renderer::ClearScreen");

  blenddesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
  hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_blend_states[2]);
  CHECK(hr == S_OK, "Create blend state for Renderer::ClearScreen");

  blenddesc.RenderTarget[0].RenderTargetWriteMask = 0;
  hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_blend_states[3]);
  CHECK(hr == S_OK, "Create blend state for Renderer::ClearScreen");

  ddesc.DepthEnable = FALSE;
  ddesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ddesc.DepthFunc = D3D11_COMPARISON_LESS;
  ddesc.StencilEnable = FALSE;
  ddesc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
  ddesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
  hr = D3D::device->CreateDepthStencilState(&ddesc, &m_reset_depth_state);
  CHECK(hr == S_OK, "Create depth state for Renderer::ResetAPIState");
  D3D::SetDebugObjectName(m_reset_depth_state, "depth stencil state for Renderer::ResetAPIState");

  D3D11_RASTERIZER_DESC rastdesc = CD3D11_RASTERIZER_DESC(D3D11_FILL_SOLID, D3D11_CULL_NONE, false,
                                                          0, 0.f, 0.f, false, false, false, false);
  hr = D3D::device->CreateRasterizerState(&rastdesc, &m_reset_rast_state);
  CHECK(hr == S_OK, "Create rasterizer state for Renderer::ResetAPIState");
  D3D::SetDebugObjectName(m_reset_rast_state, "rasterizer state for Renderer::ResetAPIState");

  m_screenshot_texture = nullptr;
}

// Kill off all device objects
void Renderer::TeardownDeviceObjects()
{
  g_framebuffer_manager.reset();

  SAFE_RELEASE(m_clear_blend_states[0]);
  SAFE_RELEASE(m_clear_blend_states[1]);
  SAFE_RELEASE(m_clear_blend_states[2]);
  SAFE_RELEASE(m_clear_blend_states[3]);
  SAFE_RELEASE(m_clear_depth_states[0]);
  SAFE_RELEASE(m_clear_depth_states[1]);
  SAFE_RELEASE(m_clear_depth_states[2]);
  SAFE_RELEASE(m_reset_blend_state);
  SAFE_RELEASE(m_reset_depth_state);
  SAFE_RELEASE(m_reset_rast_state);
  SAFE_RELEASE(m_screenshot_texture);
  SAFE_RELEASE(m_3d_vision_texture);
}

void Renderer::Create3DVisionTexture(int width, int height)
{
  // Create a staging texture for 3D vision with signature information in the last row.
  // Nvidia 3D Vision supports full SBS, so there is no loss in resolution during this process.
  NVSTEREOIMAGEHEADER header;
  header.dwSignature = NVSTEREO_IMAGE_SIGNATURE;
  header.dwWidth = static_cast<u32>(width * 2);
  header.dwHeight = static_cast<u32>(height + 1);
  header.dwBPP = 32;
  header.dwFlags = 0;

  const u32 pitch = static_cast<u32>(4 * width * 2);
  const auto memory = std::make_unique<u8[]>((height + 1) * pitch);
  u8* image_header_location = &memory[height * pitch];
  std::memcpy(image_header_location, &header, sizeof(header));

  D3D11_SUBRESOURCE_DATA sys_data;
  sys_data.SysMemPitch = pitch;
  sys_data.pSysMem = memory.get();

  m_3d_vision_texture =
      D3DTexture2D::Create(width * 2, height + 1, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT,
                           DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, &sys_data);
}

std::unique_ptr<AbstractTexture> Renderer::CreateTexture(const TextureConfig& config)
{
  return std::make_unique<DXTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> Renderer::CreateStagingTexture(StagingTextureType type,
                                                                       const TextureConfig& config)
{
  return DXStagingTexture::Create(type, config);
}

void Renderer::RenderText(const std::string& text, int left, int top, u32 color)
{
  D3D::DrawTextScaled(static_cast<float>(left + 1), static_cast<float>(top + 1), 20.f, 0.0f,
                      color & 0xFF000000, text);
  D3D::DrawTextScaled(static_cast<float>(left), static_cast<float>(top), 20.f, 0.0f, color, text);
}

TargetRectangle Renderer::ConvertEFBRectangle(const EFBRectangle& rc)
{
  TargetRectangle result;
  result.left = EFBToScaledX(rc.left);
  result.top = EFBToScaledY(rc.top);
  result.right = EFBToScaledX(rc.right);
  result.bottom = EFBToScaledY(rc.bottom);
  return result;
}

// With D3D, we have to resize the backbuffer if the window changed
// size.
bool Renderer::CheckForResize()
{
  RECT rcWindow;
  GetClientRect(D3D::hWnd, &rcWindow);
  int client_width = rcWindow.right - rcWindow.left;
  int client_height = rcWindow.bottom - rcWindow.top;

  // Sanity check
  if ((client_width != GetBackbufferWidth() || client_height != GetBackbufferHeight()) &&
      client_width >= 4 && client_height >= 4)
  {
    return true;
  }

  return false;
}

void Renderer::SetScissorRect(const EFBRectangle& rc)
{
  TargetRectangle trc = ConvertEFBRectangle(rc);
  D3D::context->RSSetScissorRects(1, trc.AsRECT());
}

// This function allows the CPU to directly access the EFB.
// There are EFB peeks (which will read the color or depth of a pixel)
// and EFB pokes (which will change the color or depth of a pixel).
//
// The behavior of EFB peeks can only be modified by:
//  - GX_PokeAlphaRead
// The behavior of EFB pokes can be modified by:
//  - GX_PokeAlphaMode (TODO)
//  - GX_PokeAlphaUpdate (TODO)
//  - GX_PokeBlendMode (TODO)
//  - GX_PokeColorUpdate (TODO)
//  - GX_PokeDither (TODO)
//  - GX_PokeDstAlpha (TODO)
//  - GX_PokeZMode (TODO)
u32 Renderer::AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data)
{
  // Convert EFB dimensions to the ones of our render target
  EFBRectangle efbPixelRc;
  efbPixelRc.left = x;
  efbPixelRc.top = y;
  efbPixelRc.right = x + 1;
  efbPixelRc.bottom = y + 1;
  TargetRectangle targetPixelRc = Renderer::ConvertEFBRectangle(efbPixelRc);

  // Take the mean of the resulting dimensions; TODO: Don't use the center pixel, compute the
  // average color instead
  D3D11_RECT RectToLock;
  if (type == EFBAccessType::PeekColor || type == EFBAccessType::PeekZ)
  {
    RectToLock.left = (targetPixelRc.left + targetPixelRc.right) / 2;
    RectToLock.top = (targetPixelRc.top + targetPixelRc.bottom) / 2;
    RectToLock.right = RectToLock.left + 1;
    RectToLock.bottom = RectToLock.top + 1;
  }
  else
  {
    RectToLock.left = targetPixelRc.left;
    RectToLock.right = targetPixelRc.right;
    RectToLock.top = targetPixelRc.top;
    RectToLock.bottom = targetPixelRc.bottom;
  }

  // Reset any game specific settings.
  ResetAPIState();
  D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, 1.f, 1.f);
  D3D::context->RSSetViewports(1, &vp);
  D3D::SetPointCopySampler();

  // Select copy and read textures depending on if we are doing a color or depth read (since they
  // are different formats).
  D3DTexture2D* source_tex;
  D3DTexture2D* read_tex;
  ID3D11Texture2D* staging_tex;
  if (type == EFBAccessType::PeekColor)
  {
    source_tex = FramebufferManager::GetEFBColorTexture();
    read_tex = FramebufferManager::GetEFBColorReadTexture();
    staging_tex = FramebufferManager::GetEFBColorStagingBuffer();
  }
  else
  {
    source_tex = FramebufferManager::GetEFBDepthTexture();
    read_tex = FramebufferManager::GetEFBDepthReadTexture();
    staging_tex = FramebufferManager::GetEFBDepthStagingBuffer();
  }

  // Select pixel shader (we don't want to average depth samples, instead select the minimum).
  ID3D11PixelShader* copy_pixel_shader;
  if (type == EFBAccessType::PeekZ && g_ActiveConfig.iMultisamples > 1)
    copy_pixel_shader = PixelShaderCache::GetDepthResolveProgram();
  else
    copy_pixel_shader = PixelShaderCache::GetColorCopyProgram(true);

  // Draw a quad to grab the texel we want to read.
  D3D::context->OMSetRenderTargets(1, &read_tex->GetRTV(), nullptr);
  D3D::drawShadedTexQuad(source_tex->GetSRV(), &RectToLock, Renderer::GetTargetWidth(),
                         Renderer::GetTargetHeight(), copy_pixel_shader,
                         VertexShaderCache::GetSimpleVertexShader(),
                         VertexShaderCache::GetSimpleInputLayout());

  // Restore expected game state.
  FramebufferManager::BindEFBRenderTarget();
  RestoreAPIState();

  // Copy the pixel from the renderable to cpu-readable buffer.
  D3D11_BOX box = CD3D11_BOX(0, 0, 0, 1, 1, 1);
  D3D::context->CopySubresourceRegion(staging_tex, 0, 0, 0, 0, read_tex->GetTex(), 0, &box);
  D3D11_MAPPED_SUBRESOURCE map;
  CHECK(D3D::context->Map(staging_tex, 0, D3D11_MAP_READ, 0, &map) == S_OK,
        "Map staging buffer failed");

  // Convert the framebuffer data to the format the game is expecting to receive.
  u32 ret;
  if (type == EFBAccessType::PeekColor)
  {
    u32 val;
    memcpy(&val, map.pData, sizeof(val));

    // our buffers are RGBA, yet a BGRA value is expected
    val = ((val & 0xFF00FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000));

    // check what to do with the alpha channel (GX_PokeAlphaRead)
    PixelEngine::UPEAlphaReadReg alpha_read_mode = PixelEngine::GetAlphaReadMode();

    if (bpmem.zcontrol.pixel_format == PEControl::RGBA6_Z24)
    {
      val = RGBA8ToRGBA6ToRGBA8(val);
    }
    else if (bpmem.zcontrol.pixel_format == PEControl::RGB565_Z16)
    {
      val = RGBA8ToRGB565ToRGBA8(val);
    }
    if (bpmem.zcontrol.pixel_format != PEControl::RGBA6_Z24)
    {
      val |= 0xFF000000;
    }

    if (alpha_read_mode.ReadMode == 2)
      ret = val;  // GX_READ_NONE
    else if (alpha_read_mode.ReadMode == 1)
      ret = (val | 0xFF000000);  // GX_READ_FF
    else                         /*if(alpha_read_mode.ReadMode == 0)*/
      ret = (val & 0x00FFFFFF);  // GX_READ_00
  }
  else  // type == EFBAccessType::PeekZ
  {
    float val;
    memcpy(&val, map.pData, sizeof(val));

    // depth buffer is inverted in the d3d backend
    val = 1.0f - val;

    if (bpmem.zcontrol.pixel_format == PEControl::RGB565_Z16)
    {
      // if Z is in 16 bit format you must return a 16 bit integer
      ret = MathUtil::Clamp<u32>(static_cast<u32>(val * 65536.0f), 0, 0xFFFF);
    }
    else
    {
      ret = MathUtil::Clamp<u32>(static_cast<u32>(val * 16777216.0f), 0, 0xFFFFFF);
    }
  }

  D3D::context->Unmap(staging_tex, 0);
  return ret;
}

void Renderer::PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points)
{
  ResetAPIState();

  if (type == EFBAccessType::PokeColor)
  {
    D3D11_VIEWPORT vp =
        CD3D11_VIEWPORT(0.0f, 0.0f, (float)GetTargetWidth(), (float)GetTargetHeight());
    D3D::context->RSSetViewports(1, &vp);
    FramebufferManager::BindEFBRenderTarget(false);
  }
  else  // if (type == EFBAccessType::PokeZ)
  {
    D3D::stateman->PushBlendState(m_clear_blend_states[3]);
    D3D::stateman->PushDepthState(m_clear_depth_states[1]);

    D3D11_VIEWPORT vp =
        CD3D11_VIEWPORT(0.0f, 0.0f, (float)GetTargetWidth(), (float)GetTargetHeight());

    D3D::context->RSSetViewports(1, &vp);
    FramebufferManager::BindEFBRenderTarget();
  }

  D3D::DrawEFBPokeQuads(type, points, num_points);

  if (type == EFBAccessType::PokeZ)
  {
    D3D::stateman->PopDepthState();
    D3D::stateman->PopBlendState();
  }

  RestoreAPIState();
}

void Renderer::SetViewport()
{
  // reversed gxsetviewport(xorig, yorig, width, height, nearz, farz)
  // [0] = width/2
  // [1] = height/2
  // [2] = 16777215 * (farz - nearz)
  // [3] = xorig + width/2 + 342
  // [4] = yorig + height/2 + 342
  // [5] = 16777215 * farz

  // D3D crashes for zero viewports
  if (xfmem.viewport.wd == 0 || xfmem.viewport.ht == 0)
    return;

  int scissorXOff = bpmem.scissorOffset.x * 2;
  int scissorYOff = bpmem.scissorOffset.y * 2;

  float X = Renderer::EFBToScaledXf(xfmem.viewport.xOrig - xfmem.viewport.wd - scissorXOff);
  float Y = Renderer::EFBToScaledYf(xfmem.viewport.yOrig + xfmem.viewport.ht - scissorYOff);
  float Wd = Renderer::EFBToScaledXf(2.0f * xfmem.viewport.wd);
  float Ht = Renderer::EFBToScaledYf(-2.0f * xfmem.viewport.ht);
  float min_depth = (xfmem.viewport.farZ - xfmem.viewport.zRange) / 16777216.0f;
  float max_depth = xfmem.viewport.farZ / 16777216.0f;
  if (Wd < 0.0f)
  {
    X += Wd;
    Wd = -Wd;
  }
  if (Ht < 0.0f)
  {
    Y += Ht;
    Ht = -Ht;
  }

  // If an inverted or oversized depth range is used, we need to calculate the depth range in the
  // vertex shader.
  if (UseVertexDepthRange())
  {
    // We need to ensure depth values are clamped the maximum value supported by the console GPU.
    min_depth = 0.0f;
    max_depth = GX_MAX_DEPTH;
  }

  // In D3D, the viewport rectangle must fit within the render target.
  X = (X >= 0.f) ? X : 0.f;
  Y = (Y >= 0.f) ? Y : 0.f;
  Wd = (X + Wd <= GetTargetWidth()) ? Wd : (GetTargetWidth() - X);
  Ht = (Y + Ht <= GetTargetHeight()) ? Ht : (GetTargetHeight() - Y);

  // We use an inverted depth range here to apply the Reverse Z trick.
  // This trick makes sure we match the precision provided by the 1:0
  // clipping depth range on the hardware.
  D3D11_VIEWPORT vp = CD3D11_VIEWPORT(X, Y, Wd, Ht, 1.0f - max_depth, 1.0f - min_depth);
  D3D::context->RSSetViewports(1, &vp);
}

void Renderer::ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable,
                           u32 color, u32 z)
{
  ResetAPIState();

  if (colorEnable && alphaEnable)
    D3D::stateman->PushBlendState(m_clear_blend_states[0]);
  else if (colorEnable)
    D3D::stateman->PushBlendState(m_clear_blend_states[1]);
  else if (alphaEnable)
    D3D::stateman->PushBlendState(m_clear_blend_states[2]);
  else
    D3D::stateman->PushBlendState(m_clear_blend_states[3]);

  // TODO: Should we enable Z testing here?
  // if (!bpmem.zmode.testenable) D3D::stateman->PushDepthState(s_clear_depth_states[0]);
  // else
  if (zEnable)
    D3D::stateman->PushDepthState(m_clear_depth_states[1]);
  else /*if (!zEnable)*/
    D3D::stateman->PushDepthState(m_clear_depth_states[2]);

  // Update the view port for clearing the picture
  TargetRectangle targetRc = Renderer::ConvertEFBRectangle(rc);
  D3D11_VIEWPORT vp =
      CD3D11_VIEWPORT((float)targetRc.left, (float)targetRc.top, (float)targetRc.GetWidth(),
                      (float)targetRc.GetHeight(), 0.f, 1.f);
  D3D::context->RSSetViewports(1, &vp);
  FramebufferManager::SetIntegerEFBRenderTarget(false);

  // Color is passed in bgra mode so we need to convert it to rgba
  u32 rgbaColor = (color & 0xFF00FF00) | ((color >> 16) & 0xFF) | ((color << 16) & 0xFF0000);
  D3D::drawClearQuad(rgbaColor, 1.0f - (z & 0xFFFFFF) / 16777216.0f);

  D3D::stateman->PopDepthState();
  D3D::stateman->PopBlendState();

  RestoreAPIState();
}

void Renderer::ReinterpretPixelData(unsigned int convtype)
{
  // TODO: MSAA support..
  D3D11_RECT source = CD3D11_RECT(0, 0, GetTargetWidth(), GetTargetHeight());

  ID3D11PixelShader* pixel_shader;
  if (convtype == 0)
    pixel_shader = PixelShaderCache::ReinterpRGB8ToRGBA6(true);
  else if (convtype == 2)
    pixel_shader = PixelShaderCache::ReinterpRGBA6ToRGB8(true);
  else
  {
    ERROR_LOG(VIDEO, "Trying to reinterpret pixel data with unsupported conversion type %d",
              convtype);
    return;
  }

  // convert data and set the target texture as our new EFB
  ResetAPIState();

  D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, static_cast<float>(GetTargetWidth()),
                                      static_cast<float>(GetTargetHeight()));
  D3D::context->RSSetViewports(1, &vp);

  D3D::context->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTempTexture()->GetRTV(),
                                   nullptr);
  D3D::SetPointCopySampler();
  D3D::drawShadedTexQuad(
      FramebufferManager::GetEFBColorTexture()->GetSRV(), &source, GetTargetWidth(),
      GetTargetHeight(), pixel_shader, VertexShaderCache::GetSimpleVertexShader(),
      VertexShaderCache::GetSimpleInputLayout(), GeometryShaderCache::GetCopyGeometryShader());

  RestoreAPIState();

  FramebufferManager::SwapReinterpretTexture();
  FramebufferManager::BindEFBRenderTarget();
}

void Renderer::SetBlendingState(const BlendingState& state)
{
  m_gx_state.blend.hex = state.hex;
}

// This function has the final picture. We adjust the aspect ratio here.
void Renderer::SwapImpl(AbstractTexture* texture, const EFBRectangle& xfb_region, u64 ticks,
                        float Gamma)
{
  ResetAPIState();

  // Prepare to copy the XFBs to our backbuffer
  UpdateDrawRectangle();
  TargetRectangle targetRc = GetTargetRectangle();

  D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);

  constexpr std::array<float, 4> clear_color{{0.f, 0.f, 0.f, 1.f}};
  D3D::context->ClearRenderTargetView(D3D::GetBackBuffer()->GetRTV(), clear_color.data());

  // activate linear filtering for the buffer copies
  D3D::SetLinearCopySampler();
  auto* xfb_texture = static_cast<DXTexture*>(texture);

  BlitScreen(xfb_region, targetRc, xfb_texture->GetRawTexIdentifier(),
             xfb_texture->GetConfig().width, xfb_texture->GetConfig().height, Gamma);

  // Reset viewport for drawing text
  D3D11_VIEWPORT vp =
      CD3D11_VIEWPORT(0.0f, 0.0f, (float)GetBackbufferWidth(), (float)GetBackbufferHeight());
  D3D::context->RSSetViewports(1, &vp);

  Renderer::DrawDebugText();

  OSD::DrawMessages();

  g_texture_cache->Cleanup(frameCount);

  // Enable configuration changes
  UpdateActiveConfig();
  g_texture_cache->OnConfigChanged(g_ActiveConfig);
  VertexShaderCache::RetreiveAsyncShaders();

  SetWindowSize(xfb_texture->GetConfig().width, xfb_texture->GetConfig().height);

  const bool window_resized = CheckForResize();
  const bool fullscreen = D3D::GetFullscreenState();
  const bool fs_changed = m_last_fullscreen_mode != fullscreen;

  // Flip/present backbuffer to frontbuffer here
  D3D::Present();

  // Resize the back buffers NOW to avoid flickering
  if (CalculateTargetSize() || window_resized || fs_changed ||
      m_last_multisamples != g_ActiveConfig.iMultisamples ||
      m_last_stereo_mode != (g_ActiveConfig.stereo_mode != StereoMode::Off))
  {
    m_last_multisamples = g_ActiveConfig.iMultisamples;
    m_last_fullscreen_mode = fullscreen;
    PixelShaderCache::InvalidateMSAAShaders();

    if (window_resized || fs_changed)
    {
      // TODO: Aren't we still holding a reference to the back buffer right now?
      D3D::Reset();
      SAFE_RELEASE(m_screenshot_texture);
      SAFE_RELEASE(m_3d_vision_texture);
      m_backbuffer_width = D3D::GetBackBufferWidth();
      m_backbuffer_height = D3D::GetBackBufferHeight();
    }

    UpdateDrawRectangle();

    m_last_stereo_mode = g_ActiveConfig.stereo_mode != StereoMode::Off;

    D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);

    g_framebuffer_manager.reset();
    g_framebuffer_manager = std::make_unique<FramebufferManager>(m_target_width, m_target_height);

    D3D::context->ClearRenderTargetView(FramebufferManager::GetEFBColorTexture()->GetRTV(),
                                        clear_color.data());
    D3D::context->ClearDepthStencilView(FramebufferManager::GetEFBDepthTexture()->GetDSV(),
                                        D3D11_CLEAR_DEPTH, 0.f, 0);
  }

  if (CheckForHostConfigChanges())
  {
    VertexShaderCache::Reload();
    GeometryShaderCache::Reload();
    PixelShaderCache::Reload();
  }

  // begin next frame
  RestoreAPIState();
  FramebufferManager::BindEFBRenderTarget();
  SetViewport();
}

// ALWAYS call RestoreAPIState for each ResetAPIState call you're doing
void Renderer::ResetAPIState()
{
  D3D::stateman->PushBlendState(m_reset_blend_state);
  D3D::stateman->PushDepthState(m_reset_depth_state);
  D3D::stateman->PushRasterizerState(m_reset_rast_state);
}

void Renderer::RestoreAPIState()
{
  // Gets us back into a more game-like state.
  D3D::stateman->PopBlendState();
  D3D::stateman->PopDepthState();
  D3D::stateman->PopRasterizerState();
  SetViewport();
  BPFunctions::SetScissor();
}

void Renderer::ApplyState()
{
  D3D::stateman->PushBlendState(m_state_cache.Get(m_gx_state.blend));
  D3D::stateman->PushDepthState(m_state_cache.Get(m_gx_state.zmode));
  D3D::stateman->PushRasterizerState(m_state_cache.Get(m_gx_state.raster));
  D3D::stateman->SetPrimitiveTopology(
      StateCache::GetPrimitiveTopology(m_gx_state.raster.primitive));
  FramebufferManager::SetIntegerEFBRenderTarget(m_gx_state.blend.logicopenable);

  for (u32 stage = 0; stage < static_cast<u32>(m_gx_state.samplers.size()); stage++)
    D3D::stateman->SetSampler(stage, m_state_cache.Get(m_gx_state.samplers[stage]));

  ID3D11Buffer* vertexConstants = VertexShaderCache::GetConstantBuffer();

  D3D::stateman->SetPixelConstants(PixelShaderCache::GetConstantBuffer(),
                                   g_ActiveConfig.bEnablePixelLighting ? vertexConstants : nullptr);
  D3D::stateman->SetVertexConstants(vertexConstants);
  D3D::stateman->SetGeometryConstants(GeometryShaderCache::GetConstantBuffer());
}

void Renderer::RestoreState()
{
  D3D::stateman->PopBlendState();
  D3D::stateman->PopDepthState();
  D3D::stateman->PopRasterizerState();
}

void Renderer::SetRasterizationState(const RasterizationState& state)
{
  m_gx_state.raster.hex = state.hex;
}

void Renderer::SetDepthState(const DepthState& state)
{
  m_gx_state.zmode.hex = state.hex;
}

void Renderer::SetSamplerState(u32 index, const SamplerState& state)
{
  m_gx_state.samplers[index].hex = state.hex;
}

void Renderer::SetInterlacingMode()
{
  // TODO
}

u16 Renderer::BBoxRead(int index)
{
  // Here we get the min/max value of the truncated position of the upscaled framebuffer.
  // So we have to correct them to the unscaled EFB sizes.
  int value = BBox::Get(index);

  if (index < 2)
  {
    // left/right
    value = value * EFB_WIDTH / m_target_width;
  }
  else
  {
    // up/down
    value = value * EFB_HEIGHT / m_target_height;
  }
  if (index & 1)
    value++;  // fix max values to describe the outer border

  return value;
}

void Renderer::BBoxWrite(int index, u16 _value)
{
  int value = _value;  // u16 isn't enough to multiply by the efb width
  if (index & 1)
    value--;
  if (index < 2)
  {
    value = value * m_target_width / EFB_WIDTH;
  }
  else
  {
    value = value * m_target_height / EFB_HEIGHT;
  }

  BBox::Set(index, value);
}

void Renderer::BlitScreen(TargetRectangle src, TargetRectangle dst, D3DTexture2D* src_texture,
                          u32 src_width, u32 src_height, float Gamma)
{
  if (g_ActiveConfig.stereo_mode == StereoMode::SBS ||
      g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    TargetRectangle leftRc, rightRc;
    std::tie(leftRc, rightRc) = ConvertStereoRectangle(dst);

    D3D11_VIEWPORT leftVp = CD3D11_VIEWPORT((float)leftRc.left, (float)leftRc.top,
                                            (float)leftRc.GetWidth(), (float)leftRc.GetHeight());
    D3D11_VIEWPORT rightVp = CD3D11_VIEWPORT((float)rightRc.left, (float)rightRc.top,
                                             (float)rightRc.GetWidth(), (float)rightRc.GetHeight());

    D3D::context->RSSetViewports(1, &leftVp);
    D3D::drawShadedTexQuad(src_texture->GetSRV(), src.AsRECT(), src_width, src_height,
                           PixelShaderCache::GetColorCopyProgram(false),
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout(), nullptr, Gamma, 0);

    D3D::context->RSSetViewports(1, &rightVp);
    D3D::drawShadedTexQuad(src_texture->GetSRV(), src.AsRECT(), src_width, src_height,
                           PixelShaderCache::GetColorCopyProgram(false),
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout(), nullptr, Gamma, 1);
  }
  else if (g_ActiveConfig.stereo_mode == StereoMode::Nvidia3DVision)
  {
    if (!m_3d_vision_texture)
      Create3DVisionTexture(m_backbuffer_width, m_backbuffer_height);

    D3D11_VIEWPORT leftVp = CD3D11_VIEWPORT((float)dst.left, (float)dst.top, (float)dst.GetWidth(),
                                            (float)dst.GetHeight());
    D3D11_VIEWPORT rightVp = CD3D11_VIEWPORT((float)(dst.left + m_backbuffer_width), (float)dst.top,
                                             (float)dst.GetWidth(), (float)dst.GetHeight());

    // Render to staging texture which is double the width of the backbuffer
    D3D::context->OMSetRenderTargets(1, &m_3d_vision_texture->GetRTV(), nullptr);

    D3D::context->RSSetViewports(1, &leftVp);
    D3D::drawShadedTexQuad(src_texture->GetSRV(), src.AsRECT(), src_width, src_height,
                           PixelShaderCache::GetColorCopyProgram(false),
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout(), nullptr, Gamma, 0);

    D3D::context->RSSetViewports(1, &rightVp);
    D3D::drawShadedTexQuad(src_texture->GetSRV(), src.AsRECT(), src_width, src_height,
                           PixelShaderCache::GetColorCopyProgram(false),
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout(), nullptr, Gamma, 1);

    // Copy the left eye to the backbuffer, if Nvidia 3D Vision is enabled it should
    // recognize the signature and automatically include the right eye frame.
    D3D11_BOX box = CD3D11_BOX(0, 0, 0, m_backbuffer_width, m_backbuffer_height, 1);
    D3D::context->CopySubresourceRegion(D3D::GetBackBuffer()->GetTex(), 0, 0, 0, 0,
                                        m_3d_vision_texture->GetTex(), 0, &box);

    // Restore render target to backbuffer
    D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);
  }
  else
  {
    D3D11_VIEWPORT vp = CD3D11_VIEWPORT((float)dst.left, (float)dst.top, (float)dst.GetWidth(),
                                        (float)dst.GetHeight());
    D3D::context->RSSetViewports(1, &vp);

    ID3D11PixelShader* pixelShader = (g_Config.stereo_mode == StereoMode::Anaglyph) ?
                                         PixelShaderCache::GetAnaglyphProgram() :
                                         PixelShaderCache::GetColorCopyProgram(false);
    ID3D11GeometryShader* geomShader = (g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer) ?
                                           GeometryShaderCache::GetCopyGeometryShader() :
                                           nullptr;
    D3D::drawShadedTexQuad(src_texture->GetSRV(), src.AsRECT(), src_width, src_height, pixelShader,
                           VertexShaderCache::GetSimpleVertexShader(),
                           VertexShaderCache::GetSimpleInputLayout(), geomShader, Gamma);
  }
}

void Renderer::SetFullscreen(bool enable_fullscreen)
{
  D3D::SetFullscreenState(enable_fullscreen);
}

bool Renderer::IsFullscreen() const
{
  return D3D::GetFullscreenState();
}

}  // namespace DX11
