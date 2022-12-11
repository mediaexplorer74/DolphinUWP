// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/OGL/Render.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Common/Atomic.h"
#include "Common/CommonTypes.h"
#include "Common/GL/GLInterfaceBase.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/LogManager.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"

#include "VideoBackends/OGL/BoundingBox.h"
#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoBackends/OGL/PostProcessing.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/RasterFont.h"
#include "VideoBackends/OGL/SamplerCache.h"
#include "VideoBackends/OGL/TextureCache.h"
#include "VideoBackends/OGL/VertexManager.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

void VideoConfig::UpdateProjectionHack()
{
  ::UpdateProjectionHack(g_Config.phack);
}

namespace OGL
{
VideoConfig g_ogl_config;

// Declarations and definitions
// ----------------------------
static std::unique_ptr<RasterFont> s_raster_font;

// 1 for no MSAA. Use s_MSAASamples > 1 to check for MSAA.
static int s_MSAASamples = 1;
static u32 s_last_multisamples = 1;
static bool s_last_stereo_mode = false;

static bool s_vsync;

// EFB cache related
static const u32 EFB_CACHE_RECT_SIZE = 64;  // Cache 64x64 blocks.
static const u32 EFB_CACHE_WIDTH =
    (EFB_WIDTH + EFB_CACHE_RECT_SIZE - 1) / EFB_CACHE_RECT_SIZE;  // round up
static const u32 EFB_CACHE_HEIGHT = (EFB_HEIGHT + EFB_CACHE_RECT_SIZE - 1) / EFB_CACHE_RECT_SIZE;
static bool s_efbCacheValid[2][EFB_CACHE_WIDTH * EFB_CACHE_HEIGHT];
static bool s_efbCacheIsCleared = false;
static std::vector<u32>
    s_efbCache[2][EFB_CACHE_WIDTH * EFB_CACHE_HEIGHT];  // 2 for PeekZ and PeekColor

static void APIENTRY ErrorCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                   GLsizei length, const char* message, const void* userParam)
{
  const char* s_source;
  const char* s_type;

  // Performance - DualCore driver performance warning:
  // DualCore application thread syncing with server thread
  if (id == 0x200b0)
    return;

  switch (source)
  {
  case GL_DEBUG_SOURCE_API_ARB:
    s_source = "API";
    break;
  case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
    s_source = "Window System";
    break;
  case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
    s_source = "Shader Compiler";
    break;
  case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
    s_source = "Third Party";
    break;
  case GL_DEBUG_SOURCE_APPLICATION_ARB:
    s_source = "Application";
    break;
  case GL_DEBUG_SOURCE_OTHER_ARB:
    s_source = "Other";
    break;
  default:
    s_source = "Unknown";
    break;
  }
  switch (type)
  {
  case GL_DEBUG_TYPE_ERROR_ARB:
    s_type = "Error";
    break;
  case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB:
    s_type = "Deprecated";
    break;
  case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:
    s_type = "Undefined";
    break;
  case GL_DEBUG_TYPE_PORTABILITY_ARB:
    s_type = "Portability";
    break;
  case GL_DEBUG_TYPE_PERFORMANCE_ARB:
    s_type = "Performance";
    break;
  case GL_DEBUG_TYPE_OTHER_ARB:
    s_type = "Other";
    break;
  default:
    s_type = "Unknown";
    break;
  }
  switch (severity)
  {
  case GL_DEBUG_SEVERITY_HIGH_ARB:
    ERROR_LOG(HOST_GPU, "id: %x, source: %s, type: %s - %s", id, s_source, s_type, message);
    break;
  case GL_DEBUG_SEVERITY_MEDIUM_ARB:
    WARN_LOG(HOST_GPU, "id: %x, source: %s, type: %s - %s", id, s_source, s_type, message);
    break;
  case GL_DEBUG_SEVERITY_LOW_ARB:
    DEBUG_LOG(HOST_GPU, "id: %x, source: %s, type: %s - %s", id, s_source, s_type, message);
    break;
  case GL_DEBUG_SEVERITY_NOTIFICATION:
    DEBUG_LOG(HOST_GPU, "id: %x, source: %s, type: %s - %s", id, s_source, s_type, message);
    break;
  default:
    ERROR_LOG(HOST_GPU, "id: %x, source: %s, type: %s - %s", id, s_source, s_type, message);
    break;
  }
}

// Two small Fallbacks to avoid GL_ARB_ES2_compatibility
static void APIENTRY DepthRangef(GLfloat neardepth, GLfloat fardepth)
{
  glDepthRange(neardepth, fardepth);
}
static void APIENTRY ClearDepthf(GLfloat depthval)
{
  glClearDepth(depthval);
}

static void InitDriverInfo()
{
  std::string svendor = std::string(g_ogl_config.gl_vendor);
  std::string srenderer = std::string(g_ogl_config.gl_renderer);
  std::string sversion = std::string(g_ogl_config.gl_version);
  DriverDetails::Vendor vendor = DriverDetails::VENDOR_UNKNOWN;
  DriverDetails::Driver driver = DriverDetails::DRIVER_UNKNOWN;
  DriverDetails::Family family = DriverDetails::Family::UNKNOWN;
  double version = 0.0;

  // Get the vendor first
  if (svendor == "NVIDIA Corporation" && srenderer != "NVIDIA Tegra")
  {
    vendor = DriverDetails::VENDOR_NVIDIA;
  }
  else if (svendor == "ATI Technologies Inc." || svendor == "Advanced Micro Devices, Inc.")
  {
    vendor = DriverDetails::VENDOR_ATI;
  }
  else if (std::string::npos != sversion.find("Mesa"))
  {
    vendor = DriverDetails::VENDOR_MESA;
  }
  else if (std::string::npos != svendor.find("Intel"))
  {
    vendor = DriverDetails::VENDOR_INTEL;
  }
  else if (svendor == "ARM")
  {
    vendor = DriverDetails::VENDOR_ARM;
  }
  else if (svendor == "http://limadriver.org/")
  {
    vendor = DriverDetails::VENDOR_ARM;
    driver = DriverDetails::DRIVER_LIMA;
  }
  else if (svendor == "Qualcomm")
  {
    vendor = DriverDetails::VENDOR_QUALCOMM;
  }
  else if (svendor == "Imagination Technologies")
  {
    vendor = DriverDetails::VENDOR_IMGTEC;
  }
  else if (svendor == "NVIDIA Corporation" && srenderer == "NVIDIA Tegra")
  {
    vendor = DriverDetails::VENDOR_TEGRA;
  }
  else if (svendor == "Vivante Corporation")
  {
    vendor = DriverDetails::VENDOR_VIVANTE;
  }

  // Get device family and driver version...if we care about it
  switch (vendor)
  {
  case DriverDetails::VENDOR_QUALCOMM:
  {
    driver = DriverDetails::DRIVER_QUALCOMM;
    double glVersion;
    sscanf(g_ogl_config.gl_version, "OpenGL ES %lg V@%lg", &glVersion, &version);
  }
  break;
  case DriverDetails::VENDOR_ARM:
    // Currently the Mali-T line has two families in it.
    // Mali-T6xx and Mali-T7xx
    // These two families are similar enough that they share bugs in their drivers.
    //
    // Mali drivers provide no way to explicitly find out what video driver is running.
    // This is similar to how we can't find the Nvidia driver version in Windows.
    // Good thing is that ARM introduces a new video driver about once every two years so we can
    // find the driver version by the features it exposes.
    // r2p0 - No OpenGL ES 3.0 support (We don't support this)
    // r3p0 - OpenGL ES 3.0 support
    // r4p0 - Supports 'GL_EXT_shader_pixel_local_storage' extension.

    driver = DriverDetails::DRIVER_ARM;
    if (GLExtensions::Supports("GL_EXT_shader_pixel_local_storage"))
      version = 400;
    else
      version = 300;
    break;
  case DriverDetails::VENDOR_MESA:
  {
    if (svendor == "nouveau")
    {
      driver = DriverDetails::DRIVER_NOUVEAU;
    }
    else if (svendor == "Intel Open Source Technology Center")
    {
      driver = DriverDetails::DRIVER_I965;
      if (srenderer.find("Sandybridge") != std::string::npos)
        family = DriverDetails::Family::INTEL_SANDY;
      else if (srenderer.find("Ivybridge") != std::string::npos)
        family = DriverDetails::Family::INTEL_IVY;
    }
    else if (std::string::npos != srenderer.find("AMD") ||
             std::string::npos != srenderer.find("ATI"))
    {
      driver = DriverDetails::DRIVER_R600;
    }

    int major = 0;
    int minor = 0;
    int release = 0;
    sscanf(g_ogl_config.gl_version, "%*s (Core Profile) Mesa %d.%d.%d", &major, &minor, &release);
    version = 100 * major + 10 * minor + release;
  }
  break;
  case DriverDetails::VENDOR_INTEL:  // Happens in OS X/Windows
  {
    u32 market_name;
    sscanf(g_ogl_config.gl_renderer, "Intel HD Graphics %d", &market_name);
    switch (market_name)
    {
    case 2000:
    case 3000:
      family = DriverDetails::Family::INTEL_SANDY;
      break;
    case 2500:
    case 4000:
      family = DriverDetails::Family::INTEL_IVY;
      break;
    default:
      family = DriverDetails::Family::UNKNOWN;
      break;
    };
#ifdef _WIN32
    int glmajor = 0;
    int glminor = 0;
    int major = 0;
    int minor = 0;
    int release = 0;
    int revision = 0;
    // Example version string: '4.3.0 - Build 10.18.10.3907'
    sscanf(g_ogl_config.gl_version, "%d.%d.0 - Build %d.%d.%d.%d", &glmajor, &glminor, &major,
           &minor, &release, &revision);
    version = 100000000 * major + 1000000 * minor + 10000 * release + revision;
    version /= 10000;
#endif
  }
  break;
  case DriverDetails::VENDOR_NVIDIA:
  {
    int glmajor = 0;
    int glminor = 0;
    int glrelease = 0;
    int major = 0;
    int minor = 0;
    // TODO: this is known to be broken on Windows
    // Nvidia seems to have removed their driver version from this string, so we can't get it.
    // hopefully we'll never have to workaround Nvidia bugs
    sscanf(g_ogl_config.gl_version, "%d.%d.%d NVIDIA %d.%d", &glmajor, &glminor, &glrelease, &major,
           &minor);
    version = 100 * major + minor;
  }
  break;
  case DriverDetails::VENDOR_IMGTEC:
  {
    // Example version string:
    // "OpenGL ES 3.2 build 1.9@4850625"
    // Ends up as "109.4850625" - "1.9" being the branch, "4850625" being the build's change ID
    // The change ID only makes sense to compare within a branch
    driver = DriverDetails::DRIVER_IMGTEC;
    double gl_version;
    int major, minor, change;
    constexpr double change_scale = 10000000;
    sscanf(g_ogl_config.gl_version, "OpenGL ES %lg build %d.%d@%d", &gl_version, &major, &minor,
           &change);
    version = 100 * major + minor;
    if (change >= change_scale)
    {
      ERROR_LOG(VIDEO, "Version changeID overflow - change:%d scale:%f", change, change_scale);
    }
    else
    {
      version += static_cast<double>(change) / change_scale;
    }
  }
  break;
  // We don't care about these
  default:
    break;
  }
  DriverDetails::Init(DriverDetails::API_OPENGL, vendor, driver, version, family);
}

// Init functions
Renderer::Renderer()
    : ::Renderer(static_cast<int>(std::max(GLInterface->GetBackBufferWidth(), 1u)),
                 static_cast<int>(std::max(GLInterface->GetBackBufferHeight(), 1u)))
{
  bool bSuccess = true;

  g_ogl_config.gl_vendor = (const char*)glGetString(GL_VENDOR);
  g_ogl_config.gl_renderer = (const char*)glGetString(GL_RENDERER);
  g_ogl_config.gl_version = (const char*)glGetString(GL_VERSION);

  InitDriverInfo();

  if (GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGL)
  {
    if (!GLExtensions::Supports("GL_ARB_framebuffer_object"))
    {
      // We want the ogl3 framebuffer instead of the ogl2 one for better blitting support.
      // It's also compatible with the gles3 one.
      PanicAlert("GPU: ERROR: Need GL_ARB_framebuffer_object for multiple render targets.\n"
                 "GPU: Does your video card support OpenGL 3.0?");
      bSuccess = false;
    }

    if (!GLExtensions::Supports("GL_ARB_vertex_array_object"))
    {
      // This extension is used to replace lots of pointer setting function.
      // Also gles3 requires to use it.
      PanicAlert("GPU: OGL ERROR: Need GL_ARB_vertex_array_object.\n"
                 "GPU: Does your video card support OpenGL 3.0?");
      bSuccess = false;
    }

    if (!GLExtensions::Supports("GL_ARB_map_buffer_range"))
    {
      // ogl3 buffer mapping for better streaming support.
      // The ogl2 one also isn't in gles3.
      PanicAlert("GPU: OGL ERROR: Need GL_ARB_map_buffer_range.\n"
                 "GPU: Does your video card support OpenGL 3.0?");
      bSuccess = false;
    }

    if (!GLExtensions::Supports("GL_ARB_uniform_buffer_object"))
    {
      // ubo allow us to keep the current constants on shader switches
      // we also can stream them much nicer and pack into it whatever we want to
      PanicAlert("GPU: OGL ERROR: Need GL_ARB_uniform_buffer_object.\n"
                 "GPU: Does your video card support OpenGL 3.1?");
      bSuccess = false;
    }
    else if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_UBO))
    {
      PanicAlert(
          "Buggy GPU driver detected.\n"
          "Please either install the closed-source GPU driver or update your Mesa 3D version.");
      bSuccess = false;
    }

    if (!GLExtensions::Supports("GL_ARB_sampler_objects"))
    {
      // Our sampler cache uses this extension. It could easyly be workaround and it's by far the
      // highest requirement, but it seems that no driver lacks support for it.
      PanicAlert("GPU: OGL ERROR: Need GL_ARB_sampler_objects.\n"
                 "GPU: Does your video card support OpenGL 3.3?");
      bSuccess = false;
    }

    // OpenGL 3 doesn't provide GLES like float functions for depth.
    // They are in core in OpenGL 4.1, so almost every driver should support them.
    // But for the oldest ones, we provide fallbacks to the old double functions.
    if (!GLExtensions::Supports("GL_ARB_ES2_compatibility"))
    {
      glDepthRangef = DepthRangef;
      glClearDepthf = ClearDepthf;
    }
  }

  // Copy the GPU name to g_Config, so Analytics can see it.
  g_Config.backend_info.AdapterName = g_ogl_config.gl_renderer;

  g_Config.backend_info.bSupportsDualSourceBlend =
      (GLExtensions::Supports("GL_ARB_blend_func_extended") ||
       GLExtensions::Supports("GL_EXT_blend_func_extended"));
  g_Config.backend_info.bSupportsPrimitiveRestart =
      !DriverDetails::HasBug(DriverDetails::BUG_PRIMITIVE_RESTART) &&
      ((GLExtensions::Version() >= 310) || GLExtensions::Supports("GL_NV_primitive_restart"));
  g_Config.backend_info.bSupportsBBox = true;
  g_Config.backend_info.bSupportsFragmentStoresAndAtomics =
      GLExtensions::Supports("GL_ARB_shader_storage_buffer_object");
  g_Config.backend_info.bSupportsGSInstancing = GLExtensions::Supports("GL_ARB_gpu_shader5");
  g_Config.backend_info.bSupportsSSAA = GLExtensions::Supports("GL_ARB_gpu_shader5") &&
                                        GLExtensions::Supports("GL_ARB_sample_shading");
  g_Config.backend_info.bSupportsGeometryShaders =
      GLExtensions::Version() >= 320 &&
      !DriverDetails::HasBug(DriverDetails::BUG_BROKEN_GEOMETRY_SHADERS);
  g_Config.backend_info.bSupportsPaletteConversion =
      GLExtensions::Supports("GL_ARB_texture_buffer_object") ||
      GLExtensions::Supports("GL_OES_texture_buffer") ||
      GLExtensions::Supports("GL_EXT_texture_buffer");
  g_Config.backend_info.bSupportsClipControl = GLExtensions::Supports("GL_ARB_clip_control");
  g_ogl_config.bSupportsCopySubImage =
      (GLExtensions::Supports("GL_ARB_copy_image") || GLExtensions::Supports("GL_NV_copy_image") ||
       GLExtensions::Supports("GL_EXT_copy_image") ||
       GLExtensions::Supports("GL_OES_copy_image")) &&
      !DriverDetails::HasBug(DriverDetails::BUG_BROKEN_COPYIMAGE);
  g_ogl_config.bSupportsTextureSubImage = GLExtensions::Supports("ARB_get_texture_sub_image");

  // Desktop OpenGL supports the binding layout if it supports 420pack
  // OpenGL ES 3.1 supports it implicitly without an extension
  g_Config.backend_info.bSupportsBindingLayout =
      GLExtensions::Supports("GL_ARB_shading_language_420pack");

  // Clip distance support is useless without a method to clamp the depth range
  g_Config.backend_info.bSupportsDepthClamp = GLExtensions::Supports("GL_ARB_depth_clamp");

  // Desktop OpenGL supports bitfield manulipation and dynamic sampler indexing if it supports
  // shader5. OpenGL ES 3.1 supports it implicitly without an extension
  g_Config.backend_info.bSupportsBitfield = GLExtensions::Supports("GL_ARB_gpu_shader5");
  g_Config.backend_info.bSupportsDynamicSamplerIndexing =
      GLExtensions::Supports("GL_ARB_gpu_shader5");

  g_ogl_config.bSupportsGLSLCache = GLExtensions::Supports("GL_ARB_get_program_binary");
  g_ogl_config.bSupportsGLPinnedMemory = GLExtensions::Supports("GL_AMD_pinned_memory");
  g_ogl_config.bSupportsGLSync = GLExtensions::Supports("GL_ARB_sync");
  g_ogl_config.bSupportsGLBaseVertex = GLExtensions::Supports("GL_ARB_draw_elements_base_vertex") ||
                                       GLExtensions::Supports("GL_EXT_draw_elements_base_vertex") ||
                                       GLExtensions::Supports("GL_OES_draw_elements_base_vertex");
  g_ogl_config.bSupportsGLBufferStorage = GLExtensions::Supports("GL_ARB_buffer_storage") ||
                                          GLExtensions::Supports("GL_EXT_buffer_storage");
  g_ogl_config.bSupportsMSAA = GLExtensions::Supports("GL_ARB_texture_multisample");
  g_ogl_config.bSupportViewportFloat = GLExtensions::Supports("GL_ARB_viewport_array");
  g_ogl_config.bSupportsDebug =
      GLExtensions::Supports("GL_KHR_debug") || GLExtensions::Supports("GL_ARB_debug_output");
  g_ogl_config.bSupportsTextureStorage = GLExtensions::Supports("GL_ARB_texture_storage");
  g_ogl_config.bSupports3DTextureStorageMultisample =
      GLExtensions::Supports("GL_ARB_texture_storage_multisample") ||
      GLExtensions::Supports("GL_OES_texture_storage_multisample_2d_array");
  g_ogl_config.bSupports2DTextureStorageMultisample =
      GLExtensions::Supports("GL_ARB_texture_storage_multisample");
  g_ogl_config.bSupportsImageLoadStore = GLExtensions::Supports("GL_ARB_shader_image_load_store");
  g_ogl_config.bSupportsConservativeDepth = GLExtensions::Supports("GL_ARB_conservative_depth");
  g_ogl_config.bSupportsAniso = GLExtensions::Supports("GL_EXT_texture_filter_anisotropic");
  g_Config.backend_info.bSupportsComputeShaders = GLExtensions::Supports("GL_ARB_compute_shader");
  g_Config.backend_info.bSupportsST3CTextures =
      GLExtensions::Supports("GL_EXT_texture_compression_s3tc");
  g_Config.backend_info.bSupportsBPTCTextures =
      GLExtensions::Supports("GL_ARB_texture_compression_bptc");

  if (GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGLES3)
  {
    g_ogl_config.SupportedESPointSize =
        GLExtensions::Supports("GL_OES_geometry_point_size") ?
            1 :
            GLExtensions::Supports("GL_EXT_geometry_point_size") ? 2 : 0;
    g_ogl_config.SupportedESTextureBuffer = GLExtensions::Supports("VERSION_GLES_3_2") ?
                                                ES_TEXBUF_TYPE::TEXBUF_CORE :
                                                GLExtensions::Supports("GL_OES_texture_buffer") ?
                                                ES_TEXBUF_TYPE::TEXBUF_OES :
                                                GLExtensions::Supports("GL_EXT_texture_buffer") ?
                                                ES_TEXBUF_TYPE::TEXBUF_EXT :
                                                ES_TEXBUF_TYPE::TEXBUF_NONE;

    g_ogl_config.bSupportsGLSLCache = true;
    g_ogl_config.bSupportsGLSync = true;

    // TODO: Implement support for GL_EXT_clip_cull_distance when there is an extension for
    // depth clamping.
    g_Config.backend_info.bSupportsDepthClamp = false;

    if (GLExtensions::Version() == 300)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSLES_300;
      g_ogl_config.bSupportsAEP = false;
      g_ogl_config.bSupportsTextureStorage = true;
      g_Config.backend_info.bSupportsGeometryShaders = false;
    }
    else if (GLExtensions::Version() == 310)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSLES_310;
      g_ogl_config.bSupportsAEP = GLExtensions::Supports("GL_ANDROID_extension_pack_es31a");
      g_Config.backend_info.bSupportsBindingLayout = true;
      g_ogl_config.bSupportsImageLoadStore = true;
      g_Config.backend_info.bSupportsGeometryShaders = g_ogl_config.bSupportsAEP;
      g_Config.backend_info.bSupportsComputeShaders = true;
      g_Config.backend_info.bSupportsGSInstancing =
          g_Config.backend_info.bSupportsGeometryShaders && g_ogl_config.SupportedESPointSize > 0;
      g_Config.backend_info.bSupportsSSAA = g_ogl_config.bSupportsAEP;
      g_Config.backend_info.bSupportsFragmentStoresAndAtomics = true;
      g_ogl_config.bSupportsMSAA = true;
      g_ogl_config.bSupportsTextureStorage = true;
      g_ogl_config.bSupports2DTextureStorageMultisample = true;
      g_Config.backend_info.bSupportsBitfield = true;
      g_Config.backend_info.bSupportsDynamicSamplerIndexing = g_ogl_config.bSupportsAEP;
      if (g_ActiveConfig.stereo_mode != StereoMode::Off && g_ActiveConfig.iMultisamples > 1 &&
          !g_ogl_config.bSupports3DTextureStorageMultisample)
      {
        // GLES 3.1 can't support stereo rendering and MSAA
        OSD::AddMessage("MSAA Stereo rendering isn't supported by your GPU.", 10000);
        Config::SetCurrent(Config::GFX_MSAA, UINT32_C(1));
      }
    }
    else
    {
      g_ogl_config.eSupportedGLSLVersion = GLSLES_320;
      g_ogl_config.bSupportsAEP = GLExtensions::Supports("GL_ANDROID_extension_pack_es31a");
      g_Config.backend_info.bSupportsBindingLayout = true;
      g_ogl_config.bSupportsImageLoadStore = true;
      g_Config.backend_info.bSupportsGeometryShaders = true;
      g_Config.backend_info.bSupportsComputeShaders = true;
      g_Config.backend_info.bSupportsGSInstancing = g_ogl_config.SupportedESPointSize > 0;
      g_Config.backend_info.bSupportsPaletteConversion = true;
      g_Config.backend_info.bSupportsSSAA = true;
      g_Config.backend_info.bSupportsFragmentStoresAndAtomics = true;
      g_ogl_config.bSupportsCopySubImage = true;
      g_ogl_config.bSupportsGLBaseVertex = true;
      g_ogl_config.bSupportsDebug = true;
      g_ogl_config.bSupportsMSAA = true;
      g_ogl_config.bSupportsTextureStorage = true;
      g_ogl_config.bSupports2DTextureStorageMultisample = true;
      g_ogl_config.bSupports3DTextureStorageMultisample = true;
      g_Config.backend_info.bSupportsBitfield = true;
      g_Config.backend_info.bSupportsDynamicSamplerIndexing = true;
    }
  }
  else
  {
    if (GLExtensions::Version() < 300)
    {
      PanicAlert("GPU: OGL ERROR: Need at least GLSL 1.30\n"
                 "GPU: Does your video card support OpenGL 3.0?\n"
                 "GPU: Your driver supports GLSL %s",
                 (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
      bSuccess = false;
    }
    else if (GLExtensions::Version() == 300)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSL_130;
      g_ogl_config.bSupportsImageLoadStore = false;  // layout keyword is only supported on glsl150+
      g_ogl_config.bSupportsConservativeDepth =
          false;  // layout keyword is only supported on glsl150+
      g_Config.backend_info.bSupportsGeometryShaders =
          false;  // geometry shaders are only supported on glsl150+
    }
    else if (GLExtensions::Version() == 310)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSL_140;
      g_ogl_config.bSupportsImageLoadStore = false;  // layout keyword is only supported on glsl150+
      g_ogl_config.bSupportsConservativeDepth =
          false;  // layout keyword is only supported on glsl150+
      g_Config.backend_info.bSupportsGeometryShaders =
          false;  // geometry shaders are only supported on glsl150+
    }
    else if (GLExtensions::Version() == 320)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSL_150;
    }
    else if (GLExtensions::Version() == 330)
    {
      g_ogl_config.eSupportedGLSLVersion = GLSL_330;
    }
    else if (GLExtensions::Version() >= 430)
    {
      // TODO: We should really parse the GL_SHADING_LANGUAGE_VERSION token.
      g_ogl_config.eSupportedGLSLVersion = GLSL_430;
      g_ogl_config.bSupportsTextureStorage = true;
      g_ogl_config.bSupportsImageLoadStore = true;
      g_Config.backend_info.bSupportsSSAA = true;

      // Compute shaders are core in GL4.3.
      g_Config.backend_info.bSupportsComputeShaders = true;
      if (GLExtensions::Version() >= 450)
        g_ogl_config.bSupportsTextureSubImage = true;
    }
    else
    {
      g_ogl_config.eSupportedGLSLVersion = GLSL_400;
      g_Config.backend_info.bSupportsSSAA = true;

      if (GLExtensions::Version() == 420)
      {
        // Texture storage and shader image load/store are core in GL4.2.
        g_ogl_config.bSupportsTextureStorage = true;
        g_ogl_config.bSupportsImageLoadStore = true;
      }
    }

    // Desktop OpenGL can't have the Android Extension Pack
    g_ogl_config.bSupportsAEP = false;
  }

  // Either method can do early-z tests. See PixelShaderGen for details.
  g_Config.backend_info.bSupportsEarlyZ =
      g_ogl_config.bSupportsImageLoadStore || g_ogl_config.bSupportsConservativeDepth;

  glGetIntegerv(GL_MAX_SAMPLES, &g_ogl_config.max_samples);
  if (g_ogl_config.max_samples < 1 || !g_ogl_config.bSupportsMSAA)
    g_ogl_config.max_samples = 1;

  // We require texel buffers, image load store, and compute shaders to enable GPU texture decoding.
  // If the driver doesn't expose the extensions, but supports GL4.3/GLES3.1, it will still be
  // enabled in the version check below.
  g_Config.backend_info.bSupportsGPUTextureDecoding =
      g_Config.backend_info.bSupportsPaletteConversion &&
      g_Config.backend_info.bSupportsComputeShaders && g_ogl_config.bSupportsImageLoadStore;

  if (g_ogl_config.bSupportsDebug)
  {
    if (GLExtensions::Supports("GL_KHR_debug"))
    {
      glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
      glDebugMessageCallback(ErrorCallback, nullptr);
    }
    else
    {
      glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
      glDebugMessageCallbackARB(ErrorCallback, nullptr);
    }
    if (LogManager::GetInstance()->IsEnabled(LogTypes::HOST_GPU, LogTypes::LERROR))
      glEnable(GL_DEBUG_OUTPUT);
    else
      glDisable(GL_DEBUG_OUTPUT);
  }

  int samples;
  glGetIntegerv(GL_SAMPLES, &samples);
  if (samples > 1)
  {
    // MSAA on default framebuffer isn't working because of glBlitFramebuffer.
    // It also isn't useful as we don't render anything to the default framebuffer.
    // We also try to get a non-msaa fb, so this only happens when forced by the driver.
    PanicAlert("MSAA on default framebuffer isn't supported.\n"
               "Please avoid forcing Dolphin to use MSAA by the driver.\n"
               "%d samples on default framebuffer found.",
               samples);
    bSuccess = false;
  }

  if (!bSuccess)
  {
    // Not all needed extensions are supported, so we have to stop here.
    // Else some of the next calls might crash.
    return;
  }

  g_Config.VerifyValidity();
  UpdateActiveConfig();

  // Since we modify the config here, we need to update the last host bits, it may have changed.
  m_last_host_config_bits = ShaderHostConfig::GetCurrent().bits;

  OSD::AddMessage(StringFromFormat("Video Info: %s, %s, %s", g_ogl_config.gl_vendor,
                                   g_ogl_config.gl_renderer, g_ogl_config.gl_version),
                  5000);

  WARN_LOG(VIDEO, "Missing OGL Extensions: %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
           g_ActiveConfig.backend_info.bSupportsDualSourceBlend ? "" : "DualSourceBlend ",
           g_ActiveConfig.backend_info.bSupportsPrimitiveRestart ? "" : "PrimitiveRestart ",
           g_ActiveConfig.backend_info.bSupportsEarlyZ ? "" : "EarlyZ ",
           g_ogl_config.bSupportsGLPinnedMemory ? "" : "PinnedMemory ",
           g_ogl_config.bSupportsGLSLCache ? "" : "ShaderCache ",
           g_ogl_config.bSupportsGLBaseVertex ? "" : "BaseVertex ",
           g_ogl_config.bSupportsGLBufferStorage ? "" : "BufferStorage ",
           g_ogl_config.bSupportsGLSync ? "" : "Sync ", g_ogl_config.bSupportsMSAA ? "" : "MSAA ",
           g_ActiveConfig.backend_info.bSupportsSSAA ? "" : "SSAA ",
           g_ActiveConfig.backend_info.bSupportsGSInstancing ? "" : "GSInstancing ",
           g_ActiveConfig.backend_info.bSupportsClipControl ? "" : "ClipControl ",
           g_ogl_config.bSupportsCopySubImage ? "" : "CopyImageSubData ",
           g_ActiveConfig.backend_info.bSupportsDepthClamp ? "" : "DepthClamp ");

  s_last_multisamples = g_ActiveConfig.iMultisamples;
  s_MSAASamples = s_last_multisamples;

  s_last_stereo_mode = g_ActiveConfig.stereo_mode != StereoMode::Off;

  // Handle VSync on/off
  s_vsync = g_ActiveConfig.IsVSync();
  if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VSYNC))
    GLInterface->SwapInterval(s_vsync);

  // Because of the fixed framebuffer size we need to disable the resolution
  // options while running

  // The stencil is used for bounding box emulation when SSBOs are not available
  glDisable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 1, 0xFF);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

  // Reset The Current Viewport
  glViewport(0, 0, GetTargetWidth(), GetTargetHeight());
  if (g_ActiveConfig.backend_info.bSupportsClipControl)
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClearDepthf(1.0f);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  if (g_ActiveConfig.backend_info.bSupportsDepthClamp)
  {
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_CLIP_DISTANCE1);
    glEnable(GL_DEPTH_CLAMP);
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // 4-byte pixel alignment

  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, GetTargetWidth(), GetTargetHeight());
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendColor(0, 0, 0, 0.5f);
  glClearDepthf(1.0f);

  if (g_ActiveConfig.backend_info.bSupportsPrimitiveRestart)
  {
    if (GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGLES3)
    {
      glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    else
    {
      if (GLExtensions::Version() >= 310)
      {
        glEnable(GL_PRIMITIVE_RESTART);
        glPrimitiveRestartIndex(65535);
      }
      else
      {
        glEnableClientState(GL_PRIMITIVE_RESTART_NV);
        glPrimitiveRestartIndexNV(65535);
      }
    }
  }
  IndexGenerator::Init();

  UpdateActiveConfig();
  ClearEFBCache();
}

Renderer::~Renderer() = default;

void Renderer::Shutdown()
{
  g_framebuffer_manager.reset();

  UpdateActiveConfig();

  s_raster_font.reset();
  m_post_processor.reset();

  OpenGL_DeleteAttributelessVAO();
}

void Renderer::Init()
{
  // Initialize the FramebufferManager
  g_framebuffer_manager = std::make_unique<FramebufferManager>(
      m_target_width, m_target_height, s_MSAASamples, BoundingBox::NeedsStencilBuffer());

  m_post_processor = std::make_unique<OpenGLPostProcessing>();
  s_raster_font = std::make_unique<RasterFont>();

  OpenGL_CreateAttributelessVAO();
}

std::unique_ptr<AbstractTexture> Renderer::CreateTexture(const TextureConfig& config)
{
  return std::make_unique<OGLTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> Renderer::CreateStagingTexture(StagingTextureType type,
                                                                       const TextureConfig& config)
{
  return OGLStagingTexture::Create(type, config);
}

void Renderer::RenderText(const std::string& text, int left, int top, u32 color)
{
  u32 backbuffer_width = std::max(GLInterface->GetBackBufferWidth(), 1u);
  u32 backbuffer_height = std::max(GLInterface->GetBackBufferHeight(), 1u);

  s_raster_font->printMultilineText(text, left * 2.0f / static_cast<float>(backbuffer_width) - 1.0f,
                                    1.0f - top * 2.0f / static_cast<float>(backbuffer_height), 0,
                                    backbuffer_width, backbuffer_height, color);
}

TargetRectangle Renderer::ConvertEFBRectangle(const EFBRectangle& rc)
{
  TargetRectangle result;
  result.left = EFBToScaledX(rc.left);
  result.top = EFBToScaledY(EFB_HEIGHT - rc.top);
  result.right = EFBToScaledX(rc.right);
  result.bottom = EFBToScaledY(EFB_HEIGHT - rc.bottom);
  return result;
}

// Function: This function handles the OpenGL glScissor() function
// ----------------------------
// Call browser: OpcodeDecoding.cpp ExecuteDisplayList > Decode() > LoadBPReg()
//		case 0x52 > SetScissorRect()
// ----------------------------
// bpmem.scissorTL.x, y = 342x342
// bpmem.scissorBR.x, y = 981x821
// Renderer::GetTargetHeight() = the fixed ini file setting
// donkopunchstania - it appears scissorBR is the bottom right pixel inside the scissor box
// therefore the width and height are (scissorBR + 1) - scissorTL
void Renderer::SetScissorRect(const EFBRectangle& rc)
{
  TargetRectangle trc = ConvertEFBRectangle(rc);
  glScissor(trc.left, trc.bottom, trc.GetWidth(), trc.GetHeight());
}

void ClearEFBCache()
{
  if (!s_efbCacheIsCleared)
  {
    s_efbCacheIsCleared = true;
    memset(s_efbCacheValid, 0, sizeof(s_efbCacheValid));
  }
}

void Renderer::UpdateEFBCache(EFBAccessType type, u32 cacheRectIdx, const EFBRectangle& efbPixelRc,
                              const TargetRectangle& targetPixelRc, const void* data)
{
  const u32 cacheType = (type == EFBAccessType::PeekZ ? 0 : 1);

  if (!s_efbCache[cacheType][cacheRectIdx].size())
    s_efbCache[cacheType][cacheRectIdx].resize(EFB_CACHE_RECT_SIZE * EFB_CACHE_RECT_SIZE);

  u32 targetPixelRcWidth = targetPixelRc.right - targetPixelRc.left;
  u32 efbPixelRcHeight = efbPixelRc.bottom - efbPixelRc.top;
  u32 efbPixelRcWidth = efbPixelRc.right - efbPixelRc.left;

  for (u32 yCache = 0; yCache < efbPixelRcHeight; ++yCache)
  {
    u32 yEFB = efbPixelRc.top + yCache;
    u32 yPixel = (EFBToScaledY(EFB_HEIGHT - yEFB) + EFBToScaledY(EFB_HEIGHT - yEFB - 1)) / 2;
    u32 yData = yPixel - targetPixelRc.bottom;

    for (u32 xCache = 0; xCache < efbPixelRcWidth; ++xCache)
    {
      u32 xEFB = efbPixelRc.left + xCache;
      u32 xPixel = (EFBToScaledX(xEFB) + EFBToScaledX(xEFB + 1)) / 2;
      u32 xData = xPixel - targetPixelRc.left;
      u32 value;
      if (type == EFBAccessType::PeekZ)
      {
        float* ptr = (float*)data;
        value = MathUtil::Clamp<u32>((u32)(ptr[yData * targetPixelRcWidth + xData] * 16777216.0f),
                                     0, 0xFFFFFF);
      }
      else
      {
        u32* ptr = (u32*)data;
        value = ptr[yData * targetPixelRcWidth + xData];
      }
      s_efbCache[cacheType][cacheRectIdx][yCache * EFB_CACHE_RECT_SIZE + xCache] = value;
    }
  }

  s_efbCacheValid[cacheType][cacheRectIdx] = true;
  s_efbCacheIsCleared = false;
}

// This function allows the CPU to directly access the EFB.
// There are EFB peeks (which will read the color or depth of a pixel)
// and EFB pokes (which will change the color or depth of a pixel).
//
// The behavior of EFB peeks can only be modified by:
// - GX_PokeAlphaRead
// The behavior of EFB pokes can be modified by:
// - GX_PokeAlphaMode (TODO)
// - GX_PokeAlphaUpdate (TODO)
// - GX_PokeBlendMode (TODO)
// - GX_PokeColorUpdate (TODO)
// - GX_PokeDither (TODO)
// - GX_PokeDstAlpha (TODO)
// - GX_PokeZMode (TODO)
u32 Renderer::AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data)
{
  u32 cacheRectIdx = (y / EFB_CACHE_RECT_SIZE) * EFB_CACHE_WIDTH + (x / EFB_CACHE_RECT_SIZE);

  EFBRectangle efbPixelRc;

  if (type == EFBAccessType::PeekColor || type == EFBAccessType::PeekZ)
  {
    // Get the rectangular target region containing the EFB pixel
    efbPixelRc.left = (x / EFB_CACHE_RECT_SIZE) * EFB_CACHE_RECT_SIZE;
    efbPixelRc.top = (y / EFB_CACHE_RECT_SIZE) * EFB_CACHE_RECT_SIZE;
    efbPixelRc.right = std::min(efbPixelRc.left + EFB_CACHE_RECT_SIZE, (u32)EFB_WIDTH);
    efbPixelRc.bottom = std::min(efbPixelRc.top + EFB_CACHE_RECT_SIZE, (u32)EFB_HEIGHT);
  }
  else
  {
    efbPixelRc.left = x;
    efbPixelRc.top = y;
    efbPixelRc.right = x + 1;
    efbPixelRc.bottom = y + 1;
  }

  TargetRectangle targetPixelRc = ConvertEFBRectangle(efbPixelRc);
  u32 targetPixelRcWidth = targetPixelRc.right - targetPixelRc.left;
  u32 targetPixelRcHeight = targetPixelRc.top - targetPixelRc.bottom;

  // TODO (FIX) : currently, AA path is broken/offset and doesn't return the correct pixel
  switch (type)
  {
  case EFBAccessType::PeekZ:
  {
    if (!s_efbCacheValid[0][cacheRectIdx])
    {
      if (s_MSAASamples > 1)
      {
        ResetAPIState();

        // Resolve our rectangle.
        FramebufferManager::GetEFBDepthTexture(efbPixelRc);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferManager::GetResolvedFramebuffer());

        RestoreAPIState();
      }

      std::unique_ptr<float[]> depthMap(new float[targetPixelRcWidth * targetPixelRcHeight]);

      glReadPixels(targetPixelRc.left, targetPixelRc.bottom, targetPixelRcWidth,
                   targetPixelRcHeight, GL_DEPTH_COMPONENT, GL_FLOAT, depthMap.get());

      UpdateEFBCache(type, cacheRectIdx, efbPixelRc, targetPixelRc, depthMap.get());
    }

    u32 xRect = x % EFB_CACHE_RECT_SIZE;
    u32 yRect = y % EFB_CACHE_RECT_SIZE;
    u32 z = s_efbCache[0][cacheRectIdx][yRect * EFB_CACHE_RECT_SIZE + xRect];

    // if Z is in 16 bit format you must return a 16 bit integer
    if (bpmem.zcontrol.pixel_format == PEControl::RGB565_Z16)
      z = z >> 8;

    return z;
  }

  case EFBAccessType::PeekColor:  // GXPeekARGB
  {
    // Although it may sound strange, this really is A8R8G8B8 and not RGBA or 24-bit...

    // Tested in Killer 7, the first 8bits represent the alpha value which is used to
    // determine if we're aiming at an enemy (0x80 / 0x88) or not (0x70)
    // Wind Waker is also using it for the pictograph to determine the color of each pixel
    if (!s_efbCacheValid[1][cacheRectIdx])
    {
      if (s_MSAASamples > 1)
      {
        ResetAPIState();

        // Resolve our rectangle.
        FramebufferManager::GetEFBColorTexture(efbPixelRc);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferManager::GetResolvedFramebuffer());

        RestoreAPIState();
      }

      std::unique_ptr<u32[]> colorMap(new u32[targetPixelRcWidth * targetPixelRcHeight]);

      if (GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGLES3)
        // XXX: Swap colours
        glReadPixels(targetPixelRc.left, targetPixelRc.bottom, targetPixelRcWidth,
                     targetPixelRcHeight, GL_RGBA, GL_UNSIGNED_BYTE, colorMap.get());
      else
        glReadPixels(targetPixelRc.left, targetPixelRc.bottom, targetPixelRcWidth,
                     targetPixelRcHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, colorMap.get());

      UpdateEFBCache(type, cacheRectIdx, efbPixelRc, targetPixelRc, colorMap.get());
    }

    u32 xRect = x % EFB_CACHE_RECT_SIZE;
    u32 yRect = y % EFB_CACHE_RECT_SIZE;
    u32 color = s_efbCache[1][cacheRectIdx][yRect * EFB_CACHE_RECT_SIZE + xRect];

    // check what to do with the alpha channel (GX_PokeAlphaRead)
    PixelEngine::UPEAlphaReadReg alpha_read_mode = PixelEngine::GetAlphaReadMode();

    if (bpmem.zcontrol.pixel_format == PEControl::RGBA6_Z24)
    {
      color = RGBA8ToRGBA6ToRGBA8(color);
    }
    else if (bpmem.zcontrol.pixel_format == PEControl::RGB565_Z16)
    {
      color = RGBA8ToRGB565ToRGBA8(color);
    }
    if (bpmem.zcontrol.pixel_format != PEControl::RGBA6_Z24)
    {
      color |= 0xFF000000;
    }
    if (alpha_read_mode.ReadMode == 2)
    {
      // GX_READ_NONE
      return color;
    }
    else if (alpha_read_mode.ReadMode == 1)
    {
      // GX_READ_FF
      return (color | 0xFF000000);
    }
    else /*if(alpha_read_mode.ReadMode == 0)*/
    {
      // GX_READ_00
      return (color & 0x00FFFFFF);
    }
  }

  default:
    break;
  }

  return 0;
}

void Renderer::PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points)
{
  FramebufferManager::PokeEFB(type, points, num_points);
}

u16 Renderer::BBoxRead(int index)
{
  int swapped_index = index;
  if (index >= 2)
    swapped_index ^= 1;  // swap 2 and 3 for top/bottom

  // Here we get the min/max value of the truncated position of the upscaled and swapped
  // framebuffer.
  // So we have to correct them to the unscaled EFB sizes.
  int value = BoundingBox::Get(swapped_index);

  if (index < 2)
  {
    // left/right
    value = value * EFB_WIDTH / m_target_width;
  }
  else
  {
    // up/down -- we have to swap up and down
    value = value * EFB_HEIGHT / m_target_height;
    value = EFB_HEIGHT - value - 1;
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
    index ^= 1;  // swap 2 and 3 for top/bottom
    value = EFB_HEIGHT - value - 1;
    value = value * m_target_height / EFB_HEIGHT;
  }

  BoundingBox::Set(index, value);
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

  int scissorXOff = bpmem.scissorOffset.x * 2;
  int scissorYOff = bpmem.scissorOffset.y * 2;

  // TODO: ceil, floor or just cast to int?
  float X = EFBToScaledXf(xfmem.viewport.xOrig - xfmem.viewport.wd - (float)scissorXOff);
  float Y = EFBToScaledYf((float)EFB_HEIGHT - xfmem.viewport.yOrig + xfmem.viewport.ht +
                          (float)scissorYOff);
  float Width = EFBToScaledXf(2.0f * xfmem.viewport.wd);
  float Height = EFBToScaledYf(-2.0f * xfmem.viewport.ht);
  float min_depth = (xfmem.viewport.farZ - xfmem.viewport.zRange) / 16777216.0f;
  float max_depth = xfmem.viewport.farZ / 16777216.0f;
  if (Width < 0)
  {
    X += Width;
    Width *= -1;
  }
  if (Height < 0)
  {
    Y += Height;
    Height *= -1;
  }

  // Update the view port
  if (g_ogl_config.bSupportViewportFloat)
  {
    glViewportIndexedf(0, X, Y, Width, Height);
  }
  else
  {
    auto iceilf = [](float f) { return static_cast<GLint>(ceilf(f)); };
    glViewport(iceilf(X), iceilf(Y), iceilf(Width), iceilf(Height));
  }

  if (!g_ActiveConfig.backend_info.bSupportsDepthClamp)
  {
    // There's no way to support oversized depth ranges in this situation. Let's just clamp the
    // range to the maximum value supported by the console GPU and hope for the best.
    min_depth = MathUtil::Clamp(min_depth, 0.0f, GX_MAX_DEPTH);
    max_depth = MathUtil::Clamp(max_depth, 0.0f, GX_MAX_DEPTH);
  }

  if (UseVertexDepthRange())
  {
    // We need to ensure depth values are clamped the maximum value supported by the console GPU.
    // Taking into account whether the depth range is inverted or not.
    if (xfmem.viewport.zRange < 0.0f)
    {
      min_depth = GX_MAX_DEPTH;
      max_depth = 0.0f;
    }
    else
    {
      min_depth = 0.0f;
      max_depth = GX_MAX_DEPTH;
    }
  }

  // Set the reversed depth range.
  glDepthRangef(max_depth, min_depth);
}

void Renderer::ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable,
                           u32 color, u32 z)
{
  ResetAPIState();

  // color
  GLboolean const color_mask = colorEnable ? GL_TRUE : GL_FALSE,
                  alpha_mask = alphaEnable ? GL_TRUE : GL_FALSE;
  glColorMask(color_mask, color_mask, color_mask, alpha_mask);

  glClearColor(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
               float((color >> 0) & 0xFF) / 255.0f, float((color >> 24) & 0xFF) / 255.0f);

  // depth
  glDepthMask(zEnable ? GL_TRUE : GL_FALSE);

  glClearDepthf(float(z & 0xFFFFFF) / 16777216.0f);

  // Update rect for clearing the picture
  glEnable(GL_SCISSOR_TEST);

  TargetRectangle const targetRc = ConvertEFBRectangle(rc);
  glScissor(targetRc.left, targetRc.bottom, targetRc.GetWidth(), targetRc.GetHeight());

  // glColorMask/glDepthMask/glScissor affect glClear (glViewport does not)
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  RestoreAPIState();

  ClearEFBCache();
}

void Renderer::BlitScreen(TargetRectangle src, TargetRectangle dst, GLuint src_texture,
                          int src_width, int src_height)
{
  OpenGLPostProcessing* post_processor = static_cast<OpenGLPostProcessing*>(m_post_processor.get());
  if (g_ActiveConfig.stereo_mode == StereoMode::SBS ||
      g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    TargetRectangle leftRc, rightRc;

    // Top-and-Bottom mode needs to compensate for inverted vertical screen coordinates.
    if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
      std::tie(rightRc, leftRc) = ConvertStereoRectangle(dst);
    else
      std::tie(leftRc, rightRc) = ConvertStereoRectangle(dst);

    post_processor->BlitFromTexture(src, leftRc, src_texture, src_width, src_height, 0);
    post_processor->BlitFromTexture(src, rightRc, src_texture, src_width, src_height, 1);
  }
  else if (g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer)
  {
    glDrawBuffer(GL_BACK_LEFT);
    post_processor->BlitFromTexture(src, dst, src_texture, src_width, src_height, 0);

    glDrawBuffer(GL_BACK_RIGHT);
    post_processor->BlitFromTexture(src, dst, src_texture, src_width, src_height, 1);

    glDrawBuffer(GL_BACK);
  }
  else
  {
    post_processor->BlitFromTexture(src, dst, src_texture, src_width, src_height, 0);
  }
}

void Renderer::ReinterpretPixelData(unsigned int convtype)
{
  if (convtype == 0 || convtype == 2)
  {
    FramebufferManager::ReinterpretPixelData(convtype);
  }
  else
  {
    ERROR_LOG(VIDEO, "Trying to reinterpret pixel data with unsupported conversion type %d",
              convtype);
  }
}

void Renderer::SetBlendingState(const BlendingState& state)
{
  bool useDualSource =
      state.usedualsrc && g_ActiveConfig.backend_info.bSupportsDualSourceBlend &&
      (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING) || state.dstalpha);

  const GLenum src_factors[8] = {
      GL_ZERO,
      GL_ONE,
      GL_DST_COLOR,
      GL_ONE_MINUS_DST_COLOR,
      useDualSource ? GL_SRC1_ALPHA : (GLenum)GL_SRC_ALPHA,
      useDualSource ? GL_ONE_MINUS_SRC1_ALPHA : (GLenum)GL_ONE_MINUS_SRC_ALPHA,
      GL_DST_ALPHA,
      GL_ONE_MINUS_DST_ALPHA};
  const GLenum dst_factors[8] = {
      GL_ZERO,
      GL_ONE,
      GL_SRC_COLOR,
      GL_ONE_MINUS_SRC_COLOR,
      useDualSource ? GL_SRC1_ALPHA : (GLenum)GL_SRC_ALPHA,
      useDualSource ? GL_ONE_MINUS_SRC1_ALPHA : (GLenum)GL_ONE_MINUS_SRC_ALPHA,
      GL_DST_ALPHA,
      GL_ONE_MINUS_DST_ALPHA};

  if (state.blendenable)
  {
    glEnable(GL_BLEND);
  }
  else
  {
    glDisable(GL_BLEND);
  }

  // Always call glBlendEquationSeparate and glBlendFuncSeparate, even when
  // GL_BLEND is disabled, as a workaround for some bugs (possibly graphics
  // driver issues?). See https://bugs.dolphin-emu.org/issues/10120 : "Sonic
  // Adventure 2 Battle: graphics crash when loading first Dark level"
  GLenum equation = state.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
  GLenum equationAlpha = state.subtractAlpha ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
  glBlendEquationSeparate(equation, equationAlpha);
  glBlendFuncSeparate(src_factors[state.srcfactor], dst_factors[state.dstfactor],
                      src_factors[state.srcfactoralpha], dst_factors[state.dstfactoralpha]);

  const GLenum logic_op_codes[16] = {
      GL_CLEAR,         GL_AND,         GL_AND_REVERSE, GL_COPY,  GL_AND_INVERTED, GL_NOOP,
      GL_XOR,           GL_OR,          GL_NOR,         GL_EQUIV, GL_INVERT,       GL_OR_REVERSE,
      GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND,        GL_SET};

  if (GLInterface->GetMode() != GLInterfaceMode::MODE_OPENGL)
  {
    // Logic ops aren't available in GLES3
  }
  else if (state.logicopenable)
  {
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(logic_op_codes[state.logicmode]);
  }
  else
  {
    glDisable(GL_COLOR_LOGIC_OP);
  }

  glColorMask(state.colorupdate, state.colorupdate, state.colorupdate, state.alphaupdate);
}

// This function has the final picture. We adjust the aspect ratio here.
void Renderer::SwapImpl(AbstractTexture* texture, const EFBRectangle& xfb_region, u64 ticks,
                        float Gamma)
{
  if (g_ogl_config.bSupportsDebug)
  {
    if (LogManager::GetInstance()->IsEnabled(LogTypes::HOST_GPU, LogTypes::LERROR))
      glEnable(GL_DEBUG_OUTPUT);
    else
      glDisable(GL_DEBUG_OUTPUT);
  }

  auto* xfb_texture = static_cast<OGLTexture*>(texture);

  TargetRectangle sourceRc = xfb_region;
  sourceRc.top = xfb_region.GetHeight();
  sourceRc.bottom = 0;

  ResetAPIState();

  UpdateDrawRectangle();
  TargetRectangle flipped_trc = GetTargetRectangle();

  // Flip top and bottom for some reason; TODO: Fix the code to suck less?
  std::swap(flipped_trc.top, flipped_trc.bottom);

  // Do our OSD callbacks
  OSD::DoCallbacks(OSD::CallbackType::OnFrame);

  // Skip screen rendering when running in headless mode.
  if (!IsHeadless())
  {
    // Copy the framebuffer to screen.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    BlitScreen(sourceRc, flipped_trc, xfb_texture->GetRawTexIdentifier(),
               xfb_texture->GetConfig().width, xfb_texture->GetConfig().height);

    // Finish up the current frame, print some stats
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Reset viewport for drawing text
    glViewport(0, 0, GLInterface->GetBackBufferWidth(), GLInterface->GetBackBufferHeight());
    DrawDebugText();
    OSD::DrawMessages();

    // Copy the rendered frame to the real window.
    GLInterface->Swap();
  }
  else
  {
    // Since we're not swapping in headless mode, ensure all commands are sent to the GPU.
    // Otherwise the driver could batch several frames togehter.
    glFlush();
  }

#ifdef ANDROID
  // Handle surface changes on Android.
  if (m_surface_needs_change.IsSet())
  {
    GLInterface->UpdateHandle(m_new_surface_handle);
    GLInterface->UpdateSurface();
    m_surface_handle = m_new_surface_handle;
    m_new_surface_handle = nullptr;
    m_surface_needs_change.Clear();
    m_surface_changed.Set();
  }
#endif

  // Update the render window position and the backbuffer size
  SetWindowSize(xfb_texture->GetConfig().width, xfb_texture->GetConfig().height);
  GLInterface->Update();

  // Was the size changed since the last frame?
  bool window_resized = false;
  int window_width = static_cast<int>(std::max(GLInterface->GetBackBufferWidth(), 1u));
  int window_height = static_cast<int>(std::max(GLInterface->GetBackBufferHeight(), 1u));
  if (window_width != m_backbuffer_width || window_height != m_backbuffer_height)
  {
    window_resized = true;
    m_backbuffer_width = window_width;
    m_backbuffer_height = window_height;
  }

  bool target_size_changed = CalculateTargetSize();
  bool stencil_buffer_enabled =
      static_cast<FramebufferManager*>(g_framebuffer_manager.get())->HasStencilBuffer();

  bool fb_needs_update = target_size_changed ||
                         s_last_multisamples != g_ActiveConfig.iMultisamples ||
                         stencil_buffer_enabled != BoundingBox::NeedsStencilBuffer() ||
                         s_last_stereo_mode != (g_ActiveConfig.stereo_mode != StereoMode::Off);

  if (window_resized || fb_needs_update)
  {
    UpdateDrawRectangle();
  }
  if (fb_needs_update)
  {
    s_last_stereo_mode = g_ActiveConfig.stereo_mode != StereoMode::Off;
    s_last_multisamples = g_ActiveConfig.iMultisamples;
    s_MSAASamples = s_last_multisamples;

    if (s_MSAASamples > 1 && s_MSAASamples > g_ogl_config.max_samples)
    {
      s_MSAASamples = g_ogl_config.max_samples;
      OSD::AddMessage(
          StringFromFormat("%d Anti Aliasing samples selected, but only %d supported by your GPU.",
                           s_last_multisamples, g_ogl_config.max_samples),
          10000);
    }

    g_framebuffer_manager.reset();
    g_framebuffer_manager = std::make_unique<FramebufferManager>(
        m_target_width, m_target_height, s_MSAASamples, BoundingBox::NeedsStencilBuffer());
    BoundingBox::SetTargetSizeChanged(m_target_width, m_target_height);
  }

  // Clear framebuffer
  if (!IsHeadless())
  {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  if (s_vsync != g_ActiveConfig.IsVSync())
  {
    s_vsync = g_ActiveConfig.IsVSync();
    if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VSYNC))
      GLInterface->SwapInterval(s_vsync);
  }

  // Clean out old stuff from caches. It's not worth it to clean out the shader caches.
  g_texture_cache->Cleanup(frameCount);
  ProgramShaderCache::RetrieveAsyncShaders();

  // Render to the framebuffer.
  FramebufferManager::SetFramebuffer(0);

  RestoreAPIState();

  g_Config.iSaveTargetId = 0;

  int old_anisotropy = g_ActiveConfig.iMaxAnisotropy;
  UpdateActiveConfig();
  g_texture_cache->OnConfigChanged(g_ActiveConfig);

  if (old_anisotropy != g_ActiveConfig.iMaxAnisotropy)
    g_sampler_cache->Clear();

  // Invalidate shader cache when the host config changes.
  if (CheckForHostConfigChanges())
    ProgramShaderCache::Reload();

  // For testing zbuffer targets.
  // Renderer::SetZBufferRender();
  // SaveTexture("tex.png", GL_TEXTURE_2D, s_FakeZTarget,
  //	      GetTargetWidth(), GetTargetHeight());

  // Invalidate EFB cache
  ClearEFBCache();
}

void Renderer::DrawEFB(GLuint framebuffer, const TargetRectangle& target_rc,
                       const TargetRectangle& source_rc)
{
  // for msaa mode, we must resolve the efb content to non-msaa
  GLuint tex = FramebufferManager::ResolveAndGetRenderTarget(source_rc);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  BlitScreen(source_rc, target_rc, tex, m_target_width, m_target_height);
}

// ALWAYS call RestoreAPIState for each ResetAPIState call you're doing
void Renderer::ResetAPIState()
{
  // Gets us to a reasonably sane state where it's possible to do things like
  // image copies with textured quads, etc.
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  if (GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGL)
    glDisable(GL_COLOR_LOGIC_OP);
  if (g_ActiveConfig.backend_info.bSupportsDepthClamp)
  {
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
  }
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void Renderer::RestoreAPIState()
{
  // Gets us back into a more game-like state.
  glEnable(GL_SCISSOR_TEST);
  if (g_ActiveConfig.backend_info.bSupportsDepthClamp)
  {
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_CLIP_DISTANCE1);
  }
  BPFunctions::SetGenerationMode();
  BPFunctions::SetScissor();
  BPFunctions::SetDepthMode();
  BPFunctions::SetBlendMode();
  SetViewport();

  ProgramShaderCache::BindLastVertexFormat();
  const VertexManager* const vm = static_cast<VertexManager*>(g_vertex_manager.get());
  glBindBuffer(GL_ARRAY_BUFFER, vm->GetVertexBufferHandle());

  OGLTexture::SetStage();
}

void Renderer::SetRasterizationState(const RasterizationState& state)
{
  // none, ccw, cw, ccw
  if (state.cullmode != GenMode::CULL_NONE)
  {
    // TODO: GX_CULL_ALL not supported, yet!
    glEnable(GL_CULL_FACE);
    glFrontFace(state.cullmode == GenMode::CULL_FRONT ? GL_CCW : GL_CW);
  }
  else
  {
    glDisable(GL_CULL_FACE);
  }
}

void Renderer::SetDepthState(const DepthState& state)
{
  const GLenum glCmpFuncs[8] = {GL_NEVER,   GL_LESS,     GL_EQUAL,  GL_LEQUAL,
                                GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

  if (state.testenable)
  {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(state.updateenable ? GL_TRUE : GL_FALSE);
    glDepthFunc(glCmpFuncs[state.func]);
  }
  else
  {
    // if the test is disabled write is disabled too
    // TODO: When PE performance metrics are being emulated via occlusion queries, we should
    // (probably?) enable depth test with depth function ALWAYS here
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
  }
}

void Renderer::SetSamplerState(u32 index, const SamplerState& state)
{
  g_sampler_cache->SetSamplerState(index, state);
}

void Renderer::SetInterlacingMode()
{
  // TODO
}

void Renderer::ChangeSurface(void* new_surface_handle)
{
// Win32 polls the window size when redrawing, X11 runs an event loop in another thread.
// This is only necessary for Android at this point, although handling resizes here
// would be more efficient than polling.
#ifdef ANDROID
  m_new_surface_handle = new_surface_handle;
  m_surface_needs_change.Set();
  m_surface_changed.Wait();
#endif
}
}
