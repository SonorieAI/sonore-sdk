// SPDX-License-Identifier: Apache-2.0
// Fuzzing the gui infrastructure: the relative-position expression parser, and
// random event streams through a tree wired with every listener-based piece.
//
// Two kinds of untrusted input reach this code. Expressions arrive as text --
// a layout read from a file, a skin, a preset -- so the parser and evaluator
// are fed mutations of real layouts, cycles in markers and components, and
// nesting past the caps. Events arrive in any order a user and a host can
// produce, and every piece here holds raw pointers to components that a
// handler may destroy; so the second half drives random pointer streams
// (mouse, pen, several fingers) through a tree whose components are destroyed,
// re-parented, cached, watched and positioned at random as the stream runs.
//
// Every case must return. Run it under AddressSanitizer: a dangling pointer
// in a listener list or a capture is exactly what it exists to find.
//
//   fuzz_gui_infra [cases] [seed]
#include <sonore/gfx/animated_position.h>
#include <sonore/gfx/backing.h>
#include <sonore/gfx/component.h>
#include <sonore/gfx/component_builder.h>
#include <sonore/gfx/mouse_input.h>
#include <sonore/gfx/movement_watcher.h>
#include <sonore/gfx/relative_position.h>
#include <sonore/gfx/text_drag.h>
#include <sonore/gfx/viewport.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace sonore;
using namespace sonore::gfx;

namespace {

struct Rng {
  uint64_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return (uint32_t) (s >> 11);
  }
  int below(int n) { return n > 0 ? (int) (next() % (uint32_t) n) : 0; }
  float unit() { return (float) (next() & 0xFFFFFF) / 16777216.0f; }
};

const char* kSeeds[] = {
    "10",
    "parent.width - 20",
    "a.right + 5",
    "min(parent.width / 2, b.left - 4)",
    "gutter * 2 + (parent.height - this.height) / 2",
    "-(a.centreX - 3.5e1) * .25",
    "max(abs(a.x - b.x), 0)",
    "parent.right",
    "inner",
    "1 / 0",
    "((((((1))))))",
    "a.bottom + b.top * c.width / 7 - 1e-3",
};

std::string mutate(Rng& r, std::string s) {
  static const char kAlphabet[] = "0123456789+-*/(). ,eEabcxyzparentwidthleftrightthis_minaxgutter$\t";
  const int edits = 1 + r.below(6);
  for (int i = 0; i < edits; ++i) {
    switch (r.below(7)) {
      case 0: // insert a character
        s.insert(s.begin() + (s.empty() ? 0 : r.below((int) s.size() + 1)),
                 kAlphabet[r.below((int) sizeof(kAlphabet) - 1)]);
        break;
      case 1: // delete one
        if (!s.empty()) s.erase(s.begin() + r.below((int) s.size()));
        break;
      case 2: // duplicate a slice
        if (!s.empty()) {
          const int at = r.below((int) s.size()), len = 1 + r.below(8);
          s.insert((size_t) r.below((int) s.size() + 1), s.substr((size_t) at, (size_t) len));
        }
        break;
      case 3: // splice in another seed
        s += kSeeds[r.below((int) (sizeof(kSeeds) / sizeof(kSeeds[0])))];
        break;
      case 4: // deep nesting, past the cap sometimes
      {
        const int depth = r.below(90);
        s = std::string((size_t) depth, '(') + s + std::string((size_t) depth, ')');
        break;
      }
      case 5: // a raw byte
        s.insert(s.begin() + (s.empty() ? 0 : r.below((int) s.size() + 1)), (char) (r.next() & 0xFF));
        break;
      default: // a long number
        s += std::string((size_t) (1 + r.below(40)), '9');
        break;
    }
  }
  if (s.size() > 6000) s.resize(6000);
  return s;
}

/** One expression case: parse, evaluate in a scope with siblings and
 *  (possibly cyclic) markers, round-trip, and apply as a live position. */
void expressionCase(Rng& r) {
  const std::string text = mutate(r, kSeeds[r.below((int) (sizeof(kSeeds) / sizeof(kSeeds[0])))]);
  Component parent;
  parent.setBounds({0, 0, 400 + (float) r.below(200), 300});
  Component a, b, c, target;
  a.setComponentID("a");
  b.setComponentID("b");
  c.setComponentID("c");
  a.setBounds({10, 10, 50, 20});
  b.setBounds({100, 40, 30, 30});
  for (Component* k : {&a, &b, &c, &target}) parent.addChild(k);
  MarkerList markers;
  markers.setMarker("gutter", RelativeCoordinate(mutate(r, "parent.width * 0.1")));
  markers.setMarker("inner", RelativeCoordinate(r.below(4) == 0 ? std::string("gutter + inner") : mutate(r, "gutter * 2")));

  const RelativeCoordinate coordinate(text);
  RelativeScope scope;
  scope.parent = &parent;
  scope.self = &target;
  scope.markers = {&markers};
  std::string why;
  const double v = coordinate.resolve(scope, &why);
  if (!why.empty() && v != 0.0) std::abort(); // an error must give 0, not garbage
  if (coordinate.isValid()) {
    // Whatever parsed must print as something that parses again.
    const RelativeCoordinate again(coordinate.toString());
    if (!again.isValid()) {
      std::printf("round trip failed: [%s] -> [%s]: %s\n", text.c_str(), coordinate.toString().c_str(),
                  again.error().c_str());
      std::abort();
    }
  }
  // As a live rectangle, sometimes on two components that name each other.
  const std::string rect = text + ", " + mutate(r, "10") + ", " + mutate(r, "a.right") + ", 50";
  RelativeRectangle(rect).applyToComponent(target, {&markers});
  if (r.below(3) == 0) {
    RelativeRectangle("b.right + 1, 0, b.right + 5, 5").applyToComponent(a);
    RelativeRectangle("a.right + 1, 0, a.right + 5, 5").applyToComponent(b);
  }
  parent.setSize(500, 300);
  markers.setMarker("gutter", RelativeCoordinate(7.0));
  if (r.below(2)) parent.removeChild(&b); // a named sibling goes away
}

// ── Random event streams ────────────────────────────────────────────────────

struct Node : Component, TextDragAndDropTarget {
  int painted = 0;
  bool acceptsText = false;
  void paint(Graphics& g) override {
    ++painted;
    g.fillAll(Colour(40, 60, 80));
    g.setColour(Colour(200, 100, 50, 180));
    g.fillEllipse(localBounds().reduced(2.0f));
  }
  TextDragAndDropTarget* asTextDragAndDropTarget() override { return acceptsText ? this : nullptr; }
  bool isInterestedInTextDrag(const std::string&) override { return true; }
  void textDropped(const std::string&, Point) override {}
  void mouseDrag(const MouseEvent& e) override {
    if (std::fabs(e.dragDistanceX()) > 6.0f)
      if (MouseRouter* r = routerFor(this))
        if (!r->isDragging()) r->startTextDrag("text", this, e.rootPosition);
  }
};

struct Counter : MouseListener {
  int events = 0;
  void mouseDown(const MouseEvent&) override { ++events; }
  void mouseDrag(const MouseEvent&) override { ++events; }
  void mouseUp(const MouseEvent&) override { ++events; }
  void mouseMove(const MouseEvent&) override { ++events; }
};

void eventCase(Rng& r) {
  Component root;
  root.setBounds({0, 0, 300, 200});
  std::vector<std::unique_ptr<Node>> nodes;
  for (int i = 0; i < 8; ++i) {
    nodes.push_back(std::make_unique<Node>());
    Node& n = *nodes.back();
    n.setComponentID("n" + std::to_string(i));
    n.acceptsText = r.below(2) == 0;
    n.setBounds({(float) r.below(250), (float) r.below(150), 20.0f + (float) r.below(80),
                 20.0f + (float) r.below(60)});
    Component* parent = (i == 0 || r.below(2)) ? &root : nodes[(size_t) r.below(i)].get();
    parent->addChild(&n);
  }
  Viewport viewport;
  viewport.setBounds({0, 0, 120, 100});
  Node content;
  content.setBounds({0, 0, 110, 600});
  viewport.setViewedComponent(&content);
  root.addChild(&viewport);

  MouseRouter router(root);
  std::vector<std::unique_ptr<ComponentMovementWatcher>> watchers;
  std::vector<std::unique_ptr<Counter>> counters;
  std::unique_ptr<MouseInactivityDetector> inactivity;
  std::unique_ptr<ViewportDragScroller> scroller;
  TextDragDispatcher external(root);
  double now = 0.0;
  Backing backing;
  backing.setContent(&root);
  backing.setScale(r.below(2) ? 1.0f : 1.5f);
  backing.setDeviceSize(r.below(2) ? 300 : 450, r.below(2) ? 200 : 300);
  backing.invalidateAll();

  auto anyNode = [&]() -> Node* {
    std::vector<Node*> live;
    for (auto& n : nodes)
      if (n) live.push_back(n.get());
    return live.empty() ? nullptr : live[(size_t) r.below((int) live.size())];
  };

  const int steps = 30 + r.below(60);
  for (int step = 0; step < steps; ++step) {
    const Point p((float) r.below(320) - 10.0f, (float) r.below(220) - 10.0f);
    now += 0.005 + r.unit() * 0.05;
    switch (r.below(22)) {
      case 0: router.mouseDown(p, 1 + r.below(2), false, false, false, r.below(6) == 0); break;
      case 1:
      case 2: router.mouseMove(p); break;
      case 3: router.mouseUp(p); break;
      case 4: router.mouseWheel(p, r.unit() * 2.0f - 1.0f); break;
      case 5: router.mouseExitWindow(); break;
      case 6: { // a pen or finger report
        PointerSample s;
        s.rootPosition = p;
        s.input.type = r.below(2) ? MouseInputSourceType::Pen : MouseInputSourceType::Touch;
        s.input.index = r.below(3);
        s.input.pressure = r.unit();
        s.primary = r.below(2) == 0;
        s.phase = (PointerSample::Phase) r.below(5);
        s.time = now;
        MouseInputSources::instance().handle(router, s);
        break;
      }
      case 7: // destroy a node mid-stream
        if (!nodes.empty()) nodes[(size_t) r.below((int) nodes.size())].reset();
        break;
      case 8: // re-parent one
        if (Node* n = anyNode()) {
          Node* to = anyNode();
          bool cycle = false;
          for (Component* c = to; c; c = c->parent())
            if (c == n) cycle = true;
          if (!cycle) (to && r.below(2) ? (Component*) to : &root)->addChild(n);
        }
        break;
      case 9:
        if (Node* n = anyNode()) n->setBufferedToImage(r.below(3) != 0);
        break;
      case 10:
        if (Node* n = anyNode()) watchers.push_back(std::make_unique<ComponentMovementWatcher>(n));
        break;
      case 11:
        if (Node* n = anyNode()) {
          counters.push_back(std::make_unique<Counter>());
          n->addMouseListener(counters.back().get(), r.below(2) == 0);
        }
        break;
      case 12:
        if (!counters.empty()) counters.erase(counters.begin() + r.below((int) counters.size()));
        break;
      case 13:
        if (Node* n = anyNode()) {
          static const char* kRects[] = {"n0.right, 0, n0.right + 10, 10", "parent.width - 20, 5, parent.width, 25",
                                         "n1.x, n2.bottom, n1.right, n2.bottom + 8", "this.x, this.y, this.right, 30"};
          RelativeRectangle(kRects[r.below(4)]).applyToComponent(*n);
        }
        break;
      case 14:
        if (Node* n = anyNode())
          n->setBounds({(float) r.below(250), (float) r.below(150), 1.0f + (float) r.below(90),
                        1.0f + (float) r.below(70)});
        break;
      case 15:
        if (Node* n = anyNode()) n->setVisible(r.below(2) == 0);
        break;
      case 16:
        if (!inactivity) {
          inactivity = std::make_unique<MouseInactivityDetector>(root);
          inactivity->setClock([&now] { return now; });
        }
        inactivity->tick();
        break;
      case 17:
        if (!scroller) {
          scroller = std::make_unique<ViewportDragScroller>(viewport, ViewportDragScroller::ScrollOnDragMode::all);
          scroller->setTicksFromVBlank(false);
          scroller->setClock([&now] { return now; });
        }
        scroller->tick(now);
        break;
      case 18:
        if (r.below(3) == 0) external.drop("dropped", p);
        else if (r.below(2)) external.dragMove("dragged", p);
        else external.dragExit();
        break;
      case 19: backing.render(); break;
      case 20:
        if (Node* n = anyNode()) n->repaint(Rect(r.unit() * 20.0f, r.unit() * 20.0f, 5.0f, 5.0f));
        break;
      default:
        if (r.below(4) == 0 && !watchers.empty()) watchers.erase(watchers.begin() + r.below((int) watchers.size()));
        break;
    }
  }
  backing.render();
  router.mouseUp({-1, -1});
}

} // namespace

int main(int argc, char** argv) {
  const int cases = argc > 1 ? std::atoi(argv[1]) : 20000;
  Rng rng{argc > 2 ? (uint64_t) std::strtoull(argv[2], nullptr, 10) : 0x9E3779B97F4A7C15ull};
  if (rng.s == 0) rng.s = 1;
  int expressions = 0, streams = 0;
  for (int i = 0; i < cases; ++i) {
    // Three expression cases to one event stream: the streams are longer.
    if (i % 4 != 3) {
      expressionCase(rng);
      ++expressions;
    } else {
      eventCase(rng);
      ++streams;
    }
  }
  std::printf("fuzz_gui_infra: %d cases (%d expression, %d event streams), all returned\n", cases,
              expressions, streams);
  std::printf("FUZZ GUI INFRA PASSED\n");
  return 0;
}
