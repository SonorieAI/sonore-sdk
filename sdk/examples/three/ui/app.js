// SPDX-License-Identifier: Apache-2.0
// Sonore Orbit's face: two knobs turned in 3D, drawn with three.js.
//
// Three things a 3D plugin face has to get right, each done here:
//  - It draws only when something changed, at most 30 frames a second, and
//    not at all while the window is hidden. An editor left open in a DAW
//    otherwise keeps a GPU busy for the whole session.
//  - A drag is ONE gesture: begin, the values, end. The host folds it into
//    one automation pass and one undo step, as it does a hardware control.
//  - It still works without WebGL: plain sliders, not a blank window.
import * as THREE from "three";
import { RoomEnvironment } from "three/addons/environments/RoomEnvironment.js";

const stage = document.getElementById("stage");
const labelLayer = document.getElementById("labels");

// A module runs before 'sonore:ready' fires, so it can wait for it.
addEventListener("sonore:ready", () => {
  let renderer;
  try {
    renderer = new THREE.WebGLRenderer({ canvas: stage, antialias: true });
  } catch {
    renderer = null;
  }
  if (!renderer) {
    fallback();
    return;
  }
  scene3d(renderer);
});

function scene3d(renderer) {
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.toneMapping = THREE.ACESFilmicToneMapping;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x0e1013);
  // Image-based light from a room three.js builds itself: real reflections on
  // the metal with no image file to ship. An HDRI in the folder would do the
  // same through HDRLoader, fetched by relative URL.
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
  pmrem.dispose();

  const camera = new THREE.PerspectiveCamera(24, 16 / 9, 0.1, 100);
  camera.position.set(0, 0, 12);

  // The panel, and a key light for the shadows that make a knob stand up.
  const panel = new THREE.Mesh(
    new THREE.PlaneGeometry(20, 12),
    // Satin and dim in the room's light: a panel that mirrors the room
    // glares, and the knobs are what should catch it.
    new THREE.MeshStandardMaterial({ color: 0x1d2126, metalness: 0.3, roughness: 0.6, envMapIntensity: 0.35 }),
  );
  panel.receiveShadow = true;
  scene.add(panel);
  const key = new THREE.DirectionalLight(0xffffff, 1.4);
  key.position.set(-4, 6, 10);
  key.castShadow = true;
  key.shadow.mapSize.set(1024, 1024);
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFSoftShadowMap;
  scene.add(key);

  // A knob: a turned aluminium cap on a skirt, with an engraved line.
  const capShape = [[0, 0], [0.95, 0], [0.95, 0.12], [0.86, 0.2], [0.8, 0.95], [0.74, 1.05], [0, 1.05]]
    .map(([x, y]) => new THREE.Vector2(x, y));
  const capGeo = new THREE.LatheGeometry(capShape, 96);
  capGeo.rotateX(Math.PI / 2); // the knob's axis points at the camera
  const metal = new THREE.MeshPhysicalMaterial({ color: 0xc9ced4, metalness: 1, roughness: 0.28, clearcoat: 0.3 });
  const ink = new THREE.MeshStandardMaterial({ color: 0x111316, roughness: 0.6 });

  const knobs = sonore.params.map((p, i) => {
    const group = new THREE.Group();
    group.position.set(i === 0 ? -2.4 : 2.4, -0.2, 0);
    const turn = new THREE.Group();
    group.add(turn);
    const cap = new THREE.Mesh(capGeo, metal);
    cap.castShadow = true;
    turn.add(cap);
    const line = new THREE.Mesh(new THREE.BoxGeometry(0.08, 0.6, 0.02), ink);
    line.position.set(0, 0.42, 1.06);
    turn.add(line);
    // A ring around it that glows with the output level.
    const ring = new THREE.Mesh(
      new THREE.TorusGeometry(1.32, 0.04, 16, 96),
      new THREE.MeshStandardMaterial({ color: 0x15171a, emissive: 0xff8a3d, emissiveIntensity: 0 }),
    );
    group.add(ring);
    scene.add(group);
    const label = document.createElement("div");
    label.className = "label";
    labelLayer.appendChild(label);
    cap.userData.index = i;
    return { p, i, turn, ring, cap, label, shown: "" };
  });

  // ── Drawing on demand ────────────────────────────────────────────────────
  let wanted = true, last = 0, level = 0, glow = 0;
  const redraw = () => (wanted = true);

  function place(k) {
    const value = sonore.get(k.i);
    k.turn.rotation.z = THREE.MathUtils.degToRad(135 - 270 * sonore.toNormalised(k.i, value));
    const text = `${k.p.label.toUpperCase()}<b>${sonore.format(k.i, value)}</b>`;
    if (text !== k.shown) k.label.innerHTML = k.shown = text;
  }

  function frame(now) {
    requestAnimationFrame(frame);
    // The ring's glow eases toward the level, so it animates; nothing else does.
    const easing = Math.abs(level - glow) > 0.002;
    if (document.hidden || (!wanted && !easing) || now - last < 33) return;
    last = now;
    wanted = false;
    glow += (level - glow) * 0.3;
    const box = stage.getBoundingClientRect();
    for (const k of knobs) {
      place(k);
      k.ring.material.emissiveIntensity = 1.6 * glow;
      // The label hangs under the ring: a point just below it, on screen.
      const at = k.turn.getWorldPosition(new THREE.Vector3()).add(new THREE.Vector3(0, -1.6, 0)).project(camera);
      k.label.style.left = `${(at.x * 0.5 + 0.5) * box.width}px`;
      k.label.style.top = `${(-at.y * 0.5 + 0.5) * box.height}px`;
    }
    renderer.render(scene, camera);
  }
  requestAnimationFrame(frame);

  function resize() {
    const w = stage.clientWidth, h = stage.clientHeight;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    // Keep both knobs in view however the host sizes the window.
    camera.position.z = Math.max(17, 12 / camera.aspect + 9);
    camera.updateProjectionMatrix();
    redraw();
  }
  addEventListener("resize", resize);
  resize();

  // Every change, from this page or the host's automation, and the meter.
  sonore.on(redraw);
  sonore.onLevel((rms) => { level = Math.min(1, rms * 4); });
  document.addEventListener("visibilitychange", redraw);

  // ── Turning a knob ───────────────────────────────────────────────────────
  const ray = new THREE.Raycaster();
  const caps = knobs.map((k) => k.cap);
  function pick(e) {
    const box = stage.getBoundingClientRect();
    ray.setFromCamera(new THREE.Vector2(((e.clientX - box.left) / box.width) * 2 - 1,
                                        -((e.clientY - box.top) / box.height) * 2 + 1), camera);
    const hit = ray.intersectObjects(caps, false)[0];
    return hit ? knobs[hit.object.userData.index] : null;
  }
  let drag = null;
  stage.addEventListener("pointerdown", (e) => {
    const k = pick(e);
    if (!k) return;
    stage.setPointerCapture(e.pointerId);
    // The gesture opens on the first movement: a click records nothing.
    drag = { k, y: e.clientY, from: sonore.toNormalised(k.i, sonore.get(k.i)), open: false };
  });
  stage.addEventListener("pointermove", (e) => {
    if (!drag) {
      stage.style.cursor = pick(e) ? "ns-resize" : "default";
      return;
    }
    if (!drag.open) {
      drag.open = true;
      sonore.begin(drag.k.i);
    }
    const travel = e.shiftKey ? 1000 : 200; // pixels for the whole range; Shift for fine
    const n = Math.min(1, Math.max(0, drag.from + (drag.y - e.clientY) / travel));
    sonore.set(drag.k.i, sonore.fromNormalised(drag.k.i, n));
    redraw();
  });
  const release = () => {
    if (drag?.open) sonore.end(drag.k.i);
    drag = null;
  };
  stage.addEventListener("pointerup", release);
  stage.addEventListener("pointercancel", release);
  stage.addEventListener("dblclick", (e) => {
    const k = pick(e);
    if (!k) return;
    sonore.begin(k.i);
    sonore.set(k.i, k.p.default);
    sonore.end(k.i);
    redraw();
  });
  stage.addEventListener("wheel", (e) => {
    const k = pick(e);
    if (!k) return;
    e.preventDefault();
    const n = Math.min(1, Math.max(0, sonore.toNormalised(k.i, sonore.get(k.i)) + (e.deltaY < 0 ? 0.02 : -0.02)));
    sonore.begin(k.i);
    sonore.set(k.i, sonore.fromNormalised(k.i, n));
    sonore.end(k.i);
    redraw();
  }, { passive: false });
}

// No WebGL in this webview: the same controls, as plain sliders.
function fallback() {
  stage.style.display = "none";
  const box = document.getElementById("fallback");
  box.style.display = "block";
  sonore.params.forEach((p, i) => {
    const row = document.createElement("label");
    const input = Object.assign(document.createElement("input"), { type: "range", min: 0, max: 1, step: 0.001 });
    const text = document.createElement("span");
    const show = (v) => {
      input.value = sonore.toNormalised(i, v);
      text.textContent = sonore.format(i, v);
    };
    input.addEventListener("pointerdown", () => sonore.begin(i));
    input.addEventListener("input", () => sonore.set(i, sonore.fromNormalised(i, Number(input.value))));
    input.addEventListener("change", () => sonore.end(i));
    row.append(p.label, input, text);
    box.appendChild(row);
    show(sonore.get(i));
    sonore.on((values, index, value) => { if (index === i) show(value); });
  });
}
