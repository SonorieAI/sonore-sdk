// SPDX-License-Identifier: Apache-2.0
//
// Box2DRenderer: draws a Box2D world through this SDK's Graphics.
//
// ── Against YOUR Box2D, never a copy of ours ────────────────────────────────
//
// The reference bundles the physics engine itself, which is how a framework
// ends up shipping a second Box2D into a project that already has one -- two
// copies of every b2 symbol, and whichever the linker meets first wins. This
// header vendors nothing. It compiles against the Box2D the plugin already
// builds, and when there is none it declares nothing at all: including it is
// free, and SONORE_BOX2D_API says which API, if any, it found.
//
// ── Which Box2D ─────────────────────────────────────────────────────────────
//
// Found with __has_include, in this order, unless SONORE_BOX2D_API is defined
// first (3, 24, 23, or 0 to switch it off):
//
//   <box2d/id.h>       Box2D 3.1 and later: the C API of b2WorldId handles.
//                      Drawn through b2World_Draw with a b2DebugDraw whose
//                      callbacks are this class. 3.0 is NOT supported: its
//                      b2DebugDraw has no b2DefaultDebugDraw() and a
//                      different string callback.
//   <box2d/b2_world.h> Box2D 2.4: the C++ API. Walked by hand -- bodies,
//                      fixtures, joints -- rather than through b2Draw,
//                      because b2Draw is installed ON the world with
//                      SetDebugDraw, and a renderer that replaced the
//                      plugin's own debug draw to paint one frame would be
//                      a renderer with a side effect.
//   <Box2D/Box2D.h>    Box2D 2.3, the same walk (IsActive for IsEnabled).
//
// ── Colours, by body ────────────────────────────────────────────────────────
//
// By body type and state, from a Palette the caller can replace: static,
// kinematic, awake dynamic, sleeping, disabled, sensor, and joints. For 2.x
// the walk decides; for 3.x Box2D decides, and passes a named colour --
// paleGreen for a static body, pink for an awake one, gray for one asleep --
// which is mapped back to the palette entry it stands for. Shapes and joints
// are drawn in two separate passes so the mapping can tell a gray revolute
// joint from a gray sleeping body.
//
// ── The transform ───────────────────────────────────────────────────────────
//
// render() maps a rectangle of the WORLD -- its left, top, right and bottom
// edges, in metres -- onto a rectangle of the component. Box2D's y axis points
// up, so a world viewed the right way up passes top > bottom, and the mapping
// flips it; nothing assumes either. Points are taken to component space here
// and drawn there, so a line is lineThickness() PIXELS wide whatever the
// zoom: strokes in world units would be a hairline at one zoom and a slab at
// another.
#pragma once

#if !defined(SONORE_BOX2D_API)
#if defined(__has_include)
#if __has_include(<box2d/id.h>) && __has_include(<box2d/box2d.h>)
#define SONORE_BOX2D_API 3
#elif __has_include(<box2d/b2_world.h>) && __has_include(<box2d/box2d.h>)
#define SONORE_BOX2D_API 24
#elif __has_include(<Box2D/Box2D.h>)
#define SONORE_BOX2D_API 23
#endif
#endif
#endif
#if !defined(SONORE_BOX2D_API)
#define SONORE_BOX2D_API 0
#endif

#if SONORE_BOX2D_API == 3 || SONORE_BOX2D_API == 24
#include <box2d/box2d.h>
#elif SONORE_BOX2D_API == 23
#include <Box2D/Box2D.h>
#endif

#if SONORE_BOX2D_API != 0

#include <cmath>
#include <cstdint>
#include <vector>

#include "colour.h"
#include "geometry.h"
#include "graphics.h"
#include "path.h"

namespace sonore {
namespace gfx {

class Box2DRenderer {
public:
  /** What each kind of body is drawn in. Shapes are filled at fillOpacity
   *  and outlined at full strength, so overlapping bodies stay legible. The
   *  defaults are Box2D 2.4's own debug-draw colours. */
  struct Palette {
    Colour staticBody{128, 230, 128};
    Colour kinematicBody{128, 128, 230};
    Colour awakeBody{230, 179, 179};
    Colour sleepingBody{153, 153, 153};
    Colour disabledBody{128, 128, 77};
    Colour sensor{245, 222, 179};
    Colour joint{128, 204, 204};
  };

  Box2DRenderer() = default;
  virtual ~Box2DRenderer() = default;

  void setPalette(const Palette& p) { palette_ = p; }
  const Palette& palette() const { return palette_; }

  /** In component pixels. The reference's getLineThickness(). */
  void setLineThickness(float pixels) { lineThickness_ = pixels > 0.0f ? pixels : 0.0f; }
  float lineThickness() const { return lineThickness_; }

  void setFillOpacity(float opacity) {
    fillOpacity_ = opacity > 0.0f ? (opacity < 1.0f ? opacity : 1.0f) : 0.0f;
  }
  float fillOpacity() const { return fillOpacity_; }

  void setDrawJoints(bool shouldDraw) { drawJoints_ = shouldDraw; }
  bool drawsJoints() const { return drawJoints_; }

  /**
   * The world-to-component mapping render() uses: (worldLeft, worldTop) lands
   * on the target's top-left corner and (worldRight, worldBottom) on its
   * bottom-right. A degenerate world rectangle maps nothing (scale zero)
   * rather than dividing by zero.
   */
  static Transform worldToComponent(float worldLeft, float worldTop, float worldRight,
                                    float worldBottom, const Rect& target) {
    const float dx = worldRight - worldLeft, dy = worldBottom - worldTop;
    const float sx = dx != 0.0f && std::isfinite(dx) ? target.w / dx : 0.0f;
    const float sy = dy != 0.0f && std::isfinite(dy) ? target.h / dy : 0.0f;
    return {sx, 0.0f, 0.0f, sy, target.x - worldLeft * sx, target.y - worldTop * sy};
  }

  /** The transform of the render in progress, or of the last one. */
  const Transform& currentTransform() const { return toComponent_; }

#if SONORE_BOX2D_API == 3
  /** Draw every shape (and, unless turned off, every joint) of `world`. */
  void render(Graphics& g, b2WorldId world, float worldLeft, float worldTop, float worldRight,
              float worldBottom, const Rect& targetArea) {
    if (!b2World_IsValid(world)) return;
    toComponent_ = worldToComponent(worldLeft, worldTop, worldRight, worldBottom, targetArea);
    Graphics::ScopedState keep(g);
    g.clipTo(targetArea);

    Pass pass{this, &g, false};
    b2DebugDraw draw = b2DefaultDebugDraw();
    draw.DrawPolygonFcn = &Box2DRenderer::thunkPolygon;
    draw.DrawSolidPolygonFcn = &Box2DRenderer::thunkSolidPolygon;
    draw.DrawCircleFcn = &Box2DRenderer::thunkCircle;
    draw.DrawSolidCircleFcn = &Box2DRenderer::thunkSolidCircle;
    draw.DrawSolidCapsuleFcn = &Box2DRenderer::thunkSolidCapsule;
    draw.DrawSegmentFcn = &Box2DRenderer::thunkSegment;
    draw.DrawTransformFcn = &Box2DRenderer::thunkTransform;
    draw.DrawPointFcn = &Box2DRenderer::thunkPoint;
    draw.context = &pass;

    // Two passes, so that every callback knows whether it is drawing a
    // shape or a joint -- the colours alone cannot say (see the header).
    draw.drawShapes = true;
    draw.drawJoints = false;
    b2World_Draw(world, &draw);
    if (drawJoints_) {
      pass.joints = true;
      draw.drawShapes = false;
      draw.drawJoints = true;
      b2World_Draw(world, &draw);
    }
  }
#else
  /** Draw every fixture (and, unless turned off, every joint) of `world`. */
  void render(Graphics& g, b2World& world, float worldLeft, float worldTop, float worldRight,
              float worldBottom, const Rect& targetArea) {
    toComponent_ = worldToComponent(worldLeft, worldTop, worldRight, worldBottom, targetArea);
    Graphics::ScopedState keep(g);
    g.clipTo(targetArea);

    for (b2Body* body = world.GetBodyList(); body; body = body->GetNext()) {
      const b2Transform& xf = body->GetTransform();
      for (b2Fixture* fixture = body->GetFixtureList(); fixture; fixture = fixture->GetNext())
        drawFixture(g, *fixture, xf, colourFor(*body, *fixture));
    }
    if (!drawJoints_) return;
    for (b2Joint* joint = world.GetJointList(); joint; joint = joint->GetNext()) drawJoint(g, *joint);
  }

  /** The palette entry for one fixture: by state first, then by type, the
   *  order Box2D's own debug draw decides in. */
  Colour colourFor(const b2Body& body, const b2Fixture& fixture) const {
#if SONORE_BOX2D_API == 23
    if (!body.IsActive()) return palette_.disabledBody;
#else
    if (!body.IsEnabled()) return palette_.disabledBody;
#endif
    if (fixture.IsSensor()) return palette_.sensor;
    if (body.GetType() == b2_staticBody) return palette_.staticBody;
    if (body.GetType() == b2_kinematicBody) return palette_.kinematicBody;
    if (!body.IsAwake()) return palette_.sleepingBody;
    return palette_.awakeBody;
  }
#endif

  // ── Primitives, in WORLD coordinates ───────────────────────────────────
  //
  // Public so an overlay -- a target marker, a trajectory -- can be drawn
  // with exactly the mapping the bodies were.

  void drawSolidPolygon(Graphics& g, const Point* world, int count, Colour c) {
    if (!world || count < 2) return;
    path_.clear();
    for (int i = 0; i < count; ++i) {
      const Point p = toComponent_.apply(world[i]);
      if (i == 0) path_.moveTo(p.x, p.y);
      else path_.lineTo(p.x, p.y);
    }
    path_.close();
    fillAndOutline(g, c);
  }

  void drawPolygonOutline(Graphics& g, const Point* world, int count, Colour c) {
    if (!world || count < 2) return;
    path_.clear();
    for (int i = 0; i < count; ++i) {
      const Point p = toComponent_.apply(world[i]);
      if (i == 0) path_.moveTo(p.x, p.y);
      else path_.lineTo(p.x, p.y);
    }
    path_.close();
    outline(g, c);
  }

  /** A filled circle, with a radius drawn along `axis` (a unit vector in
   *  world space) so a rolling wheel visibly rolls. A zero axis draws none. */
  void drawSolidCircle(Graphics& g, Point centre, float radius, Point axis, Colour c) {
    if (!(radius > 0.0f)) return;
    const Point p = toComponent_.apply(centre);
    const float rx = radius * std::fabs(toComponent_.a), ry = radius * std::fabs(toComponent_.d);
    path_.clear();
    path_.addEllipse({p.x - rx, p.y - ry, rx * 2.0f, ry * 2.0f});
    fillAndOutline(g, c);
    if (axis.x != 0.0f || axis.y != 0.0f) {
      const Point edge = toComponent_.apply({centre.x + axis.x * radius, centre.y + axis.y * radius});
      drawLineIn(g, p, edge, c);
    }
  }

  void drawCircleOutline(Graphics& g, Point centre, float radius, Colour c) {
    if (!(radius > 0.0f)) return;
    const Point p = toComponent_.apply(centre);
    const float rx = radius * std::fabs(toComponent_.a), ry = radius * std::fabs(toComponent_.d);
    path_.clear();
    path_.addEllipse({p.x - rx, p.y - ry, rx * 2.0f, ry * 2.0f});
    outline(g, c);
  }

  /** A convex polygon swollen by `radius` -- Box2D 3's rounded polygon, and
   *  with two points, its capsule. Built in world space and then mapped, so
   *  it is exact under a non-uniform zoom where offsetting in pixels would
   *  not be. */
  void drawRoundedPolygon(Graphics& g, const Point* world, int count, float radius, Colour c) {
    if (!world || count < 1) return;
    if (!(radius > 0.0f)) {
      drawSolidPolygon(g, world, count, c);
      return;
    }
    rounded_.clear();
    roundedOutline(world, count, radius, rounded_);
    drawSolidPolygon(g, rounded_.data(), (int) rounded_.size(), c);
  }

  void drawSegment(Graphics& g, Point a, Point b, Colour c) {
    drawLineIn(g, toComponent_.apply(a), toComponent_.apply(b), c);
  }

  /** A dot `pixels` across, whatever the zoom: a point has no size in the world. */
  void drawPoint(Graphics& g, Point p, float pixels, Colour c) {
    const Point q = toComponent_.apply(p);
    const float r = (pixels > 1.0f ? pixels : 1.0f) * 0.5f;
    g.setColour(c);
    g.fillEllipse({q.x - r, q.y - r, r * 2.0f, r * 2.0f});
  }

private:
  void fillAndOutline(Graphics& g, Colour c) {
    if (fillOpacity_ > 0.0f) {
      g.setColour(c.withMultipliedAlpha(fillOpacity_));
      g.fillPath(path_);
    }
    outline(g, c);
  }

  void outline(Graphics& g, Colour c) {
    if (!(lineThickness_ > 0.0f)) return;
    g.setColour(c);
    StrokeStyle style;
    style.width = lineThickness_;
    style.join = LineJoin::Round;
    g.strokePath(path_, style);
  }

  void drawLineIn(Graphics& g, Point a, Point b, Colour c) {
    if (!(lineThickness_ > 0.0f)) return;
    path_.clear();
    path_.moveTo(a.x, a.y);
    path_.lineTo(b.x, b.y);
    g.setColour(c);
    StrokeStyle style;
    style.width = lineThickness_;
    style.cap = LineCap::Round;
    g.strokePath(path_, style);
  }

  /** The outline of a convex CCW polygon offset by `radius`: each vertex
   *  becomes an arc from the previous edge's outward normal to the next's.
   *  One point is a circle; two are a capsule. */
  static void roundedOutline(const Point* v, int count, float radius, std::vector<Point>& out) {
    const float kTwoPiF = 6.28318530717958647692f;
    const int arcSteps = 8;
    if (count == 1) {
      for (int i = 0; i < arcSteps * 4; ++i) {
        const float a = kTwoPiF * (float) i / (float) (arcSteps * 4);
        out.push_back({v[0].x + std::cos(a) * radius, v[0].y + std::sin(a) * radius});
      }
      return;
    }
    for (int i = 0; i < count; ++i) {
      const Point prev = v[(i + count - 1) % count], cur = v[i], next = v[(i + 1) % count];
      // Outward normals of the edges into and out of this vertex. For a CCW
      // polygon the outward normal of edge (p -> q) is (dy, -dx).
      float inAngle = std::atan2(-(cur.x - prev.x), cur.y - prev.y);
      float outAngle = std::atan2(-(next.x - cur.x), next.y - cur.y);
      if (count == 2) {
        // A capsule: the "edges" are the segment both ways, and each end
        // turns through half a circle.
        outAngle = inAngle + kTwoPiF * 0.5f;
      }
      while (outAngle < inAngle) outAngle += kTwoPiF;
      for (int s = 0; s <= arcSteps; ++s) {
        const float a = inAngle + (outAngle - inAngle) * (float) s / (float) arcSteps;
        out.push_back({cur.x + std::cos(a) * radius, cur.y + std::sin(a) * radius});
      }
    }
  }

#if SONORE_BOX2D_API == 3
  struct Pass {
    Box2DRenderer* self;
    Graphics* g;
    bool joints;
  };

  /** A colour Box2D chose, as the palette entry it means. Box2D 3 decides
   *  the colour of a shape itself and passes it by name; these are the names
   *  b2World_Draw uses for each body state. Anything else -- a custom colour
   *  set on a shape, the red of a body with no mass -- passes through. */
  Colour shapeColour(b2HexColor c) const {
    switch (c) {
      case b2_colorPaleGreen: return palette_.staticBody;
      case b2_colorRoyalBlue: return palette_.kinematicBody;
      case b2_colorPink:
      // Bullet, speed-capped and fast-moving bodies are awake dynamic bodies
      // that Box2D singles out for its own debugging; to a user of this
      // palette they are bodies in motion.
      case b2_colorTurquoise:
      case b2_colorYellow:
      case b2_colorSalmon: return palette_.awakeBody;
      case b2_colorGray: return palette_.sleepingBody;
      case b2_colorSlateGray: return palette_.disabledBody;
      case b2_colorWheat: return palette_.sensor;
      default: return Colour::fromRGB((uint32_t) c);
    }
  }

  static Colour passColour(const Pass& p, b2HexColor c) {
    return p.joints ? p.self->palette_.joint : p.self->shapeColour(c);
  }

  static Point toPoint(b2Vec2 v) { return {v.x, v.y}; }
  static Point transformed(b2Transform xf, b2Vec2 v) {
    return {xf.q.c * v.x - xf.q.s * v.y + xf.p.x, xf.q.s * v.x + xf.q.c * v.y + xf.p.y};
  }

  static void thunkPolygon(const b2Vec2* vertices, int count, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    if (!vertices || count < 2) return;
    std::vector<Point>& pts = p.self->scratch_;
    pts.clear();
    for (int i = 0; i < count; ++i) pts.push_back(toPoint(vertices[i]));
    p.self->drawPolygonOutline(*p.g, pts.data(), count, passColour(p, color));
  }

  static void thunkSolidPolygon(b2Transform xf, const b2Vec2* vertices, int count, float radius,
                                b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    if (!vertices || count < 1) return;
    std::vector<Point>& pts = p.self->scratch_;
    pts.clear();
    for (int i = 0; i < count; ++i) pts.push_back(transformed(xf, vertices[i]));
    p.self->drawRoundedPolygon(*p.g, pts.data(), count, radius, passColour(p, color));
  }

  static void thunkCircle(b2Vec2 centre, float radius, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    p.self->drawCircleOutline(*p.g, toPoint(centre), radius, passColour(p, color));
  }

  static void thunkSolidCircle(b2Transform xf, float radius, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    p.self->drawSolidCircle(*p.g, toPoint(xf.p), radius, {xf.q.c, xf.q.s}, passColour(p, color));
  }

  static void thunkSolidCapsule(b2Vec2 a, b2Vec2 b, float radius, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    const Point ends[2] = {toPoint(a), toPoint(b)};
    p.self->drawRoundedPolygon(*p.g, ends, 2, radius, passColour(p, color));
  }

  static void thunkSegment(b2Vec2 a, b2Vec2 b, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    p.self->drawSegment(*p.g, toPoint(a), toPoint(b), passColour(p, color));
  }

  static void thunkTransform(b2Transform xf, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    // Only reached with drawMass on, which render() leaves off; drawn the
    // conventional way anyway -- a short red x axis and green y axis.
    const float k = 0.2f;
    const Point o = toPoint(xf.p);
    p.self->drawSegment(*p.g, o, {o.x + xf.q.c * k, o.y + xf.q.s * k}, Colour(230, 60, 60));
    p.self->drawSegment(*p.g, o, {o.x - xf.q.s * k, o.y + xf.q.c * k}, Colour(60, 200, 60));
  }

  static void thunkPoint(b2Vec2 at, float size, b2HexColor color, void* context) {
    Pass& p = *static_cast<Pass*>(context);
    p.self->drawPoint(*p.g, toPoint(at), size, passColour(p, color));
  }
#else
  static Point toPoint(const b2Vec2& v) { return {v.x, v.y}; }

  void drawFixture(Graphics& g, const b2Fixture& fixture, const b2Transform& xf, Colour c) {
    const b2Shape* shape = fixture.GetShape();
    if (!shape) return;
    switch (shape->GetType()) {
      case b2Shape::e_circle: {
        const b2CircleShape* circle = static_cast<const b2CircleShape*>(shape);
        const b2Vec2 centre = b2Mul(xf, circle->m_p);
        drawSolidCircle(g, toPoint(centre), circle->m_radius, {xf.q.c, xf.q.s}, c);
        break;
      }
      case b2Shape::e_polygon: {
        const b2PolygonShape* poly = static_cast<const b2PolygonShape*>(shape);
        scratch_.clear();
        for (int i = 0; i < (int) poly->m_count; ++i) scratch_.push_back(toPoint(b2Mul(xf, poly->m_vertices[i])));
        drawSolidPolygon(g, scratch_.data(), (int) scratch_.size(), c);
        break;
      }
      case b2Shape::e_edge: {
        const b2EdgeShape* edge = static_cast<const b2EdgeShape*>(shape);
        drawSegment(g, toPoint(b2Mul(xf, edge->m_vertex1)), toPoint(b2Mul(xf, edge->m_vertex2)), c);
        break;
      }
      case b2Shape::e_chain: {
        const b2ChainShape* chain = static_cast<const b2ChainShape*>(shape);
        if (!chain->m_vertices || chain->m_count < 2) break;
        Point previous = toPoint(b2Mul(xf, chain->m_vertices[0]));
        for (int i = 1; i < (int) chain->m_count; ++i) {
          const Point next = toPoint(b2Mul(xf, chain->m_vertices[i]));
          drawSegment(g, previous, next, c);
          previous = next;
        }
        break;
      }
      default: break;
    }
  }

  /** Box2D 2.x's own convention: body centre to anchor, anchor to anchor,
   *  anchor to the other body's centre -- and for a pulley, the ropes up to
   *  its ground anchors and the line between them. */
  void drawJoint(Graphics& g, b2Joint& joint) {
    const Colour c = palette_.joint;
    const Point a = toPoint(joint.GetAnchorA()), b = toPoint(joint.GetAnchorB());
    switch (joint.GetType()) {
      case e_distanceJoint: drawSegment(g, a, b, c); break;
      case e_pulleyJoint: {
        const b2PulleyJoint& pulley = static_cast<const b2PulleyJoint&>(joint);
        const Point ga = toPoint(pulley.GetGroundAnchorA()), gb = toPoint(pulley.GetGroundAnchorB());
        drawSegment(g, ga, a, c);
        drawSegment(g, gb, b, c);
        drawSegment(g, ga, gb, c);
        break;
      }
      case e_mouseJoint: drawSegment(g, a, b, c); break;
      default: {
        const Point pa = toPoint(joint.GetBodyA()->GetTransform().p);
        const Point pb = toPoint(joint.GetBodyB()->GetTransform().p);
        drawSegment(g, pa, a, c);
        drawSegment(g, a, b, c);
        drawSegment(g, pb, b, c);
        break;
      }
    }
  }
#endif

  Palette palette_;
  float lineThickness_ = 1.5f;
  float fillOpacity_ = 0.5f;
  bool drawJoints_ = true;
  Transform toComponent_;
  Path path_;
  std::vector<Point> scratch_, rounded_;
};

} // namespace gfx
} // namespace sonore

#endif // SONORE_BOX2D_API != 0
