// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a nodal circuit engine (the DK method).
//
// Every pedal and preamp in this SDK so far was derived by hand: write the
// ODE, discretise, solve. That is the right thing for a circuit with one
// nonlinearity and it is the wrong thing for a Fuzz Face, whose two
// transistors, four capacitors and a feedback loop make a hand derivation a
// week of algebra that is wrong somewhere. This is the general machine:
// a netlist goes in, and every sample the circuit's node voltages come out.
//
// The method is Yeh's nodal DK (D. T. Yeh, "Automated physical modeling of
// nonlinear audio circuits for real-time audio effects", IEEE TASLP 2010,
// and the generalisation in Holters & Zölzer, DAFx 2015):
//   1. Modified nodal analysis stamps the linear elements into a conductance
//      matrix A. Capacitors and inductors become trapezoidal COMPANION
//      models -- a conductance plus a history current -- so the linear part
//      of the circuit is one constant matrix per sample rate.
//   2. The nonlinear elements are pulled out as PORTS. With A inverted once,
//      the circuit reduces to p = p0 - K i(p): the port voltages are an
//      affine function of the port currents through a small matrix K.
//   3. That small system is solved by Newton's method every sample, warm-
//      started from the previous sample, which is why it converges in two
//      or three iterations at audio rates.
// The cost per sample is the reduced system, not the whole matrix: for a
// Fuzz Face, a 4x4 Newton and one matrix-vector product.
//
// Devices: resistor, capacitor, inductor, voltage source (the input signal
// and the supply are both sources), Shockley diode, Ebers-Moll BJT (NPN or
// PNP, transport form), Shichman-Hodges JFET (the phaser's variable
// resistor), and an IDEAL op-amp -- the MNA nullor: the solver enforces
// v+ == v- exactly, which is the op-amp of every pedal analysis in the
// literature (Yeh's Tube Screamer treatment assumes it; the pedal's
// clipping is its feedback diodes, not the chip). A circuit that relies on
// op-amp RAIL clipping needs a clipper stage after it, and a nullor with no
// feedback path makes the matrix singular, exactly like a real op-amp wired
// open-loop. Resistors and sources may change at run time --
// a potentiometer is a resistor that moves -- and a resistor change re-
// factorises the linear part, so move pots at control rate, not per sample.
//
// Two SPICE habits are kept because they are what makes a nodal solver
// robust: a `gmin` conductance from every node to ground (a node that only
// touches a capacitor is otherwise floating at DC), and junction voltage
// LIMITING in the Newton step (an unlimited step through an exponential
// overflows before it converges). The operating point at reset() is solved
// with the capacitors open, so the circuit starts settled instead of
// spending a quarter-second charging its coupling capacitors on air.
//
// Everything is double: a 100k resistor next to a 1e-7 saturation current
// is eleven orders of magnitude, which float cannot carry through an
// inverse.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * A circuit of up to MaxNodes nodes (excluding ground, node 0), MaxSources
 * voltage sources, MaxReactive capacitors + inductors and MaxPorts nonlinear
 * ports (a diode is one port, a transistor two).
 *
 * A part's node numbers are ground or ones addNode() returned; every add
 * refuses (-1) any other, so nothing is ever stamped outside the matrix.
 *
 * SIZE: a few KB of doubles. Fine as a member.
 */
template<int MaxNodes = 12, int MaxSources = 2, int MaxReactive = 8, int MaxPorts = 4,
         int MaxOpamps = 2>
class NodalCircuit {
public:
  /** The size of the nodal system: an unknown per node, per voltage
   *  source's current and per op-amp's output current. */
  static constexpr int kDim = MaxNodes + MaxSources + MaxOpamps;
  static constexpr double kVt = 0.02585; ///< thermal voltage at 300 K, in volts
  static constexpr double kGmin = 1e-9;  ///< siemens, node to ground: SPICE's floating-node insurance

  /** An empty netlist at 48 kHz. Add the parts, then setSampleRate() and
   *  reset() before the first step(). */
  NodalCircuit() { clear(); }

  /** Forget the netlist. */
  void clear() {
    nodes_ = sources_ = resistors_ = reactive_ = ports_ = devices_ = opamps_ = 0;
    valid_ = false;
  }
  /** A new node; node 0 is ground. Returns its index, 1 to MaxNodes -- or
   *  -1 once MaxNodes are in use, which every part refuses. It was 0, and a
   *  part meant for the extra node was wired to ground without a word. */
  int addNode() { return nodes_ < MaxNodes ? ++nodes_ : -1; }

  /** A resistor between nodes `a` and `b`, in ohms; 1 mΩ or less is held
   *  at 1 mΩ. Returns the id setResistor() takes, or -1 once 24 resistors
   *  are in or for a node that is not one (see the class comment). */
  int addResistor(int a, int b, double ohms) {
    if (resistors_ >= kMaxResistors || !isNode(a) || !isNode(b)) return -1;
    res_[resistors_] = {a, b, ohms > 1e-3 ? 1.0 / ohms : 1e3};
    valid_ = false;
    return resistors_++;
  }
  /** A capacitor between nodes `a` and `b`, in farads. Capacitors and
   *  inductors share MaxReactive: returns its index among them, or -1 when
   *  they are full. */
  int addCapacitor(int a, int b, double farads) { return addReactive(a, b, farads, false); }
  /** An inductor between nodes `a` and `b`, in henries; as addCapacitor(). */
  int addInductor(int a, int b, double henry) { return addReactive(a, b, henry, true); }
  /** A voltage source from `a` (+) to `b` (-). */
  int addVoltageSource(int a, int b, double volts) {
    if (sources_ >= MaxSources || !isNode(a) || !isNode(b)) return -1;
    src_[sources_] = {a, b, volts};
    valid_ = false;
    return sources_++;
  }
  /** An IDEAL op-amp (an MNA nullor): v+ == v- enforced exactly, the output
   *  driven by whatever current that takes. Infinite gain, no rails -- see
   *  the header note. Needs a feedback path. */
  int addOpamp(int plus, int minus, int out) {
    if (opamps_ >= MaxOpamps || !isNode(plus) || !isNode(minus) || !isNode(out)) return -1;
    oa_[opamps_] = {plus, minus, out};
    valid_ = false;
    return opamps_++;
  }
  /** An N-channel JFET (Shichman-Hodges square law, symmetric in vds, with
   *  channel-length modulation `lambda`). `beta` = Idss / Vp^2; `vp` is
   *  NEGATIVE for N-channel. Gate current is zero: audio circuits keep the
   *  gate junction reverse-biased. `pchannel` flips every sign. */
  int addJfet(int d, int g, int s, double beta, double vp, double lambda = 0.01,
              bool pchannel = false) {
    if (ports_ + 2 > MaxPorts || devices_ >= kMaxDevices || !isNode(d) || !isNode(g) || !isNode(s)) return -1;
    Device& dev = dev_[devices_];
    dev.kind = Device::Jfet;
    dev.port[0] = ports_;
    dev.port[1] = ports_ + 1;
    dev.beta = beta;
    dev.vp = vp;
    dev.lambda = lambda;
    dev.pnp = pchannel;
    port_[ports_++] = {g, s};
    port_[ports_++] = {d, s};
    valid_ = false;
    return devices_++;
  }

  /** A Shockley diode, anode `a`, cathode `b`. */
  int addDiode(int a, int b, double is = 2.52e-9, double n = 1.752) {
    if (ports_ + 1 > MaxPorts || devices_ >= kMaxDevices || !isNode(a) || !isNode(b)) return -1;
    Device& d = dev_[devices_];
    d.kind = Device::Diode;
    d.port[0] = ports_;
    d.is = is;
    d.n = n;
    port_[ports_++] = {a, b};
    valid_ = false;
    return devices_++;
  }
  /** An Ebers-Moll BJT. `betaR` is the reverse beta (a few, for any real
   *  transistor); `pnp` flips every junction. */
  int addBjt(int c, int b, int e, double is, double betaF, double betaR = 4.0, bool pnp = false) {
    if (ports_ + 2 > MaxPorts || devices_ >= kMaxDevices || !isNode(c) || !isNode(b) || !isNode(e)) return -1;
    Device& d = dev_[devices_];
    d.kind = Device::Bjt;
    d.port[0] = ports_;
    d.port[1] = ports_ + 1;
    d.is = is;
    d.n = 1.0;
    d.betaF = betaF;
    d.betaR = betaR;
    d.pnp = pnp;
    port_[ports_++] = {b, e};
    port_[ports_++] = {b, c};
    valid_ = false;
    return devices_++;
  }

  /** Move a resistor (a pot). Re-factorises the linear part. */
  void setResistor(int id, double ohms) {
    if (id < 0 || id >= resistors_) return;
    res_[id].g = ohms > 1e-3 ? 1.0 / ohms : 1e3;
    valid_ = false;
  }
  /** Drive a source. Cheap: no re-factorisation. */
  void setSource(int id, double volts) {
    // Finite or nothing: an Inf source drove every Newton state to NaN, and
    // NaN never converges back to anything -- FuzzFace and TubeScreamer
    // went silent for good on one infinite input sample, converged() still
    // saying yes.
    if (id >= 0 && id < sources_) src_[id].v = std::isfinite(volts) ? volts : 0.0;
  }

  /** The sample rate step() runs at, in Hz (1 or less means 48000). Marks
   *  the linear part for re-factorisation; call reset() after it to settle
   *  the circuit at the new rate. */
  void setSampleRate(double sr) {
    sr_ = sr > 1.0 ? sr : 48000.0;
    valid_ = false;
  }
  /** Solve the operating point and settle every reactive element there. */
  void reset() {
    prepare();
    // DC: capacitors open (gmin only), inductors near-shorts.
    double adc[kDim][kDim];
    stampLinear(adc, true);
    double ainv[kDim][kDim];
    invert(adc, ainv);
    for (int i = 0; i < kDim; ++i) { b_[i] = 0.0; v_[i] = 0.0; }
    for (int p = 0; p < MaxPorts; ++p) { pv_[p] = 0.0; pi_[p] = 0.0; }
    for (int r = 0; r < reactive_; ++r) rx_[r].ieq = 0.0;
    loadSources(b_);
    solve(ainv, b_, 200);
    // The companion history that carries the DC solution on: a capacitor
    // holds its voltage with no current, and an inductor its CURRENT -- the
    // DC stamp's near-short of 1e3 S times the little voltage it drops. The
    // first version gave the inductor no current, so a choke carrying bias
    // started at 0 A and climbed back with L/R: a thump at the top of every
    // render, from a reset whose whole job is to start settled.
    for (int r = 0; r < reactive_; ++r) {
      const double vr = nodeV(rx_[r].a) - nodeV(rx_[r].b);
      rx_[r].v = vr;
      if (rx_[r].inductor) {
        const double idc = 1e3 * vr;
        rx_[r].i = idc;
        rx_[r].ieq = idc + rx_[r].g * vr;
      } else {
        rx_[r].i = 0.0;
        rx_[r].ieq = -rx_[r].g * vr;
      }
    }
    // From here on, the dynamic matrix.
    if (!valid_) factorise();
  }

  /** Advance one sample with the sources at their current values. A
   *  resistor, rate or netlist change since the last step is re-factorised
   *  here first, a full matrix inversion. No allocation. */
  inline void step() {
    if (!valid_) factorise();
    // History currents and sources.
    for (int i = 0; i < kDim; ++i) b_[i] = 0.0;
    for (int r = 0; r < reactive_; ++r) {
      // Device current a->b is g*v + ieq: KCL sees -ieq into a, +ieq into b.
      if (rx_[r].a > 0) b_[rx_[r].a - 1] -= rx_[r].ieq;
      if (rx_[r].b > 0) b_[rx_[r].b - 1] += rx_[r].ieq;
    }
    loadSources(b_);
    // A step whose answer is not finite is not taken: the last good state
    // stays, and converged() says no. Nothing finite should get here, but a
    // NaN in this state is for ever, so the price of one copy is worth it.
    double pvKept[MaxPorts > 0 ? MaxPorts : 1], vKept[kDim];
    for (int p = 0; p < ports_; ++p) pvKept[p] = pv_[p];
    for (int i = 0; i < kDim; ++i) vKept[i] = v_[i];
    solve(ainv_, b_, 24);
    bool finite = true;
    for (int i = 0; i < kDim && finite; ++i) finite = std::isfinite(v_[i]);
    if (!finite) {
      for (int p = 0; p < ports_; ++p) pv_[p] = pvKept[p];
      for (int i = 0; i < kDim; ++i) v_[i] = vKept[i];
      converged_ = false;
      return;
    }
    // Update the companions.
    for (int r = 0; r < reactive_; ++r) {
      const double vr = nodeV(rx_[r].a) - nodeV(rx_[r].b);
      const double ir = rx_[r].g * vr + rx_[r].ieq;
      rx_[r].v = vr;
      rx_[r].i = ir;
      rx_[r].ieq = rx_[r].inductor ? ir + rx_[r].g * vr : -ir - rx_[r].g * vr;
    }
  }

  /** Node voltage: 0 for ground, and for a number that is not a node --
   *  past the nodes made, the unknowns are source and op-amp currents. */
  inline double voltage(int node) const { return isNode(node) ? nodeV(node) : 0.0; }
  /** Current through voltage source `id`, into its + terminal. */
  double sourceCurrent(int id) const { return id >= 0 && id < sources_ ? v_[nodes_ + id] : 0.0; }
  /** Newton iterations the last step() or reset() took: typically two or
   *  three for a warm-started step at audio rates, at most 24 for a step
   *  and 200 for reset(). */
  int newtonIterations() const { return iterations_; }
  /** Whether the last step() or reset() settled to 1e-9 V within its
   *  iteration limit. False too after a step whose answer was not finite
   *  and was thrown away, the previous state kept. */
  bool converged() const { return converged_; }

private:
  static constexpr int kMaxResistors = 24;
  static constexpr int kMaxDevices = MaxPorts;

  /** A resistor between nodes a and b, held as its conductance in siemens. */
  struct Resistor { int a, b; double g; };
  /** A capacitor (farads) or inductor (henries) as its trapezoidal companion:
   *  the conductance for the sample rate, the history current, and the last
   *  voltage across it and current through it. */
  struct Reactive { int a, b; double value, g, ieq, v, i; bool inductor; };
  /** A voltage source, a (+) to b (-), at v volts. */
  struct Source { int a, b; double v; };
  /** A nonlinear port: the voltage from a to b that Newton solves for. A
   *  diode is one (anode to cathode), a BJT two (base-emitter and
   *  base-collector), a JFET two (gate-source and drain-source). */
  struct Port { int a, b; };
  /** A nonlinear device, its ports and its model's parameters: saturation
   *  current and emission coefficient for a junction, the forward and
   *  reverse betas for a BJT, the square law's beta, pinch-off and lambda
   *  for a JFET. */
  struct Device {
    enum Kind { Diode, Bjt, Jfet } kind = Diode;
    int port[2] = {0, 0};
    double is = 1e-9, n = 1.0, betaF = 100.0, betaR = 4.0;
    double beta = 1e-3, vp = -2.0, lambda = 0.01;
    bool pnp = false;
  };
  /** An ideal op-amp: the two input nodes the solver holds equal, and the
   *  output node it drives. */
  struct Opamp { int plus, minus, out; };

  /** Ground, or a node addNode() made. A node number is a row of the
   *  matrix: one past the nodes made was stamped into a source's row, and
   *  one past kDim outside the matrix altogether, onto the stack. */
  bool isNode(int n) const { return n >= 0 && n <= nodes_; }

  int addReactive(int a, int b, double value, bool inductor) {
    if (reactive_ >= MaxReactive || !isNode(a) || !isNode(b)) return -1;
    rx_[reactive_] = {a, b, value, 0.0, 0.0, 0.0, 0.0, inductor};
    valid_ = false;
    return reactive_++;
  }

  inline double nodeV(int node) const { return node > 0 ? v_[node - 1] : 0.0; }

  void prepare() {
    const double t = 1.0 / sr_;
    for (int r = 0; r < reactive_; ++r)
      rx_[r].g = rx_[r].inductor ? t / (2.0 * rx_[r].value) : 2.0 * rx_[r].value / t;
    // Each port's limiting, from the junction it belongs to: its thermal
    // voltage, SPICE's critical voltage vt ln(vt / (sqrt2 Is)), and which way
    // is forward. A JFET's ports are quadratic, not exponential.
    for (int p = 0; p < MaxPorts; ++p) {
      limit_[p].junction = false;
      limit_[p].sign = 1.0;
      limit_[p].vt = kVt;
      limit_[p].vcrit = 0.0;
    }
    for (int d = 0; d < devices_; ++d) {
      const Device& dev = dev_[d];
      if (dev.kind == Device::Jfet) continue;
      const double vt = dev.n * kVt;
      const double vcrit = vt * std::log(vt / (1.4142135623730951 * (dev.is > 1e-30 ? dev.is : 1e-30)));
      const int count = dev.kind == Device::Bjt ? 2 : 1;
      for (int k = 0; k < count; ++k) {
        Limit& l = limit_[dev.port[k]];
        l.junction = true;
        l.sign = dev.pnp ? -1.0 : 1.0;
        l.vt = vt;
        l.vcrit = vcrit;
      }
    }
  }

  /** SPICE's junction limiting (pnjlim). A step INTO forward conduction past
   *  vcrit is taken logarithmically -- the current there grows e-fold every
   *  vt, and a full step overflows its way out of the basin. A step OUT of
   *  conduction, or one that stays below vcrit, is taken whole: nothing
   *  explodes that way, and capping it is what made a diode that had to
   *  swing 20 V in one sample take 80 iterations to get there. */
  static double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::fabs(vnew - vold) > 2.0 * vt) {
      if (vold > 0.0) {
        const double arg = 1.0 + (vnew - vold) / vt;
        vnew = arg > 0.0 ? vold + vt * std::log(arg) : vcrit;
      } else {
        vnew = vt * std::log(vnew / vt);
      }
    }
    return vnew;
  }

  void stampLinear(double a[kDim][kDim], bool dc) {
    for (int i = 0; i < kDim; ++i)
      for (int j = 0; j < kDim; ++j) a[i][j] = 0.0;
    for (int i = 0; i < nodes_; ++i) a[i][i] += kGmin;
    for (int r = 0; r < resistors_; ++r) stampG(a, res_[r].a, res_[r].b, res_[r].g);
    for (int r = 0; r < reactive_; ++r) {
      const double g = dc ? (rx_[r].inductor ? 1e3 : 0.0) : rx_[r].g;
      if (g > 0.0) stampG(a, rx_[r].a, rx_[r].b, g);
    }
    // Sources: an extra unknown (the branch current) and two constraints.
    for (int s = 0; s < sources_; ++s) {
      const int row = nodes_ + s;
      if (src_[s].a > 0) { a[src_[s].a - 1][row] += 1.0; a[row][src_[s].a - 1] += 1.0; }
      if (src_[s].b > 0) { a[src_[s].b - 1][row] -= 1.0; a[row][src_[s].b - 1] -= 1.0; }
    }
    // Ideal op-amps (nullors): one extra unknown -- the output current,
    // entering the out node's KCL -- and one constraint row, v+ == v-.
    // The stamp is asymmetric; the pivoting inverse does not care.
    for (int o = 0; o < opamps_; ++o) {
      const int row = nodes_ + sources_ + o;
      if (oa_[o].out > 0) a[oa_[o].out - 1][row] += 1.0;
      if (oa_[o].plus > 0) a[row][oa_[o].plus - 1] += 1.0;
      if (oa_[o].minus > 0) a[row][oa_[o].minus - 1] -= 1.0;
    }
    // The unknowns are packed: nodes, then source currents, then op-amp
    // currents. Unused rows beyond them stay identity so the inverse exists.
    for (int i = nodes_ + sources_ + opamps_; i < kDim; ++i) a[i][i] = 1.0;
  }
  static void stampG(double a[kDim][kDim], int na, int nb, double g) {
    if (na > 0) a[na - 1][na - 1] += g;
    if (nb > 0) a[nb - 1][nb - 1] += g;
    if (na > 0 && nb > 0) { a[na - 1][nb - 1] -= g; a[nb - 1][na - 1] -= g; }
  }

  void loadSources(double b[kDim]) const {
    for (int s = 0; s < sources_; ++s) b[nodes_ + s] = src_[s].v;
  }

  /** Gauss-Jordan with partial pivoting; kDim is small. */
  static void invert(double a[kDim][kDim], double inv[kDim][kDim]) {
    for (int i = 0; i < kDim; ++i)
      for (int j = 0; j < kDim; ++j) inv[i][j] = i == j ? 1.0 : 0.0;
    for (int c = 0; c < kDim; ++c) {
      int piv = c;
      for (int r = c + 1; r < kDim; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
      if (piv != c)
        for (int j = 0; j < kDim; ++j) {
          double t = a[c][j]; a[c][j] = a[piv][j]; a[piv][j] = t;
          t = inv[c][j]; inv[c][j] = inv[piv][j]; inv[piv][j] = t;
        }
      double d = a[c][c];
      if (std::fabs(d) < 1e-300) d = 1e-300;
      const double s = 1.0 / d;
      for (int j = 0; j < kDim; ++j) { a[c][j] *= s; inv[c][j] *= s; }
      for (int r = 0; r < kDim; ++r) {
        if (r == c) continue;
        const double f = a[r][c];
        if (f == 0.0) continue;
        for (int j = 0; j < kDim; ++j) { a[r][j] -= f * a[c][j]; inv[r][j] -= f * inv[c][j]; }
      }
    }
  }

  void factorise() {
    prepare();
    double a[kDim][kDim];
    stampLinear(a, false);
    invert(a, ainv_);
    // What the solve needs from A^-1 that does not change until the next
    // factorise: the port columns A^-1 N^T and the reduced matrix
    // K = N A^-1 N^T. Rebuilt every sample, they were a kDim x ports pass and
    // a ports^2 x kDim one per step, for numbers that had not moved.
    portColumns(ainv_, colCache_, kCache_);
    valid_ = true;
  }

  /** A^-1 N_q^T for every port q, and K = N A^-1 N^T from them. */
  void portColumns(const double ainv[kDim][kDim], double col[MaxPorts][kDim],
                   double k[MaxPorts][MaxPorts]) const {
    for (int q = 0; q < ports_; ++q)
      for (int i = 0; i < kDim; ++i) {
        double s = 0.0;
        if (port_[q].a > 0) s += ainv[i][port_[q].a - 1];
        if (port_[q].b > 0) s -= ainv[i][port_[q].b - 1];
        col[q][i] = s;
      }
    for (int p = 0; p < ports_; ++p)
      for (int q = 0; q < ports_; ++q) k[p][q] = rowV(col[q], port_[p]);
  }

  /** Newton on the reduced port system, then the full node solution. */
  void solve(const double ainv[kDim][kDim], const double b[kDim], int maxIter) {
    // p0 = N A^-1 b, K = N A^-1 N^T. The columns and K come from factorise()
    // for the dynamic matrix, which is every step(); reset()'s one-off DC
    // solve passes a matrix of its own and builds them here.
    double localCol[MaxPorts][kDim], localK[MaxPorts][MaxPorts];
    const bool cached = ainv == ainv_;
    if (!cached) portColumns(ainv, localCol, localK);
    const double (*col)[kDim] = cached ? colCache_ : localCol;
    const double (*k)[MaxPorts] = cached ? kCache_ : localK;
    double p0[MaxPorts];
    double ab[kDim];
    for (int i = 0; i < kDim; ++i) {
      double s = 0.0;
      for (int j = 0; j < kDim; ++j) s += ainv[i][j] * b[j];
      ab[i] = s;
    }
    for (int p = 0; p < ports_; ++p) p0[p] = rowV(ab, port_[p]);
    // Newton: f(p) = p - p0 + K i(p) = 0, J = I + K di/dp.
    double cur[MaxPorts], jac[MaxPorts][MaxPorts];
    converged_ = false;
    iterations_ = 0;
    for (int it = 0; it < maxIter; ++it) {
      ++iterations_;
      deviceCurrents(pv_, cur, jac);
      double f[MaxPorts], j[MaxPorts][MaxPorts];
      double worst = 0.0;
      for (int p = 0; p < ports_; ++p) {
        double s = pv_[p] - p0[p];
        for (int q = 0; q < ports_; ++q) s += k[p][q] * cur[q];
        f[p] = s;
        for (int q = 0; q < ports_; ++q) {
          double jj = p == q ? 1.0 : 0.0;
          for (int r = 0; r < ports_; ++r) jj += k[p][r] * jac[r][q];
          j[p][q] = jj;
        }
      }
      double delta[MaxPorts];
      solveSmall(j, f, delta);
      for (int p = 0; p < ports_; ++p) {
        // Junction limiting, SPICE's: see pnjlim. It was a flat 0.25 V cap on
        // every step in every direction, so with 24 iterations a port could
        // move 6 V a sample and no more: a diode clipper fed 24 V at 5 kHz
        // (15.7 V a sample) left 2798 of 4800 samples unconverged and was
        // 20 V wrong. A JFET's quadratic ports are capped generously instead.
        const double vold = pv_[p];
        double vnew = vold - delta[p];
        const Limit& l = limit_[p];
        if (l.junction) {
          vnew = l.sign * pnjlim(l.sign * vnew, l.sign * vold, l.vt, l.vcrit);
        } else {
          const double cap = 4.0;
          if (vnew > vold + cap) vnew = vold + cap;
          if (vnew < vold - cap) vnew = vold - cap;
        }
        pv_[p] = vnew;
        const double d = std::fabs(vnew - vold);
        worst = d > worst ? d : worst;
      }
      if (worst < 1e-9) { converged_ = true; break; }
    }
    deviceCurrents(pv_, cur, jac);
    for (int p = 0; p < ports_; ++p) pi_[p] = cur[p];
    // v = A^-1 (b - N^T i) = A^-1 b - sum_p (A^-1 N_p^T) i_p: ab is already in
    // hand and the port columns are cached, so a second kDim^2 product
    // becomes kDim x ports.
    for (int i = 0; i < kDim; ++i) {
      double s = ab[i];
      for (int p = 0; p < ports_; ++p) s -= col[p][i] * cur[p];
      v_[i] = s;
    }
  }
  static double rowV(const double x[kDim], const Port& p) {
    double s = 0.0;
    if (p.a > 0) s += x[p.a - 1];
    if (p.b > 0) s -= x[p.b - 1];
    return s;
  }
  /** Gaussian elimination on the ports x ports Newton system. */
  void solveSmall(double a[MaxPorts][MaxPorts], double b[MaxPorts], double x[MaxPorts]) const {
    const int n = ports_;
    for (int c = 0; c < n; ++c) {
      int piv = c;
      for (int r = c + 1; r < n; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
      if (piv != c) {
        for (int j = 0; j < n; ++j) { double t = a[c][j]; a[c][j] = a[piv][j]; a[piv][j] = t; }
        double t = b[c]; b[c] = b[piv]; b[piv] = t;
      }
      double d = a[c][c];
      if (std::fabs(d) < 1e-300) d = 1e-300;
      for (int r = c + 1; r < n; ++r) {
        const double f = a[r][c] / d;
        if (f == 0.0) continue;
        for (int j = c; j < n; ++j) a[r][j] -= f * a[c][j];
        b[r] -= f * b[c];
      }
    }
    for (int r = n - 1; r >= 0; --r) {
      double s = b[r];
      for (int j = r + 1; j < n; ++j) s -= a[r][j] * x[j];
      const double d = std::fabs(a[r][r]) < 1e-300 ? 1e-300 : a[r][r];
      x[r] = s / d;
    }
    for (int r = n; r < MaxPorts; ++r) x[r] = 0.0;
  }

  /** Port currents (a->b through the device) and their Jacobian. */
  void deviceCurrents(const double pv[MaxPorts], double cur[MaxPorts], double jac[MaxPorts][MaxPorts]) const {
    for (int p = 0; p < MaxPorts; ++p) {
      cur[p] = 0.0;
      for (int q = 0; q < MaxPorts; ++q) jac[p][q] = 0.0;
    }
    for (int d = 0; d < devices_; ++d) {
      const Device& dev = dev_[d];
      if (dev.kind == Device::Diode) {
        const int p = dev.port[0];
        const double s = 1.0 / (dev.n * kVt);
        const double e = expClamped(pv[p] * s);
        cur[p] = dev.is * (e - 1.0);
        jac[p][p] = dev.is * e * s;
      } else if (dev.kind == Device::Jfet) {
        const int p1 = dev.port[0], p2 = dev.port[1];
        const double sign = dev.pnp ? -1.0 : 1.0;
        const double vgs = sign * pv[p1], vds = sign * pv[p2];
        double id, dgs, dds;
        jfetCurrent(dev, vgs, vds, id, dgs, dds);
        cur[p1] = 0.0; // the gate draws nothing; its VOLTAGE is the control
        cur[p2] = sign * id;
        jac[p2][p1] = dgs;
        jac[p2][p2] = dds;
      } else {
        const int p1 = dev.port[0], p2 = dev.port[1];
        const double sign = dev.pnp ? -1.0 : 1.0;
        const double vbe = sign * pv[p1], vbc = sign * pv[p2];
        const double s = 1.0 / kVt;
        const double ebe = expClamped(vbe * s), ebc = expClamped(vbc * s);
        // Transport Ebers-Moll: terminal currents into the device.
        const double ic = dev.is * ((ebe - ebc) - (ebc - 1.0) / dev.betaR);
        const double ib = dev.is * ((ebe - 1.0) / dev.betaF + (ebc - 1.0) / dev.betaR);
        // Ports: (b->e) carries ib + ic, (b->c) carries -ic.
        cur[p1] = sign * (ib + ic);
        cur[p2] = sign * (-ic);
        const double dic_dbe = dev.is * ebe * s;
        const double dic_dbc = dev.is * (-ebc - ebc / dev.betaR) * s;
        const double dib_dbe = dev.is * ebe / dev.betaF * s;
        const double dib_dbc = dev.is * ebc / dev.betaR * s;
        // d(sign*i)/d(pv) = sign * di/dv * sign = di/dv.
        jac[p1][p1] = dib_dbe + dic_dbe;
        jac[p1][p2] = dib_dbc + dic_dbc;
        jac[p2][p1] = -dic_dbe;
        jac[p2][p2] = -dic_dbc;
      }
    }
  }
  /** Drain current and derivatives, symmetric in vds: below vds = 0 the
   *  drain and source swap roles -- the device has no preferred end, which
   *  is exactly why it works as a voltage-controlled resistor around zero. */
  static void jfetCurrent(const Device& d, double vgs, double vds, double& id, double& dgs,
                          double& dds) {
    if (vds >= 0.0) {
      jfetQuadrant(d, vgs, vds, id, dgs, dds);
    } else {
      double i2, g2, s2;
      jfetQuadrant(d, vgs - vds, -vds, i2, g2, s2);
      id = -i2;
      dgs = -g2;
      dds = g2 + s2;
    }
  }
  /** Shichman-Hodges for vds >= 0: cutoff, triode, saturation -- C1 at both
   *  boundaries, which is what keeps Newton honest across them. */
  static void jfetQuadrant(const Device& d, double vgs, double vds, double& id, double& dgs,
                           double& dds) {
    const double vov = vgs - d.vp;
    if (vov <= 0.0) {
      id = dgs = dds = 0.0;
      return;
    }
    const double lam = 1.0 + d.lambda * vds;
    if (vds < vov) { // triode: the variable-resistor region
      id = d.beta * (2.0 * vov * vds - vds * vds) * lam;
      dgs = d.beta * 2.0 * vds * lam;
      dds = d.beta * ((2.0 * vov - 2.0 * vds) * lam + (2.0 * vov * vds - vds * vds) * d.lambda);
    } else { // saturation
      id = d.beta * vov * vov * lam;
      dgs = d.beta * 2.0 * vov * lam;
      dds = d.beta * vov * vov * d.lambda;
    }
  }
  static double expClamped(double x) { return std::exp(x > 80.0 ? 80.0 : x); }

  int nodes_ = 0, sources_ = 0, resistors_ = 0, reactive_ = 0, ports_ = 0, devices_ = 0,
      opamps_ = 0;
  Resistor res_[kMaxResistors];
  Opamp oa_[MaxOpamps < 1 ? 1 : MaxOpamps];
  Reactive rx_[MaxReactive];
  Source src_[MaxSources];
  Port port_[MaxPorts];
  Device dev_[kMaxDevices];
  // Initialised, because step() before any reset() reads them: the Newton
  // guess pv_ above all, which as a NaN never converges to anything else.
  double ainv_[kDim][kDim] = {};
  double colCache_[MaxPorts][kDim] = {};
  double kCache_[MaxPorts][MaxPorts] = {};
  double b_[kDim] = {}, v_[kDim] = {};
  double pv_[MaxPorts] = {}, pi_[MaxPorts] = {};
  double sr_ = 48000.0;
  /** How a port's Newton step is limited, worked out in prepare(): whether
   *  it is a junction at all, which way is forward (PNP flips it), its
   *  thermal voltage and SPICE's critical voltage for pnjlim(). */
  struct Limit {
    bool junction;
    double sign, vt, vcrit;
  };
  Limit limit_[MaxPorts > 0 ? MaxPorts : 1]{};
  bool valid_ = false, converged_ = false;
  int iterations_ = 0;
};

} // namespace sonore
