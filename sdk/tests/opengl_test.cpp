// SPDX-License-Identifier: Apache-2.0
// Sonore SDK -- OpenGL inside a native editor, on a real driver.
//
//   opengl_test
//
// Two halves. The 3D maths runs everywhere and needs no GPU: rotations
// composed against known results, inverses multiplied back to the identity,
// projections taking known points to known corners, slerp, and the arcball.
//
// The GL half opens a hidden editor window, attaches contexts to a component
// in it, and checks what a plugin would rely on with real pixels read back
// from framebuffers: a shader-drawn triangle, a texture made from a Bitmap,
// a custom fragment shader, the component's own software painting composited
// over GL content, a core profile, a shared context, multisampling, the
// renderer callbacks' order and thread, the surface following its component,
// the host destroying the editor window under a running render thread, and
// two hundred attach/detach cycles with the process's GDI, USER and kernel
// handles counted.
//
// The GL half runs on Windows. Where no context can be created -- a CI runner
// with no GPU driver, a remote session with only the GDI 1.1 renderer -- it
// says so and SKIPS rather than failing. The macOS and X11 surfaces are
// compiled elsewhere and are not run by this program.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <sonore/gfx/maths3d.h>
#include <sonore/gfx/opengl.h>
#if defined(_WIN32)
#include <sonore/gfx/window_win32.h>
#endif

using namespace sonore::gfx;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// ── The maths ───────────────────────────────────────────────────────────────

using V = Vector3D<float>;
using M = Matrix3D<float>;
using Q = Quaternion<float>;

static bool approx(float a, float b, float tolerance = 1.0e-4f) {
  return std::fabs(a - b) <= tolerance;
}
static bool approxV(const V& a, const V& b, float tolerance = 1.0e-4f) {
  return approx(a.x, b.x, tolerance) && approx(a.y, b.y, tolerance) && approx(a.z, b.z, tolerance);
}

static void mathsTests() {
  std::printf("\n── Vector3D, Matrix3D, Quaternion, Draggable3DOrientation ──\n");
  const float pi = 3.14159265358979f;

  check(V::xAxis().cross(V::yAxis()) == V::zAxis(), "x cross y is z: right-handed");
  check((V::xAxis() ^ V::yAxis()) == V::zAxis(), "the reference's ^ between vectors is the cross product");
  check(approx(V(1, 2, 3) * V(4, 5, 6), 32.0f), "the reference's * between vectors is the dot product: 32");
  check(approx(V(3, 4, 0).length(), 5.0f), "|(3, 4, 0)| = 5");
  check(V().normalised() == V(), "normalising a zero vector gives zero, not NaN");

  const M rz = M::rotationAroundAxis(V::zAxis(), pi / 2.0f);
  check(approxV(rz.transformPoint(V::xAxis()), V::yAxis()), "90 degrees about z takes x to y");
  check((M::rotationAroundAxis(V::zAxis(), pi / 6.0f) * M::rotationAroundAxis(V::zAxis(), pi / 3.0f))
            .approximatelyEqual(rz, 1.0e-5f),
        "30 then 60 degrees about z compose to 90");

  const V euler(0.3f, -0.7f, 1.1f), p(0.2f, 0.5f, -0.9f);
  V stepwise = M::rotationAroundAxis(V::xAxis(), euler.x).transformPoint(p);
  stepwise = M::rotationAroundAxis(V::yAxis(), euler.y).transformPoint(stepwise);
  stepwise = M::rotationAroundAxis(V::zAxis(), euler.z).transformPoint(stepwise);
  check(approxV(M::rotation(euler).transformPoint(p), stepwise),
        "Euler rotation turns about X first, then Y, then Z");

  const M t = M::translation(V(10, 0, 0));
  check(approxV((t * rz).transformPoint(V::xAxis()), V(10, 1, 0)),
        "(T * R) applied to a point rotates first, then translates");
  check(approxV((rz * t).transformPoint(V::xAxis()), V(0, 11, 0)),
        "(R * T) translates first, then rotates");
  check(t.mat[12] == 10.0f && t.mat[3] == 0.0f && t.at(0, 3) == 10.0f,
        "the translation sits in mat[12]: column-major, as glUniformMatrix4fv takes it");

  const M general(2.0f, 0.5f, 0.0f, 0.1f, -1.0f, 3.0f, 0.25f, 0.0f, 0.5f, 0.0f, 4.0f, 0.2f, 1.0f,
                  2.0f, 3.0f, 1.0f);
  M inverse;
  check(general.invert(&inverse), "a general 4x4 matrix has an inverse");
  check((general * inverse).approximatelyEqual(M(), 1.0e-5f) &&
            (inverse * general).approximatelyEqual(M(), 1.0e-5f),
        "M * inverse(M) and inverse(M) * M are both the identity");
  check(approx(general.determinant() * inverse.determinant(), 1.0f, 1.0e-4f),
        "det(M) * det(inverse(M)) = 1");
  check(approx(M::scaling(V(2, 3, 4)).determinant(), 24.0f), "det(scale 2, 3, 4) = 24");
  check(M::translation(V(1, 2, 3)).inverted().approximatelyEqual(M::translation(V(-1, -2, -3)), 1.0e-6f),
        "a translation's inverse is the opposite translation");
  check(!M::scaling(V(1, 0, 1)).invert(nullptr), "a singular matrix reports that it has no inverse");
  check(rz.transposed().approximatelyEqual(rz.inverted(), 1.0e-6f),
        "a rotation's transpose is its inverse");

  const M perspective = M::fromPerspective(pi / 2.0f, 1.0f, 1.0f, 100.0f);
  check(approx(perspective.transformPoint(V(0, 0, -1)).z, -1.0f) &&
            approx(perspective.transformPoint(V(0, 0, -100)).z, 1.0f, 1.0e-4f),
        "perspective: the near plane maps to z = -1, the far plane to +1");
  check(approxV(perspective.transformPoint(V(1, 1, -1)), V(1, 1, -1)),
        "a 90-degree field of view puts (1, 1) on the near plane in the corner");
  check(approxV(M::fromFrustum(-2, 2, -1, 1, 1, 10).transformPoint(V(2, 1, -1)), V(1, 1, -1)),
        "a frustum's near corner maps to the clip-space corner");
  const M ortho = M::fromOrthographic(0, 200, 100, 0, -1, 1);
  check(approxV(ortho.transformPoint(V(0, 0, 0)), V(-1, 1, 0)) &&
            approxV(ortho.transformPoint(V(200, 100, 0)), V(1, -1, 0)),
        "a y-down orthographic pixel space maps (0, 0) top-left and (200, 100) bottom-right");
  check(approxV(M::lookAt(V(0, 0, 5), V(), V::yAxis()).transformPoint(V()), V(0, 0, -5)),
        "lookAt from z = 5 puts the origin five units in front of the camera");

  const Q q90 = Q::fromAngle(pi / 2.0f, V::zAxis());
  check(approxV(q90.rotate(V::xAxis()), V::yAxis()), "a quaternion of 90 degrees about z takes x to y");
  check(q90.rotationMatrix().approximatelyEqual(rz, 1.0e-6f),
        "its rotation matrix is the axis-angle matrix");
  const Q qa = Q::fromAngle(0.7f, V(1, 2, 3)), qb = Q::fromAngle(-1.3f, V(-2, 0.5f, 1));
  check((qa * qb).rotationMatrix().approximatelyEqual(qa.rotationMatrix() * qb.rotationMatrix(), 1.0e-5f),
        "the quaternion product composes exactly like the matrix product");
  const Q identity = qa * qa.inverted();
  check(approxV(identity.vector, V()) && approx(identity.scalar, 1.0f),
        "q * inverse(q) is the identity rotation");
  const Q half = Q::slerp(Q(), q90, 0.5f);
  check(approxV(half.rotate(V::xAxis()), V(std::cos(pi / 4.0f), std::sin(pi / 4.0f), 0)),
        "slerp halfway from no rotation to 90 degrees is 45 degrees");
  check(approx(Q::slerp(qa, qb, 0.0f).dot(qa), 1.0f, 1.0e-5f) &&
            approx(std::fabs(Q::slerp(qa, qb, 1.0f).dot(qb)), 1.0f, 1.0e-5f),
        "slerp lands exactly on both endpoints");
  check(approxV(Q::slerp(Q(), -q90, 0.5f).rotate(V::xAxis()), half.rotate(V::xAxis())),
        "slerp towards -q takes the short way round, the same as towards q");
  float angle = 0.0f;
  V axis;
  Q::slerp(Q(), q90, 1.0f / 3.0f).toAngleAxis(&angle, &axis);
  check(approx(angle, pi / 6.0f, 1.0e-4f) && approxV(axis, V::zAxis()),
        "a third of the way to 90 degrees is 30 degrees about the same axis");

  Draggable3DOrientation ball;
  ball.setViewport(Rect(0, 0, 200, 200));
  ball.mouseDown(Point(100, 100));
  ball.mouseDrag(Point(200, 100));
  check(approxV(ball.quaternion().rotate(V::zAxis()), V::xAxis(), 1.0e-4f),
        "arcball: a drag from the centre to the right edge turns the front (+z) to face right");
  ball.reset();
  ball.mouseDown(Point(100, 100));
  ball.mouseDrag(Point(100, 200));
  check(approxV(ball.rotationMatrix().transformPoint(V::zAxis()), V(0, -1, 0), 1.0e-4f),
        "arcball: a drag downwards turns the front to face down (y is down on screen)");
  ball.reset();
  ball.mouseDown(Point(100, 100));
  for (int i = 0; i < 2000; ++i)
    ball.mouseDrag(Point(100.0f + 60.0f * std::cos((float) i * 0.01f),
                         100.0f + 60.0f * std::sin((float) i * 0.013f)));
  check(approx(ball.quaternion().length(), 1.0f, 1.0e-5f),
        "two thousand drags later the orientation is still a unit quaternion");
}

#if defined(_WIN32)
// ── The GL half ─────────────────────────────────────────────────────────────

static void pump(int ms) {
  const ULONGLONG until = GetTickCount64() + (ULONGLONG) ms;
  do {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(2);
  } while (GetTickCount64() < until);
}

template <typename Predicate>
static bool waitUntil(Predicate ready, int timeoutMs) {
  const ULONGLONG until = GetTickCount64() + (ULONGLONG) timeoutMs;
  while (GetTickCount64() < until) {
    if (ready()) return true;
    pump(4);
  }
  return ready();
}

static bool pixelIs(const Bitmap& b, int x, int y, int r, int g, int bl, int a, int tolerance = 0) {
  const PremulColour p = b.pixelAt(x, y);
  return std::abs((int) p.r - r) <= tolerance && std::abs((int) p.g - g) <= tolerance &&
         std::abs((int) p.b - bl) <= tolerance && std::abs((int) p.a - a) <= tolerance;
}

static std::string firstLine(const std::string& s) {
  const size_t end = s.find_first_of("\r\n");
  return end == std::string::npos ? s : s.substr(0, end);
}

/** Paints an opaque square in its top-left corner and NOTHING else, so every
 *  other pixel of it must show the GL picture through. */
struct Overlay : Component {
  Colour colour = Colour(255, 0, 0);
  void paint(Graphics& g) override {
    g.setColour(colour);
    g.fillRect(Rect(0, 0, 20, 20));
  }
};

struct Background : Component {
  void paint(Graphics& g) override { g.fillAll(Colour(30, 30, 30)); }
};

/** Counts the callbacks and remembers which thread made each, and whether
 *  they ever arrived out of order. */
struct CountingRenderer : OpenGLRenderer {
  std::atomic<int> created{0}, rendered{0}, closed{0}, outOfOrder{0};
  std::atomic<std::thread::id> createdOn{std::thread::id()}, renderedOn{std::thread::id()},
      closedOn{std::thread::id()};
  Colour clearColour = Colour(0, 0, 255);

  void newOpenGLContextCreated() override {
    if (created != 0 || rendered != 0 || closed != 0) ++outOfOrder;
    createdOn = std::this_thread::get_id();
    ++created;
  }
  void renderOpenGL() override {
    if (created != 1 || closed != 0) ++outOfOrder;
    renderedOn = std::this_thread::get_id();
    OpenGLHelpers::clear(clearColour);
    ++rendered;
  }
  void openGLContextClosing() override {
    if (created != 1 || closed != 0) ++outOfOrder;
    closedOn = std::this_thread::get_id();
    ++closed;
  }
};

/** Device rectangle of a child window, in its parent's client coordinates. */
static RECT childRect(HWND child) {
  RECT r{};
  GetWindowRect(child, &r);
  MapWindowPoints(nullptr, GetParent(child), (POINT*) &r, 2);
  return r;
}

static Bitmap quadrantBitmap() {
  Bitmap b(32, 32);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      PremulColour c;
      if (y < 16)
        c = x < 16 ? PremulColour(255, 0, 0, 255) : PremulColour(0, 255, 0, 255);
      else
        c = x < 16 ? PremulColour(0, 0, 255, 255) : PremulColour(128, 128, 128, 128);
      b.setPixel(x, y, c);
    }
  return b;
}

static std::string shaderFor(const OpenGLContext& c, const std::string& code, bool fragment) {
  if (!c.isCoreProfile()) return code;
  return fragment ? OpenGLHelpers::translateFragmentShaderToV3(code)
                  : OpenGLHelpers::translateVertexShaderToV3(code);
}

/** Everything that needs the context current: run on its GL thread. */
static void sceneChecks(OpenGLContext& c, const char* label) {
  std::printf("  .... scene checks, %s context\n", label);
  const OpenGLFunctions& gl = c.functions();
  check(c.isActive() && OpenGLContext::currentContext() == &c && OpenGLHelpers::isContextActive(),
        "on the GL thread the context is current");

  // ── Compile and link errors carry the driver's log ──
  {
    OpenGLShaderProgram bad(c);
    const bool compiled = bad.addFragmentShader(
        shaderFor(c, "void main() { gl_FragColor = vec4(undeclaredThing, 1.0); }\n", true));
    check(!compiled && !bad.lastError().empty(),
          "a shader that does not compile fails, with the compiler's log");
    std::printf("       (driver: %s)\n", firstLine(bad.lastError()).c_str());
    // A vertex stage with no main() compiles -- it could be a library of
    // functions -- and cannot link. Every driver agrees on that, where a
    // varying read but never written is an error on some and a zero on others
    // (this machine's AMD driver links that one happily).
    OpenGLShaderProgram unlinked(c);
    unlinked.addVertexShader(shaderFor(c, "vec4 helper() { return vec4(0.0); }\n", false));
    unlinked.addFragmentShader(shaderFor(c, "void main() { gl_FragColor = vec4(1.0); }\n", true));
    check(!unlinked.link() && !unlinked.lastError().empty(),
          "a program with no vertex main() fails to link, with the linker's log");
  }

  // ── A coloured triangle, through a uniform matrix ──
  OpenGLShaderProgram triangle(c);
  const bool built =
      triangle.addVertexShader(shaderFor(c,
                                         "attribute vec2 position;\n"
                                         "attribute vec3 colour;\n"
                                         "uniform mat4 transform;\n"
                                         "varying vec3 v;\n"
                                         "void main() { v = colour; gl_Position = transform * vec4(position, 0.0, 1.0); }\n",
                                         false)) &&
      triangle.addFragmentShader(shaderFor(c,
                                           "varying vec3 v;\n"
                                           "void main() { gl_FragColor = vec4(v, 1.0); }\n",
                                           true));
  triangle.bindAttributeLocation(0, "position");
  triangle.bindAttributeLocation(1, "colour");
  check(built && triangle.link(), "a vertex and a fragment shader compile and link");
  if (!triangle.lastError().empty()) std::printf("       (%s)\n", triangle.lastError().c_str());
  const OpenGLShaderProgram::Uniform transformUniform(triangle, "transform");
  const OpenGLShaderProgram::Attribute colourAttribute(triangle, "colour");
  check(transformUniform.uniformID >= 0 && colourAttribute.attributeID == 1,
        "Uniform and Attribute look up the active uniform and the bound attribute location");

  const GLfloat vertices[] = {-1, -1, 1, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1};
  GLuint buffer = 0, vertexArray = 0;
  gl.glGenBuffers(1, &buffer);
  if (gl.hasVertexArrays) {
    gl.glGenVertexArrays(1, &vertexArray);
    gl.glBindVertexArray(vertexArray);
  }
  gl.glBindBuffer(GL_ARRAY_BUFFER, buffer);
  gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(vertices), vertices, GL_STATIC_DRAW);
  gl.glEnableVertexAttribArray(0);
  gl.glEnableVertexAttribArray(1);
  gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), nullptr);
  gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                           (const void*) (2 * sizeof(GLfloat)));

  OpenGLFrameBuffer target;
  check(target.initialise(c, 64, 64, true, true), "a 64 x 64 framebuffer with depth and stencil");
  auto drawTriangle = [&](const M& transform, Bitmap& out) {
    target.makeCurrentRenderingTarget();
    gl.glClearColor(0, 0, 0, 1);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.glDisable(GL_BLEND);
    triangle.use();
    transformUniform.setMatrix4(transform);
    if (vertexArray) gl.glBindVertexArray(vertexArray);
    gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    target.releaseAsRenderingTarget();
    target.readPixels(out);
  };
  Bitmap pixels;
  drawTriangle(M(), pixels);
  check(pixels.width() == 64 && pixels.height() == 64, "the framebuffer reads back 64 x 64");
  const PremulColour nearRed = pixels.pixelAt(1, 62), nearGreen = pixels.pixelAt(60, 63),
                     nearBlue = pixels.pixelAt(0, 1);
  check(nearRed.r > 230 && nearRed.g < 25 && nearRed.b < 25,
        "the bottom-left corner of the image is the red vertex (rows read back upright)");
  check(nearGreen.g > 200 && nearGreen.r < 40 && nearGreen.b < 40,
        "the bottom-right corner is the green vertex");
  check(nearBlue.b > 230 && nearBlue.r < 25 && nearBlue.g < 25, "the top-left corner is the blue vertex");
  check(pixelIs(pixels, 62, 1, 0, 0, 0, 255), "outside the triangle the clear colour is untouched");

  drawTriangle(M::rotationAroundAxis(V::zAxis(), 3.14159265f), pixels);
  check(pixelIs(pixels, 1, 62, 0, 0, 0, 255) && pixels.pixelAt(62, 1).r > 230,
        "a 180-degree rotation uniform moves the red corner to the top-right");
  drawTriangle(M::translation(V(1, 0, 0)), pixels);
  check(pixelIs(pixels, 1, 62, 0, 0, 0, 255) && pixels.pixelAt(33, 62).r > 200,
        "a translation uniform moves it right by half the target (the matrix arrives column-major)");

  gl.glDisableVertexAttribArray(0);
  gl.glDisableVertexAttribArray(1);
  if (vertexArray) {
    gl.glBindVertexArray(0);
    gl.glDeleteVertexArrays(1, &vertexArray);
  }
  gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
  gl.glDeleteBuffers(1, &buffer);
  gl.glUseProgram(0);
  target.release();
  triangle.release();

  // ── A Bitmap as a texture, drawn as a quad ──
  const Bitmap quadrants = quadrantBitmap();
  OpenGLTexture texture;
  check(texture.loadImage(quadrants) && texture.width() == 32 && texture.textureID() != 0,
        "a 32 x 32 Bitmap uploads as a texture");
  OpenGLFrameBuffer canvas;
  canvas.initialise(c, 64, 64);
  canvas.makeCurrentAndClear();
  check(c.copyTexture(texture.textureID(), Rect(0, 0, 64, 64), false),
        "copyTexture draws it into the framebuffer");
  canvas.releaseAsRenderingTarget();
  canvas.readPixels(pixels);
  check(pixelIs(pixels, 16, 16, 255, 0, 0, 255) && pixelIs(pixels, 48, 16, 0, 255, 0, 255) &&
            pixelIs(pixels, 16, 48, 0, 0, 255, 255),
        "the quadrants land where they were in the Bitmap: top stays top, left stays left");
  check(pixelIs(pixels, 48, 48, 128, 128, 128, 128),
        "a half-transparent premultiplied pixel arrives unchanged: no double multiplication");
  canvas.release();
  texture.release();

  // ── Framebuffer pixel I/O ──
  OpenGLFrameBuffer image;
  check(image.initialise(c, quadrants), "a framebuffer initialised from a Bitmap");
  Bitmap back;
  check(image.readPixels(back) && back.sizeInBytes() == quadrants.sizeInBytes() &&
            std::memcmp(back.data(), quadrants.data(), back.sizeInBytes()) == 0,
        "...reads back byte-for-byte identical");
  Bitmap corner;
  image.readPixels(corner, PixelRect(16, 0, 16, 16));
  check(corner.width() == 16 && pixelIs(corner, 0, 0, 0, 255, 0, 255) &&
            pixelIs(corner, 15, 15, 0, 255, 0, 255),
        "reading a sub-rectangle gives that rectangle (top-right quadrant: green)");
  Bitmap white(4, 4);
  white.clear(Colour(255, 255, 255));
  image.writePixels(white, PixelRect(2, 3, 4, 4));
  image.readPixels(back);
  check(pixelIs(back, 2, 3, 255, 255, 255, 255) && pixelIs(back, 5, 6, 255, 255, 255, 255) &&
            pixelIs(back, 6, 3, 255, 0, 0, 255) && pixelIs(back, 2, 7, 255, 0, 0, 255),
        "writePixels replaces exactly the rectangle it was given");
  OpenGLFrameBuffer copy;
  check(copy.initialise(image), "a framebuffer copied from another");
  Bitmap copied;
  copy.readPixels(copied);
  check(std::memcmp(copied.data(), back.data(), back.sizeInBytes()) == 0, "...holds the same pixels");
  image.saveAndRelease();
  check(!image.isValid() && image.reloadSavedCopy(c), "saveAndRelease frees it, reloadSavedCopy restores it");
  image.readPixels(copied);
  check(std::memcmp(copied.data(), back.data(), back.sizeInBytes()) == 0, "...with the same pixels");
  image.clear(Colour(0, 0, 255));
  image.readPixels(copied);
  check(pixelIs(copied, 0, 0, 0, 0, 255, 255) && pixelIs(copied, 31, 31, 0, 0, 255, 255),
        "clear fills it");
  image.release();
  copy.release();

  // ── OpenGLImageType: a GPU image the SDK's Graphics can draw on ──
  {
    std::unique_ptr<OpenGLFrameBuffer> gpu = OpenGLImageType::fromBitmap(c, quadrants);
    check(gpu != nullptr, "OpenGLImageType makes a GPU image from a Bitmap");
    const bool painted = gpu && OpenGLImageType::paint(*gpu, [](Graphics& g) {
      g.setColour(Colour(255, 255, 0));
      g.fillRect(Rect(0, 0, 8, 8));
    });
    const Bitmap result = gpu ? OpenGLImageType::toBitmap(*gpu) : Bitmap();
    check(painted && pixelIs(result, 2, 2, 255, 255, 0, 255) && pixelIs(result, 20, 2, 0, 255, 0, 255),
          "...Graphics paints on it, and only where it painted");
    std::unique_ptr<OpenGLFrameBuffer> blank = OpenGLImageType::create(c, 10, 10);
    const Bitmap empty = blank ? OpenGLImageType::toBitmap(*blank) : Bitmap();
    check(blank && empty.width() == 10 && pixelIs(empty, 5, 5, 0, 0, 0, 0),
          "a new GPU image starts transparent");
  }

  // ── A custom fragment shader filling a rectangle ──
  {
    const float s = c.renderingScale() > 0.0f ? c.renderingScale() : 1.0f;
    OpenGLGraphicsContextCustomShader ramp(
        "void main() {\n"
        "  float t = (pixelPos.x - screenBounds.x) / screenBounds.z;\n"
        "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0) * pixelAlpha;\n"
        "}\n");
    check(ramp.checkCompilation(c), "a custom shader compiles against pixelPos, screenBounds, pixelAlpha");
    if (!ramp.lastError().empty()) std::printf("       (%s)\n", ramp.lastError().c_str());
    OpenGLFrameBuffer strip;
    strip.initialise(c, 64, 16);
    strip.makeCurrentAndClear();
    check(ramp.fillRect(c, Rect(0, 0, 64.0f / s, 16.0f / s)), "...and fills a rectangle");
    strip.releaseAsRenderingTarget();
    strip.readPixels(pixels);
    check(pixelIs(pixels, 0, 8, 2, 0, 0, 255, 2) && pixelIs(pixels, 32, 8, 130, 0, 0, 255, 2) &&
              pixelIs(pixels, 63, 8, 253, 0, 0, 255, 2),
          "pixelPos runs across the rectangle: red ramps 2 -> 130 -> 253");
    strip.makeCurrentAndClear();
    ramp.fillRect(c, Rect(0, 0, 64.0f / s, 16.0f / s), 0.5f);
    strip.releaseAsRenderingTarget();
    strip.readPixels(pixels);
    check(pixelIs(pixels, 63, 8, 127, 0, 0, 128, 2), "pixelAlpha scales the output, premultiplied");
    strip.release();
    ramp.release();
    OpenGLGraphicsContextCustomShader broken("void main() { gl_FragColor = nothingHere; }\n");
    check(!broken.checkCompilation(c) && !broken.lastError().empty(),
          "a broken custom shader reports why");
  }

  // ── OpenGLHelpers ──
  {
    OpenGLFrameBuffer box;
    box.initialise(c, 32, 32);
    box.makeCurrentAndClear();
    OpenGLHelpers::enableScissorTest(PixelRect(0, 0, 16, 8));
    OpenGLHelpers::clear(Colour(255, 255, 255));
    gl.glDisable(GL_SCISSOR_TEST);
    box.releaseAsRenderingTarget();
    box.readPixels(pixels);
    check(pixelIs(pixels, 2, 2, 255, 255, 255, 255) && pixelIs(pixels, 20, 2, 0, 0, 0, 0) &&
              pixelIs(pixels, 2, 12, 0, 0, 0, 0),
          "enableScissorTest takes a top-left rectangle and clips the clear to it");
    box.release();

    gl.glEnable(0x7FFF);
    const bool raised = gl.glGetError() != GL_NO_ERROR;
    gl.glEnable(0x7FFF);
    OpenGLHelpers::resetErrorState();
    check(raised && gl.glGetError() == GL_NO_ERROR, "resetErrorState drains the error queue");

    std::string first;
    if (gl.isVersionAtLeast(3, 0)) {
      if (const GLubyte* name = gl.glGetStringi(GL_EXTENSIONS, 0)) first = (const char*) name;
    } else if (const GLubyte* all = gl.glGetString(GL_EXTENSIONS)) {
      first = firstLine(std::string((const char*) all).substr(0, std::string((const char*) all).find(' ')));
    }
    check(!first.empty() && OpenGLHelpers::isExtensionSupported(first.c_str()) &&
              !OpenGLHelpers::isExtensionSupported(first.substr(0, first.size() - 1).c_str()),
          "isExtensionSupported finds a listed extension, and not a prefix of it");
    check(std::string(OpenGLHelpers::glslVersionString()) ==
              (c.isCoreProfile() ? "#version 150" : "#version 110"),
          "glslVersionString matches the profile");
    check(OpenGLHelpers::extensionFunction("glGenFramebuffers") != nullptr,
          "extensionFunction resolves an entry point by name");
  }
}

/** The component's own painting, composited by the context into a
 *  framebuffer the size of the surface, over opaque blue. */
static Bitmap compositeOf(OpenGLContext& context) {
  Bitmap out;
  context.executeOnGLThread(
      [&](OpenGLContext& c) {
        OpenGLFrameBuffer fb;
        if (!fb.initialise(c, c.frameBufferWidth(), c.frameBufferHeight())) return;
        fb.makeCurrentRenderingTarget();
        OpenGLHelpers::clear(Colour(0, 0, 255));
        c.drawComponentImage();
        fb.releaseAsRenderingTarget();
        fb.readPixels(out);
        fb.release();
      },
      true);
  return out;
}

struct Counts {
  DWORD gdi = 0, user = 0, handles = 0;
  size_t bytes = 0;
};
static Counts processCounts() {
  Counts c;
  c.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  c.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  GetProcessHandleCount(GetCurrentProcess(), &c.handles);
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &pmc, sizeof(pmc));
  c.bytes = (size_t) pmc.PrivateUsage;
  return c;
}

struct AppCounters {
  std::atomic<int> initialised{0}, rendered{0}, shutDown{0};
};

struct Spinner : OpenGLAppComponent {
  explicit Spinner(AppCounters& counters) : counters_(counters) {}
  ~Spinner() override { shutdownOpenGL(); }
  void initialise() override { ++counters_.initialised; }
  void render() override {
    ++counters_.rendered;
    OpenGLHelpers::clear(Colour(0, 128, 0));
  }
  void shutdown() override { ++counters_.shutDown; }
  AppCounters& counters_;
};

static int glTests() {
  std::printf("\n── OpenGL, on this machine's driver ──\n");
  const char* reason = OpenGLContext::unavailableReason();
  if (reason && *reason) {
    std::printf("  SKIPPED -- no OpenGL here: %s\n", reason);
    return 0;
  }

  Background root;
  Overlay view;
  root.addChild(&view);
  view.setBounds(Rect(10, 10, 64, 48));
  NativeWindow window;
  if (!window.open(nullptr, root, 200, 150)) {
    std::printf("  SKIPPED -- no window could be opened to attach a context to\n");
    return 0;
  }
  HWND parent = (HWND) window.handle();
  const std::thread::id mainThread = std::this_thread::get_id();

  // ── Attach, render-thread mode ──
  CountingRenderer renderer;
  OpenGLContext context;
  context.setRenderer(&renderer);
  if (!context.attachTo(view, parent)) {
    std::printf("  SKIPPED -- no OpenGL context could be created: %s\n", context.lastError().c_str());
    return 0;
  }
  std::printf("  GL_VERSION  %s\n  GL_RENDERER %s\n", context.versionString().c_str(),
              context.rendererString().c_str());
  check(waitUntil([&]() { return renderer.rendered >= 1; }, 3000), "the render thread draws a first frame");
  check(renderer.created == 1 && renderer.createdOn.load() != mainThread &&
            renderer.renderedOn.load() == renderer.createdOn.load(),
        "created once, on the render thread, which is also where frames are drawn");
  check(!context.isActive() && OpenGLContext::currentContext() == nullptr &&
            !OpenGLHelpers::isContextActive(),
        "after attachTo nothing is left current on the calling thread");
  check(context.areShadersAvailable() && context.functions().hasFramebuffers,
        "the context has shaders and framebuffers");

  HWND surface = (HWND) context.nativeSurfaceHandle();
  pump(60);
  const float s = context.renderingScale();
  RECT r = childRect(surface);
  auto dev = [s](float v) { return (int) std::floor(v * s + 0.5f); };
  check(GetParent(surface) == parent && r.left == dev(10) && r.top == dev(10) &&
            r.right == dev(74) && r.bottom == dev(58),
        "the GL surface is a child of the editor window, over the component, in device pixels");
  check(context.frameBufferWidth() == dev(74) - dev(10) && context.frameBufferHeight() == dev(58) - dev(10),
        "...and the framebuffer is that size");
  const POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  POINT screen = centre;
  ClientToScreen(parent, &screen);
  check(SendMessageW(surface, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) == HTTRANSPARENT,
        "the surface is transparent to the mouse, so the component tree still gets every click");
  check((GetWindowLongPtrW(parent, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
        "the editor window stops painting over the surface (WS_CLIPCHILDREN)");

  // ── Real pixels ──
  check(context.executeOnGLThread([](OpenGLContext& c) { sceneChecks(c, "compatibility"); }, true),
        "executeOnGLThread ran the scene on the GL thread and waited for it");

  // ── The component's own painting, over the GL picture ──
  pump(150);
  Bitmap composite = compositeOf(context);
  const int fw = context.frameBufferWidth(), fh = context.frameBufferHeight();
  const int edge = (int) std::ceil(20.0f * s) + 1;
  check(composite.width() == fw && pixelIs(composite, 2, 2, 255, 0, 0, 255),
        "the component's painted square is composited over the GL content");
  check(pixelIs(composite, fw - 3, fh - 3, 0, 0, 255, 255) && pixelIs(composite, edge, 2, 0, 0, 255, 255),
        "where the component painted nothing, the GL picture shows through");
  view.colour = Colour(0, 255, 0);
  view.repaint();
  pump(150);
  composite = compositeOf(context);
  check(pixelIs(composite, 2, 2, 0, 255, 0, 255), "a change to the component's painting reaches the GL surface");

  // ── Frames on demand, and continuously ──
  pump(100);
  const uint64_t still = context.frameCount();
  pump(250);
  check(context.frameCount() <= still + 1, "nothing is drawn while nothing asks");
  context.triggerRepaint();
  check(waitUntil([&]() { return context.frameCount() > still; }, 1000), "triggerRepaint draws a frame");
  context.setContinuousRepainting(true);
  const uint64_t before = context.frameCount();
  pump(300);
  const uint64_t continuousFrames = context.frameCount() - before;
  std::printf("       (%llu frames in 300 ms, continuous)\n", (unsigned long long) continuousFrames);
  check(continuousFrames >= 5, "continuous repainting keeps drawing");
  context.setSwapInterval(0);
  pump(100);
  check(context.swapInterval() == 0, "setSwapInterval(0) reaches the driver");
  context.setSwapInterval(1);
  pump(100);
  check(context.swapInterval() == 1, "setSwapInterval(1) -- vsync -- reaches the driver");
  context.setContinuousRepainting(false);
  pump(50);

  // ── Following the component ──
  view.setBounds(Rect(30, 20, 80, 60));
  pump(80);
  r = childRect(surface);
  check(r.left == dev(30) && r.top == dev(20) && r.right == dev(110) && r.bottom == dev(80) &&
            context.frameBufferWidth() == dev(110) - dev(30),
        "moving and resizing the component moves and resizes the surface");
  view.setVisible(false);
  pump(80);
  check((GetWindowLongPtrW(surface, GWL_STYLE) & WS_VISIBLE) == 0, "hiding the component hides the surface");
  const uint64_t hiddenFrames = context.frameCount();
  context.triggerRepaint();
  pump(100);
  check(context.frameCount() == hiddenFrames, "...and nothing is drawn while it is hidden");
  view.setVisible(true);
  pump(80);
  check((GetWindowLongPtrW(surface, GWL_STYLE) & WS_VISIBLE) != 0, "showing it shows the surface again");

  Component clipper;
  clipper.setBounds(Rect(0, 0, 50, 50));
  root.addChild(&clipper);
  clipper.addChild(&view);
  view.setBounds(Rect(30, 30, 64, 48));
  pump(80);
  RECT box{};
  const int kind = GetWindowRgnBox(surface, &box);
  check(kind == SIMPLEREGION && box.left == 0 && box.top == 0 && box.right == dev(50) - dev(30) &&
            box.bottom == dev(50) - dev(30),
        "a component half outside its parent clips the surface to the visible part");
  root.addChild(&view);
  root.removeChild(&clipper);
  view.setBounds(Rect(10, 10, 64, 48));
  pump(80);
  check(GetWindowRgnBox(surface, &box) == ERROR, "...and the clip is removed when it fits again");

  // ── Detach ──
  const int renderedBeforeDetach = renderer.rendered;
  context.detach();
  check(renderer.closed == 1 && renderer.closedOn.load() == renderer.createdOn.load(),
        "detach closes the renderer once, on its own thread");
  check(renderer.outOfOrder == 0, "created, rendered, closed: never out of order");
  check(!context.isAttached() && !IsWindow(surface), "the surface window is gone");
  check((GetWindowLongPtrW(parent, GWL_STYLE) & WS_CLIPCHILDREN) == 0,
        "the editor window's style is put back as it was");
  pump(100);
  check(renderer.rendered == renderedBeforeDetach, "no frame after detach");

  // ── Message-thread mode ──
  {
    CountingRenderer r2;
    OpenGLContext onMessageThread;
    onMessageThread.setRenderer(&r2);
    onMessageThread.setRenderThreadEnabled(false);
    check(onMessageThread.attachTo(view, parent) && r2.created == 1 &&
              r2.createdOn.load() == mainThread,
          "message-thread mode: created during attachTo, on the calling thread");
    onMessageThread.triggerRepaint();
    check(waitUntil([&]() { return r2.rendered >= 1; }, 2000) && r2.renderedOn.load() == mainThread,
          "...and frames are drawn on the message thread");
    // The setting is for the NEXT attachTo. Read live, turning it on here
    // sent triggerRepaint to wake a render thread that does not exist and
    // stopped tick() rendering: this attachment drew nothing more.
    onMessageThread.setRenderThreadEnabled(true);
    const int drawnBefore = r2.rendered;
    onMessageThread.triggerRepaint();
    check(waitUntil([&]() { return r2.rendered > drawnBefore; }, 2000) && r2.renderedOn.load() == mainThread,
          "setRenderThreadEnabled while attached waits for the next attachTo: frames go on, on the message thread");
    onMessageThread.setRenderThreadEnabled(false);
    bool inline_ = false;
    onMessageThread.executeOnGLThread(
        [&](OpenGLContext& c) { inline_ = c.isActive() && std::this_thread::get_id() == mainThread; },
        false);
    check(inline_ && !onMessageThread.isActive(),
          "executeOnGLThread runs at once, current, and leaves nothing current behind");

    // A host that has its own context current on its UI thread must find it
    // still current after a plugin attaches one.
    check(onMessageThread.makeActive(), "a context made current on the message thread (the 'host's')");
    const HGLRC host = gldetail::saveCurrentGLContext().context;
    CountingRenderer r3;
    OpenGLContext guest;
    guest.setRenderer(&r3);
    check(guest.attachTo(view, parent) && gldetail::saveCurrentGLContext().context == host,
          "attaching another context puts the host's current context back");
    OpenGLContext::deactivateCurrentContext();
    guest.detach();
    check(r3.closed == 1 || r3.created == 0, "the guest closes cleanly");
    onMessageThread.detach();
    check(r2.closed == 1 && r2.closedOn.load() == mainThread && r2.outOfOrder == 0,
          "message-thread mode closes on the message thread, in order");
  }

  // ── Core profile, multisampling, and a shared context ──
  {
    CountingRenderer rc;
    OpenGLContext core;
    core.setRenderer(&rc);
    core.setOpenGLVersionRequired(OpenGLVersion::openGL3_2);
    core.setMultisamplingEnabled(true);
    OpenGLPixelFormat format;
    format.multisamplingLevel = 4;
    core.setPixelFormat(format);
    const bool coreOk = core.attachTo(view, parent);
    check(coreOk, "a 3.2 core profile context with 4x multisampling attaches");
    if (!coreOk) std::printf("       (%s)\n", core.lastError().c_str());
    if (coreOk) {
      std::printf("       (core: %s, %d samples)\n", core.versionString().c_str(),
                  core.multisamplingSamples());
      check(core.isCoreProfile() && core.functions().isVersionAtLeast(3, 2) &&
                core.functions().hasVertexArrays,
            "...it IS a core profile, 3.2 or later, with vertex arrays");
      check(core.multisamplingSamples() >= 2, "...and the surface is multisampled");
      check(core.executeOnGLThread([](OpenGLContext& c) { sceneChecks(c, "core 3.2"); }, true),
            "the same scene runs on the core profile, shaders translated");

      CountingRenderer rs;
      OpenGLContext shared;
      shared.setRenderer(&rs);
      shared.setOpenGLVersionRequired(OpenGLVersion::openGL3_2);
      shared.setMultisamplingEnabled(true);
      shared.setPixelFormat(format);
      shared.setSharedContext(&core);
      check(shared.attachTo(view, parent), "a second context sharing the first attaches");
      OpenGLTexture made;
      const Bitmap quadrants = quadrantBitmap();
      core.executeOnGLThread([&](OpenGLContext&) { made.loadImage(quadrants); }, true);
      Bitmap seen;
      bool known = false;
      shared.executeOnGLThread(
          [&](OpenGLContext& c) {
            known = c.functions().glIsTexture(made.textureID()) == GL_TRUE;
            OpenGLFrameBuffer fb;
            fb.initialise(c, 32, 32);
            fb.makeCurrentAndClear();
            c.copyTexture(made.textureID(), Rect(0, 0, 32, 32), false);
            fb.releaseAsRenderingTarget();
            fb.readPixels(seen);
            fb.release();
          },
          true);
      check(known && pixelIs(seen, 4, 4, 255, 0, 0, 255) && pixelIs(seen, 28, 4, 0, 255, 0, 255),
            "a texture made in one context is drawn by the context sharing it");
      core.executeOnGLThread([&](OpenGLContext&) { made.release(); }, true);
      shared.detach();
    }
    core.detach();
    check(rc.closed == rc.created && rc.outOfOrder == 0, "the core context closes in order");
  }

  // ── The host destroys the editor window under a running render thread ──
  {
    Background root2;
    Overlay view2;
    root2.addChild(&view2);
    view2.setBounds(Rect(5, 5, 50, 40));
    NativeWindow doomedWindow;
    doomedWindow.open(nullptr, root2, 120, 90);
    CountingRenderer rd;
    OpenGLContext doomed;
    doomed.setRenderer(&rd);
    doomed.setContinuousRepainting(true);
    check(doomed.attachTo(view2, doomedWindow.handle()), "a continuously rendering context in a second window");
    check(waitUntil([&]() { return rd.rendered >= 3; }, 2000), "...is drawing frames");
    HWND doomedSurface = (HWND) doomed.nativeSurfaceHandle();
    doomedWindow.close();
    check(rd.closed == 1 && rd.closedOn.load() == rd.createdOn.load(),
          "destroying the editor window closes the renderer inside the destruction, on its thread");
    check(!doomed.isAttached() && !IsWindow(doomedSurface), "the context knows its surface is gone");
    const int frozen = rd.rendered;
    pump(120);
    check(rd.rendered == frozen, "no frame is drawn after the window is gone");
    check(!doomed.executeOnGLThread([](OpenGLContext&) {}, true), "work for a dead context is refused");
    doomed.detach();
    check(rd.closed == 1 && rd.outOfOrder == 0, "a detach afterwards closes nothing twice");
  }

  // ── OpenGLAppComponent ──
  {
    AppCounters counters;
    {
      Spinner spinner(counters);
      root.addChild(&spinner);
      spinner.setBounds(Rect(100, 60, 60, 40));
      check(spinner.attachToNativeWindow(parent), "an OpenGLAppComponent attaches itself");
      check(waitUntil([&]() { return spinner.frameCounter() >= 3; }, 2000) && counters.initialised == 1,
            "...initialises once and renders continuously");
    }
    check(counters.shutDown == 1, "...and shuts down once when it is destroyed");
  }

  // ── Two hundred attach / detach cycles, counting handles ──
  {
    int cycles = 200;
    if (const char* env = std::getenv("SONORE_GL_SOAK_CYCLES")) cycles = std::atoi(env);
    if (cycles < 100) cycles = 100;
    std::printf("  .... %d attach/detach cycles (every fourth: the host destroys the window first)\n",
                cycles);
    Counts atWarm;
    std::vector<double> samples;
    int failures = 0, unclosed = 0;
    const int warm = 20;
    for (int cycle = 0; cycle < cycles; ++cycle) {
      CountingRenderer rl;
      OpenGLContext c;
      c.setRenderer(&rl);
      const bool hostCloses = cycle % 4 == 3;
      HWND host = hostCloses ? CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 220, 170,
                                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
                             : parent;
      if (!c.attachTo(view, host)) {
        ++failures;
      } else {
        const uint64_t want = 1;
        if (!waitUntil([&]() { return c.frameCount() >= want; }, 2000)) ++failures;
        if (hostCloses) DestroyWindow(host);
        c.detach();
      }
      if (rl.created != 1 || rl.closed != 1 || rl.outOfOrder != 0) ++unclosed;
      pump(2);
      if (cycle + 1 == warm) atWarm = processCounts();
      if (cycle + 1 >= warm && (cycle + 1) % 10 == 0)
        samples.push_back((double) processCounts().bytes / 1024.0);
    }
    if (std::getenv("SONORE_GL_SOAK_VERBOSE")) {
      for (size_t i = 0; i < samples.size(); ++i) std::printf("%.0f ", samples[i] - samples[0]);
      std::printf("\n");
    }
    pump(100);
    const Counts end = processCounts();
    const int measured = cycles - warm;
    std::printf("       after warm-up: GDI %+ld, USER %+ld, handles %+ld, private %+.1f KB over %d cycles\n",
                (long) end.gdi - (long) atWarm.gdi, (long) end.user - (long) atWarm.user,
                (long) end.handles - (long) atWarm.handles,
                ((double) end.bytes - (double) atWarm.bytes) / 1024.0, measured);
    check(failures == 0, "every context in the soak attached and drew a frame");
    check(unclosed == 0,
          "every one was closed exactly once, in order, including when the host destroyed the "
          "window first");
    check((long) end.gdi - (long) atWarm.gdi <= 2, "GDI objects do not grow (no leaked DC)");
    check((long) end.user - (long) atWarm.user <= 2, "USER objects do not grow (no leaked window)");
    check(((double) end.handles - (double) atWarm.handles) / measured < 0.5,
          "kernel handles do not grow (no leaked context or thread)");
    // Private bytes, by their FLOOR. The driver pools in chunks of tens of
    // megabytes and returns them when it likes: over 780 cycles on this
    // machine the raw figure swung between -28 MB and +40 MB with no trend at
    // all, so "end minus start" fails or passes by luck. A leak raises the
    // floor, and a leaked context raises it by about 25 MB a cycle (measured),
    // so the lowest sample of each half of the run is compared, at a limit an
    // order of magnitude below that. Not tighter: one pooled chunk landing in
    // the second half already reads as 200-450 KB a cycle over 200 cycles.
    const size_t halfway = samples.size() / 2;
    double firstFloor = 1e300, secondFloor = 1e300;
    for (size_t i = 0; i < samples.size(); ++i) {
      double& floor = i < halfway ? firstFloor : secondFloor;
      if (samples[i] < floor) floor = samples[i];
    }
    const double cyclesApart = (double) halfway * 10.0;
    const double perCycleKB = cyclesApart > 0.0 ? (secondFloor - firstFloor) / cyclesApart : 0.0;
    std::printf("       private-bytes floor %+.1f KB a cycle between the two halves\n", perCycleKB);
    check(halfway > 0 && perCycleKB < 2048.0,
          "private bytes have no rising floor (under 2 MB a cycle; one leaked context is ~25 MB)");
  }

  window.close();
  return 0;
}
#endif

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── OpenGL and the 3D maths ─────────────────────────────────────────\n");
  mathsTests();
#if defined(_WIN32)
  glTests();
#else
  std::printf("\n── OpenGL ──\n  SKIPPED -- the GL half runs on Windows in this harness; the macOS and X11 "
              "surfaces are compiled, not run\n");
#endif
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "OPENGL TESTS PASSED\n" : "OPENGL TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
