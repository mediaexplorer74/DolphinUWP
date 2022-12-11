// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The GPU draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------

#include "VideoCommon/RenderBase.h"

#include <cinttypes>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Profiler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/Movie.h"

#include "VideoCommon/AVIDump.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/FPSCounter.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

// TODO: Move these out of here.
int frameCount;
int OSDChoice;
static int OSDTime;

std::unique_ptr<Renderer> g_renderer;

// The maximum depth that is written to the depth buffer should never exceed this value.
// This is necessary because we use a 2^24 divisor for all our depth values to prevent
// floating-point round-trip errors. However the console GPU doesn't ever write a value
// to the depth buffer that exceeds 2^24 - 1.
const float Renderer::GX_MAX_DEPTH = 16777215.0f / 16777216.0f;

static float AspectToWidescreen(float aspect)
{
  return aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

Renderer::Renderer(int backbuffer_width, int backbuffer_height)
    : m_backbuffer_width(backbuffer_width), m_backbuffer_height(backbuffer_height)
{
  UpdateActiveConfig();
  UpdateDrawRectangle();
  CalculateTargetSize();

  OSDChoice = 0;
  OSDTime = 0;

  if (SConfig::GetInstance().bWii)
    m_aspect_wide = Config::Get(Config::SYSCONF_WIDESCREEN);

  m_surface_handle = Host_GetRenderHandle();
  m_last_host_config_bits = ShaderHostConfig::GetCurrent().bits;
}

Renderer::~Renderer() = default;

void Renderer::RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc, u32 fbStride, u32 fbHeight,
                           float Gamma)
{
  CheckFifoRecording();

  if (!fbStride || !fbHeight)
    return;
}

unsigned int Renderer::GetEFBScale() const
{
  return m_efb_scale;
}

int Renderer::EFBToScaledX(int x) const
{
  return x * static_cast<int>(m_efb_scale);
}

int Renderer::EFBToScaledY(int y) const
{
  return y * static_cast<int>(m_efb_scale);
}

float Renderer::EFBToScaledXf(float x) const
{
  return x * ((float)GetTargetWidth() / (float)EFB_WIDTH);
}

float Renderer::EFBToScaledYf(float y) const
{
  return y * ((float)GetTargetHeight() / (float)EFB_HEIGHT);
}

std::tuple<int, int> Renderer::CalculateTargetScale(int x, int y) const
{
  return std::make_tuple(x * static_cast<int>(m_efb_scale), y * static_cast<int>(m_efb_scale));
}

// return true if target size changed
bool Renderer::CalculateTargetSize()
{
  if (g_ActiveConfig.iEFBScale == EFB_SCALE_AUTO_INTEGRAL)
  {
    // Set a scale based on the window size
    int width = EFB_WIDTH * m_target_rectangle.GetWidth() / m_last_xfb_width;
    int height = EFB_HEIGHT * m_target_rectangle.GetWidth() / m_last_xfb_height;
    m_efb_scale = std::max((width - 1) / EFB_WIDTH + 1, (height - 1) / EFB_HEIGHT + 1);
  }
  else
  {
    m_efb_scale = g_ActiveConfig.iEFBScale;
  }

  const u32 max_size = g_ActiveConfig.backend_info.MaxTextureSize;
  if (max_size < EFB_WIDTH * m_efb_scale)
    m_efb_scale = max_size / EFB_WIDTH;

  int new_efb_width = 0;
  int new_efb_height = 0;
  std::tie(new_efb_width, new_efb_height) = CalculateTargetScale(EFB_WIDTH, EFB_HEIGHT);

  if (new_efb_width != m_target_width || new_efb_height != m_target_height)
  {
    m_target_width = new_efb_width;
    m_target_height = new_efb_height;
    PixelShaderManager::SetEfbScaleChanged(EFBToScaledXf(1), EFBToScaledYf(1));
    return true;
  }
  return false;
}

std::tuple<TargetRectangle, TargetRectangle>
Renderer::ConvertStereoRectangle(const TargetRectangle& rc) const
{
  // Resize target to half its original size
  TargetRectangle draw_rc = rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    // The height may be negative due to flipped rectangles
    int height = rc.bottom - rc.top;
    draw_rc.top += height / 4;
    draw_rc.bottom -= height / 4;
  }
  else
  {
    int width = rc.right - rc.left;
    draw_rc.left += width / 4;
    draw_rc.right -= width / 4;
  }

  // Create two target rectangle offset to the sides of the backbuffer
  TargetRectangle left_rc = draw_rc;
  TargetRectangle right_rc = draw_rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    left_rc.top -= m_backbuffer_height / 4;
    left_rc.bottom -= m_backbuffer_height / 4;
    right_rc.top += m_backbuffer_height / 4;
    right_rc.bottom += m_backbuffer_height / 4;
  }
  else
  {
    left_rc.left -= m_backbuffer_width / 4;
    left_rc.right -= m_backbuffer_width / 4;
    right_rc.left += m_backbuffer_width / 4;
    right_rc.right += m_backbuffer_width / 4;
  }

  return std::make_tuple(left_rc, right_rc);
}

void Renderer::SaveScreenshot(const std::string& filename, bool wait_for_completion)
{
  // We must not hold the lock while waiting for the screenshot to complete.
  {
    std::lock_guard<std::mutex> lk(m_screenshot_lock);
    m_screenshot_name = filename;
    m_screenshot_request.Set();
  }

  if (wait_for_completion)
  {
    // This is currently only used by Android, and it was using a wait time of 2 seconds.
    m_screenshot_completed.WaitFor(std::chrono::seconds(2));
  }
}

bool Renderer::CheckForHostConfigChanges()
{
  ShaderHostConfig new_host_config = ShaderHostConfig::GetCurrent();
  if (new_host_config.bits == m_last_host_config_bits)
    return false;

  OSD::AddMessage("Video config changed, reloading shaders.", OSD::Duration::NORMAL);
  m_last_host_config_bits = new_host_config.bits;
  return true;
}

// Create On-Screen-Messages
void Renderer::DrawDebugText()
{
  std::string final_yellow, final_cyan;

  if (g_ActiveConfig.bShowFPS || SConfig::GetInstance().m_ShowFrameCount)
  {
    if (g_ActiveConfig.bShowFPS)
      final_cyan += StringFromFormat("FPS: %.2f", m_fps_counter.GetFPS());

    if (g_ActiveConfig.bShowFPS && SConfig::GetInstance().m_ShowFrameCount)
      final_cyan += " - ";
    if (SConfig::GetInstance().m_ShowFrameCount)
    {
      final_cyan += StringFromFormat("Frame: %" PRIu64, Movie::GetCurrentFrame());
      if (Movie::IsPlayingInput())
        final_cyan += StringFromFormat("\nInput: %" PRIu64 " / %" PRIu64,
                                       Movie::GetCurrentInputCount(), Movie::GetTotalInputCount());
    }

    final_cyan += "\n";
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowLag)
  {
    final_cyan += StringFromFormat("Lag: %" PRIu64 "\n", Movie::GetCurrentLagCount());
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowInputDisplay)
  {
    final_cyan += Movie::GetInputDisplay();
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowRTC)
  {
    final_cyan += Movie::GetRTCDisplay();
    final_yellow += "\n";
  }

  // OSD Menu messages
  if (OSDChoice > 0)
  {
    OSDTime = Common::Timer::GetTimeMs() + 3000;
    OSDChoice = -OSDChoice;
  }

  if ((u32)OSDTime > Common::Timer::GetTimeMs())
  {
    std::string res_text;
    switch (g_ActiveConfig.iEFBScale)
    {
    case EFB_SCALE_AUTO_INTEGRAL:
      res_text = "Auto (integral)";
      break;
    case 1:
      res_text = "Native";
      break;
    default:
      res_text = StringFromFormat("%dx", g_ActiveConfig.iEFBScale);
      break;
    }
    const char* ar_text = "";
    switch (g_ActiveConfig.aspect_mode)
    {
    case AspectMode::Auto:
      ar_text = "Auto";
      break;
    case AspectMode::Stretch:
      ar_text = "Stretch";
      break;
    case AspectMode::Analog:
      ar_text = "Force 4:3";
      break;
    case AspectMode::AnalogWide:
      ar_text = "Force 16:9";
      break;
    }

    const char* const efbcopy_text = g_ActiveConfig.bSkipEFBCopyToRam ? "to Texture" : "to RAM";
    const char* const xfbcopy_text = g_ActiveConfig.bSkipXFBCopyToRam ? "to Texture" : "to RAM";

    // The rows
    const std::string lines[] = {
        std::string("Internal Resolution: ") + res_text,
        std::string("Aspect Ratio: ") + ar_text + (g_ActiveConfig.bCrop ? " (crop)" : ""),
        std::string("Copy EFB: ") + efbcopy_text,
        std::string("Fog: ") + (g_ActiveConfig.bDisableFog ? "Disabled" : "Enabled"),
        SConfig::GetInstance().m_EmulationSpeed <= 0 ?
            "Speed Limit: Unlimited" :
            StringFromFormat("Speed Limit: %li%%",
                             std::lround(SConfig::GetInstance().m_EmulationSpeed * 100.f)),
        std::string("Copy XFB: ") + xfbcopy_text +
            (g_ActiveConfig.bImmediateXFB ? " (Immediate)" : ""),
    };

    enum
    {
      lines_count = sizeof(lines) / sizeof(*lines)
    };

    // The latest changed setting in yellow
    for (int i = 0; i != lines_count; ++i)
    {
      if (OSDChoice == -i - 1)
        final_yellow += lines[i];
      final_yellow += '\n';
    }

    // The other settings in cyan
    for (int i = 0; i != lines_count; ++i)
    {
      if (OSDChoice != -i - 1)
        final_cyan += lines[i];
      final_cyan += '\n';
    }
  }

  final_cyan += Common::Profiler::ToString();

  if (g_ActiveConfig.bOverlayStats)
    final_cyan += Statistics::ToString();

  if (g_ActiveConfig.bOverlayProjStats)
    final_cyan += Statistics::ToStringProj();

  // and then the text
  RenderText(final_cyan, 20, 20, 0xFF00FFFF);
  RenderText(final_yellow, 20, 20, 0xFFFFFF00);
}

float Renderer::CalculateDrawAspectRatio() const
{
  if (g_ActiveConfig.aspect_mode == AspectMode::Stretch)
  {
    // If stretch is enabled, we prefer the aspect ratio of the window.
    return (static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height));
  }

  // The rendering window aspect ratio as a proportion of the 4:3 or 16:9 ratio
  if (g_ActiveConfig.aspect_mode == AspectMode::AnalogWide ||
      (g_ActiveConfig.aspect_mode != AspectMode::Analog && m_aspect_wide))
  {
    return AspectToWidescreen(VideoInterface::GetAspectRatio());
  }
  else
  {
    return VideoInterface::GetAspectRatio();
  }
}

bool Renderer::IsHeadless() const
{
  return !m_surface_handle;
}

std::tuple<float, float> Renderer::ScaleToDisplayAspectRatio(const int width,
                                                             const int height) const
{
  // Scale either the width or height depending the content aspect ratio.
  // This way we preserve as much resolution as possible when scaling.
  float scaled_width = static_cast<float>(width);
  float scaled_height = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio();
  if (scaled_width / scaled_height >= draw_aspect)
    scaled_height = scaled_width / draw_aspect;
  else
    scaled_width = scaled_height * draw_aspect;
  return std::make_tuple(scaled_width, scaled_height);
}

void Renderer::UpdateDrawRectangle()
{
  // The rendering window size
  const float win_width = static_cast<float>(m_backbuffer_width);
  const float win_height = static_cast<float>(m_backbuffer_height);

  // Update aspect ratio hack values
  // Won't take effect until next frame
  // Don't know if there is a better place for this code so there isn't a 1 frame delay
  if (g_ActiveConfig.bWidescreenHack)
  {
    float source_aspect = VideoInterface::GetAspectRatio();
    if (m_aspect_wide)
      source_aspect = AspectToWidescreen(source_aspect);
    float target_aspect = 0.0f;

    switch (g_ActiveConfig.aspect_mode)
    {
    case AspectMode::Stretch:
      target_aspect = win_width / win_height;
      break;
    case AspectMode::Analog:
      target_aspect = VideoInterface::GetAspectRatio();
      break;
    case AspectMode::AnalogWide:
      target_aspect = AspectToWidescreen(VideoInterface::GetAspectRatio());
      break;
    case AspectMode::Auto:
      target_aspect = source_aspect;
      break;
    }

    float adjust = source_aspect / target_aspect;
    if (adjust > 1)
    {
      // Vert+
      g_Config.fAspectRatioHackW = 1;
      g_Config.fAspectRatioHackH = 1 / adjust;
    }
    else
    {
      // Hor+
      g_Config.fAspectRatioHackW = adjust;
      g_Config.fAspectRatioHackH = 1;
    }
  }
  else
  {
    // Hack is disabled
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  float draw_width, draw_height, crop_width, crop_height;

  // get the picture aspect ratio
  draw_width = crop_width = CalculateDrawAspectRatio();
  draw_height = crop_height = 1;

  // crop the picture to a standard aspect ratio
  if (g_ActiveConfig.bCrop && g_ActiveConfig.aspect_mode != AspectMode::Stretch)
  {
    float expected_aspect = (g_ActiveConfig.aspect_mode == AspectMode::AnalogWide ||
                             (g_ActiveConfig.aspect_mode != AspectMode::Analog && m_aspect_wide)) ?
                                (16.0f / 9.0f) :
                                (4.0f / 3.0f);
    if (crop_width / crop_height >= expected_aspect)
    {
      // the picture is flatter than it should be
      crop_width = crop_height * expected_aspect;
    }
    else
    {
      // the picture is skinnier than it should be
      crop_height = crop_width / expected_aspect;
    }
  }

  // scale the picture to fit the rendering window
  if (win_width / win_height >= crop_width / crop_height)
  {
    // the window is flatter than the picture
    draw_width *= win_height / crop_height;
    crop_width *= win_height / crop_height;
    draw_height *= win_height / crop_height;
    crop_height = win_height;
  }
  else
  {
    // the window is skinnier than the picture
    draw_width *= win_width / crop_width;
    draw_height *= win_width / crop_width;
    crop_height *= win_width / crop_width;
    crop_width = win_width;
  }

  // ensure divisibility by 4 to make it compatible with all the video encoders
  draw_width = std::ceil(draw_width) - static_cast<int>(std::ceil(draw_width)) % 4;
  draw_height = std::ceil(draw_height) - static_cast<int>(std::ceil(draw_height)) % 4;

  m_target_rectangle.left = static_cast<int>(std::round(win_width / 2.0 - draw_width / 2.0));
  m_target_rectangle.top = static_cast<int>(std::round(win_height / 2.0 - draw_height / 2.0));
  m_target_rectangle.right = m_target_rectangle.left + static_cast<int>(draw_width);
  m_target_rectangle.bottom = m_target_rectangle.top + static_cast<int>(draw_height);
}

void Renderer::SetWindowSize(int width, int height)
{
  // Scale the window size by the EFB scale.
  if (g_ActiveConfig.iEFBScale != EFB_SCALE_AUTO_INTEGRAL)
    std::tie(width, height) = CalculateTargetScale(width, height);

  std::tie(width, height) = CalculateOutputDimensions(width, height);

  // Track the last values of width/height to avoid sending a window resize event every frame.
  if (width != m_last_window_request_width || height != m_last_window_request_height)
  {
    m_last_window_request_width = width;
    m_last_window_request_height = height;
    Host_RequestRenderWindowSize(width, height);
  }
}

std::tuple<int, int> Renderer::CalculateOutputDimensions(int width, int height)
{
  width = std::max(width, 1);
  height = std::max(height, 1);

  float scaled_width, scaled_height;
  std::tie(scaled_width, scaled_height) = ScaleToDisplayAspectRatio(width, height);

  if (g_ActiveConfig.bCrop)
  {
    // Force 4:3 or 16:9 by cropping the image.
    float current_aspect = scaled_width / scaled_height;
    float expected_aspect = (g_ActiveConfig.aspect_mode == AspectMode::AnalogWide ||
                             (g_ActiveConfig.aspect_mode != AspectMode::Analog && m_aspect_wide)) ?
                                (16.0f / 9.0f) :
                                (4.0f / 3.0f);
    if (current_aspect > expected_aspect)
    {
      // keep height, crop width
      scaled_width = scaled_height * expected_aspect;
    }
    else
    {
      // keep width, crop height
      scaled_height = scaled_width / expected_aspect;
    }
  }

  width = static_cast<int>(std::ceil(scaled_width));
  height = static_cast<int>(std::ceil(scaled_height));

  // UpdateDrawRectangle() makes sure that the rendered image is divisible by four for video
  // encoders, so do that here too to match it
  width -= width % 4;
  height -= height % 4;

  return std::make_tuple(width, height);
}

void Renderer::CheckFifoRecording()
{
  bool wasRecording = g_bRecordFifoData;
  g_bRecordFifoData = FifoRecorder::GetInstance().IsRecording();

  if (g_bRecordFifoData)
  {
    if (!wasRecording)
    {
      RecordVideoMemory();
    }

    FifoRecorder::GetInstance().EndFrame(CommandProcessor::fifo.CPBase,
                                         CommandProcessor::fifo.CPEnd);
  }
}

void Renderer::RecordVideoMemory()
{
  const u32* bpmem_ptr = reinterpret_cast<const u32*>(&bpmem);
  u32 cpmem[256] = {};
  // The FIFO recording format splits XF memory into xfmem and xfregs; follow
  // that split here.
  const u32* xfmem_ptr = reinterpret_cast<const u32*>(&xfmem);
  const u32* xfregs_ptr = reinterpret_cast<const u32*>(&xfmem) + FifoDataFile::XF_MEM_SIZE;
  u32 xfregs_size = sizeof(XFMemory) / 4 - FifoDataFile::XF_MEM_SIZE;

  FillCPMemoryArray(cpmem);

  FifoRecorder::GetInstance().SetVideoMemory(bpmem_ptr, cpmem, xfmem_ptr, xfregs_ptr, xfregs_size,
                                             texMem);
}

void Renderer::Swap(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc,
                    u64 ticks)
{
  // Heuristic to detect if a GameCube game is in 16:9 anamorphic widescreen mode.
  if (!SConfig::GetInstance().bWii)
  {
    size_t flush_count_4_3, flush_count_anamorphic;
    std::tie(flush_count_4_3, flush_count_anamorphic) =
        g_vertex_manager->ResetFlushAspectRatioCount();
    size_t flush_total = flush_count_4_3 + flush_count_anamorphic;

    // Modify the threshold based on which aspect ratio we're already using: if
    // the game's in 4:3, it probably won't switch to anamorphic, and vice-versa.
    if (m_aspect_wide)
      m_aspect_wide = !(flush_count_4_3 > 0.75 * flush_total);
    else
      m_aspect_wide = flush_count_anamorphic > 0.75 * flush_total;
  }

  // Ensure the last frame was written to the dump.
  // This is required even if frame dumping has stopped, since the frame dump is one frame
  // behind the renderer.
  FlushFrameDump();

  bool update_frame_count = false;
  if (xfbAddr && fbWidth && fbStride && fbHeight)
  {
    constexpr int force_safe_texture_cache_hash = 0;
    // Get the current XFB from texture cache
    auto* xfb_entry = g_texture_cache->GetXFBTexture(
        xfbAddr, fbStride, fbHeight, TextureFormat::XFB, force_safe_texture_cache_hash);

    if (xfb_entry && xfb_entry->id != m_last_xfb_id)
    {
      m_last_xfb_texture = xfb_entry->texture.get();
      m_last_xfb_id = xfb_entry->id;
      m_last_xfb_ticks = ticks;

      auto xfb_rect = xfb_entry->texture->GetConfig().GetRect();
      xfb_rect.right -= EFBToScaledX(fbStride - fbWidth);

      m_last_xfb_region = xfb_rect;

      // TODO: merge more generic parts into VideoCommon
      g_renderer->SwapImpl(xfb_entry->texture.get(), xfb_rect, ticks, xfb_entry->gamma);

      m_fps_counter.Update();
      update_frame_count = true;

      if (IsFrameDumping())
        DumpCurrentFrame();
    }

    // Update our last xfb values
    m_last_xfb_width = (fbStride < 1 || fbStride > MAX_XFB_WIDTH) ? MAX_XFB_WIDTH : fbStride;
    m_last_xfb_height = (fbHeight < 1 || fbHeight > MAX_XFB_HEIGHT) ? MAX_XFB_HEIGHT : fbHeight;
  }

  frameCount++;
  GFX_DEBUGGER_PAUSE_AT(NEXT_FRAME, true);

  // Begin new frame
  // Set default viewport and scissor, for the clear to work correctly
  // New frame
  stats.ResetFrame();

  Core::Callback_VideoCopiedToXFB(update_frame_count);
}

bool Renderer::IsFrameDumping()
{
  if (m_screenshot_request.IsSet())
    return true;

  if (SConfig::GetInstance().m_DumpFrames)
    return true;

  return false;
}

void Renderer::DumpCurrentFrame()
{
  // Scale/render to frame dump texture.
  RenderFrameDump();

  // Queue a readback for the next frame.
  QueueFrameDumpReadback();
}

void Renderer::RenderFrameDump()
{
  int target_width, target_height;
  if (!g_ActiveConfig.bInternalResolutionFrameDumps && !IsHeadless())
  {
    auto target_rect = GetTargetRectangle();
    target_width = target_rect.GetWidth();
    target_height = target_rect.GetHeight();
  }
  else
  {
    std::tie(target_width, target_height) = CalculateOutputDimensions(
        m_last_xfb_texture->GetConfig().width, m_last_xfb_texture->GetConfig().height);
  }

  // Ensure framebuffer exists (we lazily allocate it in case frame dumping isn't used).
  // Or, resize texture if it isn't large enough to accommodate the current frame.
  if (!m_frame_dump_render_texture ||
      m_frame_dump_render_texture->GetConfig().width != static_cast<u32>(target_width) ||
      m_frame_dump_render_texture->GetConfig().height == static_cast<u32>(target_height))
  {
    // Recreate texture objects. Release before creating so we don't temporarily use twice the RAM.
    TextureConfig config(target_width, target_height, 1, 1, AbstractTextureFormat::RGBA8, true);
    m_frame_dump_render_texture.reset();
    m_frame_dump_render_texture = CreateTexture(config);
    _assert_(m_frame_dump_render_texture);
  }

  // Scaling is likely to occur here, but if possible, do a bit-for-bit copy.
  if (m_last_xfb_region.GetWidth() != target_width ||
      m_last_xfb_region.GetHeight() != target_height)
  {
    m_frame_dump_render_texture->ScaleRectangleFromTexture(
        m_last_xfb_texture, m_last_xfb_region, EFBRectangle{0, 0, target_width, target_height});
  }
  else
  {
    m_frame_dump_render_texture->CopyRectangleFromTexture(
        m_last_xfb_texture, m_last_xfb_region, 0, 0,
        EFBRectangle{0, 0, target_width, target_height}, 0, 0);
  }
}

void Renderer::QueueFrameDumpReadback()
{
  // Index 0 was just sent to AVI dump. Swap with the second texture.
  if (m_frame_dump_readback_textures[0])
    std::swap(m_frame_dump_readback_textures[0], m_frame_dump_readback_textures[1]);

  std::unique_ptr<AbstractStagingTexture>& rbtex = m_frame_dump_readback_textures[0];
  if (!rbtex || rbtex->GetConfig() != m_frame_dump_render_texture->GetConfig())
  {
    rbtex = CreateStagingTexture(StagingTextureType::Readback,
                                 m_frame_dump_render_texture->GetConfig());
  }

  m_last_frame_state = AVIDump::FetchState(m_last_xfb_ticks);
  m_last_frame_exported = true;
  rbtex->CopyFromTexture(m_frame_dump_render_texture.get(), 0, 0);
}

void Renderer::FlushFrameDump()
{
  if (!m_last_frame_exported)
    return;

  // Ensure the previously-queued frame was encoded.
  FinishFrameData();

  // Queue encoding of the last frame dumped.
  std::unique_ptr<AbstractStagingTexture>& rbtex = m_frame_dump_readback_textures[0];
  rbtex->Flush();
  if (rbtex->Map())
  {
    DumpFrameData(reinterpret_cast<u8*>(rbtex->GetMappedPointer()), rbtex->GetConfig().width,
                  rbtex->GetConfig().height, static_cast<int>(rbtex->GetMappedStride()),
                  m_last_frame_state);
    rbtex->Unmap();
  }

  m_last_frame_exported = false;

  // Shutdown frame dumping if it is no longer active.
  if (!IsFrameDumping())
    ShutdownFrameDumping();
}

void Renderer::ShutdownFrameDumping()
{
  // Ensure the last queued readback has been sent to the encoder.
  FlushFrameDump();

  if (!m_frame_dump_thread_running.IsSet())
    return;

  // Ensure previous frame has been encoded.
  FinishFrameData();

  // Wake thread up, and wait for it to exit.
  m_frame_dump_thread_running.Clear();
  m_frame_dump_start.Set();
  if (m_frame_dump_thread.joinable())
    m_frame_dump_thread.join();
}

void Renderer::DumpFrameData(const u8* data, int w, int h, int stride, const AVIDump::Frame& state)
{
  m_frame_dump_config = FrameDumpConfig{data, w, h, stride, state};

  if (!m_frame_dump_thread_running.IsSet())
  {
    if (m_frame_dump_thread.joinable())
      m_frame_dump_thread.join();
    m_frame_dump_thread_running.Set();
    m_frame_dump_thread = std::thread(&Renderer::RunFrameDumps, this);
  }

  // Wake worker thread up.
  m_frame_dump_start.Set();
  m_frame_dump_frame_running = true;
}

void Renderer::FinishFrameData()
{
  if (!m_frame_dump_frame_running)
    return;

  m_frame_dump_done.Wait();
  m_frame_dump_frame_running = false;
}

void Renderer::RunFrameDumps()
{
  Common::SetCurrentThreadName("FrameDumping");
  bool dump_to_avi = !g_ActiveConfig.bDumpFramesAsImages;
  bool frame_dump_started = false;

// If Dolphin was compiled without libav, we only support dumping to images.
#if !defined(HAVE_FFMPEG)
  if (dump_to_avi)
  {
    WARN_LOG(VIDEO, "AVI frame dump requested, but Dolphin was compiled without libav. "
                    "Frame dump will be saved as images instead.");
    dump_to_avi = false;
  }
#endif

  while (true)
  {
    m_frame_dump_start.Wait();
    if (!m_frame_dump_thread_running.IsSet())
      break;

    auto config = m_frame_dump_config;

    // Save screenshot
    if (m_screenshot_request.TestAndClear())
    {
      std::lock_guard<std::mutex> lk(m_screenshot_lock);

      if (TextureToPng(config.data, config.stride, m_screenshot_name, config.width, config.height,
                       false))
        OSD::AddMessage("Screenshot saved to " + m_screenshot_name);

      // Reset settings
      m_screenshot_name.clear();
      m_screenshot_completed.Set();
    }

    if (SConfig::GetInstance().m_DumpFrames)
    {
      if (!frame_dump_started)
      {
        if (dump_to_avi)
          frame_dump_started = StartFrameDumpToAVI(config);
        else
          frame_dump_started = StartFrameDumpToImage(config);

        // Stop frame dumping if we fail to start.
        if (!frame_dump_started)
          SConfig::GetInstance().m_DumpFrames = false;
      }

      // If we failed to start frame dumping, don't write a frame.
      if (frame_dump_started)
      {
        if (dump_to_avi)
          DumpFrameToAVI(config);
        else
          DumpFrameToImage(config);
      }
    }

    m_frame_dump_done.Set();
  }

  if (frame_dump_started)
  {
    // No additional cleanup is needed when dumping to images.
    if (dump_to_avi)
      StopFrameDumpToAVI();
  }
}

#if defined(HAVE_FFMPEG)

bool Renderer::StartFrameDumpToAVI(const FrameDumpConfig& config)
{
  return AVIDump::Start(config.width, config.height);
}

void Renderer::DumpFrameToAVI(const FrameDumpConfig& config)
{
  AVIDump::AddFrame(config.data, config.width, config.height, config.stride, config.state);
}

void Renderer::StopFrameDumpToAVI()
{
  AVIDump::Stop();
}

#else

bool Renderer::StartFrameDumpToAVI(const FrameDumpConfig& config)
{
  return false;
}

void Renderer::DumpFrameToAVI(const FrameDumpConfig& config)
{
}

void Renderer::StopFrameDumpToAVI()
{
}

#endif  // defined(HAVE_FFMPEG)

std::string Renderer::GetFrameDumpNextImageFileName() const
{
  return StringFromFormat("%sframedump_%u.png", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(),
                          m_frame_dump_image_counter);
}

bool Renderer::StartFrameDumpToImage(const FrameDumpConfig& config)
{
  m_frame_dump_image_counter = 1;
  if (!SConfig::GetInstance().m_DumpFramesSilent)
  {
    // Only check for the presence of the first image to confirm overwriting.
    // A previous run will always have at least one image, and it's safe to assume that if the user
    // has allowed the first image to be overwritten, this will apply any remaining images as well.
    std::string filename = GetFrameDumpNextImageFileName();
    if (File::Exists(filename))
    {
      if (!AskYesNoT("Frame dump image(s) '%s' already exists. Overwrite?", filename.c_str()))
        return false;
    }
  }

  return true;
}

void Renderer::DumpFrameToImage(const FrameDumpConfig& config)
{
  std::string filename = GetFrameDumpNextImageFileName();
  TextureToPng(config.data, config.stride, filename, config.width, config.height, false);
  m_frame_dump_image_counter++;
}

bool Renderer::UseVertexDepthRange() const
{
  // We can't compute the depth range in the vertex shader if we don't support depth clamp.
  if (!g_ActiveConfig.backend_info.bSupportsDepthClamp)
    return false;

  // We need a full depth range if a ztexture is used.
  if (bpmem.ztex2.type != ZTEXTURE_DISABLE && !bpmem.zcontrol.early_ztest)
    return true;

  // If an inverted depth range is unsupported, we also need to check if the range is inverted.
  if (!g_ActiveConfig.backend_info.bSupportsReversedDepthRange && xfmem.viewport.zRange < 0.0f)
    return true;

  // If an oversized depth range or a ztexture is used, we need to calculate the depth range
  // in the vertex shader.
  return fabs(xfmem.viewport.zRange) > 16777215.0f || fabs(xfmem.viewport.farZ) > 16777215.0f;
}
