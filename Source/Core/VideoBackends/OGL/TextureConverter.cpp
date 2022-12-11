// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Fast image conversion using OpenGL shaders.

#include "VideoBackends/OGL/TextureConverter.h"

#include <string>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/HW/Memmap.h"

#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoBackends/OGL/SamplerCache.h"
#include "VideoBackends/OGL/TextureCache.h"

#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace TextureConverter
{
using OGL::TextureCache;

std::unique_ptr<AbstractTexture> s_encoding_render_texture;
std::unique_ptr<AbstractStagingTexture> s_encoding_readback_texture;

const int renderBufferWidth = EFB_WIDTH * 4;
const int renderBufferHeight = 1024;

struct EncodingProgram
{
  SHADER program;
  GLint copy_position_uniform;
  GLint y_scale_uniform;
};
static std::map<EFBCopyParams, EncodingProgram> s_encoding_programs;

static EncodingProgram& GetOrCreateEncodingShader(const EFBCopyParams& params)
{
  auto iter = s_encoding_programs.find(params);
  if (iter != s_encoding_programs.end())
    return iter->second;

  const char* shader =
      TextureConversionShaderTiled::GenerateEncodingShader(params, APIType::OpenGL);

#if defined(_DEBUG) || defined(DEBUGFAST)
  if (g_ActiveConfig.iLog & CONF_SAVESHADERS && shader)
  {
    static int counter = 0;
    std::string filename =
        StringFromFormat("%senc_%04i.txt", File::GetUserPath(D_DUMP_IDX).c_str(), counter++);

    SaveData(filename, shader);
  }
#endif

  const char* VProgram = "void main()\n"
                         "{\n"
                         "	vec2 rawpos = vec2(gl_VertexID&1, gl_VertexID&2);\n"
                         "	gl_Position = vec4(rawpos*2.0-1.0, 0.0, 1.0);\n"
                         "}\n";

  EncodingProgram program;
  if (!ProgramShaderCache::CompileShader(program.program, VProgram, shader))
    PanicAlert("Failed to compile texture encoding shader.");

  program.copy_position_uniform = glGetUniformLocation(program.program.glprogid, "position");
  program.y_scale_uniform = glGetUniformLocation(program.program.glprogid, "y_scale");
  return s_encoding_programs.emplace(params, program).first->second;
}

void Init()
{
  TextureConfig config(renderBufferWidth, renderBufferHeight, 1, 1, AbstractTextureFormat::BGRA8,
                       true);
  s_encoding_render_texture = g_renderer->CreateTexture(config);
  s_encoding_readback_texture =
      g_renderer->CreateStagingTexture(StagingTextureType::Readback, config);
}

void Shutdown()
{
  s_encoding_readback_texture.reset();
  s_encoding_render_texture.reset();

  for (auto& program : s_encoding_programs)
    program.second.program.Destroy();
  s_encoding_programs.clear();
}

// dst_line_size, writeStride in bytes

static void EncodeToRamUsingShader(GLuint srcTexture, u8* destAddr, u32 dst_line_size,
                                   u32 dstHeight, u32 writeStride, bool linearFilter, float y_scale)
{
  FramebufferManager::SetFramebuffer(
      static_cast<OGLTexture*>(s_encoding_render_texture.get())->GetFramebuffer());

  OpenGL_BindAttributelessVAO();

  // set source texture
  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D_ARRAY, srcTexture);

  // We also linear filtering for both box filtering and downsampling higher resolutions to 1x
  // TODO: This only produces perfect downsampling for 2x IR, other resolutions will need more
  //       complex down filtering to average all pixels and produce the correct result.
  // Also, box filtering won't be correct for anything other than 1x IR
  if (linearFilter || g_renderer->GetEFBScale() != 1 || y_scale > 1.0f)
    g_sampler_cache->BindLinearSampler(9);
  else
    g_sampler_cache->BindNearestSampler(9);

  glViewport(0, 0, (GLsizei)(dst_line_size / 4), (GLsizei)dstHeight);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  MathUtil::Rectangle<int> copy_rect(0, 0, dst_line_size / 4, dstHeight);
  s_encoding_readback_texture->CopyFromTexture(s_encoding_render_texture.get(), copy_rect, 0, 0,
                                               copy_rect);
  s_encoding_readback_texture->ReadTexels(copy_rect, destAddr, writeStride);

  FramebufferManager::SetFramebuffer(0);
  OGLTexture::SetStage();
}

void EncodeToRamFromTexture(u8* dest_ptr, const EFBCopyParams& params, u32 native_width,
                            u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
                            const EFBRectangle& src_rect, bool scale_by_half)
{
  g_renderer->ResetAPIState();

  EncodingProgram& texconv_shader = GetOrCreateEncodingShader(params);

  texconv_shader.program.Bind();
  glUniform4i(texconv_shader.copy_position_uniform, src_rect.left, src_rect.top, native_width,
              scale_by_half ? 2 : 1);
  glUniform1f(texconv_shader.y_scale_uniform, params.y_scale);

  const GLuint read_texture = params.depth ?
                                  FramebufferManager::ResolveAndGetDepthTarget(src_rect) :
                                  FramebufferManager::ResolveAndGetRenderTarget(src_rect);

  EncodeToRamUsingShader(read_texture, dest_ptr, bytes_per_row, num_blocks_y, memory_stride,
                         scale_by_half && !params.depth, params.y_scale);

  FramebufferManager::SetFramebuffer(0);
  g_renderer->RestoreAPIState();
}

}  // namespace

}  // namespace OGL
