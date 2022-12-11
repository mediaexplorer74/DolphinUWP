// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <memory>

#include "Common/GL/GLInterfaceBase.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWTexture.h"

std::unique_ptr<SWOGLWindow> SWOGLWindow::s_instance;

void SWOGLWindow::Init(void* window_handle)
{
  InitInterface();
  GLInterface->SetMode(GLInterfaceMode::MODE_DETECT);
  if (!GLInterface->Create(window_handle))
  {
    ERROR_LOG(VIDEO, "GLInterface::Create failed.");
  }

  s_instance.reset(new SWOGLWindow());
}

void SWOGLWindow::Shutdown()
{
  GLInterface->Shutdown();
  GLInterface.reset();

  s_instance.reset();
}

void SWOGLWindow::Prepare()
{
  if (m_init)
    return;
  m_init = true;

  // Init extension support.
  if (!GLExtensions::Init())
  {
    ERROR_LOG(VIDEO, "GLExtensions::Init failed!Does your video card support OpenGL 2.0?");
    return;
  }
  else if (GLExtensions::Version() < 310)
  {
    ERROR_LOG(VIDEO, "OpenGL Version %d detected, but at least 3.1 is required.",
              GLExtensions::Version());
    return;
  }

  std::string frag_shader = "in vec2 TexCoord;\n"
                            "out vec4 ColorOut;\n"
                            "uniform sampler2D samp;\n"
                            "void main() {\n"
                            "	ColorOut = texture(samp, TexCoord);\n"
                            "}\n";

  std::string vertex_shader = "out vec2 TexCoord;\n"
                              "void main() {\n"
                              "	vec2 rawpos = vec2(gl_VertexID & 1, (gl_VertexID & 2) >> 1);\n"
                              "	gl_Position = vec4(rawpos * 2.0 - 1.0, 0.0, 1.0);\n"
                              "	TexCoord = vec2(rawpos.x, -rawpos.y);\n"
                              "}\n";

  std::string header = GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGL ?
                           "#version 140\n" :
                           "#version 300 es\n"
                           "precision highp float;\n";

  m_image_program = OpenGL_CompileProgram(header + vertex_shader, header + frag_shader);

  glUseProgram(m_image_program);

  glUniform1i(glGetUniformLocation(m_image_program, "samp"), 0);
  glGenTextures(1, &m_image_texture);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glGenVertexArrays(1, &m_image_vao);
}

void SWOGLWindow::PrintText(const std::string& text, int x, int y, u32 color)
{
  m_text.push_back({text, x, y, color});
}

void SWOGLWindow::ShowImage(AbstractTexture* image, const EFBRectangle& xfb_region)
{
  SW::SWTexture* sw_image = static_cast<SW::SWTexture*>(image);
  GLInterface->Update();  // just updates the render window position and the backbuffer size

  GLsizei glWidth = (GLsizei)GLInterface->GetBackBufferWidth();
  GLsizei glHeight = (GLsizei)GLInterface->GetBackBufferHeight();

  glViewport(0, 0, glWidth, glHeight);

  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);

  // TODO: Apply xfb_region

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // 4-byte pixel alignment
  glPixelStorei(GL_UNPACK_ROW_LENGTH, sw_image->GetConfig().width);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(sw_image->GetConfig().width),
               static_cast<GLsizei>(sw_image->GetConfig().height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               sw_image->GetData());

  glUseProgram(m_image_program);

  glBindVertexArray(m_image_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // TODO: implement OSD
  //	for (TextData& text : m_text)
  //	{
  //	}
  m_text.clear();

  GLInterface->Swap();
}

int SWOGLWindow::PeekMessages()
{
  return GLInterface->PeekMessages();
}
