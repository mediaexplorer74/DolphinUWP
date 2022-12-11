// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/TextureCache.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/RenderState.h"

class AbstractStagingTexture;

namespace Vulkan
{
class StateTracker;
class StreamBuffer;
class Texture2D;
class VertexFormat;
class VKTexture;
class XFBSource;

class FramebufferManager : public FramebufferManagerBase
{
public:
  FramebufferManager();
  ~FramebufferManager();

  static FramebufferManager* GetInstance();

  bool Initialize();

  VkRenderPass GetEFBLoadRenderPass() const { return m_efb_load_render_pass; }
  VkRenderPass GetEFBClearRenderPass() const { return m_efb_clear_render_pass; }
  Texture2D* GetEFBColorTexture() const { return m_efb_color_texture.get(); }
  Texture2D* GetEFBDepthTexture() const { return m_efb_depth_texture.get(); }
  VkFramebuffer GetEFBFramebuffer() const { return m_efb_framebuffer; }
  u32 GetEFBWidth() const;
  u32 GetEFBHeight() const;
  u32 GetEFBLayers() const;
  VkSampleCountFlagBits GetEFBSamples() const;
  MultisamplingState GetEFBMultisamplingState() const;

  void ResizeEFBTextures();

  // Recompile shaders, use when MSAA mode changes.
  void RecreateRenderPass();
  void RecompileShaders();

  // Reinterpret pixel format of EFB color texture.
  // Assumes no render pass is currently in progress.
  // Swaps EFB framebuffers, so re-bind afterwards.
  void ReinterpretPixelData(int convtype);

  // This render pass can be used for other readback operations.
  VkRenderPass GetColorCopyForReadbackRenderPass() const { return m_copy_color_render_pass; }
  // Resolve color/depth textures to a non-msaa texture, and return it.
  Texture2D* ResolveEFBColorTexture(const VkRect2D& region);
  Texture2D* ResolveEFBDepthTexture(const VkRect2D& region);

  // Returns the texture that the EFB color texture is resolved to when multisampling is enabled.
  // Ensure ResolveEFBColorTexture is called before this method.
  Texture2D* GetResolvedEFBColorTexture() const { return m_efb_resolve_color_texture.get(); }
  // Reads a framebuffer value back from the GPU. This may block if the cache is not current.
  u32 PeekEFBColor(u32 x, u32 y);
  float PeekEFBDepth(u32 x, u32 y);
  void InvalidatePeekCache();

  // Writes a value to the framebuffer. This will never block, and writes will be batched.
  void PokeEFBColor(u32 x, u32 y, u32 color);
  void PokeEFBDepth(u32 x, u32 y, float depth);
  void FlushEFBPokes();

private:
  struct EFBPokeVertex
  {
    float position[4];
    u32 color;
  };

  bool CreateEFBRenderPass();
  void DestroyEFBRenderPass();
  bool CreateEFBFramebuffer();
  void DestroyEFBFramebuffer();

  bool CompileConversionShaders();
  void DestroyConversionShaders();

  bool CreateReadbackRenderPasses();
  void DestroyReadbackRenderPasses();
  bool CompileReadbackShaders();
  void DestroyReadbackShaders();
  bool CreateReadbackTextures();
  void DestroyReadbackTextures();
  bool CreateReadbackFramebuffer();
  void DestroyReadbackFramebuffer();

  void CreatePokeVertexFormat();
  bool CreatePokeVertexBuffer();
  void DestroyPokeVertexBuffer();
  bool CompilePokeShaders();
  void DestroyPokeShaders();

  bool PopulateColorReadbackTexture();
  bool PopulateDepthReadbackTexture();

  void CreatePokeVertices(std::vector<EFBPokeVertex>* destination_list, u32 x, u32 y, float z,
                          u32 color);

  void DrawPokeVertices(const EFBPokeVertex* vertices, size_t vertex_count, bool write_color,
                        bool write_depth);

  VkRenderPass m_efb_load_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_efb_clear_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_depth_resolve_render_pass = VK_NULL_HANDLE;

  std::unique_ptr<Texture2D> m_efb_color_texture;
  std::unique_ptr<Texture2D> m_efb_convert_color_texture;
  std::unique_ptr<Texture2D> m_efb_depth_texture;
  std::unique_ptr<Texture2D> m_efb_resolve_color_texture;
  std::unique_ptr<Texture2D> m_efb_resolve_depth_texture;
  VkFramebuffer m_efb_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_efb_convert_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_depth_resolve_framebuffer = VK_NULL_HANDLE;

  // Format conversion shaders
  VkShaderModule m_ps_rgb8_to_rgba6 = VK_NULL_HANDLE;
  VkShaderModule m_ps_rgba6_to_rgb8 = VK_NULL_HANDLE;
  VkShaderModule m_ps_depth_resolve = VK_NULL_HANDLE;

  // EFB readback texture
  std::unique_ptr<Texture2D> m_color_copy_texture;
  std::unique_ptr<Texture2D> m_depth_copy_texture;
  VkFramebuffer m_color_copy_framebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_depth_copy_framebuffer = VK_NULL_HANDLE;

  // CPU-side EFB readback texture
  std::unique_ptr<AbstractStagingTexture> m_color_readback_texture;
  std::unique_ptr<AbstractStagingTexture> m_depth_readback_texture;
  bool m_color_readback_texture_valid = false;
  bool m_depth_readback_texture_valid = false;

  // EFB poke drawing setup
  std::unique_ptr<VertexFormat> m_poke_vertex_format;
  std::unique_ptr<StreamBuffer> m_poke_vertex_stream_buffer;
  std::vector<EFBPokeVertex> m_color_poke_vertices;
  std::vector<EFBPokeVertex> m_depth_poke_vertices;
  PrimitiveType m_poke_primitive = PrimitiveType::TriangleStrip;

  VkRenderPass m_copy_color_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_copy_depth_render_pass = VK_NULL_HANDLE;
  VkShaderModule m_copy_color_shader = VK_NULL_HANDLE;
  VkShaderModule m_copy_depth_shader = VK_NULL_HANDLE;

  VkShaderModule m_poke_vertex_shader = VK_NULL_HANDLE;
  VkShaderModule m_poke_geometry_shader = VK_NULL_HANDLE;
  VkShaderModule m_poke_fragment_shader = VK_NULL_HANDLE;
};

}  // namespace Vulkan
