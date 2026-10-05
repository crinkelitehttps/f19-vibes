// Minimal OpenGL 3.3 core function loader (via SDL_GL_GetProcAddress).
#pragma once

#define GL_GLEXT_PROTOTYPES 0
#include <GL/glcorearb.h>

namespace f19::gl {

#define F19_GL_FUNCS(X)                                                          \
    X(PFNGLGETSTRINGPROC, GetString)                                             \
    X(PFNGLGETERRORPROC, GetError)                                               \
    X(PFNGLVIEWPORTPROC, Viewport)                                               \
    X(PFNGLSCISSORPROC, Scissor)                                                 \
    X(PFNGLCLEARCOLORPROC, ClearColor)                                           \
    X(PFNGLCLEARDEPTHPROC, ClearDepth)                                           \
    X(PFNGLCLEARPROC, Clear)                                                     \
    X(PFNGLENABLEPROC, Enable)                                                   \
    X(PFNGLDISABLEPROC, Disable)                                                 \
    X(PFNGLBLENDFUNCPROC, BlendFunc)                                             \
    X(PFNGLDEPTHFUNCPROC, DepthFunc)                                             \
    X(PFNGLDEPTHMASKPROC, DepthMask)                                             \
    X(PFNGLCOLORMASKPROC, ColorMask)                                             \
    X(PFNGLPOLYGONOFFSETPROC, PolygonOffset)                                     \
    X(PFNGLDRAWARRAYSPROC, DrawArrays)                                           \
    X(PFNGLREADPIXELSPROC, ReadPixels)                                           \
    X(PFNGLPIXELSTOREIPROC, PixelStorei)                                         \
    X(PFNGLGENTEXTURESPROC, GenTextures)                                         \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures)                                   \
    X(PFNGLBINDTEXTUREPROC, BindTexture)                                         \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D)                                           \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D)                                     \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri)                                     \
    X(PFNGLTEXPARAMETERFPROC, TexParameterf)                                     \
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap)                                   \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                     \
    X(PFNGLCREATESHADERPROC, CreateShader)                                       \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                       \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                     \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                         \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                               \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                       \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                     \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                       \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                         \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                       \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                             \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                           \
    X(PFNGLUNIFORM1IPROC, Uniform1i)                                             \
    X(PFNGLUNIFORM2FPROC, Uniform2f)                                             \
    X(PFNGLUNIFORM4FPROC, Uniform4f)                                             \
    X(PFNGLUNIFORM1FPROC, Uniform1f)                                             \
    X(PFNGLUNIFORM3FPROC, Uniform3f)                                             \
    X(PFNGLUNIFORMMATRIX3FVPROC, UniformMatrix3fv)                               \
    X(PFNGLUNIFORMMATRIX4FVPROC, UniformMatrix4fv)                               \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                 \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                 \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                           \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                           \
    X(PFNGLBUFFERDATAPROC, BufferData)                                           \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)                         \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)                 \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                 \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                           \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                 \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                       \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer)                 \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                   \
    X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers)                               \
    X(PFNGLDELETERENDERBUFFERSPROC, DeleteRenderbuffers)                         \
    X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer)                               \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, RenderbufferStorageMultisample)   \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer)

#define F19_GL_DECLARE(type, name) extern type name;
F19_GL_FUNCS(F19_GL_DECLARE)
#undef F19_GL_DECLARE

// Load all functions; call with a current context. Returns false if any is missing.
bool load();

}  // namespace f19::gl
