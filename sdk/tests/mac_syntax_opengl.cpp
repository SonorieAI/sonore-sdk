// SPDX-License-Identifier: Apache-2.0
// The macOS half of gfx/opengl.h -- NSOpenGLContext through the Objective-C
// runtime -- compiled on Windows. Nothing here runs; the compiler reading it
// is the test, exactly as for mac_syntax_plugin.cpp, and with the same limit:
// it proves every name exists and every call fits its declaration in
// tests/au_shim, not that those declarations match Apple's ABI.
//
// Every inline member of the classes below is type-checked when the header
// is read, so instantiating them here is enough to put the whole Mac surface
// in front of the compiler; the static_assert proves it was the Mac branch
// that was read and not the Windows one.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include <sonore/gfx/opengl.h>

static_assert(sonore::gfx::gldetail::kNSPFADoubleBuffer == 5,
              "the NSOpenGLContext branch of opengl_api.h was not the one compiled");

namespace {

struct Renderer : sonore::gfx::OpenGLRenderer {
  void newOpenGLContextCreated() override {}
  void renderOpenGL() override {}
  void openGLContextClosing() override {}
};

struct View : sonore::gfx::OpenGLAppComponent {
  ~View() override { shutdownOpenGL(); }
  void initialise() override {}
  void render() override {}
  void shutdown() override {}
};

} // namespace

// Never called: this is an OBJECT library, compiled and not linked. An
// external function rather than an inline one, so the compiler generates code
// for it and every template it uses -- each objc_msgSend cast is instantiated
// with its real argument types, which is where a wrong signature shows.
int macSyntaxOpenGL(void* parentView) {
  using namespace sonore::gfx;
  Renderer renderer;
  Component component;
  OpenGLContext context;
  context.setRenderer(&renderer);
  context.setOpenGLVersionRequired(OpenGLVersion::openGL4_1);
  context.setMultisamplingEnabled(true);
  context.setSwapInterval(1);
  if (!context.attachTo(component, parentView)) return 1;
  context.tick();
  context.triggerRepaint();
  context.executeOnGLThread([](OpenGLContext& c) {
    OpenGLShaderProgram program(c);
    program.addVertexShader(OpenGLHelpers::translateVertexShaderToV3("void main() {}"));
    OpenGLTexture texture;
    texture.loadImage(Bitmap(4, 4));
    OpenGLFrameBuffer fb;
    fb.initialise(c, 8, 8, true, true);
    OpenGLGraphicsContextCustomShader shader("void main() { gl_FragColor = vec4(1.0); }");
    shader.fillRect(c, Rect(0, 0, 8, 8));
    std::unique_ptr<OpenGLFrameBuffer> image = OpenGLImageType::create(c, 4, 4);
    c.copyTexture(texture.textureID(), Rect(0, 0, 8, 8), false);
    c.drawComponentImage();
  }, true);
  View view;
  view.attachToNativeWindow(parentView);
  Draggable3DOrientation ball;
  ball.mouseDrag(Point(1, 1));
  context.detach();
  return (int) ball.rotationMatrix().mat[0];
}
