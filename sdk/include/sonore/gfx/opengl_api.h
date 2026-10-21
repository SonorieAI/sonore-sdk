// SPDX-License-Identifier: Apache-2.0
//
// The OpenGL entry points, loaded by hand, and the three platform halves of a
// GL surface: WGL on Windows, NSOpenGLContext on macOS, GLX on X11.
//
// ── Why there is no GLEW and no glad ────────────────────────────────────────
//
// The SDK is header-only and vendors nothing it can avoid, and a loader is a
// table of function pointers and a lookup per entry -- a few hundred lines,
// not a dependency. More to the point, a plugin is a guest in a host process
// that may have loaded its OWN GL loader, with its own globals, and a second
// copy of GLEW's global function table is a link-time collision waiting for
// the right host. So the table here is a struct, owned by each context and
// loaded when that context is created, and nothing in it is global.
//
// ── Why nothing links opengl32 / OpenGL.framework / libGL ───────────────────
//
// Each is opened at RUNTIME, the same rule window_x11.h follows for libX11. A
// plugin that hard-links libGL fails to LOAD on a render farm without one,
// and fails with a loader error -- the least diagnosable failure a plugin can
// have. Opened at runtime, the plugin loads, OpenGLContext::attachTo returns
// false, and lastError() says which library was missing.
//
// ── What a caller gets ──────────────────────────────────────────────────────
//
// The GL type names (GLuint, GLenum, ...) at global scope, exactly as the
// Khronos headers declare them, so a file that also includes <GL/gl.h> sees
// the same typedef twice, which C++ allows. The GL_* constants as macros, each
// guarded and spelled exactly as the Khronos headers spell it, so the same file
// sees an identical redefinition rather than a conflicting one. And the
// functions as members of OpenGLFunctions, reached through the context:
//
//     auto& gl = context.functions();
//     gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
//     gl.glClear(GL_COLOR_BUFFER_BIT);
//
// Members rather than free functions because a free glClear here would be
// ambiguous against the one <GL/gl.h> declares in any file that has both.
//
// ── The platform surfaces ───────────────────────────────────────────────────
//
// Each is a CHILD of the editor's own window or view, placed over the
// component the context is attached to. A separate native surface rather than
// GL drawn into the editor's window, because the editor's window is a GDI /
// CoreGraphics / XPutImage blit of the software-rasterised bitmap, and mixing
// that with a GL swap chain in one window is undefined on all three platforms.
// The child is transparent to the mouse, so the component tree under it still
// receives every click exactly as it did.
//
// Windows is RUN by sdk/tests/opengl_test.cpp on real hardware. The macOS and
// X11 halves are compiled only -- against the Apple shims in tests/au_shim and
// against real Xlib headers -- and have not executed on any machine here.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "component.h"
#include "geometry.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include "window_cocoa.h"
#elif defined(__linux__)
#include "window_x11.h"
#endif

// ── The GL types, as Khronos declares them ──────────────────────────────────
//
// GLsizeiptr and GLintptr follow khrplatform.h: long long on 64-bit Windows,
// long everywhere else. glext.h has used ptrdiff_t for them in the past, which
// is a different type on 32-bit Windows; matching the CURRENT Khronos headers
// is the choice that agrees with what a user is most likely to include beside
// this one.
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef unsigned char GLubyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
#if defined(_WIN64)
typedef signed long long int GLsizeiptr;
typedef signed long long int GLintptr;
#else
typedef signed long int GLsizeiptr;
typedef signed long int GLintptr;
#endif

#if defined(_WIN32)
#define SONORE_GL_CALL __stdcall
#else
#define SONORE_GL_CALL
#endif

// ── The constants, guarded and spelled as Khronos spells them ───────────────
#ifndef GL_FALSE
#define GL_FALSE 0
#endif
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#ifndef GL_NO_ERROR
#define GL_NO_ERROR 0
#endif
#ifndef GL_ZERO
#define GL_ZERO 0
#endif
#ifndef GL_ONE
#define GL_ONE 1
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif
#ifndef GL_STENCIL_BUFFER_BIT
#define GL_STENCIL_BUFFER_BIT 0x00000400
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_POINTS
#define GL_POINTS 0x0000
#endif
#ifndef GL_LINES
#define GL_LINES 0x0001
#endif
#ifndef GL_LINE_LOOP
#define GL_LINE_LOOP 0x0002
#endif
#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP 0x0003
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x0004
#endif
#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP 0x0005
#endif
#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN 0x0006
#endif
#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#ifndef GL_NEVER
#define GL_NEVER 0x0200
#endif
#ifndef GL_LESS
#define GL_LESS 0x0201
#endif
#ifndef GL_EQUAL
#define GL_EQUAL 0x0202
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL 0x0203
#endif
#ifndef GL_GREATER
#define GL_GREATER 0x0204
#endif
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_GEQUAL
#define GL_GEQUAL 0x0206
#endif
#ifndef GL_ALWAYS
#define GL_ALWAYS 0x0207
#endif
#ifndef GL_SRC_COLOR
#define GL_SRC_COLOR 0x0300
#endif
#ifndef GL_ONE_MINUS_SRC_COLOR
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#endif
#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x0302
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif
#ifndef GL_DST_ALPHA
#define GL_DST_ALPHA 0x0304
#endif
#ifndef GL_ONE_MINUS_DST_ALPHA
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#endif
#ifndef GL_DST_COLOR
#define GL_DST_COLOR 0x0306
#endif
#ifndef GL_ONE_MINUS_DST_COLOR
#define GL_ONE_MINUS_DST_COLOR 0x0307
#endif
#ifndef GL_FRONT
#define GL_FRONT 0x0404
#endif
#ifndef GL_BACK
#define GL_BACK 0x0405
#endif
#ifndef GL_FRONT_AND_BACK
#define GL_FRONT_AND_BACK 0x0408
#endif
#ifndef GL_INVALID_ENUM
#define GL_INVALID_ENUM 0x0500
#endif
#ifndef GL_INVALID_VALUE
#define GL_INVALID_VALUE 0x0501
#endif
#ifndef GL_INVALID_OPERATION
#define GL_INVALID_OPERATION 0x0502
#endif
#ifndef GL_OUT_OF_MEMORY
#define GL_OUT_OF_MEMORY 0x0505
#endif
#ifndef GL_INVALID_FRAMEBUFFER_OPERATION
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#endif
#ifndef GL_CW
#define GL_CW 0x0900
#endif
#ifndef GL_CCW
#define GL_CCW 0x0901
#endif
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH 0x0B20
#endif
#ifndef GL_CULL_FACE
#define GL_CULL_FACE 0x0B44
#endif
#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x0B71
#endif
#ifndef GL_STENCIL_TEST
#define GL_STENCIL_TEST 0x0B90
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_BLEND
#define GL_BLEND 0x0BE2
#endif
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif
#ifndef GL_INT
#define GL_INT 0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW 0x1700
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION 0x1701
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT 0x1902
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_ALPHA
#define GL_ALPHA 0x1906
#endif
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_KEEP
#define GL_KEEP 0x1E00
#endif
#ifndef GL_REPLACE
#define GL_REPLACE 0x1E01
#endif
#ifndef GL_INCR
#define GL_INCR 0x1E02
#endif
#ifndef GL_DECR
#define GL_DECR 0x1E03
#endif
#ifndef GL_VENDOR
#define GL_VENDOR 0x1F00
#endif
#ifndef GL_RENDERER
#define GL_RENDERER 0x1F01
#endif
#ifndef GL_VERSION
#define GL_VERSION 0x1F02
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1F03
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_SAMPLE_BUFFERS
#define GL_SAMPLE_BUFFERS 0x80A8
#endif
#ifndef GL_SAMPLES
#define GL_SAMPLES 0x80A9
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_VALIDATE_STATUS
#define GL_VALIDATE_STATUS 0x8B83
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RENDERBUFFER_BINDING
#define GL_RENDERBUFFER_BINDING 0x8CA7
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_STENCIL_INDEX8
#define GL_STENCIL_INDEX8 0x8D48
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif
#ifndef GL_CONTEXT_COMPATIBILITY_PROFILE_BIT
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#endif

// Every entry point the table holds: return type, name, parameter types.
//
// The GL 3.2 core set this SDK and a typical plugin visualiser use, plus the
// handful of fixed-function calls a 2.1 compatibility context still offers --
// those last ones resolve on a core context too (the libraries export them)
// and raise GL_INVALID_OPERATION when called there, exactly as the spec says.
#define SONORE_GL_FUNCTION_LIST(X)                                                            \
  X(void, glActiveTexture, (GLenum))                                                         \
  X(void, glAttachShader, (GLuint, GLuint))                                                  \
  X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))                             \
  X(void, glBindBuffer, (GLenum, GLuint))                                                    \
  X(void, glBindFramebuffer, (GLenum, GLuint))                                               \
  X(void, glBindRenderbuffer, (GLenum, GLuint))                                              \
  X(void, glBindTexture, (GLenum, GLuint))                                                   \
  X(void, glBindVertexArray, (GLuint))                                                       \
  X(void, glBlendEquation, (GLenum))                                                         \
  X(void, glBlendEquationSeparate, (GLenum, GLenum))                                         \
  X(void, glBlendFunc, (GLenum, GLenum))                                                     \
  X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                             \
  X(void, glBlitFramebuffer,                                                                 \
    (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))            \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                           \
  X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                      \
  X(GLenum, glCheckFramebufferStatus, (GLenum))                                              \
  X(void, glClear, (GLbitfield))                                                             \
  X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                \
  X(void, glClearDepth, (GLdouble))                                                          \
  X(void, glClearStencil, (GLint))                                                           \
  X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                         \
  X(void, glCompileShader, (GLuint))                                                         \
  X(void, glCopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
  X(GLuint, glCreateProgram, ())                                                             \
  X(GLuint, glCreateShader, (GLenum))                                                        \
  X(void, glCullFace, (GLenum))                                                              \
  X(void, glDeleteBuffers, (GLsizei, const GLuint*))                                         \
  X(void, glDeleteFramebuffers, (GLsizei, const GLuint*))                                    \
  X(void, glDeleteProgram, (GLuint))                                                         \
  X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))                                   \
  X(void, glDeleteShader, (GLuint))                                                          \
  X(void, glDeleteTextures, (GLsizei, const GLuint*))                                        \
  X(void, glDeleteVertexArrays, (GLsizei, const GLuint*))                                    \
  X(void, glDepthFunc, (GLenum))                                                             \
  X(void, glDepthMask, (GLboolean))                                                          \
  X(void, glDetachShader, (GLuint, GLuint))                                                  \
  X(void, glDisable, (GLenum))                                                               \
  X(void, glDisableVertexAttribArray, (GLuint))                                              \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                            \
  X(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                          \
  X(void, glDrawBuffer, (GLenum))                                                            \
  X(void, glDrawBuffers, (GLsizei, const GLenum*))                                           \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))                            \
  X(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void*, GLsizei))          \
  X(void, glEnable, (GLenum))                                                                \
  X(void, glEnableVertexAttribArray, (GLuint))                                               \
  X(void, glFinish, ())                                                                      \
  X(void, glFlush, ())                                                                       \
  X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                       \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                   \
  X(void, glFrontFace, (GLenum))                                                             \
  X(void, glGenBuffers, (GLsizei, GLuint*))                                                  \
  X(void, glGenFramebuffers, (GLsizei, GLuint*))                                             \
  X(void, glGenRenderbuffers, (GLsizei, GLuint*))                                            \
  X(void, glGenTextures, (GLsizei, GLuint*))                                                 \
  X(void, glGenVertexArrays, (GLsizei, GLuint*))                                             \
  X(void, glGenerateMipmap, (GLenum))                                                        \
  X(GLint, glGetAttribLocation, (GLuint, const GLchar*))                                     \
  X(void, glGetBooleanv, (GLenum, GLboolean*))                                               \
  X(GLenum, glGetError, ())                                                                  \
  X(void, glGetFloatv, (GLenum, GLfloat*))                                                   \
  X(void, glGetIntegerv, (GLenum, GLint*))                                                   \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                         \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                          \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                          \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                           \
  X(const GLubyte*, glGetString, (GLenum))                                                   \
  X(const GLubyte*, glGetStringi, (GLenum, GLuint))                                          \
  X(void, glGetTexImage, (GLenum, GLint, GLenum, GLenum, void*))                             \
  X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                    \
  X(void, glHint, (GLenum, GLenum))                                                          \
  X(GLboolean, glIsBuffer, (GLuint))                                                         \
  X(GLboolean, glIsEnabled, (GLenum))                                                        \
  X(GLboolean, glIsFramebuffer, (GLuint))                                                    \
  X(GLboolean, glIsProgram, (GLuint))                                                        \
  X(GLboolean, glIsRenderbuffer, (GLuint))                                                   \
  X(GLboolean, glIsShader, (GLuint))                                                         \
  X(GLboolean, glIsTexture, (GLuint))                                                        \
  X(void, glLineWidth, (GLfloat))                                                            \
  X(void, glLinkProgram, (GLuint))                                                           \
  X(void, glPixelStorei, (GLenum, GLint))                                                    \
  X(void, glPolygonMode, (GLenum, GLenum))                                                   \
  X(void, glPolygonOffset, (GLfloat, GLfloat))                                               \
  X(void, glReadBuffer, (GLenum))                                                            \
  X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))             \
  X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                         \
  X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))     \
  X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                       \
  X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))             \
  X(void, glStencilFunc, (GLenum, GLint, GLuint))                                            \
  X(void, glStencilMask, (GLuint))                                                           \
  X(void, glStencilOp, (GLenum, GLenum, GLenum))                                             \
  X(void, glTexImage2D,                                                                      \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))            \
  X(void, glTexParameterf, (GLenum, GLenum, GLfloat))                                        \
  X(void, glTexParameteri, (GLenum, GLenum, GLint))                                          \
  X(void, glTexSubImage2D,                                                                   \
    (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))            \
  X(void, glUniform1f, (GLint, GLfloat))                                                     \
  X(void, glUniform2f, (GLint, GLfloat, GLfloat))                                            \
  X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                   \
  X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                          \
  X(void, glUniform1i, (GLint, GLint))                                                       \
  X(void, glUniform2i, (GLint, GLint, GLint))                                                \
  X(void, glUniform3i, (GLint, GLint, GLint, GLint))                                         \
  X(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint))                                  \
  X(void, glUniform1fv, (GLint, GLsizei, const GLfloat*))                                    \
  X(void, glUniform2fv, (GLint, GLsizei, const GLfloat*))                                    \
  X(void, glUniform3fv, (GLint, GLsizei, const GLfloat*))                                    \
  X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*))                                    \
  X(void, glUniform1iv, (GLint, GLsizei, const GLint*))                                      \
  X(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat*))                   \
  X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat*))                   \
  X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                   \
  X(void, glUseProgram, (GLuint))                                                            \
  X(void, glValidateProgram, (GLuint))                                                       \
  X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                    \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))   \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                      \
  X(void, glBegin, (GLenum))                                                                 \
  X(void, glEnd, ())                                                                         \
  X(void, glVertex2f, (GLfloat, GLfloat))                                                    \
  X(void, glVertex3f, (GLfloat, GLfloat, GLfloat))                                           \
  X(void, glColor4f, (GLfloat, GLfloat, GLfloat, GLfloat))                                   \
  X(void, glColor4ub, (GLubyte, GLubyte, GLubyte, GLubyte))                                  \
  X(void, glTexCoord2f, (GLfloat, GLfloat))                                                  \
  X(void, glMatrixMode, (GLenum))                                                            \
  X(void, glLoadIdentity, ())                                                                \
  X(void, glLoadMatrixf, (const GLfloat*))                                                   \
  X(void, glOrtho, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))             \
  X(void, glPushMatrix, ())                                                                  \
  X(void, glPopMatrix, ())

namespace sonore {
namespace gfx {

/**
 * Which GL a context asks for.
 *
 * defaultGLVersion is the COMPATIBILITY profile at whatever version the driver
 * offers -- 4.6 on a current Windows driver, 2.1 on macOS, whose compatibility
 * profile stopped there. The others are CORE profiles, where the fixed-function
 * pipeline is gone and shaders need `#version 150` or later. macOS has 3.2 and
 * 4.1 core and nothing above; asking it for 4.3 fails rather than quietly
 * handing back something lower.
 */
enum class OpenGLVersion {
  defaultGLVersion, ///< The compatibility profile, at whatever version the driver gives.
  openGL3_2,        ///< A core profile of 3.2 or a compatible later version.
  openGL4_1,        ///< A core profile of 4.1 or later: the highest macOS has.
  openGL4_3         ///< A core profile of 4.3 or later; fails on macOS.
};

/**
 * What the surface's pixels are made of.
 *
 * multisamplingLevel only takes effect when the context's multisampling is
 * enabled -- the two are separate so a plugin can offer an anti-aliasing
 * toggle without rebuilding its format. When the driver cannot give the
 * requested samples, the context falls back to none rather than failing:
 * jagged edges are a worse picture, a black rectangle is no picture.
 */
struct OpenGLPixelFormat {
  int redBits = 8,   ///< Bits of red per pixel asked for; the driver may give more, as for all of these.
      greenBits = 8, ///< Bits of green per pixel asked for.
      blueBits = 8,  ///< Bits of blue per pixel asked for.
      alphaBits = 8; ///< Bits of alpha per pixel asked for.
  /** Bits of depth buffer asked for; 0 asks for none. */
  int depthBufferBits = 24;
  /** Bits of stencil buffer asked for; 0 asks for none. */
  int stencilBufferBits = 8;
  /** Samples per pixel asked for when multisampling is enabled. */
  int multisamplingLevel = 4;

  /** 8 bits per channel, a 24-bit depth and an 8-bit stencil buffer, 4
   *  samples. */
  OpenGLPixelFormat() {}
  /** `bitsPerRGBComponent` for each of red, green and blue, and the other
   *  three as given; multisamplingLevel keeps its default of 4. */
  OpenGLPixelFormat(int bitsPerRGBComponent, int alpha, int depth, int stencil)
      : redBits(bitsPerRGBComponent), greenBits(bitsPerRGBComponent),
        blueBits(bitsPerRGBComponent), alphaBits(alpha), depthBufferBits(depth),
        stencilBufferBits(stencil) {}
};

/**
 * The GL entry points of one context, and what that context turned out to be.
 *
 * Every pointer starts null and is resolved by load(). A null pointer is the
 * library saying the driver lacks that function, and the feature flags below
 * are derived from the VERSION rather than from non-null pointers alone:
 * glXGetProcAddressARB returns a non-null stub for any name at all, so on X11
 * a pointer is not proof of anything.
 */
struct OpenGLFunctions {
#define SONORE_GL_DECLARE(ret, name, params) ret(SONORE_GL_CALL* name) params = nullptr;
  SONORE_GL_FUNCTION_LIST(SONORE_GL_DECLARE)
#undef SONORE_GL_DECLARE

  int majorVersion = 0, ///< The context's GL version, from GL_VERSION; 0 until load() succeeds.
      minorVersion = 0; ///< The minor half of that version.
  /** A core profile (3.2 or later with the core bit set): no fixed-function
   *  calls, and nothing draws without a vertex array. */
  bool coreProfile = false;
  /** glCreateShader and friends: GL 2.0. */
  bool hasShaders = false;
  /** glGenFramebuffers and friends: GL 3.0, or ARB_framebuffer_object. */
  bool hasFramebuffers = false;
  /** glGenVertexArrays: GL 3.0. Needed to draw ANYTHING on a core profile. */
  bool hasVertexArrays = false;

  /** Whether the context's version is `major`.`minor` or later. */
  bool isVersionAtLeast(int major, int minor) const {
    return majorVersion > major || (majorVersion == major && minorVersion >= minor);
  }

  /** Resolve every entry point through `resolve` -- the platform's lookup --
   *  with the context CURRENT on the calling thread, which WGL requires. */
  bool load(void* (*resolve)(const char*)) {
    if (!resolve) return false;
#define SONORE_GL_RESOLVE(ret, name, params)                                                  \
  {                                                                                           \
    void* p = resolve(#name);                                                                 \
    std::memcpy(&name, &p, sizeof(p));                                                        \
  }
    SONORE_GL_FUNCTION_LIST(SONORE_GL_RESOLVE)
#undef SONORE_GL_RESOLVE
    if (!glGetString || !glGetIntegerv || !glGetError) return false;
    parseVersion((const char*) glGetString(GL_VERSION));
    if (majorVersion == 0) return false;
    coreProfile = false;
    if (isVersionAtLeast(3, 2)) {
      GLint mask = 0;
      glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
      // A driver that predates the query leaves an error behind; drained so
      // the first glGetError a user calls is about their own code.
      while (glGetError() != GL_NO_ERROR) {
      }
      coreProfile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    }
    hasShaders = isVersionAtLeast(2, 0) && glCreateShader && glShaderSource && glCompileShader &&
                 glCreateProgram && glLinkProgram && glUseProgram && glGetUniformLocation &&
                 glVertexAttribPointer && glGenBuffers && glBufferData;
    hasFramebuffers = (isVersionAtLeast(3, 0) || hasExtension("GL_ARB_framebuffer_object")) &&
                      glGenFramebuffers && glBindFramebuffer && glFramebufferTexture2D &&
                      glCheckFramebufferStatus && glGenRenderbuffers;
    hasVertexArrays = isVersionAtLeast(3, 0) && glGenVertexArrays && glBindVertexArray;
    return true;
  }

  /** Whether the CURRENT context names `extension`. Through glGetStringi on
   *  3.0 and later, because a core profile has no GL_EXTENSIONS string at all
   *  -- asking for it there is an error, not an empty answer. */
  bool hasExtension(const char* extension) const {
    if (!extension || !*extension) return false;
    if (isVersionAtLeast(3, 0) && glGetStringi && glGetIntegerv) {
      GLint count = 0;
      glGetIntegerv(GL_NUM_EXTENSIONS, &count);
      for (GLint i = 0; i < count; ++i) {
        const char* name = (const char*) glGetStringi(GL_EXTENSIONS, (GLuint) i);
        if (name && std::strcmp(name, extension) == 0) return true;
      }
      return false;
    }
    if (!glGetString) return false;
    const char* all = (const char*) glGetString(GL_EXTENSIONS);
    if (!all) return false;
    const size_t length = std::strlen(extension);
    // Whole words only: GL_EXT_texture is a prefix of a dozen other names.
    for (const char* p = std::strstr(all, extension); p; p = std::strstr(p + 1, extension)) {
      const bool startsWord = p == all || p[-1] == ' ';
      const bool endsWord = p[length] == ' ' || p[length] == '\0';
      if (startsWord && endsWord) return true;
    }
    return false;
  }

private:
  /** "4.6.13596 Compatibility Profile Context", "2.1 Metal - 83", "3.3 (Core
   *  Profile) Mesa 23.0" -- the version is the first two numbers, whatever
   *  the vendor writes after them. */
  void parseVersion(const char* text) {
    majorVersion = minorVersion = 0;
    if (!text) return;
    while (*text && (*text < '0' || *text > '9')) ++text;
    while (*text >= '0' && *text <= '9') majorVersion = majorVersion * 10 + (*text++ - '0');
    if (*text == '.') {
      ++text;
      while (*text >= '0' && *text <= '9') minorVersion = minorVersion * 10 + (*text++ - '0');
    }
  }
};

namespace gldetail {

/** The context-creation knobs the platform halves need, gathered so each of
 *  them takes one argument rather than five. */
struct SurfaceRequest {
  OpenGLPixelFormat format;
  OpenGLVersion version = OpenGLVersion::defaultGLVersion;
  bool multisampling = false;
  void* shareWith = nullptr;
};

/** Where the surface goes, in the units the platform positions children in:
 *  device pixels on Windows and X11, points on macOS. */
struct SurfaceBounds {
  int x = 0, y = 0, w = 0, h = 0;
  bool visible = false;
  /** The visible part, in the surface's OWN coordinates, when an ancestor
   *  clips it -- a GL view scrolled half out of a viewport. Empty = whole. */
  PixelRect clip;
  bool clipped = false;

  bool operator==(const SurfaceBounds& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h && visible == o.visible &&
           clipped == o.clipped && clip.x == o.clip.x && clip.y == o.clip.y &&
           clip.w == o.clip.w && clip.h == o.clip.h;
  }
  bool operator!=(const SurfaceBounds& o) const { return !(*this == o); }
};

#if defined(_WIN32)
// ═══ Windows: WGL ═══════════════════════════════════════════════════════════

/**
 * opengl32.dll, opened at runtime, and the WGL extensions.
 *
 * The extensions -- the ARB pixel-format chooser that knows about
 * multisampling, the ARB context creator that knows about core profiles, and
 * the swap-interval call -- can only be looked up with a context CURRENT, and
 * a window can only ever have one pixel format set. So they are fetched once
 * per process through a throwaway window and a throwaway legacy context, which
 * is the standard dance and the only way in.
 */
struct WglLibrary {
  HMODULE module = nullptr;
  HGLRC(WINAPI* createContext)(HDC) = nullptr;
  BOOL(WINAPI* deleteContext)(HGLRC) = nullptr;
  BOOL(WINAPI* makeCurrent)(HDC, HGLRC) = nullptr;
  PROC(WINAPI* getProcAddress)(LPCSTR) = nullptr;
  HGLRC(WINAPI* getCurrentContext)() = nullptr;
  HDC(WINAPI* getCurrentDC)() = nullptr;
  BOOL(WINAPI* shareLists)(HGLRC, HGLRC) = nullptr;
  BOOL(WINAPI* choosePixelFormatARB)(HDC, const int*, const FLOAT*, UINT, int*, UINT*) = nullptr;
  HGLRC(WINAPI* createContextAttribsARB)(HDC, HGLRC, const int*) = nullptr;
  BOOL(WINAPI* swapIntervalEXT)(int) = nullptr;
  int(WINAPI* getSwapIntervalEXT)() = nullptr;
  bool ok = false;
  std::string error;
};

constexpr int kWglDrawToWindow = 0x2001, kWglAcceleration = 0x2003,
              kWglSupportOpenGL = 0x2010, kWglDoubleBuffer = 0x2011, kWglPixelType = 0x2013,
              kWglColorBits = 0x2014, kWglRedBits = 0x2015, kWglGreenBits = 0x2017,
              kWglBlueBits = 0x2019, kWglAlphaBits = 0x201B, kWglDepthBits = 0x2022,
              kWglStencilBits = 0x2023, kWglFullAcceleration = 0x2027, kWglTypeRgba = 0x202B,
              kWglSampleBuffers = 0x2041, kWglSamples = 0x2042, kWglContextMajor = 0x2091,
              kWglContextMinor = 0x2092, kWglContextProfileMask = 0x9126,
              kWglCoreProfileBit = 0x0001, kWglCompatibilityProfileBit = 0x0002;

/** A message only this module's surfaces understand: "render now", posted to
 *  the message thread when rendering happens there. WM_APP and above are the
 *  application's own range; nothing else will send this to our class. */
constexpr UINT kRepaintMessage = WM_APP + 0x5E1;
constexpr UINT_PTR kSurfaceTimerId = 0x5E1;

inline HMODULE glModuleOfThisCode() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR) &glModuleOfThisCode, &m);
  return m;
}

class NativeSurface;
inline LRESULT CALLBACK glSurfaceWindowProc(HWND, UINT, WPARAM, LPARAM);

inline std::wstring& glSurfaceClassName() {
  static std::wstring name;
  return name;
}

/** Our own class, named per module for the reason window_win32.h gives: two
 *  plugins sharing one class name would share one window procedure, and the
 *  one in the module that unloads first takes the other's windows with it.
 *  CS_OWNDC because a GL surface wants the same DC for its whole life -- a
 *  pooled DC can come back with a different pixel format. */
inline ATOM registerGLSurfaceClass() {
  static ATOM atom = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = glSurfaceWindowProc;
    wc.hInstance = glModuleOfThisCode();
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    ATOM a = 0;
    for (int attempt = 0; attempt < 64 && !a; ++attempt) {
      wchar_t buffer[80];
      std::swprintf(buffer, 80, L"SonoreGLSurface_%p_%d", (void*) glModuleOfThisCode(), attempt);
      glSurfaceClassName() = buffer;
      wc.lpszClassName = glSurfaceClassName().c_str();
      a = RegisterClassExW(&wc);
      if (!a && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    if (!a) glSurfaceClassName().clear();
    return a;
  }();
  return atom;
}

inline void fillLegacyPixelFormat(PIXELFORMATDESCRIPTOR& pfd, const OpenGLPixelFormat& f) {
  std::memset(&pfd, 0, sizeof(pfd));
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = (BYTE) (f.redBits + f.greenBits + f.blueBits);
  pfd.cAlphaBits = (BYTE) f.alphaBits;
  pfd.cDepthBits = (BYTE) f.depthBufferBits;
  pfd.cStencilBits = (BYTE) f.stencilBufferBits;
  pfd.iLayerType = PFD_MAIN_PLANE;
}

inline WglLibrary loadWglLibrary() {
  WglLibrary w;
  // From System32 and nowhere else: a bare name is searched for in the
  // host's own directory first, and an opengl32.dll planted beside a DAW's
  // executable would be loaded into every plugin that asked for GL.
  w.module = LoadLibraryExW(L"opengl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!w.module) {
    w.error = "opengl32.dll could not be loaded";
    return w;
  }
  bool all = true;
  auto bind = [&](const char* name, void* slot) {
    void* p = (void*) GetProcAddress(w.module, name);
    std::memcpy(slot, &p, sizeof(p));
    if (!p) all = false;
  };
  bind("wglCreateContext", &w.createContext);
  bind("wglDeleteContext", &w.deleteContext);
  bind("wglMakeCurrent", &w.makeCurrent);
  bind("wglGetProcAddress", &w.getProcAddress);
  bind("wglGetCurrentContext", &w.getCurrentContext);
  bind("wglGetCurrentDC", &w.getCurrentDC);
  bind("wglShareLists", &w.shareLists);
  if (!all) {
    w.error = "opengl32.dll is missing a WGL entry point";
    return w;
  }

  if (!registerGLSurfaceClass()) {
    w.error = "the GL surface window class could not be registered";
    return w;
  }
  // The throwaway window. Never shown, never parented, destroyed before this
  // returns. Its procedure finds no owner and falls through to DefWindowProc.
  HWND dummy = CreateWindowExW(0, glSurfaceClassName().c_str(), L"", WS_POPUP | WS_CLIPCHILDREN |
                               WS_CLIPSIBLINGS, 0, 0, 4, 4, nullptr, nullptr,
                               glModuleOfThisCode(), nullptr);
  if (!dummy) {
    w.error = "could not create a window to query WGL with";
    return w;
  }
  HDC dc = GetDC(dummy);
  PIXELFORMATDESCRIPTOR pfd;
  fillLegacyPixelFormat(pfd, OpenGLPixelFormat());
  const int format = dc ? ChoosePixelFormat(dc, &pfd) : 0;
  if (format == 0 || !SetPixelFormat(dc, format, &pfd)) {
    w.error = "no OpenGL pixel format is offered (no GL driver installed?)";
  } else if (HGLRC legacy = w.createContext(dc)) {
    // Whatever the calling thread had current is put BACK afterwards. A host
    // that draws its own interface with GL on this thread would otherwise find
    // its context gone the first time a plugin opened an editor.
    const HGLRC previousContext = w.getCurrentContext();
    const HDC previousDC = w.getCurrentDC();
    if (w.makeCurrent(dc, legacy)) {
      auto ext = [&](const char* name, void* slot) {
        void* p = (void*) w.getProcAddress(name);
        const intptr_t v = (intptr_t) p;
        if (v == 1 || v == 2 || v == 3 || v == -1) p = nullptr;
        std::memcpy(slot, &p, sizeof(p));
      };
      ext("wglChoosePixelFormatARB", &w.choosePixelFormatARB);
      ext("wglCreateContextAttribsARB", &w.createContextAttribsARB);
      ext("wglSwapIntervalEXT", &w.swapIntervalEXT);
      ext("wglGetSwapIntervalEXT", &w.getSwapIntervalEXT);
      w.ok = true;
    } else {
      w.error = "wglMakeCurrent failed on a fresh legacy context";
    }
    w.makeCurrent(previousDC, previousContext);
    w.deleteContext(legacy);
  } else {
    w.error = "wglCreateContext failed: the driver offers no OpenGL context";
  }
  if (dc) ReleaseDC(dummy, dc);
  DestroyWindow(dummy);
  return w;
}

inline const WglLibrary& wglLibrary() {
  static const WglLibrary library = loadWglLibrary();
  return library;
}

/** The entry point for `name`. wglGetProcAddress knows nothing of the GL 1.1
 *  functions opengl32 exports directly -- it returns null, or on some drivers
 *  1, 2, 3 or -1, for those -- so they come from the module instead. */
inline void* resolveGLFunction(const char* name) {
  const WglLibrary& w = wglLibrary();
  if (!w.module) return nullptr;
  void* p = w.getProcAddress ? (void*) w.getProcAddress(name) : nullptr;
  const intptr_t v = (intptr_t) p;
  if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) p = (void*) GetProcAddress(w.module, name);
  return p;
}

/** Whatever the thread had current before a context of ours was made current
 *  on it -- the host's own, possibly -- so it can be put back. */
struct SavedCurrent {
  HDC dc = nullptr;
  HGLRC context = nullptr;
};

inline SavedCurrent saveCurrentGLContext() {
  SavedCurrent s;
  const WglLibrary& w = wglLibrary();
  if (w.ok) {
    s.dc = w.getCurrentDC();
    s.context = w.getCurrentContext();
  }
  return s;
}

inline void restoreGLContext(const SavedCurrent& s) {
  const WglLibrary& w = wglLibrary();
  if (w.ok) w.makeCurrent(s.dc, s.context);
}

inline bool anyGLContextCurrent() {
  const WglLibrary& w = wglLibrary();
  return w.ok && w.getCurrentContext() != nullptr;
}

/**
 * A WGL context in a child window of the editor.
 *
 * Created, positioned and destroyed on the message thread. Made current,
 * drawn and swapped on whichever thread renders -- WGL allows that, provided
 * the context is current on only one thread at a time, which OpenGLContext
 * guarantees by handing it over rather than sharing it.
 */
class NativeSurface {
public:
  NativeSurface() {}
  NativeSurface(const NativeSurface&) = delete;
  NativeSurface& operator=(const NativeSurface&) = delete;
  ~NativeSurface() { destroy(); }

  /** Fired on the message thread. onTick from the surface's own timer,
   *  onRepaint when the window needs drawing (WM_PAINT, or a posted request),
   *  onDestroyed when the WINDOW is being destroyed by somebody else -- the
   *  host tearing the editor down under us. */
  std::function<void()> onTick, onRepaint, onDestroyed;

  bool create(void* parent, const SurfaceRequest& request, std::string& error) {
    destroy();
    const WglLibrary& w = wglLibrary();
    if (!w.ok) {
      error = w.error;
      return false;
    }
    HWND parentWindow = (HWND) parent;
    if (!parentWindow || !IsWindow(parentWindow)) {
      error = "the native parent handle is not a window";
      return false;
    }
    // The parent must not paint over us. Its WM_PAINT blits the whole
    // software-rendered editor, and without WS_CLIPCHILDREN that blit lands on
    // top of the GL surface every time anything else in the editor changes --
    // a flicker at exactly the moments a user is looking.
    const LONG_PTR style = GetWindowLongPtrW(parentWindow, GWL_STYLE);
    if (!(style & WS_CLIPCHILDREN)) {
      SetWindowLongPtrW(parentWindow, GWL_STYLE, style | WS_CLIPCHILDREN);
      addedClipChildren_ = true;
    }
    parent_ = parentWindow;

    hwnd_ = CreateWindowExW(0, glSurfaceClassName().c_str(), L"",
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1, 1,
                            parentWindow, nullptr, glModuleOfThisCode(), this);
    if (!hwnd_) {
      error = "could not create the GL surface window";
      destroy();
      return false;
    }
    dc_ = GetDC(hwnd_);
    if (!dc_) {
      error = "the GL surface window has no device context";
      destroy();
      return false;
    }

    int format = choosePixelFormat(request);
    if (format == 0 && request.multisampling) {
      SurfaceRequest without = request;
      without.multisampling = false;
      format = choosePixelFormat(without);
    }
    if (format == 0) {
      error = "no pixel format matches the requested OpenGLPixelFormat";
      destroy();
      return false;
    }
    PIXELFORMATDESCRIPTOR pfd;
    std::memset(&pfd, 0, sizeof(pfd));
    DescribePixelFormat(dc_, format, sizeof(pfd), &pfd);
    if (!SetPixelFormat(dc_, format, &pfd)) {
      error = "SetPixelFormat failed on the GL surface";
      destroy();
      return false;
    }

    context_ = createContext(request);
    if (!context_) {
      error = request.version == OpenGLVersion::defaultGLVersion
                  ? "wglCreateContext failed"
                  : "the driver cannot create the requested core-profile OpenGL version";
      destroy();
      return false;
    }
    SetTimer(hwnd_, kSurfaceTimerId, 16, nullptr);
    return true;
  }

  /** Window and DC and context, in that order of dependence reversed. The
   *  context must not be current on ANY thread: wglDeleteContext refuses a
   *  context another thread holds, and the caller has stopped that thread. */
  void destroy() {
    releaseContextAndDC();
    if (hwnd_) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      KillTimer(hwnd_, kSurfaceTimerId);
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
    if (addedClipChildren_ && parent_ && IsWindow(parent_)) {
      const LONG_PTR style = GetWindowLongPtrW(parent_, GWL_STYLE);
      SetWindowLongPtrW(parent_, GWL_STYLE, style & ~(LONG_PTR) WS_CLIPCHILDREN);
    }
    addedClipChildren_ = false;
    parent_ = nullptr;
    lastBounds_ = SurfaceBounds();
  }

  /** The context and DC only, for a window that is already being destroyed
   *  by somebody else: the handle is not ours to destroy twice. */
  void releaseContextAndDC() {
    const WglLibrary& w = wglLibrary();
    if (context_) {
      if (w.getCurrentContext() == context_) w.makeCurrent(nullptr, nullptr);
      w.deleteContext(context_);
      context_ = nullptr;
    }
    if (dc_ && hwnd_) ReleaseDC(hwnd_, dc_);
    dc_ = nullptr;
  }

  bool isAlive() const { return hwnd_ != nullptr && context_ != nullptr; }

  bool makeActive() {
    return context_ && dc_ && wglLibrary().makeCurrent(dc_, context_) != FALSE;
  }
  void makeInactive() {
    const WglLibrary& w = wglLibrary();
    if (w.ok && w.getCurrentContext() == context_) w.makeCurrent(nullptr, nullptr);
  }
  bool isActive() const { return context_ && wglLibrary().getCurrentContext() == context_; }

  void swap() {
    if (dc_) SwapBuffers(dc_);
  }

  /** With the context current. False where WGL_EXT_swap_control is absent. */
  bool applySwapInterval(int interval) {
    const WglLibrary& w = wglLibrary();
    return w.swapIntervalEXT && w.swapIntervalEXT(interval) != FALSE;
  }
  int querySwapInterval() const {
    const WglLibrary& w = wglLibrary();
    return w.getSwapIntervalEXT ? w.getSwapIntervalEXT() : -1;
  }

  /** Nothing on Windows: the window can be moved while another thread draws
   *  into it. The other two platforms need the lock. */
  void lockFrame() {}
  void unlockFrame() {}

  /** Device pixels per logical one, read off the editor itself: its client
   *  area against the size its root component was laid out to. That is the
   *  scale the editor's Backing actually used -- the host's, when the host set
   *  one -- which the monitor's DPI is not always. */
  float scaleFor(const Component& root) const {
    RECT r{};
    if (parent_ && GetClientRect(parent_, &r) && root.bounds().w > 0.0f && r.right > 0)
      return (float) r.right / root.bounds().w;
    return 1.0f;
  }

  /** Device pixels, the parent's client coordinates. Only applied when
   *  something changed: SetWindowPos on every tick of a still editor would be
   *  thirty pointless trips through the window manager a second. */
  void setBounds(const SurfaceBounds& b) {
    if (!hwnd_ || b == lastBounds_) return;
    const bool clipChanged = b.clipped != lastBounds_.clipped || b.clip.x != lastBounds_.clip.x ||
                             b.clip.y != lastBounds_.clip.y || b.clip.w != lastBounds_.clip.w ||
                             b.clip.h != lastBounds_.clip.h;
    lastBounds_ = b;
    SetWindowPos(hwnd_, nullptr, b.x, b.y, b.w > 0 ? b.w : 1, b.h > 0 ? b.h : 1,
                 SWP_NOZORDER | SWP_NOACTIVATE | (b.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (clipChanged) {
      // The system owns a region once it is set, so this one is not deleted.
      HRGN region = b.clipped ? CreateRectRgn(b.clip.x, b.clip.y, b.clip.right(), b.clip.bottom())
                              : nullptr;
      SetWindowRgn(hwnd_, region, TRUE);
    }
  }

  /** "Render on the message thread soon", coalesced: one message in flight at
   *  most, however many repaints are asked for before it arrives. */
  void postRepaint() {
    if (hwnd_ && !repaintPosted_.exchange(true)) PostMessageW(hwnd_, kRepaintMessage, 0, 0);
  }

  /** Nothing to drain: Windows delivers to the window procedure unasked. */
  void pumpEvents() {}

  void* rawContext() const { return (void*) context_; }
  void* surfaceHandle() const { return (void*) hwnd_; }

  /** Called by the window procedure. */
  LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      // Transparent to the mouse: every click, move and wheel goes to the
      // editor window under us and through its component tree, exactly as if
      // the GL surface were not there. The alternative -- forwarding each
      // message by hand -- is a list of messages that is wrong the first time
      // somebody adds one.
      case WM_NCHITTEST:
        return HTTRANSPARENT;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (onRepaint) onRepaint();
        return 0;
      }
      case WM_TIMER:
        if (wp == kSurfaceTimerId && onTick) onTick();
        return 0;
      case kRepaintMessage:
        repaintPosted_ = false;
        if (onRepaint) onRepaint();
        return 0;
      case WM_DESTROY:
        // Somebody else is destroying us -- the editor window was closed, so
        // Windows is taking its children with it. The window and its DC are
        // still valid for the length of this message, which is exactly the
        // window in which the render thread can be stopped and the renderer
        // can free its GL objects. After it, any GL call would be into a
        // surface that no longer exists.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        KillTimer(hwnd, kSurfaceTimerId);
        if (onDestroyed) onDestroyed();
        releaseContextAndDC();
        hwnd_ = nullptr;
        if (addedClipChildren_ && parent_ && IsWindow(parent_)) {
          const LONG_PTR style = GetWindowLongPtrW(parent_, GWL_STYLE);
          SetWindowLongPtrW(parent_, GWL_STYLE, style & ~(LONG_PTR) WS_CLIPCHILDREN);
        }
        addedClipChildren_ = false;
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

private:
  int choosePixelFormat(const SurfaceRequest& r) {
    const WglLibrary& w = wglLibrary();
    if (w.choosePixelFormatARB) {
      std::vector<int> a = {kWglDrawToWindow, 1, kWglSupportOpenGL, 1, kWglDoubleBuffer, 1,
                            kWglAcceleration, kWglFullAcceleration, kWglPixelType, kWglTypeRgba,
                            kWglColorBits, r.format.redBits + r.format.greenBits + r.format.blueBits,
                            kWglRedBits, r.format.redBits, kWglGreenBits, r.format.greenBits,
                            kWglBlueBits, r.format.blueBits, kWglAlphaBits, r.format.alphaBits,
                            kWglDepthBits, r.format.depthBufferBits, kWglStencilBits,
                            r.format.stencilBufferBits};
      if (r.multisampling && r.format.multisamplingLevel > 1) {
        a.push_back(kWglSampleBuffers);
        a.push_back(1);
        a.push_back(kWglSamples);
        a.push_back(r.format.multisamplingLevel);
      }
      a.push_back(0);
      int format = 0;
      UINT count = 0;
      if (w.choosePixelFormatARB(dc_, a.data(), nullptr, 1, &format, &count) && count > 0)
        return format;
      if (r.multisampling) return 0; // the caller retries without
    }
    PIXELFORMATDESCRIPTOR pfd;
    fillLegacyPixelFormat(pfd, r.format);
    return ChoosePixelFormat(dc_, &pfd);
  }

  HGLRC createContext(const SurfaceRequest& r) {
    const WglLibrary& w = wglLibrary();
    HGLRC share = (HGLRC) r.shareWith;
    if (w.createContextAttribsARB) {
      int major = 0, minor = 0;
      switch (r.version) {
        case OpenGLVersion::openGL3_2: major = 3; minor = 2; break;
        case OpenGLVersion::openGL4_1: major = 4; minor = 1; break;
        case OpenGLVersion::openGL4_3: major = 4; minor = 3; break;
        default: break;
      }
      std::vector<int> a;
      if (major > 0) {
        a = {kWglContextMajor, major, kWglContextMinor, minor, kWglContextProfileMask,
             kWglCoreProfileBit};
      }
      a.push_back(0);
      if (HGLRC c = w.createContextAttribsARB(dc_, share, a.data())) return c;
      if (major > 0) return nullptr; // a core version the driver does not have
    } else if (r.version != OpenGLVersion::defaultGLVersion) {
      return nullptr; // no ARB creator: no core profiles at all
    }
    HGLRC c = w.createContext(dc_);
    if (c && share && !w.shareLists(share, c)) {
      w.deleteContext(c);
      return nullptr;
    }
    return c;
  }

  HWND hwnd_ = nullptr;
  HWND parent_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC context_ = nullptr;
  bool addedClipChildren_ = false;
  std::atomic<bool> repaintPosted_{false};
  SurfaceBounds lastBounds_;
};

inline LRESULT CALLBACK glSurfaceWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* create = (CREATESTRUCTW*) lp;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  // Transparent to the mouse even with no owner -- during creation, and for
  // the throwaway query window -- so no click is ever eaten.
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;
  auto* self = (NativeSurface*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
  return self->handle(hwnd, msg, wp, lp);
}

inline const char* platformGLUnavailableReason() {
  const WglLibrary& w = wglLibrary();
  return w.ok ? "" : w.error.c_str();
}

#elif defined(__APPLE__)
// ═══ macOS: NSOpenGLContext ════════════════════════════════════════════════
//
// Deprecated since 10.14 and still present on every release since, including
// Apple silicon, where it runs on Metal underneath. Written against the
// Objective-C runtime like the rest of the Cocoa code here, so a project needs
// no .mm file. Compiled against tests/au_shim; never run.

constexpr uint32_t kNSPFADoubleBuffer = 5, kNSPFAColorSize = 8, kNSPFAAlphaSize = 11,
                   kNSPFADepthSize = 12, kNSPFAStencilSize = 13, kNSPFASampleBuffers = 55,
                   kNSPFASamples = 56, kNSPFAMultisample = 59, kNSPFAAccelerated = 73,
                   kNSPFAOpenGLProfile = 99, kNSProfileLegacy = 0x1000,
                   kNSProfile3_2Core = 0x3200, kNSProfile4_1Core = 0x4100;
constexpr long kNSContextSwapInterval = 222;

inline void* openGLFramework() {
  static void* lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
  return lib;
}

inline void* resolveGLFunction(const char* name) {
  void* lib = openGLFramework();
  return lib ? dlsym(lib, name) : nullptr;
}

struct SavedCurrent {
  id context = nullptr;
};

inline SavedCurrent saveCurrentGLContext() {
  SavedCurrent s;
  if (id cls = cocoa::cls("NSOpenGLContext"))
    s.context = cocoa::msg<id>(cls, sel_registerName("currentContext"));
  return s;
}

inline void restoreGLContext(const SavedCurrent& s) {
  if (s.context) {
    cocoa::msg<void>(s.context, sel_registerName("makeCurrentContext"));
  } else if (id cls = cocoa::cls("NSOpenGLContext")) {
    cocoa::msg<void>(cls, sel_registerName("clearCurrentContext"));
  }
}

inline bool anyGLContextCurrent() { return saveCurrentGLContext().context != nullptr; }

class NativeSurface;

inline NativeSurface* glSurfaceOfView(id view) {
  void* value = nullptr;
  object_getInstanceVariable(view, "sonoreGLSurface", &value);
  return static_cast<NativeSurface*>(value);
}

inline BOOL sonoreGLViewIsFlipped(id, SEL) { return YES; }
inline BOOL sonoreGLViewIsOpaque(id, SEL) { return YES; }
/** nil: the click is not ours, so AppKit keeps looking -- and finds the
 *  editor's view underneath, whose component tree handles it. */
inline id sonoreGLViewHitTest(id, SEL, cocoa::CGPoint2) { return nullptr; }
inline void sonoreGLViewTick(id self, SEL, id);
inline void sonoreGLViewRepaint(id self, SEL, id);

inline Class registeredGLViewClass() {
  static Class klass = [] {
    // Unique to this binary, for the reason NativeWindowCocoa gives.
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreGLView_%llx",
                  (unsigned long long) (uintptr_t) (void*) &sonoreGLViewIsFlipped);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) cocoa::cls("NSView"), name, 0);
    if (!c) return (Class) objc_getClass(name);
    class_addIvar(c, "sonoreGLSurface", sizeof(void*), 3, "^v");
    class_addMethod(c, sel_registerName("isFlipped"), (IMP) sonoreGLViewIsFlipped, "c@:");
    class_addMethod(c, sel_registerName("isOpaque"), (IMP) sonoreGLViewIsOpaque, "c@:");
    class_addMethod(c, sel_registerName("hitTest:"), (IMP) sonoreGLViewHitTest,
                    "@@:{CGPoint=dd}");
    class_addMethod(c, sel_registerName("sonoreGLTick:"), (IMP) sonoreGLViewTick, "v@:@");
    class_addMethod(c, sel_registerName("sonoreGLRepaint:"), (IMP) sonoreGLViewRepaint, "v@:@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

class NativeSurface {
public:
  NativeSurface() {}
  NativeSurface(const NativeSurface&) = delete;
  NativeSurface& operator=(const NativeSurface&) = delete;
  ~NativeSurface() { destroy(); }

  std::function<void()> onTick, onRepaint, onDestroyed;

  bool create(void* parent, const SurfaceRequest& request, std::string& error) {
    destroy();
    if (!cocoa::cls("NSOpenGLContext") || !openGLFramework()) {
      error = "NSOpenGLContext or OpenGL.framework is not available in this process";
      return false;
    }
    if (!parent) {
      error = "the native parent handle is not an NSView";
      return false;
    }
    pixelFormat_ = makePixelFormat(request, request.multisampling);
    if (!pixelFormat_ && request.multisampling) pixelFormat_ = makePixelFormat(request, false);
    if (!pixelFormat_) {
      error = request.version == OpenGLVersion::openGL4_3
                  ? "macOS offers OpenGL 4.1 core at most; 4.3 cannot be created"
                  : "no NSOpenGLPixelFormat matches the requested format";
      return false;
    }
    id alloc = cocoa::msg<id>(cocoa::cls("NSOpenGLContext"), sel_registerName("alloc"));
    context_ = cocoa::msg<id>(alloc, sel_registerName("initWithFormat:shareContext:"), pixelFormat_,
                              (id) request.shareWith);
    if (!context_) {
      error = "NSOpenGLContext could not be created";
      destroy();
      return false;
    }
    Class viewClass = registeredGLViewClass();
    view_ = cocoa::msg<id>((id) viewClass, sel_registerName("alloc"));
    const cocoa::CGRect2 frame{{0.0, 0.0}, {1.0, 1.0}};
    using InitFrame = id (*)(id, SEL, cocoa::CGRect2);
    view_ = reinterpret_cast<InitFrame>(objc_msgSend)(view_, sel_registerName("initWithFrame:"),
                                                      frame);
    if (!view_) {
      error = "the GL view could not be created";
      destroy();
      return false;
    }
    object_setInstanceVariable(view_, "sonoreGLSurface", this);
    // Retina: a drawable in PIXELS rather than points, so the GL content is as
    // sharp as the software-rendered editor around it.
    cocoa::msg<void>(view_, sel_registerName("setWantsBestResolutionOpenGLSurface:"), (BOOL) YES);
    cocoa::msg<void>(view_, sel_registerName("setHidden:"), (BOOL) YES);
    cocoa::msg<void>((id) parent, sel_registerName("addSubview:"), view_);
    cocoa::msg<void>(context_, sel_registerName("setView:"), view_);

    using Sched = id (*)(id, SEL, double, id, SEL, id, BOOL);
    timer_ = reinterpret_cast<Sched>(objc_msgSend)(
        (id) cocoa::cls("NSTimer"),
        sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
        1.0 / 60.0, view_, sel_registerName("sonoreGLTick:"), (id) nullptr, (BOOL) YES);
    cocoa::keepTickingInEveryMode(timer_);
    return true;
  }

  void destroy() {
    if (timer_) {
      cocoa::msg<void>(timer_, sel_registerName("invalidate"));
      timer_ = nullptr;
    }
    if (context_) {
      std::lock_guard<std::mutex> lock(frameLock_);
      cocoa::msg<void>(context_, sel_registerName("clearDrawable"));
      cocoa::msg<void>(context_, sel_registerName("release"));
      context_ = nullptr;
    }
    if (pixelFormat_) {
      cocoa::msg<void>(pixelFormat_, sel_registerName("release"));
      pixelFormat_ = nullptr;
    }
    if (view_) {
      object_setInstanceVariable(view_, "sonoreGLSurface", nullptr);
      cocoa::msg<void>(view_, sel_registerName("removeFromSuperview"));
      cocoa::msg<void>(view_, sel_registerName("release"));
      view_ = nullptr;
    }
    lastBounds_ = SurfaceBounds();
  }

  void releaseContextAndDC() {}
  bool isAlive() const { return context_ != nullptr && view_ != nullptr; }

  bool makeActive() {
    if (!context_) return false;
    cocoa::msg<void>(context_, sel_registerName("makeCurrentContext"));
    return true;
  }
  void makeInactive() {
    if (isActive()) cocoa::msg<void>(cocoa::cls("NSOpenGLContext"), sel_registerName("clearCurrentContext"));
  }
  bool isActive() const { return context_ && saveCurrentGLContext().context == context_; }

  void swap() {
    if (context_) cocoa::msg<void>(context_, sel_registerName("flushBuffer"));
  }

  bool applySwapInterval(int interval) {
    if (!context_) return false;
    const int value = interval;
    cocoa::msg<void>(context_, sel_registerName("setValues:forParameter:"), &value,
                     kNSContextSwapInterval);
    return true;
  }
  int querySwapInterval() const {
    if (!context_) return -1;
    int value = -1;
    cocoa::msg<void>(context_, sel_registerName("getValues:forParameter:"), &value,
                     kNSContextSwapInterval);
    return value;
  }

  /** -[NSOpenGLContext update] must run on the main thread when the view
   *  moves, and must not run while another thread is drawing through the
   *  context. The render thread holds this for each frame; setBounds holds it
   *  around the move and the update. */
  void lockFrame() { frameLock_.lock(); }
  void unlockFrame() { frameLock_.unlock(); }

  /** The backing scale: macOS lays out in POINTS, so the component tree's
   *  logical units are already points and only the drawable is multiplied. */
  float scaleFor(const Component&) const {
    id window = view_ ? cocoa::msg<id>(view_, sel_registerName("window")) : (id) nullptr;
    if (!window) return 1.0f;
    const double s = cocoa::msg<double>(window, sel_registerName("backingScaleFactor"));
    return s > 0.0 ? (float) s : 1.0f;
  }

  /** POINTS in the parent's flipped coordinates -- the editor's view is
   *  flipped, so a child's frame origin is its top-left. */
  void setBounds(const SurfaceBounds& b) {
    if (!view_ || b == lastBounds_) return;
    lastBounds_ = b;
    std::lock_guard<std::mutex> lock(frameLock_);
    const cocoa::CGRect2 frame{{(double) b.x, (double) b.y},
                               {(double) (b.w > 0 ? b.w : 1), (double) (b.h > 0 ? b.h : 1)}};
    using SetFrame = void (*)(id, SEL, cocoa::CGRect2);
    reinterpret_cast<SetFrame>(objc_msgSend)(view_, sel_registerName("setFrame:"), frame);
    cocoa::msg<void>(view_, sel_registerName("setHidden:"), (BOOL) (b.visible ? NO : YES));
    if (context_) cocoa::msg<void>(context_, sel_registerName("update"));
  }

  void postRepaint() {
    if (view_ && !repaintPosted_.exchange(true))
      cocoa::msg<void>(view_, sel_registerName("performSelectorOnMainThread:withObject:waitUntilDone:"),
                       sel_registerName("sonoreGLRepaint:"), (id) nullptr, (BOOL) NO);
  }

  void* rawContext() const { return (void*) context_; }
  void* surfaceHandle() const { return (void*) view_; }

  /** Nothing to drain: AppKit delivers to the view unasked. */
  void pumpEvents() {}

  void tickFromView() {
    if (onTick) onTick();
  }
  void repaintFromView() {
    repaintPosted_ = false;
    if (onRepaint) onRepaint();
  }

private:
  static id makePixelFormat(const SurfaceRequest& r, bool multisample) {
    uint32_t profile = kNSProfileLegacy;
    if (r.version == OpenGLVersion::openGL3_2) profile = kNSProfile3_2Core;
    if (r.version == OpenGLVersion::openGL4_1) profile = kNSProfile4_1Core;
    if (r.version == OpenGLVersion::openGL4_3) return nullptr;
    std::vector<uint32_t> a = {kNSPFADoubleBuffer, kNSPFAAccelerated, kNSPFAOpenGLProfile, profile,
                               kNSPFAColorSize,
                               (uint32_t) (r.format.redBits + r.format.greenBits + r.format.blueBits),
                               kNSPFAAlphaSize, (uint32_t) r.format.alphaBits, kNSPFADepthSize,
                               (uint32_t) r.format.depthBufferBits, kNSPFAStencilSize,
                               (uint32_t) r.format.stencilBufferBits};
    if (multisample && r.format.multisamplingLevel > 1) {
      a.push_back(kNSPFAMultisample);
      a.push_back(kNSPFASampleBuffers);
      a.push_back(1u);
      a.push_back(kNSPFASamples);
      a.push_back((uint32_t) r.format.multisamplingLevel);
    }
    a.push_back(0u);
    id alloc = cocoa::msg<id>(cocoa::cls("NSOpenGLPixelFormat"), sel_registerName("alloc"));
    return cocoa::msg<id>(alloc, sel_registerName("initWithAttributes:"),
                          (const uint32_t*) a.data());
  }

  id view_ = nullptr;
  id context_ = nullptr;
  id pixelFormat_ = nullptr;
  id timer_ = nullptr;
  std::mutex frameLock_;
  std::atomic<bool> repaintPosted_{false};
  SurfaceBounds lastBounds_;
};

inline void sonoreGLViewTick(id self, SEL, id) {
  if (NativeSurface* s = glSurfaceOfView(self)) s->tickFromView();
}
inline void sonoreGLViewRepaint(id self, SEL, id) {
  if (NativeSurface* s = glSurfaceOfView(self)) s->repaintFromView();
}

inline const char* platformGLUnavailableReason() {
  return (cocoa::cls("NSOpenGLContext") && openGLFramework())
             ? ""
             : "NSOpenGLContext or OpenGL.framework is not available in this process";
}

#elif defined(__linux__) && defined(SONORE_X11_HEADERS)
// ═══ X11: GLX ═══════════════════════════════════════════════════════════════
//
// libGL.so.1 opened at runtime and the GLX calls declared here rather than
// taken from <GL/glx.h>: the GLX ABI this needs is a dozen functions over
// opaque pointers and XIDs, nothing like XEvent's thirty-structure union, so
// declaring it costs less than requiring a -dev package at build time.
// Compiled against real Xlib headers; never run.

constexpr int kGlxRgbaType = 0x8014, kGlxWindowBit = 0x00000001, kGlxRgbaBit = 0x00000001,
              kGlxXRenderable = 0x8012, kGlxDrawableType = 0x8010, kGlxRenderType = 0x8011,
              kGlxDoubleBuffer = 5, kGlxRedSize = 8, kGlxGreenSize = 9, kGlxBlueSize = 10,
              kGlxAlphaSize = 11, kGlxDepthSize = 12, kGlxStencilSize = 13,
              kGlxSampleBuffers = 100000, kGlxSamples = 100001, kGlxContextMajor = 0x2091,
              kGlxContextMinor = 0x2092, kGlxContextProfileMask = 0x9126,
              kGlxCoreProfileBit = 0x0001;

struct GlxLibrary {
  void* lib = nullptr;
  void** (*glXChooseFBConfig)(::Display*, int, const int*, int*) = nullptr;
  ::XVisualInfo* (*glXGetVisualFromFBConfig)(::Display*, void*) = nullptr;
  void* (*glXCreateNewContext)(::Display*, void*, int, void*, int) = nullptr;
  int (*glXMakeCurrent)(::Display*, unsigned long, void*) = nullptr;
  void (*glXSwapBuffers)(::Display*, unsigned long) = nullptr;
  void (*glXDestroyContext)(::Display*, void*) = nullptr;
  void* (*glXGetCurrentContext)() = nullptr;
  unsigned long (*glXGetCurrentDrawable)() = nullptr;
  ::Display* (*glXGetCurrentDisplay)() = nullptr;
  const char* (*glXQueryExtensionsString)(::Display*, int) = nullptr;
  void* (*glXGetProcAddressARB)(const unsigned char*) = nullptr;
  // Not in libX11's list in window_x11.h, and only needed here.
  ::Colormap (*XCreateColormap)(::Display*, ::Window, ::Visual*, int) = nullptr;
  int (*XFreeColormap)(::Display*, ::Colormap) = nullptr;
  int (*XMoveResizeWindow)(::Display*, ::Window, int, int, unsigned, unsigned) = nullptr;
  int (*(*XSetErrorHandler)(int (*)(::Display*, ::XErrorEvent*)))(::Display*, ::XErrorEvent*) =
      nullptr;
  bool ok = false;
  std::string error;
};

inline GlxLibrary loadGlxLibrary() {
  GlxLibrary g;
  if (!x11::xlib().ok) {
    g.error = "libX11.so.6 is not on this machine";
    return g;
  }
  g.lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!g.lib) g.lib = dlopen("libGL.so", RTLD_LAZY | RTLD_LOCAL);
  if (!g.lib) {
    g.error = "libGL.so.1 is not on this machine";
    return g;
  }
  bool all = true;
  auto bind = [&](void* lib, const char* name, void* slot) {
    void* p = dlsym(lib, name);
    if (!p) all = false;
    std::memcpy(slot, &p, sizeof(p));
  };
  bind(g.lib, "glXChooseFBConfig", &g.glXChooseFBConfig);
  bind(g.lib, "glXGetVisualFromFBConfig", &g.glXGetVisualFromFBConfig);
  bind(g.lib, "glXCreateNewContext", &g.glXCreateNewContext);
  bind(g.lib, "glXMakeCurrent", &g.glXMakeCurrent);
  bind(g.lib, "glXSwapBuffers", &g.glXSwapBuffers);
  bind(g.lib, "glXDestroyContext", &g.glXDestroyContext);
  bind(g.lib, "glXGetCurrentContext", &g.glXGetCurrentContext);
  bind(g.lib, "glXGetCurrentDrawable", &g.glXGetCurrentDrawable);
  bind(g.lib, "glXGetCurrentDisplay", &g.glXGetCurrentDisplay);
  bind(g.lib, "glXQueryExtensionsString", &g.glXQueryExtensionsString);
  bind(g.lib, "glXGetProcAddressARB", &g.glXGetProcAddressARB);
  void* x = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
  if (!x) x = dlopen("libX11.so", RTLD_LAZY | RTLD_GLOBAL);
  if (x) {
    bind(x, "XCreateColormap", &g.XCreateColormap);
    bind(x, "XFreeColormap", &g.XFreeColormap);
    bind(x, "XMoveResizeWindow", &g.XMoveResizeWindow);
    bind(x, "XSetErrorHandler", &g.XSetErrorHandler);
  } else {
    all = false;
  }
  if (!all) {
    g.error = "libGL.so.1 or libX11.so.6 is missing a GLX entry point";
    return g;
  }
  g.ok = true;
  return g;
}

inline const GlxLibrary& glxLibrary() {
  static const GlxLibrary library = loadGlxLibrary();
  return library;
}

inline void* resolveGLFunction(const char* name) {
  const GlxLibrary& g = glxLibrary();
  if (!g.ok) return nullptr;
  // dlsym first: for the GL 1.x functions libGL exports directly it is exact,
  // where glXGetProcAddressARB hands back a dispatch stub for ANY name.
  if (void* p = dlsym(g.lib, name)) return p;
  return g.glXGetProcAddressARB((const unsigned char*) name);
}

struct SavedCurrent {
  ::Display* display = nullptr;
  unsigned long drawable = 0;
  void* context = nullptr;
};

inline SavedCurrent saveCurrentGLContext() {
  SavedCurrent s;
  const GlxLibrary& g = glxLibrary();
  if (g.ok) {
    s.display = g.glXGetCurrentDisplay();
    s.drawable = g.glXGetCurrentDrawable();
    s.context = g.glXGetCurrentContext();
  }
  return s;
}

inline void restoreGLContext(const SavedCurrent& s) {
  const GlxLibrary& g = glxLibrary();
  if (g.ok && s.display) g.glXMakeCurrent(s.display, s.drawable, s.context);
}

inline bool anyGLContextCurrent() {
  const GlxLibrary& g = glxLibrary();
  return g.ok && g.glXGetCurrentContext() != nullptr;
}

/**
 * The X error handler, for OUR connections only.
 *
 * When the host destroys the editor window, the server destroys our child
 * with it, and the next GLX call on it is a BadDrawable error -- which Xlib's
 * default handler answers by printing and calling exit(). In a plugin that is
 * the host process, gone. So while any surface is alive a handler is
 * installed that swallows errors on the connections this file opened and
 * passes everything else to whatever was installed before.
 */
struct GlxErrorTrap {
  std::mutex lock;
  std::vector<::Display*> displays;
  int (*previous)(::Display*, ::XErrorEvent*) = nullptr;
  bool installed = false;
};

inline GlxErrorTrap& glxErrorTrap() {
  static GlxErrorTrap trap;
  return trap;
}

inline int glxErrorHandler(::Display* display, ::XErrorEvent* event) {
  GlxErrorTrap& t = glxErrorTrap();
  {
    std::lock_guard<std::mutex> lock(t.lock);
    for (::Display* d : t.displays)
      if (d == display) return 0;
  }
  return t.previous ? t.previous(display, event) : 0;
}

class NativeSurface {
public:
  NativeSurface() {}
  NativeSurface(const NativeSurface&) = delete;
  NativeSurface& operator=(const NativeSurface&) = delete;
  ~NativeSurface() { destroy(); }

  std::function<void()> onTick, onRepaint, onDestroyed;

  bool create(void* parent, const SurfaceRequest& request, std::string& error) {
    destroy();
    const GlxLibrary& g = glxLibrary();
    if (!g.ok) {
      error = g.error;
      return false;
    }
    const x11::XlibApi& x = x11::xlib();
    if (!parent) {
      error = "the native parent handle is not an X window";
      return false;
    }
    display_ = x.XOpenDisplay(nullptr);
    if (!display_) {
      error = "no X display could be opened";
      return false;
    }
    parentWindow_ = (::Window) (uintptr_t) parent;
    trapErrors(true);
    const int screen = x.XDefaultScreen(display_);
    void* config = chooseConfig(request, request.multisampling, screen);
    if (!config && request.multisampling) config = chooseConfig(request, false, screen);
    if (!config) {
      error = "no GLX framebuffer configuration matches the requested format";
      destroy();
      return false;
    }
    ::XVisualInfo* visual = g.glXGetVisualFromFBConfig(display_, config);
    if (!visual) {
      error = "the GLX configuration has no X visual";
      destroy();
      return false;
    }
    colormap_ = g.XCreateColormap(display_, RootWindow(display_, screen), visual->visual,
                                  AllocNone);
    ::XSetWindowAttributes swa;
    std::memset(&swa, 0, sizeof(swa));
    swa.colormap = colormap_;
    swa.border_pixel = 0;
    // StructureNotify and Exposure ONLY. Selecting no pointer events is what
    // makes this window transparent to the mouse: X delivers a click to the
    // nearest ancestor that asked for it, which is the editor's own window.
    swa.event_mask = StructureNotifyMask | ExposureMask;
    window_ = x.XCreateWindow(display_, (::Window) (uintptr_t) parent, 0, 0, 1, 1, 0,
                              visual->depth, InputOutput, visual->visual,
                              CWColormap | CWBorderPixel | CWEventMask, &swa);
    x.XFree(visual);
    if (!window_) {
      error = "could not create the GL child window";
      destroy();
      return false;
    }
    context_ = createContext(request, config, screen);
    if (!context_) {
      error = request.version == OpenGLVersion::defaultGLVersion
                  ? "glXCreateNewContext failed"
                  : "the driver cannot create the requested core-profile OpenGL version";
      destroy();
      return false;
    }
    x.XSync(display_, 0);
    return true;
  }

  void destroy() {
    const GlxLibrary& g = glxLibrary();
    const x11::XlibApi& x = x11::xlib();
    std::lock_guard<std::mutex> lock(displayLock_);
    if (display_) {
      if (context_) {
        if (g.glXGetCurrentContext() == context_) g.glXMakeCurrent(display_, 0, nullptr);
        g.glXDestroyContext(display_, context_);
      }
      if (window_ && !windowGone_) x.XDestroyWindow(display_, window_);
      if (colormap_) g.XFreeColormap(display_, colormap_);
      x.XSync(display_, 0);
      trapErrors(false);
      x.XCloseDisplay(display_);
    }
    display_ = nullptr;
    context_ = nullptr;
    window_ = 0;
    parentWindow_ = 0;
    colormap_ = 0;
    windowGone_ = false;
    lastBounds_ = SurfaceBounds();
  }

  void releaseContextAndDC() {}
  bool isAlive() const { return display_ && window_ && context_ && !windowGone_; }

  bool makeActive() {
    return isAlive() && glxLibrary().glXMakeCurrent(display_, window_, context_) != 0;
  }
  void makeInactive() {
    if (display_ && isActive()) glxLibrary().glXMakeCurrent(display_, 0, nullptr);
  }
  bool isActive() const { return context_ && glxLibrary().glXGetCurrentContext() == context_; }

  void swap() {
    if (isAlive()) glxLibrary().glXSwapBuffers(display_, window_);
  }

  /** GLX_EXT_swap_control, then MESA, then SGI -- the three names one
   *  feature has had. Looked up only when the extension string names it,
   *  since glXGetProcAddressARB returns a stub for anything. */
  bool applySwapInterval(int interval) {
    const GlxLibrary& g = glxLibrary();
    if (!display_) return false;
    const char* ext = g.glXQueryExtensionsString(display_, x11::xlib().XDefaultScreen(display_));
    auto has = [ext](const char* name) { return ext && std::strstr(ext, name) != nullptr; };
    if (has("GLX_EXT_swap_control")) {
      auto fn = (void (*)(::Display*, unsigned long, int)) g.glXGetProcAddressARB(
          (const unsigned char*) "glXSwapIntervalEXT");
      if (fn) {
        fn(display_, window_, interval);
        swapInterval_ = interval;
        return true;
      }
    }
    if (has("GLX_MESA_swap_control")) {
      auto fn = (int (*)(unsigned)) g.glXGetProcAddressARB(
          (const unsigned char*) "glXSwapIntervalMESA");
      if (fn && fn((unsigned) interval) == 0) {
        swapInterval_ = interval;
        return true;
      }
    }
    if (has("GLX_SGI_swap_control") && interval > 0) {
      auto fn = (int (*)(int)) g.glXGetProcAddressARB((const unsigned char*) "glXSwapIntervalSGI");
      if (fn && fn(interval) == 0) {
        swapInterval_ = interval;
        return true;
      }
    }
    return false;
  }
  int querySwapInterval() const { return swapInterval_; }

  /** One Display connection, used from two threads, in a process that may
   *  never have called XInitThreads -- a plugin cannot, the host went first.
   *  So every use of it is serialised here: the render thread holds this for
   *  a whole frame, the message thread for each move. */
  void lockFrame() { displayLock_.lock(); }
  void unlockFrame() { displayLock_.unlock(); }

  float scaleFor(const Component& root) const {
    if (!display_ || !parentWindow_ || root.bounds().w <= 0.0f) return 1.0f;
    std::lock_guard<std::mutex> lock(displayLock_);
    ::XWindowAttributes a;
    std::memset(&a, 0, sizeof(a));
    if (!x11::xlib().XGetWindowAttributes(display_, parentWindow_, &a) || a.width <= 0) return 1.0f;
    return (float) a.width / root.bounds().w;
  }

  void setBounds(const SurfaceBounds& b) {
    if (!isAlive() || b == lastBounds_) return;
    const bool visibilityChanged = b.visible != lastBounds_.visible;
    lastBounds_ = b;
    std::lock_guard<std::mutex> lock(displayLock_);
    const x11::XlibApi& x = x11::xlib();
    glxLibrary().XMoveResizeWindow(display_, window_, b.x, b.y, (unsigned) (b.w > 0 ? b.w : 1),
                                   (unsigned) (b.h > 0 ? b.h : 1));
    if (visibilityChanged) {
      if (b.visible)
        x.XMapWindow(display_, window_);
      else
        x.XUnmapWindow(display_, window_);
    }
    x.XFlush(display_);
  }

  /** X has no clock and no way to post to ourselves: a repaint request is
   *  picked up by the next tick, which the owner's event pump drives. */
  void postRepaint() { repaintPending_ = true; }

  /** Drain our connection's events: an Expose is a repaint, a DestroyNotify
   *  for our window means the editor was torn down under us. Called from the
   *  owner's tick, on the message thread. */
  void pumpEvents() {
    if (!display_) return;
    bool expose = false, gone = false;
    {
      std::lock_guard<std::mutex> lock(displayLock_);
      const x11::XlibApi& x = x11::xlib();
      while (x.XPending(display_) > 0) {
        ::XEvent e;
        x.XNextEvent(display_, &e);
        if (e.type == Expose) expose = true;
        if (e.type == DestroyNotify && e.xdestroywindow.window == window_) gone = true;
      }
    }
    if (gone) {
      windowGone_ = true;
      if (onDestroyed) onDestroyed();
      return;
    }
    if ((expose || repaintPending_.exchange(false)) && onRepaint) onRepaint();
  }

  void* rawContext() const { return context_; }
  void* surfaceHandle() const { return (void*) (uintptr_t) window_; }

private:
  void trapErrors(bool add) {
    const GlxLibrary& g = glxLibrary();
    GlxErrorTrap& t = glxErrorTrap();
    std::lock_guard<std::mutex> lock(t.lock);
    if (add) {
      t.displays.push_back(display_);
      if (!t.installed) {
        t.previous = g.XSetErrorHandler(&glxErrorHandler);
        t.installed = true;
      }
      return;
    }
    for (size_t i = 0; i < t.displays.size(); ++i)
      if (t.displays[i] == display_) {
        t.displays.erase(t.displays.begin() + (long) i);
        break;
      }
    if (t.displays.empty() && t.installed) {
      // Restored only if the handler is still OURS. Somebody who installed
      // their own on top of it is owed theirs back, not ours removed from
      // under them.
      int (*current)(::Display*, ::XErrorEvent*) = g.XSetErrorHandler(t.previous);
      if (current != &glxErrorHandler) g.XSetErrorHandler(current);
      t.installed = false;
      t.previous = nullptr;
    }
  }

  void* chooseConfig(const SurfaceRequest& r, bool multisample, int screen) {
    const GlxLibrary& g = glxLibrary();
    std::vector<int> a = {kGlxXRenderable, 1, kGlxDrawableType, kGlxWindowBit, kGlxRenderType,
                          kGlxRgbaBit, kGlxDoubleBuffer, 1, kGlxRedSize, r.format.redBits,
                          kGlxGreenSize, r.format.greenBits, kGlxBlueSize, r.format.blueBits,
                          kGlxAlphaSize, r.format.alphaBits, kGlxDepthSize,
                          r.format.depthBufferBits, kGlxStencilSize, r.format.stencilBufferBits};
    if (multisample && r.format.multisamplingLevel > 1) {
      a.push_back(kGlxSampleBuffers);
      a.push_back(1);
      a.push_back(kGlxSamples);
      a.push_back(r.format.multisamplingLevel);
    }
    a.push_back(0);
    int count = 0;
    void** configs = g.glXChooseFBConfig(display_, screen, a.data(), &count);
    if (!configs) return nullptr;
    void* chosen = count > 0 ? configs[0] : nullptr;
    x11::xlib().XFree(configs);
    return chosen;
  }

  void* createContext(const SurfaceRequest& r, void* config, int screen) {
    const GlxLibrary& g = glxLibrary();
    int major = 0, minor = 0;
    switch (r.version) {
      case OpenGLVersion::openGL3_2: major = 3; minor = 2; break;
      case OpenGLVersion::openGL4_1: major = 4; minor = 1; break;
      case OpenGLVersion::openGL4_3: major = 4; minor = 3; break;
      default: break;
    }
    const char* ext = g.glXQueryExtensionsString(display_, screen);
    if (major > 0) {
      if (!ext || !std::strstr(ext, "GLX_ARB_create_context")) return nullptr;
      auto create = (void* (*) (::Display*, void*, void*, int, const int*))
          g.glXGetProcAddressARB((const unsigned char*) "glXCreateContextAttribsARB");
      if (!create) return nullptr;
      const int attribs[] = {kGlxContextMajor, major, kGlxContextMinor, minor,
                             kGlxContextProfileMask, kGlxCoreProfileBit, 0};
      return create(display_, config, r.shareWith, 1, attribs);
    }
    return g.glXCreateNewContext(display_, config, kGlxRgbaType, r.shareWith, 1);
  }

  ::Display* display_ = nullptr;
  ::Window window_ = 0;
  ::Window parentWindow_ = 0;
  ::Colormap colormap_ = 0;
  void* context_ = nullptr;
  int swapInterval_ = -1;
  std::atomic<bool> windowGone_{false};
  std::atomic<bool> repaintPending_{false};
  mutable std::mutex displayLock_;
  SurfaceBounds lastBounds_;
};

inline const char* platformGLUnavailableReason() {
  const GlxLibrary& g = glxLibrary();
  return g.ok ? "" : g.error.c_str();
}

#else
// ═══ No GL backend ══════════════════════════════════════════════════════════
//
// Linux built without the X11 headers, or a platform with no windowing code
// here. Everything compiles; attachTo fails and says why.

inline void* resolveGLFunction(const char*) { return nullptr; }
struct SavedCurrent {};
inline SavedCurrent saveCurrentGLContext() { return SavedCurrent(); }
inline void restoreGLContext(const SavedCurrent&) {}
inline bool anyGLContextCurrent() { return false; }

class NativeSurface {
public:
  std::function<void()> onTick, onRepaint, onDestroyed;
  bool create(void*, const SurfaceRequest&, std::string& error) {
    error = platformReason();
    return false;
  }
  void destroy() {}
  void releaseContextAndDC() {}
  bool isAlive() const { return false; }
  bool makeActive() { return false; }
  void makeInactive() {}
  bool isActive() const { return false; }
  void swap() {}
  bool applySwapInterval(int) { return false; }
  int querySwapInterval() const { return -1; }
  void lockFrame() {}
  void unlockFrame() {}
  float scaleFor(const Component&) const { return 1.0f; }
  void setBounds(const SurfaceBounds&) {}
  void postRepaint() {}
  void pumpEvents() {}
  void* rawContext() const { return nullptr; }
  void* surfaceHandle() const { return nullptr; }
  static const char* platformReason() {
#if defined(__linux__)
    return "this plugin was built without the X11 headers, so it has no GL surface";
#else
    return "no OpenGL surface backend for this platform";
#endif
  }
};

inline const char* platformGLUnavailableReason() { return NativeSurface::platformReason(); }
#endif

} // namespace gldetail
} // namespace gfx
} // namespace sonore
