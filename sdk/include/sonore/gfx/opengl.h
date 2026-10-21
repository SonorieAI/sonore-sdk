// SPDX-License-Identifier: Apache-2.0
//
// OpenGL inside a native editor: a context attached to a component, the
// renderer callbacks, shaders, textures, framebuffers, and the component's own
// software-rendered painting composited over the GL picture.
//
// ── Why this exists when everything else here is software ───────────────────
//
// The toolkit rasterises on the CPU on purpose -- identical pixels on every
// machine, nothing to fail to initialise -- and that is still the default and
// still the right answer for knobs and meters. It is the wrong answer for a
// 3D spectrum waterfall, a 60 fps oscilloscope with phosphor decay, or a
// shader-driven visualiser, which is what a plugin reaches for GL to draw. So
// GL is here as an OPT-IN: a component asks for a context, and nothing else in
// the editor changes.
//
// ── How it fits into an editor that is one bitmap ───────────────────────────
//
// The context makes a child surface (opengl_api.h) the size of its component,
// over it, and follows it as the layout moves. The child is transparent to the
// mouse, so the component tree still gets every event.
//
// What the component PAINTS still matters: its paint() and its children --
// labels, a legend, a knob over the scope -- are rasterised in software into a
// transparent Bitmap at the device scale, uploaded as a texture, and drawn over
// the GL picture after renderOpenGL() returns, with premultiplied blending. A
// plugin mixes GL content and ordinary widgets in one rectangle without
// knowing which is which. setComponentPaintingEnabled(false) turns it off for
// a component that draws nothing of its own.
//
// That image is repainted on the message thread at most thirty times a second,
// compared with the last one, and uploaded only when it CHANGED. It is
// repainted on a clock rather than on repaint() because this tree records
// damage on the root, where the editor's own backing store clears it the
// moment it has drawn it -- there is nothing left for a second consumer to
// read. The cost is one software paint of the component per tick; a GL
// component's own paint is usually a few labels.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// By default rendering runs on a thread of its own, paced by the swap interval
// -- the reference's behaviour, and the right one for a plugin, since a vsynced
// swap on the host's UI thread would stall the host's whole interface for up
// to a frame. setRenderThreadEnabled(false) renders on the message thread
// instead, driven by the surface's own timer, for a renderer that must touch
// the component tree or that simply wants no second thread.
//
// Either way the three renderer callbacks arrive on ONE thread, in order:
// newOpenGLContextCreated once, renderOpenGL per frame, openGLContextClosing
// once -- and the closing call always happens while the surface still exists,
// including when the HOST destroys the editor window under us (the surface
// hears its own destruction and shuts the renderer down inside it). After
// closing returns there are no more GL calls on that context, ever.
//
// ── Ownership ───────────────────────────────────────────────────────────────
//
// GL objects belong to the context that made them and can only be deleted
// with that context current. OpenGLShaderProgram, OpenGLTexture and
// OpenGLFrameBuffer release themselves in their destructors when their
// context is current on the destroying thread, and otherwise simply forget the
// handle -- deleting through no context is a crash on some drivers, and the
// objects die with the context anyway. Release them in openGLContextClosing.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>

#include "bitmap.h"
#include "graphics.h"
#include "maths3d.h"
#include "opengl_api.h"

namespace sonore {
namespace gfx {

class OpenGLContext;

/**
 * What a context calls to draw.
 *
 * All three on the context's GL thread, with the context current:
 * created once before the first frame, render once per frame, closing once
 * after the last -- the place to delete every GL object the renderer made.
 */
class OpenGLRenderer {
public:
  /** The context does not own its renderer: deleting it is the caller's. */
  virtual ~OpenGLRenderer() {}
  /** Once, before the first frame: make shaders, textures and buffers here. */
  virtual void newOpenGLContextCreated() = 0;
  /** Once per frame, with the viewport set to the whole surface in device
   *  pixels (OpenGLContext::frameBufferWidth and frameBufferHeight). The
   *  component's own painting is drawn over the result after it returns. */
  virtual void renderOpenGL() = 0;
  /** Once, after the last frame, while the surface still exists: delete
   *  every GL object made in the other two. The renderer is not called
   *  again for this attachment. */
  virtual void openGLContextClosing() = 0;
};

namespace gldetail {

inline OpenGLContext*& currentContextSlot() {
  static thread_local OpenGLContext* slot = nullptr;
  return slot;
}

/** `word` replaced by `with` wherever it stands as a whole identifier. */
inline std::string replaceIdentifier(const std::string& text, const char* word, const char* with) {
  auto isIdent = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  const size_t n = std::strlen(word);
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, n, word) == 0 && (i == 0 || !isIdent(text[i - 1])) &&
        (i + n >= text.size() || !isIdent(text[i + n]))) {
      out += with;
      i += n;
    } else {
      out += text[i++];
    }
  }
  return out;
}

/** The source without a leading #version line, which a translation replaces. */
inline std::string withoutVersionLine(const std::string& code) {
  size_t i = 0;
  while (i < code.size() && (code[i] == ' ' || code[i] == '\t' || code[i] == '\r' || code[i] == '\n'))
    ++i;
  if (code.compare(i, 8, "#version") != 0) return code;
  const size_t end = code.find('\n', i);
  return end == std::string::npos ? std::string() : code.substr(end + 1);
}

/**
 * GLSL 1.10 rewritten as 1.50, for a core profile.
 *
 * Shaders here are written once, in the old dialect every GL can compile --
 * attribute, varying, texture2D, gl_FragColor -- and rewritten when the context
 * is a core profile, which rejects all four. Whole identifiers only, so a
 * variable called `myvarying` survives. The fragment output gets a name of its
 * own and a declaration; with exactly one output it is bound to location 0
 * without asking.
 */
inline std::string translateShaderToV3(const std::string& code, bool fragment) {
  std::string body = withoutVersionLine(code);
  if (fragment) {
    body = replaceIdentifier(body, "varying", "in");
    body = replaceIdentifier(body, "texture2D", "texture");
    body = replaceIdentifier(body, "gl_FragColor", "sonoreFragColour");
    return "#version 150\nout vec4 sonoreFragColour;\n" + body;
  }
  body = replaceIdentifier(body, "attribute", "in");
  body = replaceIdentifier(body, "varying", "out");
  body = replaceIdentifier(body, "texture2D", "texture");
  return "#version 150\n" + body;
}

inline std::string shaderInfoLog(const OpenGLFunctions& gl, GLuint object, bool isProgram) {
  GLint length = 0;
  if (isProgram)
    gl.glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
  else
    gl.glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
  if (length <= 1) return std::string();
  std::string log((size_t) length, '\0');
  GLsizei written = 0;
  if (isProgram)
    gl.glGetProgramInfoLog(object, length, &written, &log[0]);
  else
    gl.glGetShaderInfoLog(object, length, &written, &log[0]);
  log.resize(written > 0 ? (size_t) written : 0);
  return log;
}

/** One stage compiled, or 0 with the driver's log in `error`. */
inline GLuint compileShaderStage(const OpenGLFunctions& gl, GLenum type, const std::string& code,
                                 std::string& error) {
  const GLuint shader = gl.glCreateShader(type);
  if (!shader) {
    error = "glCreateShader failed";
    return 0;
  }
  const GLchar* source = code.c_str();
  gl.glShaderSource(shader, 1, &source, nullptr);
  gl.glCompileShader(shader);
  GLint ok = 0;
  gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    error = shaderInfoLog(gl, shader, false);
    if (error.empty()) error = "the shader did not compile, and the driver gave no log";
    gl.glDeleteShader(shader);
    return 0;
  }
  return shader;
}

/** The quad every texture copy is drawn with: two triangles in a strip, a
 *  texture, an opacity. Owned by a context, created on its GL thread. */
struct TextureQuad {
  GLuint program = 0, buffer = 0, vertexArray = 0;
  GLint imageLocation = -1, opacityLocation = -1;
  bool failed = false;

  bool ensure(const OpenGLFunctions& gl, std::string& error) {
    if (program) return true;
    if (failed || !gl.hasShaders) return false;
    const char* vertex =
        "attribute vec2 position;\n"
        "attribute vec2 textureCoordinate;\n"
        "varying vec2 uv;\n"
        "void main() { uv = textureCoordinate; gl_Position = vec4(position, 0.0, 1.0); }\n";
    const char* fragment =
        "uniform sampler2D image;\n"
        "uniform float opacity;\n"
        "varying vec2 uv;\n"
        "void main() { gl_FragColor = texture2D(image, uv) * opacity; }\n";
    const std::string vs = gl.coreProfile ? translateShaderToV3(vertex, false) : vertex;
    const std::string fs = gl.coreProfile ? translateShaderToV3(fragment, true) : fragment;
    const GLuint v = compileShaderStage(gl, GL_VERTEX_SHADER, vs, error);
    const GLuint f = v ? compileShaderStage(gl, GL_FRAGMENT_SHADER, fs, error) : 0;
    if (!v || !f) {
      if (v) gl.glDeleteShader(v);
      failed = true;
      return false;
    }
    program = gl.glCreateProgram();
    gl.glAttachShader(program, v);
    gl.glAttachShader(program, f);
    gl.glBindAttribLocation(program, 0, "position");
    gl.glBindAttribLocation(program, 1, "textureCoordinate");
    gl.glLinkProgram(program);
    gl.glDeleteShader(v);
    gl.glDeleteShader(f);
    GLint linked = 0;
    gl.glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      error = shaderInfoLog(gl, program, true);
      gl.glDeleteProgram(program);
      program = 0;
      failed = true;
      return false;
    }
    imageLocation = gl.glGetUniformLocation(program, "image");
    opacityLocation = gl.glGetUniformLocation(program, "opacity");
    gl.glGenBuffers(1, &buffer);
    if (gl.hasVertexArrays) gl.glGenVertexArrays(1, &vertexArray);
    return true;
  }

  void release(const OpenGLFunctions& gl) {
    if (program) gl.glDeleteProgram(program);
    if (buffer) gl.glDeleteBuffers(1, &buffer);
    if (vertexArray && gl.glDeleteVertexArrays) gl.glDeleteVertexArrays(1, &vertexArray);
    program = buffer = vertexArray = 0;
    failed = false;
  }
};

constexpr GLenum kBlendDstRgb = 0x80C8, kBlendSrcRgb = 0x80C9, kBlendDstAlpha = 0x80CA,
                 kBlendSrcAlpha = 0x80CB;

/**
 * The bits of GL state a helper here changes, put back afterwards.
 *
 * A renderer that set up its own program, buffer and blend mode and then found
 * them changed because the context composited the component image is a
 * renderer whose SECOND frame draws wrongly -- the most confusing frame to get
 * wrong. So every helper that draws restores what it touched.
 */
struct SavedDrawState {
  GLint program = 0, arrayBuffer = 0, vertexArray = 0, texture = 0, activeTexture = 0;
  GLint blendSrcRgb = 1, blendDstRgb = 0, blendSrcAlpha = 1, blendDstAlpha = 0;
  GLboolean blend = 0, depth = 0, cull = 0;

  void save(const OpenGLFunctions& gl) {
    gl.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    if (gl.hasVertexArrays) gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
    gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    gl.glGetIntegerv(kBlendSrcRgb, &blendSrcRgb);
    gl.glGetIntegerv(kBlendDstRgb, &blendDstRgb);
    gl.glGetIntegerv(kBlendSrcAlpha, &blendSrcAlpha);
    gl.glGetIntegerv(kBlendDstAlpha, &blendDstAlpha);
    blend = gl.glIsEnabled(GL_BLEND);
    depth = gl.glIsEnabled(GL_DEPTH_TEST);
    cull = gl.glIsEnabled(GL_CULL_FACE);
  }

  void restore(const OpenGLFunctions& gl) const {
    gl.glUseProgram((GLuint) program);
    if (gl.hasVertexArrays) gl.glBindVertexArray((GLuint) vertexArray);
    gl.glBindBuffer(GL_ARRAY_BUFFER, (GLuint) arrayBuffer);
    gl.glBindTexture(GL_TEXTURE_2D, (GLuint) texture);
    gl.glActiveTexture((GLenum) activeTexture);
    if (gl.glBlendFuncSeparate)
      gl.glBlendFuncSeparate((GLenum) blendSrcRgb, (GLenum) blendDstRgb, (GLenum) blendSrcAlpha,
                             (GLenum) blendDstAlpha);
    else
      gl.glBlendFunc((GLenum) blendSrcRgb, (GLenum) blendDstRgb);
    if (blend) gl.glEnable(GL_BLEND); else gl.glDisable(GL_BLEND);
    if (depth) gl.glEnable(GL_DEPTH_TEST); else gl.glDisable(GL_DEPTH_TEST);
    if (cull) gl.glEnable(GL_CULL_FACE); else gl.glDisable(GL_CULL_FACE);
  }
};

/** Whether every pixel of a premultiplied image is fully transparent -- an
 *  image there is no point compositing at all. */
inline bool isFullyTransparent(const Bitmap& b) {
  const uint8_t* p = b.data();
  for (size_t i = 3, n = b.sizeInBytes(); i < n; i += 4)
    if (p[i] != 0) return false;
  return true;
}

} // namespace gldetail

/**
 * A GL context attached to a component in a native editor.
 *
 * Configure it (renderer, pixel format, version, threading), then attachTo the
 * component and the native window it is shown in -- the editor's own handle,
 * NativeWindow::handle() or NativeEditor::handle(). detach() before the
 * component goes away; the destructor detaches too, which covers a context
 * held as a member of the component it draws.
 *
 * On X11 there is no clock the surface can own: call tick() from the editor's
 * own tick, as the native editor's peer does for itself. Windows and macOS
 * tick the surface from a timer of its own.
 */
class OpenGLContext {
public:
  /** Unattached, with no renderer: the default pixel format, the
   *  compatibility profile, no multisampling, a render thread, vsync. */
  OpenGLContext() {}
  /** Not copyable: a context is one surface and one thread. */
  OpenGLContext(const OpenGLContext&) = delete;
  /** Not copyable. */
  OpenGLContext& operator=(const OpenGLContext&) = delete;
  /** Detaches: the renderer's closing call runs first if it has not. */
  ~OpenGLContext() { detach(); }

  // ── Configuration ────────────────────────────────────────────────────────

  /** Not owned. May be changed while attached; the next frame uses it. */
  void setRenderer(OpenGLRenderer* renderer) { renderer_ = renderer; }
  /** The renderer set, or null. */
  OpenGLRenderer* renderer() const { return renderer_; }

  /** These four take effect at the next attachTo. */
  void setPixelFormat(const OpenGLPixelFormat& format) { request_.format = format; }
  /** The profile and version to ask for; when the driver lacks it, attachTo
   *  fails rather than giving a lower one. */
  void setOpenGLVersionRequired(OpenGLVersion version) { request_.version = version; }
  /** Ask for OpenGLPixelFormat::multisamplingLevel samples per pixel. A
   *  driver that cannot gives a surface without multisampling, not a
   *  failure. Off by default. */
  void setMultisamplingEnabled(bool enabled) { request_.multisampling = enabled; }
  /** Whether multisampling is asked for; multisamplingSamples() says what
   *  the surface got. */
  bool isMultisamplingEnabled() const { return request_.multisampling; }

  /**
   * Share textures, buffers and programs with another context: a raw HGLRC,
   * NSOpenGLContext* or GLXContext. Two editors of one plugin drawing the same
   * waveform texture want this; it must be set before attachTo, and the other
   * context must outlive this one.
   */
  void setNativeSharedContext(void* nativeContextToShareWith) {
    request_.shareWith = nativeContextToShareWith;
  }
  /** The same, from another of these. It must already be attached. */
  void setSharedContext(const OpenGLContext* other) {
    request_.shareWith = other ? other->rawContext() : nullptr;
  }

  /** Whether the component's own paint() and children are drawn over the GL
   *  picture. On by default. */
  void setComponentPaintingEnabled(bool enabled) { componentPainting_ = enabled; }
  /** Whether the component's painting is drawn over the GL picture. */
  bool isComponentPaintingEnabled() const { return componentPainting_; }

  /** Render frame after frame, paced by the swap interval, rather than only
   *  when triggerRepaint asks. */
  void setContinuousRepainting(bool continuous) {
    continuous_ = continuous;
    wakeRenderThread();
  }
  /** Whether frames follow one another without triggerRepaint. Off by
   *  default. */
  bool isContinuouslyRepainting() const { return continuous_; }

  /** A render thread of its own (the default), or the message thread. Takes
   *  effect at the next attachTo. */
  void setRenderThreadEnabled(bool enabled) { renderThreadEnabled_ = enabled; }
  /** The setting for the next attachTo, not whether a thread is running. */
  bool isRenderThreadEnabled() const { return renderThreadEnabled_; }

  /**
   * Frames per swap: 1 is vsync, 0 is as fast as possible. Applied on the GL
   * thread at the next frame. False when the platform has no swap control --
   * known only once attached; before then the request is stored and true.
   *
   * With 0 and continuous repainting the context still waits a few
   * milliseconds between frames: a hidden window often returns from a swap
   * immediately, and an uncapped loop there is a core spinning for nothing.
   */
  bool setSwapInterval(int framesPerSwap) {
    desiredSwapInterval_ = framesPerSwap < 0 ? 0 : framesPerSwap;
    triggerRepaint();
    return !isAttached() || swapControlAvailable_;
  }
  /** What the driver reports for the context, or the request before attach. */
  int swapInterval() const {
    const int applied = appliedSwapInterval_;
    return applied >= 0 ? applied : desiredSwapInterval_.load();
  }

  // ── Attaching ────────────────────────────────────────────────────────────

  /**
   * Create the surface over `component` inside `nativeParent` and start
   * rendering. False with lastError() set when there is no GL to be had --
   * no driver, a remote session with only the GDI 1.1 renderer, a headless
   * machine -- and the component then simply shows its software painting.
   */
  bool attachTo(Component& component, void* nativeParent) {
    detach();
    error_.clear();
    // The mode, for the whole of this attachment. Read live, a change made
    // while attached sent repaints to a render thread that did not exist, or
    // drew on the message thread while one did.
    threaded_ = renderThreadEnabled_;
    if (!nativeParent) {
      error_ = "attachTo needs the native window the component is shown in";
      return false;
    }
    std::unique_ptr<gldetail::NativeSurface> surface(new gldetail::NativeSurface());
    surface->onTick = [this]() { tick(); };
    surface->onRepaint = [this]() { surfaceWantsRepaint(); };
    surface->onDestroyed = [this]() { surfaceDestroyed(); };
    if (!surface->create(nativeParent, request_, error_)) return false;
    surface_ = std::move(surface);
    component_ = &component;
    surfaceGone_ = false;
    created_ = closed_ = false;
    frameCount_ = 0;
    appliedSwapInterval_ = -1;

    // The entry points are loaded HERE, on the message thread, with the
    // context briefly current -- so a failure is reported by attachTo's return
    // value rather than discovered later on a thread nobody is watching.
    bool ok = false;
    {
      const gldetail::SavedCurrent saved = gldetail::saveCurrentGLContext();
      OpenGLContext* const previous = gldetail::currentContextSlot();
      if (surface_->makeActive()) {
        gldetail::currentContextSlot() = this;
        if (!functions_.load(&gldetail::resolveGLFunction)) {
          error_ = "the OpenGL entry points could not be loaded";
        } else if (!functions_.hasShaders) {
          error_ = "OpenGL " + std::to_string(functions_.majorVersion) + "." +
                   std::to_string(functions_.minorVersion) +
                   " has no shaders; 2.0 or later is required. This is the software renderer a "
                   "remote session or a machine without a GPU driver falls back to";
        } else {
          ok = true;
          describe();
          swapControlAvailable_ = surface_->applySwapInterval(desiredSwapInterval_);
          appliedSwapInterval_ = surface_->querySwapInterval();
          if (!threaded_) contextCreated();
        }
        gldetail::currentContextSlot() = previous;
      } else {
        error_ = "the new OpenGL context could not be made current";
      }
      gldetail::restoreGLContext(saved);
    }
    if (!ok) {
      surface_->destroy();
      surface_.reset();
      component_ = nullptr;
      functions_ = OpenGLFunctions();
      return false;
    }
    followComponent();
    if (threaded_) startRenderThread();
    triggerRepaint();
    return true;
  }

  /**
   * Stop rendering, let the renderer release its GL objects, and destroy the
   * surface. Message thread. Safe to call twice, and safe after the host has
   * already destroyed the window -- that case was handled when it happened.
   * Never from the GL thread itself: it would wait for its own exit.
   */
  void detach() {
    if (!surface_) {
      component_ = nullptr;
      return;
    }
    {
      // Not from the GL thread: it would wait for its own exit. The id is
      // read under the lock the GL thread wrote it under; and renderThread_,
      // the message thread's, is not read here at all, since this may be
      // the GL thread asking.
      std::lock_guard<std::mutex> lock(threadLock_);
      if (glThreadId_ == std::this_thread::get_id()) return;
    }
    shutdownGL();
    surface_->destroy();
    surface_.reset();
    component_ = nullptr;
    surfaceGone_ = false;
    functions_ = OpenGLFunctions();
    std::lock_guard<std::mutex> lock(imageLock_);
    pendingImage_ = Bitmap();
    imagePending_ = false;
    staging_ = Bitmap();
    lastPainted_ = Bitmap();
    uploadImage_ = Bitmap();
  }

  /** Attached, with a surface that still exists. False after the host
   *  destroyed the editor window, even before detach() is called. */
  bool isAttached() const { return component_ != nullptr && surface_ != nullptr && !surfaceGone_; }
  /** The component given to attachTo, until detach; null when not attached.
   *  Still set after the host destroyed the window, until detach(). */
  Component* targetComponent() const { return component_; }

  /** Why the last attachTo failed, in words a log can carry. */
  const std::string& lastError() const { return error_; }

  /** Empty where this platform can create contexts at all; otherwise why it
   *  cannot -- for a plugin deciding up front whether to offer a GL view. */
  static const char* unavailableReason() { return gldetail::platformGLUnavailableReason(); }

  // ── The GL thread ────────────────────────────────────────────────────────

  /** The context current on the calling thread, if it is one of these. */
  static OpenGLContext* currentContext() { return gldetail::currentContextSlot(); }

  /** Current on the calling thread. Inside the renderer callbacks, always. */
  bool isActive() const { return gldetail::currentContextSlot() == this; }

  /**
   * Make this context current on the calling thread. Only meaningful where no
   * other thread holds it: with the render thread disabled, or before attach
   * starts one. False otherwise -- a context is current on one thread at most.
   */
  bool makeActive() {
    if (!surface_ || !surface_->isAlive() || !surface_->makeActive()) return false;
    gldetail::currentContextSlot() = this;
    return true;
  }
  /** Release whichever of these is current on the calling thread, if any.
   *  A context the host made current is left alone. */
  static void deactivateCurrentContext() {
    if (OpenGLContext* c = gldetail::currentContextSlot()) {
      if (c->surface_) c->surface_->makeInactive();
      gldetail::currentContextSlot() = nullptr;
    }
  }

  /** The entry points of THIS context. Valid once attachTo has succeeded. */
  const OpenGLFunctions& functions() const { return functions_; }

  /** The platform context -- an HGLRC, an NSOpenGLContext* or a GLXContext --
   *  or null when not attached. Owned by this; for setNativeSharedContext or
   *  a platform call. */
  void* rawContext() const { return surface_ ? surface_->rawContext() : nullptr; }
  /** The child surface: the HWND, the NSView* or the X window id; null when
   *  not attached. Owned by this. */
  void* nativeSurfaceHandle() const { return surface_ ? surface_->surfaceHandle() : nullptr; }

  /** GL 2.0 shaders, which attachTo requires: true whenever attached. */
  bool areShadersAvailable() const { return functions_.hasShaders; }
  /** A core profile: shaders need `#version 150` or the translate helpers,
   *  and drawing needs a vertex array. */
  bool isCoreProfile() const { return functions_.coreProfile; }
  /** "4.6.13596 Compatibility Profile Context ...", as the driver says it. */
  const std::string& versionString() const { return versionString_; }
  /** "AMD Radeon(TM) Graphics", "Apple M1", "llvmpipe" -- for a log line. */
  const std::string& rendererString() const { return rendererString_; }
  /** Samples per pixel of the surface: 0 or 1 without multisampling. */
  int multisamplingSamples() const { return samples_; }

  /** Device pixels per logical unit of the component, as last measured. */
  float renderingScale() const { return scale_; }
  /** The surface's size in device pixels: the default framebuffer. */
  int frameBufferWidth() const { return deviceWidth_; }
  /** The surface's height in device pixels. */
  int frameBufferHeight() const { return deviceHeight_; }

  /** Frames swapped since attachTo. */
  uint64_t frameCount() const { return frameCount_; }

  /** Ask for a frame. Any thread, and it does not lock: the render thread is
   *  woken, or the message thread is posted to. */
  void triggerRepaint() {
    repaintRequested_ = true;
    if (!surface_) return;
    if (threaded_)
      wakeRenderThread();
    else
      surface_->postRepaint();
  }

  /**
   * Run `work` on the GL thread with the context current.
   *
   * Immediately when called there already (or with the render thread off,
   * from the message thread). Otherwise queued before the next frame; with
   * `blockUntilFinished` this waits for it, and returns false if the context
   * shut down before it ran.
   */
  bool executeOnGLThread(std::function<void(OpenGLContext&)> work, bool blockUntilFinished) {
    if (!work || !surface_ || !surface_->isAlive()) return false;
    if (isActive()) {
      work(*this);
      return true;
    }
    if (!renderThread_.joinable()) {
      const gldetail::SavedCurrent saved = gldetail::saveCurrentGLContext();
      OpenGLContext* const previous = gldetail::currentContextSlot();
      bool ran = false;
      if (surface_->makeActive()) {
        gldetail::currentContextSlot() = this;
        work(*this);
        ran = true;
      }
      gldetail::currentContextSlot() = previous;
      gldetail::restoreGLContext(saved);
      return ran;
    }
    auto item = std::make_shared<WorkItem>();
    item->work = std::move(work);
    {
      std::lock_guard<std::mutex> lock(threadLock_);
      if (stopRequested_ || !threadRunning_) return false;
      workQueue_.push_back(item);
    }
    wakeRenderThread();
    if (!blockUntilFinished) return true;
    std::unique_lock<std::mutex> lock(threadLock_);
    workDone_.wait(lock, [&]() { return item->done || !threadRunning_; });
    return item->done;
  }

  // ── Drawing helpers, for inside renderOpenGL ─────────────────────────────

  /**
   * Draw `texture` into `target` -- device pixels, top-left origin, in the
   * current viewport -- with premultiplied blending and `opacity`.
   *
   * `textureOriginIsBottomLeft` is true for anything GL RENDERED (a
   * framebuffer's texture) and false for a texture loaded from a Bitmap, whose
   * first row is its top. Get it wrong and the picture is upside down, which
   * is why it is an argument and not a guess.
   */
  bool copyTexture(GLuint texture, const Rect& target, bool textureOriginIsBottomLeft,
                   float opacity = 1.0f) {
    const OpenGLFunctions& gl = functions_;
    if (!isActive() || texture == 0) return false;
    std::string error;
    if (!quad_.ensure(gl, error)) {
      if (!error.empty()) error_ = error;
      return false;
    }
    GLint viewport[4] = {0, 0, 0, 0};
    gl.glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return false;
    const float vw = (float) viewport[2], vh = (float) viewport[3];
    const float x0 = target.x / vw * 2.0f - 1.0f, x1 = target.right() / vw * 2.0f - 1.0f;
    const float yTop = 1.0f - target.y / vh * 2.0f, yBottom = 1.0f - target.bottom() / vh * 2.0f;
    const float tTop = textureOriginIsBottomLeft ? 1.0f : 0.0f;
    const float tBottom = textureOriginIsBottomLeft ? 0.0f : 1.0f;
    const GLfloat vertices[16] = {x0, yTop, 0.0f, tTop,    x0, yBottom, 0.0f, tBottom,
                                  x1, yTop, 1.0f, tTop,    x1, yBottom, 1.0f, tBottom};

    gldetail::SavedDrawState saved;
    saved.save(gl);
    if (quad_.vertexArray) gl.glBindVertexArray(quad_.vertexArray);
    gl.glBindBuffer(GL_ARRAY_BUFFER, quad_.buffer);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(vertices), vertices, GL_STREAM_DRAW);
    gl.glEnableVertexAttribArray(0);
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei) sizeof(GLfloat), nullptr);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei) sizeof(GLfloat),
                             (const void*) (2 * sizeof(GLfloat)));
    gl.glUseProgram(quad_.program);
    gl.glUniform1i(quad_.imageLocation, 0);
    gl.glUniform1f(quad_.opacityLocation, opacity);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, texture);
    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!quad_.vertexArray) {
      gl.glDisableVertexAttribArray(0);
      gl.glDisableVertexAttribArray(1);
    }
    saved.restore(gl);
    return true;
  }

  /**
   * The component's software painting, drawn over whatever is in the current
   * framebuffer, filling the viewport. The context does this itself after
   * renderOpenGL; it is public so a renderer can composite the same image into
   * a framebuffer of its own -- a post-processing pass, or a test.
   */
  bool drawComponentImage(float opacity = 1.0f) {
    if (!isActive()) return false;
    uploadPendingComponentImage();
    if (!componentTexture_ || componentImageEmpty_) return true;
    GLint viewport[4] = {0, 0, 0, 0};
    functions_.glGetIntegerv(GL_VIEWPORT, viewport);
    return copyTexture(componentTexture_, Rect(0.0f, 0.0f, (float) viewport[2], (float) viewport[3]),
                       false, opacity);
  }

  // ── The message thread ───────────────────────────────────────────────────

  /**
   * The context's clock, on the message thread: follow the component's
   * position, size and visibility; repaint its image if due; and with the
   * render thread off, render. Windows and macOS call this from the surface's
   * own timer; on X11 the caller's event pump must.
   */
  void tick() {
    if (!surface_ || surfaceGone_) return;
    surface_->pumpEvents();
    if (!surface_ || surfaceGone_ || !surface_->isAlive()) return;
    followComponent();
    paintComponentImageIfDue();
    if (!threaded_ && (continuous_ || repaintRequested_)) renderOnMessageThread();
  }

private:
  /** One job queued by executeOnGLThread for the render thread. Shared with
   *  the caller, which may be waiting on `done` -- set under threadLock_
   *  once the job has run, and never set for a job dropped at shutdown. */
  struct WorkItem {
    std::function<void(OpenGLContext&)> work;
    bool done = false;
  };

  // ── Following the component ──────────────────────────────────────────────

  static int roundToInt(float v) { return (int) std::floor(v + 0.5f); }

  void followComponent() {
    if (!component_ || !surface_) return;
    Component* root = component_->root();
    const float s = surface_->scaleFor(*root);
    const Point origin = component_->localToRoot({0.0f, 0.0f});
    const Rect r(origin.x, origin.y, component_->bounds().w, component_->bounds().h);
    // The visible part: the component's rectangle cut down by every ancestor,
    // since a component scrolled half out of a viewport is drawn clipped by
    // the software painter and the GL surface must agree.
    Rect visible = r;
    for (Component* p = component_->parent(); p != nullptr; p = p->parent()) {
      const Point o = p->localToRoot({0.0f, 0.0f});
      visible = visible.intersection(Rect(o.x, o.y, p->bounds().w, p->bounds().h));
    }
    const bool showing = component_->isShowing() && !r.isEmpty() && !visible.isEmpty();

    // Edges rounded, not origin-and-size: a component at 10.4 with width 20.4
    // ends at 30.8, and rounding the width separately would leave the surface
    // a pixel short of where the software painter stopped.
    const int left = roundToInt(r.x * s), top = roundToInt(r.y * s);
    const int deviceW = roundToInt(r.right() * s) - left;
    const int deviceH = roundToInt(r.bottom() * s) - top;

    gldetail::SurfaceBounds b;
#if defined(__APPLE__)
    // Points: AppKit positions subviews in the same units the tree is in.
    b.x = roundToInt(r.x);
    b.y = roundToInt(r.y);
    b.w = roundToInt(r.right()) - b.x;
    b.h = roundToInt(r.bottom()) - b.y;
#else
    b.x = left;
    b.y = top;
    b.w = deviceW;
    b.h = deviceH;
    if (showing && (visible.x != r.x || visible.y != r.y || visible.w != r.w || visible.h != r.h)) {
      b.clipped = true;
      const int cl = roundToInt(visible.x * s) - left, ct = roundToInt(visible.y * s) - top;
      b.clip = PixelRect(cl, ct, roundToInt(visible.right() * s) - left - cl,
                         roundToInt(visible.bottom() * s) - top - ct);
    }
#endif
    b.visible = showing;
    surface_->setBounds(b);

    const bool changed = deviceW != deviceWidth_ || deviceH != deviceHeight_ ||
                         showing != visible_ || s != scale_;
    scale_ = s;
    logicalWidth_ = r.w;
    logicalHeight_ = r.h;
    deviceWidth_ = deviceW;
    deviceHeight_ = deviceH;
    visible_ = showing;
    if (changed) {
      lastComponentPaint_ = std::chrono::steady_clock::time_point();
      triggerRepaint();
    }
  }

  // ── The component's own painting ─────────────────────────────────────────

  void paintComponentImageIfDue() {
    if (!componentPainting_ || !component_ || !visible_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastComponentPaint_ < std::chrono::milliseconds(30)) return;
    lastComponentPaint_ = now;
    const int w = deviceWidth_, h = deviceHeight_;
    if (w <= 0 || h <= 0) return;
    if (staging_.width() != w || staging_.height() != h)
      staging_.resize(w, h);
    else
      std::memset(staging_.data(), 0, staging_.sizeInBytes());
    {
      Graphics g(staging_);
      g.addTransform(Transform::scaling(scale_, scale_));
      component_->paintTree(g);
    }
    if (staging_.width() == lastPainted_.width() && staging_.height() == lastPainted_.height() &&
        std::memcmp(staging_.data(), lastPainted_.data(), staging_.sizeInBytes()) == 0)
      return;
    std::swap(staging_, lastPainted_);
    {
      std::lock_guard<std::mutex> lock(imageLock_);
      pendingImage_ = lastPainted_;
      pendingImageEmpty_ = gldetail::isFullyTransparent(lastPainted_);
      imagePending_ = true;
    }
    triggerRepaint();
  }

  /** GL thread: the newest painted image, into the component texture. */
  void uploadPendingComponentImage() {
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lock(imageLock_);
      if (imagePending_) {
        std::swap(uploadImage_, pendingImage_);
        componentImageEmpty_ = pendingImageEmpty_;
        imagePending_ = false;
        fresh = true;
      }
    }
    if (!fresh || uploadImage_.isEmpty()) return;
    const OpenGLFunctions& gl = functions_;
    GLint previous = 0;
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    if (!componentTexture_) {
      gl.glGenTextures(1, &componentTexture_);
      gl.glBindTexture(GL_TEXTURE_2D, componentTexture_);
      gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl.glBindTexture(GL_TEXTURE_2D, componentTexture_);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (uploadImage_.width() == componentTextureWidth_ &&
        uploadImage_.height() == componentTextureHeight_) {
      gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uploadImage_.width(), uploadImage_.height(),
                         GL_RGBA, GL_UNSIGNED_BYTE, uploadImage_.data());
    } else {
      gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, uploadImage_.width(), uploadImage_.height(), 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, uploadImage_.data());
      componentTextureWidth_ = uploadImage_.width();
      componentTextureHeight_ = uploadImage_.height();
    }
    gl.glBindTexture(GL_TEXTURE_2D, (GLuint) previous);
  }

  // ── Frames ───────────────────────────────────────────────────────────────

  void contextCreated() {
    if (created_) return;
    created_ = true;
    if (OpenGLRenderer* r = renderer_) r->newOpenGLContextCreated();
  }

  /** The renderer's last call, then everything this class made -- in that
   *  order, so a renderer that composites through drawComponentImage in its
   *  closing call still has the texture. */
  void contextClosing() {
    if (!created_ || closed_) return;
    closed_ = true;
    if (OpenGLRenderer* r = renderer_) r->openGLContextClosing();
    quad_.release(functions_);
    if (componentTexture_) functions_.glDeleteTextures(1, &componentTexture_);
    componentTexture_ = 0;
    componentTextureWidth_ = componentTextureHeight_ = 0;
    componentImageEmpty_ = true;
  }

  /** One frame, with the context current on this thread. */
  void renderFrame() {
    const int w = deviceWidth_, h = deviceHeight_;
    if (!visible_ || w <= 0 || h <= 0) return;
    const OpenGLFunctions& gl = functions_;
    const int wanted = desiredSwapInterval_;
    if (wanted != lastRequestedSwapInterval_) {
      lastRequestedSwapInterval_ = wanted;
      swapControlAvailable_ = surface_->applySwapInterval(wanted);
      appliedSwapInterval_ = surface_->querySwapInterval();
    }
    gl.glViewport(0, 0, w, h);
    if (OpenGLRenderer* r = renderer_) r->renderOpenGL();
    if (componentPainting_) {
      // Into the WINDOW, whatever the renderer left bound: a renderer that
      // forgot to unbind its framebuffer must not take the overlay with it.
      if (gl.hasFramebuffers) gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
      gl.glViewport(0, 0, w, h);
      drawComponentImage();
    }
    surface_->swap();
    ++frameCount_;
  }

  void renderOnMessageThread() {
    if (!surface_ || !surface_->isAlive() || inMessageThreadFrame_) return;
    repaintRequested_ = false;
    inMessageThreadFrame_ = true;
    const gldetail::SavedCurrent saved = gldetail::saveCurrentGLContext();
    OpenGLContext* const previous = gldetail::currentContextSlot();
    if (surface_->makeActive()) {
      gldetail::currentContextSlot() = this;
      runQueuedWork();
      renderFrame();
    }
    gldetail::currentContextSlot() = previous;
    gldetail::restoreGLContext(saved);
    inMessageThreadFrame_ = false;
  }

  void surfaceWantsRepaint() {
    if (threaded_)
      triggerRepaint();
    else {
      repaintRequested_ = true;
      renderOnMessageThread();
    }
  }

  /** The surface is being destroyed by somebody else. Inside this call it
   *  still exists, so the renderer is shut down HERE, on its own thread, with
   *  a working context -- after it, nothing may touch GL on it. */
  void surfaceDestroyed() {
    if (surfaceGone_) return;
    shutdownGL();
    surfaceGone_ = true;
    visible_ = false;
  }

  void shutdownGL() {
    if (renderThread_.joinable()) {
      {
        std::lock_guard<std::mutex> lock(threadLock_);
        stopRequested_ = true;
      }
      wake_.notify_all();
      renderThread_.join();
      stopRequested_ = false;
      return;
    }
    if (!created_ || closed_) return;
    const gldetail::SavedCurrent saved = gldetail::saveCurrentGLContext();
    OpenGLContext* const previous = gldetail::currentContextSlot();
    if (surface_ && surface_->makeActive()) {
      gldetail::currentContextSlot() = this;
      contextClosing();
    } else {
      closed_ = true; // no context to call it with; nothing may be drawn
    }
    gldetail::currentContextSlot() = previous;
    gldetail::restoreGLContext(saved);
  }

  // ── The render thread ────────────────────────────────────────────────────

  void startRenderThread() {
    stopRequested_ = false;
    threadRunning_ = true;
    renderThread_ = std::thread([this]() { renderThreadMain(); });
  }

  void wakeRenderThread() {
    // Deliberately WITHOUT the lock, so triggerRepaint can be called from a
    // thread that must not block. The cost is a possible lost wake-up between
    // the render thread's check and its wait, which the wait's timeout below
    // bounds to a few milliseconds.
    wake_.notify_one();
  }

  void renderThreadMain() {
    {
      // Written here, read by detach() on whichever thread calls it: under
      // the lock, or the message thread raced this write.
      std::lock_guard<std::mutex> lock(threadLock_);
      glThreadId_ = std::this_thread::get_id();
    }
    surface_->lockFrame();
    const bool active = surface_->makeActive();
    surface_->unlockFrame();
    if (active) {
      gldetail::currentContextSlot() = this;
      contextCreated();
      auto lastFrame = std::chrono::steady_clock::now() - std::chrono::seconds(1);
      const auto minimumPeriod = std::chrono::milliseconds(4);
      for (;;) {
        {
          std::unique_lock<std::mutex> lock(threadLock_);
          for (;;) {
            if (stopRequested_ || !workQueue_.empty() || repaintRequested_) break;
            if (continuous_) {
              const auto next = lastFrame + minimumPeriod;
              if (std::chrono::steady_clock::now() >= next) break;
              wake_.wait_until(lock, next);
            } else {
              wake_.wait_for(lock, std::chrono::milliseconds(20));
            }
          }
          if (stopRequested_) break;
        }
        runQueuedWork();
        const bool frame = repaintRequested_.exchange(false) || continuous_;
        if (frame) {
          lastFrame = std::chrono::steady_clock::now();
          surface_->lockFrame();
          renderFrame();
          surface_->unlockFrame();
        }
      }
      runQueuedWork();
      contextClosing();
      surface_->makeInactive();
      gldetail::currentContextSlot() = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(threadLock_);
      threadRunning_ = false;
      // No GL thread now: an id a later thread may be given is not this one.
      glThreadId_ = std::thread::id();
      // Anything still queued will never run; its waiters are told so.
      workQueue_.clear();
    }
    workDone_.notify_all();
  }

  void runQueuedWork() {
    std::deque<std::shared_ptr<WorkItem>> batch;
    {
      std::lock_guard<std::mutex> lock(threadLock_);
      batch.swap(workQueue_);
    }
    if (batch.empty()) return;
    for (auto& item : batch) item->work(*this);
    {
      std::lock_guard<std::mutex> lock(threadLock_);
      for (auto& item : batch) item->done = true;
    }
    workDone_.notify_all();
  }

  void describe() {
    const OpenGLFunctions& gl = functions_;
    const char* v = (const char*) gl.glGetString(GL_VERSION);
    const char* r = (const char*) gl.glGetString(GL_RENDERER);
    versionString_ = v ? v : "";
    rendererString_ = r ? r : "";
    GLint samples = 0;
    gl.glGetIntegerv(GL_SAMPLES, &samples);
    samples_ = samples;
    while (gl.glGetError() != GL_NO_ERROR) {
    }
  }

  // Configuration
  gldetail::SurfaceRequest request_;
  std::atomic<OpenGLRenderer*> renderer_{nullptr};
  std::atomic<bool> componentPainting_{true};
  std::atomic<bool> continuous_{false};
  bool renderThreadEnabled_ = true; // the setting, for the next attachTo
  // renderThreadEnabled_ as attachTo found it: what this attachment does.
  // Atomic: triggerRepaint reads it on any thread.
  std::atomic<bool> threaded_{false};
  std::atomic<int> desiredSwapInterval_{1};
  int lastRequestedSwapInterval_ = 1;
  std::atomic<int> appliedSwapInterval_{-1};
  std::atomic<bool> swapControlAvailable_{false};

  // The surface and what it is attached to
  std::unique_ptr<gldetail::NativeSurface> surface_;
  Component* component_ = nullptr;
  bool surfaceGone_ = false;
  std::string error_;
  OpenGLFunctions functions_;
  std::string versionString_, rendererString_;
  int samples_ = 0;

  // Geometry, written by the message thread, read by the GL thread
  std::atomic<int> deviceWidth_{0}, deviceHeight_{0};
  std::atomic<float> scale_{1.0f};
  std::atomic<bool> visible_{false};
  float logicalWidth_ = 0.0f, logicalHeight_ = 0.0f;

  // Frames
  std::atomic<bool> repaintRequested_{false};
  std::atomic<uint64_t> frameCount_{0};
  bool created_ = false, closed_ = false;
  bool inMessageThreadFrame_ = false;
  gldetail::TextureQuad quad_;

  // The component image: painted on the message thread, uploaded on the GL one
  std::chrono::steady_clock::time_point lastComponentPaint_;
  Bitmap staging_, lastPainted_, pendingImage_, uploadImage_;
  std::mutex imageLock_;
  bool imagePending_ = false, pendingImageEmpty_ = true;
  bool componentImageEmpty_ = true;
  GLuint componentTexture_ = 0;
  int componentTextureWidth_ = 0, componentTextureHeight_ = 0;

  // The render thread
  std::thread renderThread_;
  std::thread::id glThreadId_; // under threadLock_; empty when no GL thread runs
  std::mutex threadLock_;
  std::condition_variable wake_, workDone_;
  bool stopRequested_ = false;
  bool threadRunning_ = false;
  std::deque<std::shared_ptr<WorkItem>> workQueue_;
};

/**
 * Small things a renderer reaches for, gathered where the reference keeps them.
 */
class OpenGLHelpers {
public:
  /** Whether ANY GL context is current on this thread -- ours or the host's. */
  static bool isContextActive() { return gldetail::anyGLContextCurrent(); }

  /** Drain the error queue, so the next glGetError is about the next call. */
  static void resetErrorState() {
    if (OpenGLContext* c = OpenGLContext::currentContext()) {
      const OpenGLFunctions& gl = c->functions();
      for (int i = 0; i < 64 && gl.glGetError() != GL_NO_ERROR; ++i) {
      }
    }
  }

  /** Clear the current framebuffer to `colour`, premultiplied -- the form
   *  every surface here holds, so a translucent clear composites correctly
   *  under the component image. */
  static void clear(Colour colour) {
    if (OpenGLContext* c = OpenGLContext::currentContext()) {
      const OpenGLFunctions& gl = c->functions();
      const PremulColour p = PremulColour::from(colour);
      gl.glClearColor(p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, p.a / 255.0f);
      gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
  }

  /** Scissor to `area`, in device pixels with a TOP-left origin like
   *  everything else here; flipped against the current viewport for GL. */
  static void enableScissorTest(const PixelRect& area) {
    if (OpenGLContext* c = OpenGLContext::currentContext()) {
      const OpenGLFunctions& gl = c->functions();
      GLint viewport[4] = {0, 0, 0, 0};
      gl.glGetIntegerv(GL_VIEWPORT, viewport);
      gl.glEnable(GL_SCISSOR_TEST);
      gl.glScissor(viewport[0] + area.x, viewport[1] + viewport[3] - area.bottom(), area.w, area.h);
    }
  }

  /** Whether the context of these current on the calling thread names the
   *  extension `name` ("GL_ARB_debug_output"). False with none current. */
  static bool isExtensionSupported(const char* name) {
    OpenGLContext* c = OpenGLContext::currentContext();
    return c && c->functions().hasExtension(name);
  }

  /** Any entry point by name, through the platform's own lookup. */
  static void* extensionFunction(const char* name) { return gldetail::resolveGLFunction(name); }

  /** GLSL 1.10 source rewritten for a core profile; see translateShaderToV3. */
  static std::string translateVertexShaderToV3(const std::string& code) {
    return gldetail::translateShaderToV3(code, false);
  }
  /** The same for a fragment shader: `varying` becomes `in`, `texture2D`
   *  becomes `texture`, and gl_FragColor an output declared for it. */
  static std::string translateFragmentShaderToV3(const std::string& code) {
    return gldetail::translateShaderToV3(code, true);
  }

  /** The `#version` line the current context's shaders want. */
  static const char* glslVersionString() {
    OpenGLContext* c = OpenGLContext::currentContext();
    return (c && c->isCoreProfile()) ? "#version 150" : "#version 110";
  }
};

/**
 * Vertex and fragment shaders linked into a program, with the driver's error
 * log when they do not.
 *
 * Built on the GL thread, with the context current. Shaders are compiled as
 * written; for a core profile, pass them through
 * OpenGLHelpers::translate*ToV3 first, or write `#version 150` directly.
 */
class OpenGLShaderProgram {
public:
  /** An empty program for `context`, which must outlive it. Nothing is made
   *  in GL until the first addShader. */
  explicit OpenGLShaderProgram(const OpenGLContext& context) : context_(context) {}
  /** Not copyable: the program is one GL object. */
  OpenGLShaderProgram(const OpenGLShaderProgram&) = delete;
  /** Not copyable. */
  OpenGLShaderProgram& operator=(const OpenGLShaderProgram&) = delete;
  /** release(): deleted if the context is current on this thread, else
   *  only forgotten. */
  ~OpenGLShaderProgram() { release(); }

  /** Compile one stage and attach it. False with lastError() holding the
   *  compiler's log -- line numbers included, which is the whole point. */
  bool addShader(const std::string& code, GLenum type) {
    const OpenGLFunctions* gl = functions();
    if (!gl) return fail("the program's context is not current on this thread");
    if (!gl->hasShaders) return fail("this context has no shaders");
    if (!program_) program_ = gl->glCreateProgram();
    if (!program_) return fail("glCreateProgram failed");
    std::string log;
    const GLuint shader = gldetail::compileShaderStage(*gl, type, code, log);
    if (!shader) return fail(log);
    gl->glAttachShader(program_, shader);
    shaders_.push_back(shader);
    error_.clear();
    return true;
  }
  /** addShader with GL_VERTEX_SHADER. */
  bool addVertexShader(const std::string& code) { return addShader(code, GL_VERTEX_SHADER); }
  /** addShader with GL_FRAGMENT_SHADER. */
  bool addFragmentShader(const std::string& code) { return addShader(code, GL_FRAGMENT_SHADER); }

  /** Bind a vertex attribute to a location before link -- a core profile
   *  draws through locations, and fixing them up front saves a lookup per
   *  draw. */
  void bindAttributeLocation(GLuint location, const char* name) {
    const OpenGLFunctions* gl = functions();
    if (gl && program_) gl->glBindAttribLocation(program_, location, name);
  }

  /** Link the stages added so far, then delete them: a program is linked
   *  once. False with lastError() holding the linker's log; the program is
   *  then deleted too, and a retry starts again from addShader. */
  bool link() {
    const OpenGLFunctions* gl = functions();
    if (!gl) return fail("the program's context is not current on this thread");
    if (!program_ || shaders_.empty()) return fail("nothing to link: add shaders first");
    gl->glLinkProgram(program_);
    GLint linked = 0;
    gl->glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    // The stages are no longer needed once linked (or failed); detaching and
    // deleting them now means a long-lived program holds no shader objects.
    for (GLuint s : shaders_) {
      gl->glDetachShader(program_, s);
      gl->glDeleteShader(s);
    }
    shaders_.clear();
    if (!linked) {
      std::string log = gldetail::shaderInfoLog(*gl, program_, true);
      gl->glDeleteProgram(program_);
      program_ = 0;
      return fail(log.empty() ? "the program did not link, and the driver gave no log" : log);
    }
    error_.clear();
    return true;
  }

  /** The compiler's or linker's log, or why nothing was tried; empty after
   *  a success. */
  const std::string& lastError() const { return error_; }
  /** The GL program name, 0 before the first addShader or after a failed
   *  link or release(). */
  GLuint programID() const { return program_; }

  /** Make this the program in use (glUseProgram), for drawing and for the
   *  setUniform calls. Nothing happens unless the context is current on the
   *  calling thread. */
  void use() const {
    if (const OpenGLFunctions* gl = functions()) gl->glUseProgram(program_);
  }

  /** -1 when the name is not an ACTIVE uniform -- one the compiler did not
   *  optimise away. Setting -1 is silently ignored by GL, by design. */
  GLint uniformLocation(const char* name) const {
    const OpenGLFunctions* gl = functions();
    return (gl && program_) ? gl->glGetUniformLocation(program_, name) : -1;
  }

  // Each of these sets a uniform of the program currently in use(): GL sets
  // uniforms on the bound program, not on the one named.

  /** A `float` uniform, looked up by name on every call (Uniform looks it up
   *  once). Call use() first. Nothing happens unless the context is current
   *  on the calling thread; an unknown name is ignored. */
  void setUniform(const char* name, GLfloat v) {
    if (auto* gl = functions()) gl->glUniform1f(uniformLocation(name), v);
  }
  /** A `vec2` uniform; as the `float` overload. */
  void setUniform(const char* name, GLfloat a, GLfloat b) {
    if (auto* gl = functions()) gl->glUniform2f(uniformLocation(name), a, b);
  }
  /** A `vec3` uniform; as the `float` overload. */
  void setUniform(const char* name, GLfloat a, GLfloat b, GLfloat c) {
    if (auto* gl = functions()) gl->glUniform3f(uniformLocation(name), a, b, c);
  }
  /** A `vec4` uniform; as the `float` overload. */
  void setUniform(const char* name, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    if (auto* gl = functions()) gl->glUniform4f(uniformLocation(name), a, b, c, d);
  }
  /** An `int` or `sampler2D` uniform (a sampler takes its texture unit);
   *  as the `float` overload. */
  void setUniform(const char* name, GLint v) {
    if (auto* gl = functions()) gl->glUniform1i(uniformLocation(name), v);
  }
  /** A `float` array uniform of `count` elements from `values`; as the
   *  `float` overload. */
  void setUniform(const char* name, const GLfloat* values, GLsizei count) {
    if (auto* gl = functions()) gl->glUniform1fv(uniformLocation(name), count, values);
  }
  /** `count` `mat4` uniforms, sixteen floats each from `values`: column-major,
   *  or row-major with `transpose`. As the `float` overload. */
  void setUniformMat4(const char* name, const GLfloat* values, GLsizei count, bool transpose) {
    if (auto* gl = functions())
      gl->glUniformMatrix4fv(uniformLocation(name), count, transpose ? GL_TRUE : GL_FALSE, values);
  }
  /** A Matrix3D is column-major already: no transpose. */
  void setUniformMat4(const char* name, const Matrix3D<float>& m) {
    setUniformMat4(name, m.mat, 1, false);
  }

  /** Delete the program. With the context current, or it is only forgotten. */
  void release() {
    const OpenGLFunctions* gl = functions();
    if (gl) {
      for (GLuint s : shaders_) gl->glDeleteShader(s);
      if (program_) gl->glDeleteProgram(program_);
    }
    shaders_.clear();
    program_ = 0;
  }

  /** A uniform looked up once and set many times -- the per-frame path. */
  struct Uniform {
    /** Look `name` up in `program`, which must be linked and whose context
     *  must be current; -1 (a no-op to set) otherwise. The program's context
     *  must outlive this. */
    Uniform(const OpenGLShaderProgram& program, const char* name)
        : uniformID(program.uniformLocation(name)), context_(program.context_) {}
    /** Set a `float` on the program in use. Nothing happens unless the
     *  program's context is current on the calling thread. */
    void set(GLfloat v) const { if (auto* gl = fns()) gl->glUniform1f(uniformID, v); }
    /** A `vec2`; as the `float` overload. */
    void set(GLfloat a, GLfloat b) const { if (auto* gl = fns()) gl->glUniform2f(uniformID, a, b); }
    /** A `vec3`; as the `float` overload. */
    void set(GLfloat a, GLfloat b, GLfloat c) const {
      if (auto* gl = fns()) gl->glUniform3f(uniformID, a, b, c);
    }
    /** A `vec4`; as the `float` overload. */
    void set(GLfloat a, GLfloat b, GLfloat c, GLfloat d) const {
      if (auto* gl = fns()) gl->glUniform4f(uniformID, a, b, c, d);
    }
    /** An `int`, or a sampler's texture unit; as the `float` overload. */
    void set(GLint v) const { if (auto* gl = fns()) gl->glUniform1i(uniformID, v); }
    /** A `float` array of `count` elements; as the `float` overload. */
    void set(const GLfloat* values, GLsizei count) const {
      if (auto* gl = fns()) gl->glUniform1fv(uniformID, count, values);
    }
    /** `count` `mat4`s of sixteen floats: column-major, or row-major with
     *  `transpose`. As the `float` overload. */
    void setMatrix4(const GLfloat* values, GLsizei count, bool transpose) const {
      if (auto* gl = fns())
        gl->glUniformMatrix4fv(uniformID, count, transpose ? GL_TRUE : GL_FALSE, values);
    }
    /** A Matrix3D, which is column-major already. */
    void setMatrix4(const Matrix3D<float>& m) const { setMatrix4(m.mat, 1, false); }
    /** The location, or -1 when the name is not an active uniform. */
    GLint uniformID;

  private:
    const OpenGLFunctions* fns() const {
      return context_.isActive() ? &context_.functions() : nullptr;
    }
    const OpenGLContext& context_;
  };

  /** A vertex attribute's location, looked up once. */
  struct Attribute {
    /** Look `name` up in `program`, which must be linked and whose context
     *  must be current. */
    Attribute(const OpenGLShaderProgram& program, const char* name)
        : attributeID((GLuint) program.attributeLocation(name)) {}
    /** The location, for glVertexAttribPointer. An attribute that is not
     *  active comes out as -1 cast to GLuint, 0xFFFFFFFF: check before
     *  enabling it. */
    GLuint attributeID;
  };

  /** A vertex attribute's location, or -1 when the name is not an active
   *  attribute, the program is not linked, or the context is not current on
   *  the calling thread. */
  GLint attributeLocation(const char* name) const {
    const OpenGLFunctions* gl = functions();
    return (gl && program_) ? gl->glGetAttribLocation(program_, name) : -1;
  }

private:
  const OpenGLFunctions* functions() const {
    return context_.isActive() ? &context_.functions() : nullptr;
  }
  bool fail(const std::string& why) {
    error_ = why;
    return false;
  }

  const OpenGLContext& context_;
  GLuint program_ = 0;
  std::vector<GLuint> shaders_;
  std::string error_;
};

/**
 * A 2D texture holding a Bitmap's pixels.
 *
 * The Bitmap's premultiplied RGBA goes up as it is -- no conversion, no
 * un-premultiplying -- so it composites with GL_ONE, GL_ONE_MINUS_SRC_ALPHA
 * exactly as the software painter composites it. Its FIRST ROW is the top of
 * the image, so texture coordinate t = 0 is the top; copyTexture's flag for
 * this is false.
 */
class OpenGLTexture {
public:
  /** No texture yet: loadImage or loadPixels makes one. */
  OpenGLTexture() {}
  /** Not copyable: the texture is one GL object. */
  OpenGLTexture(const OpenGLTexture&) = delete;
  /** Not copyable. */
  OpenGLTexture& operator=(const OpenGLTexture&) = delete;
  /** release(): deleted if its context is current on this thread, else only
   *  forgotten. */
  ~OpenGLTexture() { release(); }

  /** `image`'s pixels, as loadPixels takes them. */
  bool loadImage(const Bitmap& image) {
    return loadPixels(image.data(), image.width(), image.height());
  }

  /** Premultiplied RGBA8, `width * 4` bytes per row, top row first.
   *
   *  Into the context current on the calling thread (one of these), which
   *  then owns the texture; loaded again, the same texture is reused. False,
   *  with nothing changed, when no context is current, `pixels` is null, or
   *  a side is 0 or past GL_MAX_TEXTURE_SIZE. Filtered linearly and clamped
   *  at the edges. */
  bool loadPixels(const uint8_t* pixels, int width, int height) {
    OpenGLContext* c = OpenGLContext::currentContext();
    if (!c || !pixels || !isValidSize(width, height)) return false;
    const OpenGLFunctions& gl = c->functions();
    GLint maxSize = 0;
    gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (maxSize > 0 && (width > maxSize || height > maxSize)) return false;
    if (owner_ != c) release();
    owner_ = c;
    GLint previous = 0;
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    if (!id_) gl.glGenTextures(1, &id_);
    gl.glBindTexture(GL_TEXTURE_2D, id_);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels);
    gl.glBindTexture(GL_TEXTURE_2D, (GLuint) previous);
    width_ = width;
    height_ = height;
    return true;
  }

  /** Delete the texture if its context is current on the calling thread --
   *  as in openGLContextClosing -- and otherwise only forget it. Empty
   *  afterwards either way. */
  void release() {
    if (id_ && owner_ && owner_->isActive()) owner_->functions().glDeleteTextures(1, &id_);
    id_ = 0;
    width_ = height_ = 0;
    owner_ = nullptr;
  }

  /** Bind to GL_TEXTURE_2D on the active texture unit. Nothing happens
   *  unless its context is current on the calling thread. */
  void bind() const {
    if (owner_ && owner_->isActive()) owner_->functions().glBindTexture(GL_TEXTURE_2D, id_);
  }
  /** Bind 0 to GL_TEXTURE_2D; as bind(). */
  void unbind() const {
    if (owner_ && owner_->isActive()) owner_->functions().glBindTexture(GL_TEXTURE_2D, 0);
  }

  /** The GL texture name, 0 when nothing is loaded. */
  GLuint textureID() const { return id_; }
  /** The loaded width in pixels, 0 when nothing is loaded. */
  int width() const { return width_; }
  /** The loaded height in pixels, 0 when nothing is loaded. */
  int height() const { return height_; }

  /** Both sides at least 1. Any such size is accepted, power of two or not
   *  (the driver's maximum is checked at load). */
  static bool isValidSize(int width, int height) { return width > 0 && height > 0; }

private:
  OpenGLContext* owner_ = nullptr;
  GLuint id_ = 0;
  int width_ = 0, height_ = 0;
};

/**
 * Render to texture: a framebuffer object with an RGBA8 texture behind it,
 * and optionally a depth and stencil buffer.
 *
 * Pixels read back with readPixels arrive in a Bitmap the right way UP -- GL
 * stores rows bottom-first, and the flip happens here, once, so a caller
 * comparing against a software-rendered Bitmap compares like with like.
 */
class OpenGLFrameBuffer {
public:
  /** Not made yet: initialise makes it. */
  OpenGLFrameBuffer() {}
  /** Not copyable: initialise(OpenGLFrameBuffer&) copies the pixels. */
  OpenGLFrameBuffer(const OpenGLFrameBuffer&) = delete;
  /** Not copyable. */
  OpenGLFrameBuffer& operator=(const OpenGLFrameBuffer&) = delete;
  /** release(): deleted if its context is current on this thread, else
   *  only forgotten. */
  ~OpenGLFrameBuffer() { release(); }

  /** A cleared, transparent `width` x `height` target. With `context` current. */
  bool initialise(OpenGLContext& context, int width, int height, bool withDepthBuffer = false,
                  bool withStencilBuffer = false) {
    release();
    if (!context.isActive() || width <= 0 || height <= 0) return false;
    const OpenGLFunctions& gl = context.functions();
    if (!gl.hasFramebuffers) return false;
    GLint maxSize = 0;
    gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (maxSize > 0 && (width > maxSize || height > maxSize)) return false;
    context_ = &context;
    width_ = width;
    height_ = height;

    GLint previousFrameBuffer = 0, previousTexture = 0, previousRenderBuffer = 0;
    gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFrameBuffer);
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    gl.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderBuffer);

    gl.glGenTextures(1, &texture_);
    gl.glBindTexture(GL_TEXTURE_2D, texture_);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    nullptr);
    gl.glGenFramebuffers(1, &frameBuffer_);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer_);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    if (withDepthBuffer || withStencilBuffer) {
      gl.glGenRenderbuffers(1, &renderBuffer_);
      gl.glBindRenderbuffer(GL_RENDERBUFFER, renderBuffer_);
      if (withDepthBuffer && withStencilBuffer) {
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                     renderBuffer_);
      } else if (withDepthBuffer) {
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                     renderBuffer_);
      } else {
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                     renderBuffer_);
      }
    }
    const bool complete = gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
      gl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      gl.glClear(GL_COLOR_BUFFER_BIT | (withDepthBuffer ? GL_DEPTH_BUFFER_BIT : 0u) |
                 (withStencilBuffer ? GL_STENCIL_BUFFER_BIT : 0u));
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previousFrameBuffer);
    gl.glBindTexture(GL_TEXTURE_2D, (GLuint) previousTexture);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) previousRenderBuffer);
    if (!complete) {
      release();
      return false;
    }
    return true;
  }

  /** A target holding `image`'s pixels, upright. */
  bool initialise(OpenGLContext& context, const Bitmap& image) {
    if (!initialise(context, image.width(), image.height())) return false;
    return writePixels(image, PixelRect(0, 0, image.width(), image.height()));
  }

  /** A copy of another target's pixels, in the same context. */
  bool initialise(OpenGLFrameBuffer& other) {
    if (!other.isValid()) return false;
    Bitmap pixels;
    if (!other.readPixels(pixels, PixelRect(0, 0, other.width(), other.height()))) return false;
    return initialise(*other.context_, pixels);
  }

  /** Delete the framebuffer, its texture and its depth or stencil buffer if
   *  its context is current on the calling thread, and otherwise only forget
   *  them. Invalid afterwards either way; a saved copy is kept. */
  void release() {
    if (context_ && context_->isActive()) {
      const OpenGLFunctions& gl = context_->functions();
      if (frameBuffer_) gl.glDeleteFramebuffers(1, &frameBuffer_);
      if (renderBuffer_) gl.glDeleteRenderbuffers(1, &renderBuffer_);
      if (texture_) gl.glDeleteTextures(1, &texture_);
    }
    frameBuffer_ = renderBuffer_ = texture_ = 0;
    width_ = height_ = 0;
    context_ = nullptr;
  }

  /** Keep the pixels in main memory and free the GPU side -- for a context
   *  that is about to go away and come back. reloadSavedCopy puts them back. */
  void saveAndRelease() {
    if (isValid()) readPixels(savedCopy_, PixelRect(0, 0, width_, height_));
    release();
  }
  /** Remake the target in `context` (current) from the copy saveAndRelease
   *  kept, and drop the copy. False when there is no copy or the target
   *  could not be made; the copy is kept then. The depth and stencil buffers
   *  are not remade. */
  bool reloadSavedCopy(OpenGLContext& context) {
    if (savedCopy_.isEmpty()) return false;
    const bool ok = initialise(context, savedCopy_);
    if (ok) savedCopy_ = Bitmap();
    return ok;
  }

  /** Made, and not released since. */
  bool isValid() const { return frameBuffer_ != 0 && context_ != nullptr; }
  /** The width in pixels, 0 when not made. */
  int width() const { return width_; }
  /** The height in pixels, 0 when not made. */
  int height() const { return height_; }
  /** The colour texture, to draw with -- copyTexture's flag for it is true,
   *  since GL rendered it bottom row first. 0 when not made. */
  GLuint textureID() const { return texture_; }
  /** The GL framebuffer name, 0 when not made. */
  GLuint frameBufferID() const { return frameBuffer_; }

  /** Draw into this from now on, with a viewport covering it. Remembers what
   *  was bound and the viewport, for releaseAsRenderingTarget. */
  bool makeCurrentRenderingTarget() {
    const OpenGLFunctions* gl = active();
    if (!gl) return false;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousTarget_);
    gl->glGetIntegerv(GL_VIEWPORT, previousViewport_);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer_);
    gl->glViewport(0, 0, width_, height_);
    return true;
  }

  /** Bind again the framebuffer and viewport that makeCurrentRenderingTarget
   *  replaced. Nothing happens unless its context is current on the calling
   *  thread. */
  void releaseAsRenderingTarget() {
    const OpenGLFunctions* gl = active();
    if (!gl) return;
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previousTarget_);
    gl->glViewport(previousViewport_[0], previousViewport_[1], previousViewport_[2],
                   previousViewport_[3]);
  }

  /** makeCurrentRenderingTarget, then clear the colour to transparent. The
   *  depth and stencil buffers are not cleared. */
  bool makeCurrentAndClear() {
    if (!makeCurrentRenderingTarget()) return false;
    const OpenGLFunctions* gl = active();
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    return true;
  }

  /** The framebuffer bound for drawing on the current context, 0 being the
   *  window. */
  static GLuint currentFrameBufferTarget() {
    OpenGLContext* c = OpenGLContext::currentContext();
    if (!c || !c->functions().hasFramebuffers) return 0;
    GLint bound = 0;
    c->functions().glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    return (GLuint) bound;
  }

  /** Fill with `colour`, premultiplied. */
  void clear(Colour colour) {
    const OpenGLFunctions* gl = active();
    if (!gl) return;
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer_);
    const PremulColour p = PremulColour::from(colour);
    gl->glClearColor(p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, p.a / 255.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previous);
  }

  /**
   * `area` (top-left origin, like the Bitmap) into `out`, resized to fit.
   * Premultiplied RGBA, exactly as GL holds it.
   */
  bool readPixels(Bitmap& out, const PixelRect& area) {
    const OpenGLFunctions* gl = active();
    const PixelRect a = area.intersection(PixelRect(0, 0, width_, height_));
    if (!gl || a.isEmpty()) return false;
    std::vector<uint8_t> rows((size_t) a.w * (size_t) a.h * 4u);
    GLint previousDraw = 0, previousRead = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousDraw);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer_);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadPixels(a.x, height_ - a.bottom(), a.w, a.h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) previousDraw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) previousRead);
    out.resize(a.w, a.h);
    const size_t stride = (size_t) a.w * 4u;
    for (int y = 0; y < a.h; ++y)
      std::memcpy(out.data() + (size_t) y * stride, rows.data() + (size_t) (a.h - 1 - y) * stride,
                  stride);
    return true;
  }
  /** The whole target into `out`, upright. False when not made or its
   *  context is not current on the calling thread. */
  bool readPixels(Bitmap& out) { return readPixels(out, PixelRect(0, 0, width_, height_)); }

  /** `source` into `area` (top-left origin), flipped into GL's row order. The
   *  source is cropped to the area's size. */
  bool writePixels(const Bitmap& source, const PixelRect& area) {
    const OpenGLFunctions* gl = active();
    const PixelRect a = area.intersection(PixelRect(0, 0, width_, height_));
    if (!gl || a.isEmpty() || source.width() < a.w || source.height() < a.h) return false;
    std::vector<uint8_t> rows((size_t) a.w * (size_t) a.h * 4u);
    const size_t stride = (size_t) a.w * 4u;
    for (int y = 0; y < a.h; ++y)
      std::memcpy(rows.data() + (size_t) (a.h - 1 - y) * stride,
                  source.data() + source.offsetOf(0, y), stride);
    GLint previous = 0;
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    gl->glBindTexture(GL_TEXTURE_2D, texture_);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, a.x, height_ - a.bottom(), a.w, a.h, GL_RGBA,
                        GL_UNSIGNED_BYTE, rows.data());
    gl->glBindTexture(GL_TEXTURE_2D, (GLuint) previous);
    return true;
  }

private:
  const OpenGLFunctions* active() const {
    return (context_ && context_->isActive() && frameBuffer_) ? &context_->functions() : nullptr;
  }

  OpenGLContext* context_ = nullptr;
  GLuint frameBuffer_ = 0, texture_ = 0, renderBuffer_ = 0;
  int width_ = 0, height_ = 0;
  GLint previousTarget_ = 0;
  GLint previousViewport_[4] = {0, 0, 0, 0};
  Bitmap savedCopy_;
};

/**
 * Images that live on the GPU, and the way between them and a Bitmap.
 *
 * The reference's version of this makes Image objects whose pixels are a
 * framebuffer, so its Graphics can draw into them with GL. Here there is one
 * rasteriser and it is the software one, so a GPU image is an
 * OpenGLFrameBuffer, and "drawing into it with Graphics" is honestly what it
 * is: paint() reads the pixels back, lets the ordinary Graphics draw on them,
 * and uploads the result. The picture is identical to drawing the Bitmap
 * directly -- which is the point of having one rasteriser.
 */
class OpenGLImageType {
public:
  /** A transparent GPU image. With `context` current; null when it has no
   *  framebuffer support or the size is past the driver's limit. */
  static std::unique_ptr<OpenGLFrameBuffer> create(OpenGLContext& context, int width, int height) {
    std::unique_ptr<OpenGLFrameBuffer> fb(new OpenGLFrameBuffer());
    if (!fb->initialise(context, width, height)) return nullptr;
    return fb;
  }

  /** A GPU image holding `image`'s pixels. */
  static std::unique_ptr<OpenGLFrameBuffer> fromBitmap(OpenGLContext& context, const Bitmap& image) {
    std::unique_ptr<OpenGLFrameBuffer> fb(new OpenGLFrameBuffer());
    if (!fb->initialise(context, image)) return nullptr;
    return fb;
  }

  /** The GPU image's pixels, upright, in a Bitmap. Empty on failure. */
  static Bitmap toBitmap(OpenGLFrameBuffer& image) {
    Bitmap out;
    if (!image.readPixels(out)) out = Bitmap();
    return out;
  }

  /** Draw on a GPU image with the SDK's Graphics -- in software, round trip
   *  included. `scale` is applied first, for an image held at device size. */
  static bool paint(OpenGLFrameBuffer& image, const std::function<void(Graphics&)>& painter,
                    float scale = 1.0f) {
    Bitmap pixels;
    if (!painter || !image.readPixels(pixels)) return false;
    {
      Graphics g(pixels);
      if (scale != 1.0f) g.addTransform(Transform::scaling(scale, scale));
      painter(g);
    }
    return image.writePixels(pixels, PixelRect(0, 0, pixels.width(), pixels.height()));
  }
};

/**
 * A fragment shader that fills a rectangle -- per-pixel effects the software
 * painter could not afford: a plasma background, a live spectrogram colour
 * map, a glow that follows the audio.
 *
 * The shader is the body of a GLSL 1.10 fragment shader. These are declared
 * for it, and it writes gl_FragColor as PREMULTIPLIED alpha, like everything
 * else this SDK composites:
 *
 *     varying vec2 pixelPos;     // logical pixels, y down, of the target
 *     uniform vec4 screenBounds; // the rectangle being filled: x, y, w, h
 *     uniform float pixelAlpha;  // the opacity fillRect was given
 *
 * It is translated for a core profile automatically.
 */
class OpenGLGraphicsContextCustomShader {
public:
  /** Keeps the source; nothing is compiled until checkCompilation or the
   *  first fillRect. Any thread. */
  explicit OpenGLGraphicsContextCustomShader(std::string fragmentShaderCode)
      : code_(std::move(fragmentShaderCode)) {}
  /** Not copyable: it holds a GL program. */
  OpenGLGraphicsContextCustomShader(const OpenGLGraphicsContextCustomShader&) = delete;
  /** Not copyable. */
  OpenGLGraphicsContextCustomShader& operator=(const OpenGLGraphicsContextCustomShader&) = delete;
  /** release(): freed if the context is current on this thread, else only
   *  forgotten. */
  ~OpenGLGraphicsContextCustomShader() { release(); }

  /** Called after the program is bound and before the draw, to set any
   *  uniforms of the shader's own. */
  std::function<void(OpenGLShaderProgram&)> onShaderActivated;

  /** The shader body as given to the constructor, before the declarations
   *  and any translation are added. */
  const std::string& fragmentShaderCode() const { return code_; }
  /** The compiler's or linker's log from the last failed build, or why none
   *  was tried; empty after a success. A failed build is not retried in the
   *  same context. */
  const std::string& lastError() const { return error_; }

  /** Compile now rather than at the first fill, to report a bad shader at
   *  creation rather than mid-frame. */
  bool checkCompilation(OpenGLContext& context) { return program(context) != nullptr; }

  /** The linked program, or null with lastError() set. */
  OpenGLShaderProgram* program(OpenGLContext& context) {
    if (program_ && owner_ == &context) return program_.get();
    if (failed_ && owner_ == &context) return nullptr;
    release();
    owner_ = &context;
    if (!context.isActive()) {
      error_ = "the context is not current on this thread";
      return nullptr;
    }
    const bool v3 = context.isCoreProfile();
    const std::string vertex =
        "attribute vec2 position;\n"
        "uniform vec2 sonoreTargetSize;\n"
        "varying vec2 pixelPos;\n"
        "void main() {\n"
        "  pixelPos = position;\n"
        "  gl_Position = vec4(position.x / sonoreTargetSize.x * 2.0 - 1.0,\n"
        "                     1.0 - position.y / sonoreTargetSize.y * 2.0, 0.0, 1.0);\n"
        "}\n";
    const std::string fragment = "uniform vec4 screenBounds;\n"
                                 "uniform float pixelAlpha;\n"
                                 "varying vec2 pixelPos;\n" +
                                 gldetail::withoutVersionLine(code_);
    std::unique_ptr<OpenGLShaderProgram> p(new OpenGLShaderProgram(context));
    const bool ok =
        p->addVertexShader(v3 ? gldetail::translateShaderToV3(vertex, false) : vertex) &&
        p->addFragmentShader(v3 ? gldetail::translateShaderToV3(fragment, true) : fragment);
    if (ok) p->bindAttributeLocation(0, "position");
    if (!ok || !p->link()) {
      error_ = p->lastError();
      failed_ = true;
      return nullptr;
    }
    const OpenGLFunctions& gl = context.functions();
    gl.glGenBuffers(1, &buffer_);
    if (gl.hasVertexArrays) gl.glGenVertexArrays(1, &vertexArray_);
    program_ = std::move(p);
    error_.clear();
    return program_.get();
  }

  /**
   * Fill `area` -- LOGICAL pixels of the current render target, top-left
   * origin, the units the component is laid out in -- with the shader, over
   * whatever is there. Inside renderOpenGL, with `context` current.
   */
  bool fillRect(OpenGLContext& context, const Rect& area, float alpha = 1.0f) {
    OpenGLShaderProgram* p = program(context);
    if (!p || area.isEmpty()) return false;
    const OpenGLFunctions& gl = context.functions();
    GLint viewport[4] = {0, 0, 0, 0};
    gl.glGetIntegerv(GL_VIEWPORT, viewport);
    const float scale = context.renderingScale() > 0.0f ? context.renderingScale() : 1.0f;
    const float targetW = (float) viewport[2] / scale, targetH = (float) viewport[3] / scale;
    if (targetW <= 0.0f || targetH <= 0.0f) return false;
    const GLfloat vertices[8] = {area.x,       area.y,         area.x,       area.bottom(),
                                 area.right(), area.y,         area.right(), area.bottom()};
    gldetail::SavedDrawState saved;
    saved.save(gl);
    if (vertexArray_) gl.glBindVertexArray(vertexArray_);
    gl.glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(vertices), vertices, GL_STREAM_DRAW);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * (GLsizei) sizeof(GLfloat), nullptr);
    p->use();
    gl.glUniform2f(p->uniformLocation("sonoreTargetSize"), targetW, targetH);
    gl.glUniform4f(p->uniformLocation("screenBounds"), area.x, area.y, area.w, area.h);
    gl.glUniform1f(p->uniformLocation("pixelAlpha"), alpha);
    if (onShaderActivated) onShaderActivated(*p);
    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.glDisable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);
    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!vertexArray_) gl.glDisableVertexAttribArray(0);
    saved.restore(gl);
    return true;
  }

  /** Free the program and its buffers. With the context current, as in
   *  openGLContextClosing; otherwise they are forgotten. */
  void release() {
    if (owner_ && owner_->isActive()) {
      const OpenGLFunctions& gl = owner_->functions();
      if (buffer_) gl.glDeleteBuffers(1, &buffer_);
      if (vertexArray_ && gl.glDeleteVertexArrays) gl.glDeleteVertexArrays(1, &vertexArray_);
    }
    if (program_) program_->release();
    program_.reset();
    buffer_ = vertexArray_ = 0;
    owner_ = nullptr;
    failed_ = false;
  }

private:
  std::string code_;
  std::string error_;
  std::unique_ptr<OpenGLShaderProgram> program_;
  OpenGLContext* owner_ = nullptr;
  GLuint buffer_ = 0, vertexArray_ = 0;
  bool failed_ = false;
};

/**
 * A component that is a GL view: it owns its context, renders continuously,
 * and turns the three renderer callbacks into initialise / render / shutdown.
 *
 * Call attachToNativeWindow once the component is in an editor. A subclass
 * MUST call shutdownOpenGL() in its own destructor: the render thread calls
 * render() and shutdown(), which are the subclass's, and by the time this
 * base destructor runs the subclass part no longer exists.
 */
class OpenGLAppComponent : public Component, public OpenGLRenderer {
public:
  /** Sets this as the context's renderer, repainting continuously. Nothing
   *  is attached yet. */
  OpenGLAppComponent() {
    openGLContext.setRenderer(this);
    openGLContext.setContinuousRepainting(true);
  }
  /** Detaches without calling shutdown(), which is the subclass's and gone
   *  by now: call shutdownOpenGL() in the subclass's destructor instead. */
  ~OpenGLAppComponent() override {
    // A last resort for a subclass that forgot shutdownOpenGL: no further
    // callbacks into a half-destroyed object, then the surface goes.
    openGLContext.setRenderer(nullptr);
    openGLContext.detach();
  }

  /** newOpenGLContextCreated: on the GL thread, once, before the first
   *  frame. */
  virtual void initialise() = 0;
  /** renderOpenGL: on the GL thread, once per frame. */
  virtual void render() = 0;
  /** openGLContextClosing: on the GL thread, once; delete the GL objects
   *  made in initialise and render here. */
  virtual void shutdown() = 0;

  /** openGLContext.attachTo(*this, nativeParent): the editor's native window.
   *  False with openGLContext.lastError() set when there is no GL. */
  bool attachToNativeWindow(void* nativeParent) {
    return openGLContext.attachTo(*this, nativeParent);
  }
  /** Stop rendering and run shutdown(), then destroy the surface. Message
   *  thread; call it from the subclass's destructor. */
  void shutdownOpenGL() { openGLContext.detach(); }

  /** Frames rendered since the context was created. */
  int frameCounter() const { return frameCounter_; }

  /** The context this component draws through; already given this as its
   *  renderer, with continuous repainting on. Configure it before
   *  attachToNativeWindow. */
  OpenGLContext openGLContext;

private:
  void newOpenGLContextCreated() override {
    frameCounter_ = 0;
    initialise();
  }
  void renderOpenGL() override {
    ++frameCounter_;
    render();
  }
  void openGLContextClosing() override { shutdown(); }

  std::atomic<int> frameCounter_{0};
};

} // namespace gfx
} // namespace sonore
