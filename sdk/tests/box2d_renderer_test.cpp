// SPDX-License-Identifier: Apache-2.0
// Box2DRenderer against a REAL Box2D -- built only when one is supplied.
//
// The SDK does not vendor Box2D (gfx/box2d_renderer.h says why), so this test
// cannot run on a checkout that has none. CMake builds it when
// SONORE_BOX2D_DIR points at a Box2D source tree: 3.1 or later (the C API) or
// 2.4 (the C++ one). The same file covers both; the header picks the API and
// so does the scene below.
//
// What it checks is ink: a small scene -- a static ground box, a static chain
// and segment, a falling box and ball, a kinematic bar with a box hanging from
// it on a distance joint -- rendered into a Bitmap, and the pixels where each
// body is supposed to be read back for the colour its type and state should
// give it. Then the world is stepped until things settle and read again: the
// box has moved, and fallen asleep, and is now drawn in the sleeping colour.
#include <sonore/gfx/box2d_renderer.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#if SONORE_BOX2D_API == 0
#error "box2d_renderer_test needs Box2D's headers on the include path (see SONORE_BOX2D_DIR)"
#endif

using namespace sonore::gfx;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

// The view: x from -10 to 10, y from 13 down to -1, onto 200 x 140 pixels.
// Ten pixels to the metre, y flipped: pixel (px, py) is world
// (px / 10 - 10, 13 - py / 10).
constexpr float kLeft = -10.0f, kTop = 13.0f, kRight = 10.0f, kBottom = -1.0f;
constexpr int kWidth = 200, kHeight = 140;

PremulColour at(const Bitmap& b, float wx, float wy) {
  const int px = (int) std::floor((wx - kLeft) * 10.0f);
  const int py = (int) std::floor((kTop - wy) * 10.0f);
  return b.pixelAt(px, py);
}

bool isBackground(PremulColour c) { return c.r == 0 && c.g == 0 && c.b == 0; }
bool greenish(PremulColour c) { return c.g > c.r + 30 && c.g > c.b + 30; }
bool reddish(PremulColour c) { return c.r > c.g + 15 && c.r > c.b + 15; }
bool bluish(PremulColour c) { return c.b > c.r + 30 && c.b > c.g + 30; }
bool greyish(PremulColour c) {
  return c.r > 40 && std::abs((int) c.r - (int) c.g) < 10 && std::abs((int) c.g - (int) c.b) < 10;
}
bool jointish(PremulColour c) { return c.g > c.r + 30 && c.b > c.r + 30; }

/** Any pixel within `radius` of the world point satisfying `test`: thin
 *  lines are antialiased, and which pixel carries most of one is not the
 *  question. */
template <typename Test>
bool inkNear(const Bitmap& b, float wx, float wy, int radius, Test test) {
  const int cx = (int) std::floor((wx - kLeft) * 10.0f);
  const int cy = (int) std::floor((kTop - wy) * 10.0f);
  for (int y = cy - radius; y <= cy + radius; ++y)
    for (int x = cx - radius; x <= cx + radius; ++x)
      if (test(b.pixelAt(x, y))) return true;
  return false;
}

void report(const char* what, PremulColour c) {
  std::printf("       %s = (%d, %d, %d, %d)\n", what, c.r, c.g, c.b, c.a);
}

#if SONORE_BOX2D_API == 3

struct Scene {
  b2WorldId world{};
  b2BodyId box{}, ball{}, bar{}, hanging{};

  Scene() {
    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = b2Vec2{0.0f, -10.0f};
    world = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    const b2BodyId ground = b2CreateBody(world, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    const b2Polygon slab = b2MakeBox(10.0f, 0.5f);
    b2CreatePolygonShape(ground, &sd, &slab);
    const b2Segment segment{{3.0f, 10.0f}, {9.0f, 10.0f}};
    b2CreateSegmentShape(ground, &sd, &segment);
    const b2Vec2 points[4] = {{-9.0f, 10.0f}, {-7.0f, 9.0f}, {-5.0f, 9.5f}, {-3.0f, 10.0f}};
    b2ChainDef cd = b2DefaultChainDef();
    cd.points = points;
    cd.count = 4;
    cd.isLoop = false;
    b2CreateChain(ground, &cd);

    bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = b2Vec2{-4.0f, 3.0f};
    box = b2CreateBody(world, &bd);
    const b2Polygon square = b2MakeBox(1.0f, 1.0f);
    b2CreatePolygonShape(box, &sd, &square);

    bd.position = b2Vec2{4.0f, 6.0f};
    ball = b2CreateBody(world, &bd);
    const b2Circle circle{{0.0f, 0.0f}, 1.0f};
    b2CreateCircleShape(ball, &sd, &circle);

    bd = b2DefaultBodyDef();
    bd.type = b2_kinematicBody;
    bd.position = b2Vec2{0.0f, 8.0f};
    bar = b2CreateBody(world, &bd);
    const b2Polygon barShape = b2MakeBox(1.0f, 0.5f);
    b2CreatePolygonShape(bar, &sd, &barShape);

    bd.type = b2_dynamicBody;
    bd.position = b2Vec2{0.0f, 5.0f};
    hanging = b2CreateBody(world, &bd);
    const b2Polygon small = b2MakeBox(0.5f, 0.5f);
    b2CreatePolygonShape(hanging, &sd, &small);

    b2DistanceJointDef jd = b2DefaultDistanceJointDef();
    jd.bodyIdA = bar;
    jd.bodyIdB = hanging;
    jd.localAnchorA = b2Vec2{0.0f, 0.0f};
    jd.localAnchorB = b2Vec2{0.0f, 0.0f};
    jd.length = 3.0f;
    b2CreateDistanceJoint(world, &jd);
  }
  ~Scene() { b2DestroyWorld(world); }

  void step(int steps) {
    for (int i = 0; i < steps; ++i) b2World_Step(world, 1.0f / 60.0f, 4);
  }
  bool boxAwake() const { return b2Body_IsAwake(box); }
  float boxY() const { return b2Body_GetPosition(box).y; }
  void render(Box2DRenderer& r, Graphics& g) {
    r.render(g, world, kLeft, kTop, kRight, kBottom, Rect(0.0f, 0.0f, (float) kWidth, (float) kHeight));
  }
};

#else

struct Scene {
  b2World world{b2Vec2(0.0f, -10.0f)};
  b2Body* box = nullptr;
  b2Body* ball = nullptr;
  b2Body* bar = nullptr;
  b2Body* hanging = nullptr;

  Scene() {
    b2BodyDef bd;
    b2Body* ground = world.CreateBody(&bd);
    b2PolygonShape slab;
    slab.SetAsBox(10.0f, 0.5f);
    ground->CreateFixture(&slab, 0.0f);
    b2EdgeShape edge;
#if SONORE_BOX2D_API == 23
    edge.Set(b2Vec2(3.0f, 10.0f), b2Vec2(9.0f, 10.0f));
#else
    edge.SetTwoSided(b2Vec2(3.0f, 10.0f), b2Vec2(9.0f, 10.0f));
#endif
    ground->CreateFixture(&edge, 0.0f);
    const b2Vec2 points[4] = {b2Vec2(-9.0f, 10.0f), b2Vec2(-7.0f, 9.0f), b2Vec2(-5.0f, 9.5f),
                              b2Vec2(-3.0f, 10.0f)};
    b2ChainShape chain;
#if SONORE_BOX2D_API == 23
    chain.CreateChain(points, 4);
#else
    chain.CreateChain(points, 4, b2Vec2(-10.0f, 10.0f), b2Vec2(-2.0f, 10.0f));
#endif
    ground->CreateFixture(&chain, 0.0f);

    bd.type = b2_dynamicBody;
    bd.position.Set(-4.0f, 3.0f);
    box = world.CreateBody(&bd);
    b2PolygonShape square;
    square.SetAsBox(1.0f, 1.0f);
    box->CreateFixture(&square, 1.0f);

    bd.position.Set(4.0f, 6.0f);
    ball = world.CreateBody(&bd);
    b2CircleShape circle;
    circle.m_radius = 1.0f;
    ball->CreateFixture(&circle, 1.0f);

    bd.type = b2_kinematicBody;
    bd.position.Set(0.0f, 8.0f);
    bar = world.CreateBody(&bd);
    b2PolygonShape barShape;
    barShape.SetAsBox(1.0f, 0.5f);
    bar->CreateFixture(&barShape, 1.0f);

    bd.type = b2_dynamicBody;
    bd.position.Set(0.0f, 5.0f);
    hanging = world.CreateBody(&bd);
    b2PolygonShape small;
    small.SetAsBox(0.5f, 0.5f);
    hanging->CreateFixture(&small, 1.0f);

    b2DistanceJointDef jd;
    jd.Initialize(bar, hanging, bar->GetPosition(), hanging->GetPosition());
    world.CreateJoint(&jd);
  }

  void step(int steps) {
    for (int i = 0; i < steps; ++i) world.Step(1.0f / 60.0f, 8, 3);
  }
  bool boxAwake() const { return box->IsAwake(); }
  float boxY() const { return box->GetPosition().y; }
  void render(Box2DRenderer& r, Graphics& g) {
    r.render(g, world, kLeft, kTop, kRight, kBottom, Rect(0.0f, 0.0f, (float) kWidth, (float) kHeight));
  }
};

#endif

} // namespace

int main() {
  std::printf("Box2DRenderer against Box2D API %d\n", SONORE_BOX2D_API);

  {
    const Transform t = Box2DRenderer::worldToComponent(kLeft, kTop, kRight, kBottom,
                                                        Rect(0.0f, 0.0f, (float) kWidth, (float) kHeight));
    const Point tl = t.apply({kLeft, kTop}), br = t.apply({kRight, kBottom});
    const Point origin = t.apply({0.0f, 0.0f});
    check(tl.x == 0.0f && tl.y == 0.0f, "world top-left lands on the target's top-left");
    check(br.x == 200.0f && br.y == 140.0f, "world bottom-right lands on the target's bottom-right");
    check(origin.x == 100.0f && origin.y == 130.0f, "y is flipped: world origin at (100, 130)");
    const Transform degenerate = Box2DRenderer::worldToComponent(1.0f, 1.0f, 1.0f, 1.0f, Rect(0, 0, 10, 10));
    check(degenerate.a == 0.0f && degenerate.d == 0.0f, "a zero-size world rectangle maps to scale zero");
  }

  Scene scene;
  Box2DRenderer renderer;
  Bitmap before(kWidth, kHeight);
  before.clear(Colour(0, 0, 0));
  {
    Graphics g(before);
    scene.render(renderer, g);
  }

  const PremulColour ground = at(before, 0.0f, 0.0f);
  const PremulColour box = at(before, -4.0f, 3.0f);
  const PremulColour ball = at(before, 4.0f, 6.0f);
  // Off-centre: the joint's anchor is the bar's centre, and the rope is
  // drawn over it there.
  const PremulColour bar = at(before, 0.6f, 8.2f);
  const PremulColour rope = at(before, 0.0f, 6.5f);
  const PremulColour empty = at(before, -8.0f, 1.5f);
  report("ground", ground);
  report("box", box);
  report("ball", ball);
  report("bar", bar);
  report("rope", rope);
  check(greenish(ground), "static ground is filled in the static colour");
  check(reddish(box), "the dynamic box is filled in the awake colour");
  check(reddish(ball), "the dynamic ball is filled in the awake colour");
  check(bluish(bar), "the kinematic bar is filled in the kinematic colour");
  check(jointish(rope), "the distance joint is drawn in the joint colour between the bodies");
  check(isBackground(empty), "nothing is drawn where there is no body");
  check(inkNear(before, -7.0f, 9.0f, 2, greenish), "the chain's middle vertex has static ink");
  check(inkNear(before, -6.0f, 9.25f, 2, greenish), "the chain is drawn between its vertices");
  check(isBackground(at(before, -7.0f, 7.5f)), "the chain is a line, not a filled shape");
  check(inkNear(before, 6.0f, 10.0f, 1, greenish), "the segment shape has static ink along it");
  check(inkNear(before, 5.0f, 6.0f, 1, [](PremulColour c) { return c.r > 150 && c.r > c.b + 20; }),
        "the ball's outline is drawn at full strength at its rim");
  // The outline of the ground slab at its top edge (y = 0.5) is the full
  // colour, the fill inside it half: both, in the order the palette says.
  check(inkNear(before, -2.0f, 0.5f, 1, [](PremulColour c) { return c.g > 200; }),
        "the ground's outline is full strength, its fill is not");
  check(ground.g < 150, "the ground's fill is at half opacity");

  scene.step(360);
  std::printf("       after 6 s: box at y = %.3f, %s\n", scene.boxY(), scene.boxAwake() ? "awake" : "asleep");
  check(std::fabs(scene.boxY() - 1.5f) < 0.05f, "the box fell onto the ground (y = 1.5)");
  check(!scene.boxAwake(), "and fell asleep");

  Bitmap after(kWidth, kHeight);
  after.clear(Colour(0, 0, 0));
  {
    Graphics g(after);
    scene.render(renderer, g);
  }
  const PremulColour restingBox = at(after, -4.0f, 1.5f);
  report("resting box", restingBox);
  check(greyish(restingBox), "the sleeping box is drawn in the sleeping colour");
  check(isBackground(at(after, -4.0f, 3.4f)), "and is no longer drawn where it started");
  check(greenish(at(after, 0.0f, 0.0f)), "the ground has not moved");

  // A palette of the caller's own, and joints switched off.
  Box2DRenderer custom;
  Box2DRenderer::Palette p;
  p.staticBody = Colour(255, 0, 0);
  p.sleepingBody = Colour(0, 0, 255);
  custom.setPalette(p);
  custom.setDrawJoints(false);
  custom.setFillOpacity(1.0f);
  custom.setLineThickness(0.0f);
  Bitmap own(kWidth, kHeight);
  own.clear(Colour(0, 0, 0));
  {
    Graphics g(own);
    scene.render(custom, g);
  }
  const PremulColour ownGround = at(own, 0.0f, 0.0f), ownBox = at(own, -4.0f, 1.5f);
  report("custom ground", ownGround);
  report("custom box", ownBox);
  check(ownGround.r == 255 && ownGround.g == 0 && ownGround.b == 0, "a custom static colour is used, opaque");
  check(ownBox.b == 255 && ownBox.r == 0, "a custom sleeping colour is used");
  check(isBackground(at(own, 0.0f, 6.5f)), "joints switched off draw no joint");

  // Drawn into a sub-rectangle of a larger component: nothing outside it.
  Bitmap framed(300, 200);
  framed.clear(Colour(0, 0, 0));
  {
    Graphics g(framed);
    renderer.render(g, scene.world, kLeft, kTop, kRight, kBottom, Rect(50.0f, 30.0f, 200.0f, 140.0f));
  }
  bool outside = false;
  for (int y = 0; y < 200; ++y)
    for (int x = 0; x < 300; ++x)
      if ((x < 50 || x >= 250 || y < 30 || y >= 170) && !isBackground(framed.pixelAt(x, y))) outside = true;
  check(!outside, "a render into a target rectangle stays inside it");
  check(greenish(framed.pixelAt(150, 160)), "and the ground lands at the offset position");

  std::printf("%s\n", failures ? "BOX2D RENDERER TESTS FAILED" : "BOX2D RENDERER TESTS PASSED");
  return failures ? 1 : 0;
}
