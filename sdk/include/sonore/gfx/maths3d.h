// SPDX-License-Identifier: Apache-2.0
//
// Three-dimensional vectors, 4x4 matrices, quaternions, and an arcball that
// turns mouse drags into a rotation.
//
// ── Why these live beside the OpenGL classes and not in geometry.h ──────────
//
// geometry.h is the 2D world every widget is laid out in, and everything there
// is float because a knob at 63.5 pixels is a real position. These are the
// types a shader is FED: a model-view-projection matrix, a camera orbiting a
// spectrum waterfall, the orientation a user dragged a 3D scope into. Nothing
// in the 2D toolkit needs them, and nothing here needs a Component -- so they
// get a header of their own, with no platform code in it, and are usable (and
// tested) without a GL context at all.
//
// ── The conventions, stated once ────────────────────────────────────────────
//
//  - Right-handed. A positive rotation about an axis turns counter-clockwise
//    when the axis points at the viewer, which is what every GL tutorial,
//    every textbook and every glm-style library assumes. Getting the hand
//    wrong produces a scene that is mirrored, which does not read as a maths
//    bug -- it reads as "the camera is behind the object".
//
//  - Matrix3D is COLUMN-MAJOR, mat[column * 4 + row], because that is the
//    order glUniformMatrix4fv takes with transpose = GL_FALSE. A row-major
//    store would need a transpose on every upload, or worse, would be uploaded
//    untransposed and work until the first non-symmetric matrix.
//
//  - Products read right to left, as in the maths: (A * B) applied to v is
//    A(B(v)), so `projection * view * model` is the usual spelling and a point
//    goes through the model first. Transform in geometry.h names its
//    composition `then` precisely because left-to-right is the trap; here the
//    operator is kept because a reader porting shader maths expects it.
//
//  - Quaternions are unit quaternions whenever they represent a rotation.
//    fromAngle produces one; a product of two is one up to rounding, so
//    anything that accumulates thousands of them -- an arcball does, once per
//    mouse move -- renormalises as it goes. A quaternion that has drifted to
//    length 1.01 rotates AND scales, and the object visibly grows.
#pragma once

#include <cmath>

#include "geometry.h"

namespace sonore {
namespace gfx {

/** x, y and z, with the arithmetic a shader's inputs need. */
template <typename Type>
class Vector3D {
public:
  Type x = Type(0), ///< To the right, in this header's right-handed space.
       y = Type(0), ///< Up.
       z = Type(0); ///< Towards the viewer: a camera looks down negative z.

  /** The zero vector. */
  Vector3D() {}
  /** The three components, x first. */
  Vector3D(Type xValue, Type yValue, Type zValue) : x(xValue), y(yValue), z(zValue) {}

  /** The unit vector (1, 0, 0). */
  static Vector3D xAxis() { return {Type(1), Type(0), Type(0)}; }
  /** The unit vector (0, 1, 0). */
  static Vector3D yAxis() { return {Type(0), Type(1), Type(0)}; }
  /** The unit vector (0, 0, 1). */
  static Vector3D zAxis() { return {Type(0), Type(0), Type(1)}; }

  /** Component by component. */
  Vector3D operator+(const Vector3D& o) const { return {x + o.x, y + o.y, z + o.z}; }
  /** Component by component. */
  Vector3D operator-(const Vector3D& o) const { return {x - o.x, y - o.y, z - o.z}; }
  /** Every component negated: the opposite direction. */
  Vector3D operator-() const { return {-x, -y, -z}; }
  /** Every component times `s`. A vector times a VECTOR is the dot product;
   *  see dot(). */
  Vector3D operator*(Type s) const { return {x * s, y * s, z * s}; }
  /** Every component divided by `s`; no check for zero. */
  Vector3D operator/(Type s) const { return {x / s, y / s, z / s}; }
  /** Component by component, in place. */
  Vector3D& operator+=(const Vector3D& o) { x += o.x; y += o.y; z += o.z; return *this; }
  /** Component by component, in place. */
  Vector3D& operator-=(const Vector3D& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  /** Every component times `s`, in place. */
  Vector3D& operator*=(Type s) { x *= s; y *= s; z *= s; return *this; }
  /** Every component divided by `s`, in place; no check for zero. */
  Vector3D& operator/=(Type s) { x /= s; y /= s; z /= s; return *this; }

  /** The dot product, as the reference spells it -- and named below, because
   *  `a * b` returning a SCALAR surprises anyone who has not seen it before. */
  Type operator*(const Vector3D& o) const { return dot(o); }
  /** The cross product, the reference's spelling. Note the precedence: `^` binds
   *  more loosely than `+`, so `a ^ b + c` is `a ^ (b + c)`. cross() has no such
   *  trap. */
  Vector3D operator^(const Vector3D& o) const { return cross(o); }

  /** The sum of the component products: the cosine of the angle between
   *  the two times both lengths, 0 for perpendicular vectors. */
  Type dot(const Vector3D& o) const { return x * o.x + y * o.y + z * o.z; }
  /** Perpendicular to both, by the right-hand rule: xAxis().cross(yAxis())
   *  is zAxis(). Its length is the sine of the angle times both lengths. */
  Vector3D cross(const Vector3D& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }

  /** The length squared, without the square root: for comparing lengths. */
  Type lengthSquared() const { return x * x + y * y + z * z; }
  /** The Euclidean length. */
  Type length() const { return (Type) std::sqrt(lengthSquared()); }

  /** Unit length, or zero for a zero vector. A NaN here would reach a shader
   *  as a NaN normal and light nothing, silently. */
  Vector3D normalised() const {
    const Type len = length();
    return len > Type(0) ? Vector3D(x / len, y / len, z / len) : Vector3D();
  }

  /** True when the length is at most 1e-6 (the squared length at most
   *  1e-12): too short to be a direction worth normalising. */
  bool lengthIsBelowEpsilon() const { return lengthSquared() <= Type(1.0e-12); }

  /** Exact comparison of every component; no tolerance. */
  bool operator==(const Vector3D& o) const { return x == o.x && y == o.y && z == o.z; }
  /** Exact comparison of every component; no tolerance. */
  bool operator!=(const Vector3D& o) const { return !(*this == o); }
};

/**
 * A 4x4 matrix, column-major, as OpenGL takes it.
 *
 * Default-constructed it is the identity -- the one default for which "I
 * forgot to set the model matrix" still draws something.
 */
template <typename Type>
class Matrix3D {
public:
  /** mat[column * 4 + row]. Public, as the reference has it, so it can be handed
   *  straight to glUniformMatrix4fv. */
  Type mat[16];

  /** The identity. */
  Matrix3D() { setIdentity(); }

  /** Sixteen values in COLUMN order: the first four are the first column. */
  Matrix3D(Type m00, Type m10, Type m20, Type m30, Type m01, Type m11, Type m21, Type m31,
           Type m02, Type m12, Type m22, Type m32, Type m03, Type m13, Type m23, Type m33) {
    mat[0] = m00; mat[1] = m10; mat[2] = m20; mat[3] = m30;
    mat[4] = m01; mat[5] = m11; mat[6] = m21; mat[7] = m31;
    mat[8] = m02; mat[9] = m12; mat[10] = m22; mat[11] = m32;
    mat[12] = m03; mat[13] = m13; mat[14] = m23; mat[15] = m33;
  }

  /** Sixteen values in column order, from an array. */
  explicit Matrix3D(const Type* values) {
    for (int i = 0; i < 16; ++i) mat[i] = values[i];
  }

  /** A translation. */
  explicit Matrix3D(const Vector3D<Type>& translation) {
    setIdentity();
    mat[12] = translation.x;
    mat[13] = translation.y;
    mat[14] = translation.z;
  }

  /** Element access by ROW and COLUMN, so a formula off a page can be typed in
   *  without anybody transposing it in their head. */
  Type at(int row, int column) const { return mat[column * 4 + row]; }
  /** The element at `row`, `column` (both 0..3, unchecked), to write. */
  Type& at(int row, int column) { return mat[column * 4 + row]; }

  /** The matrix that leaves every point where it is. */
  static Matrix3D identity() { return Matrix3D(); }

  /** A move by `t`: the identity with `t` in the last column. */
  static Matrix3D translation(const Vector3D<Type>& t) { return Matrix3D(t); }

  /** A scale about the origin by `s.x`, `s.y` and `s.z` along the three axes. */
  static Matrix3D scaling(const Vector3D<Type>& s) {
    Matrix3D m;
    m.mat[0] = s.x;
    m.mat[5] = s.y;
    m.mat[10] = s.z;
    return m;
  }

  /**
   * A perspective projection from the six planes of the view volume, as
   * glFrustum makes it.
   *
   * The planes are named nearDistance and farDistance rather than the near
   * and far every textbook uses: windows.h defines `near` and `far` as
   * macros, and a parameter called either stops compiling in any file that
   * includes it first.
   */
  static Matrix3D fromFrustum(Type left, Type right, Type bottom, Type top, Type nearDistance,
                              Type farDistance) {
    Matrix3D m;
    m.mat[0] = (Type(2) * nearDistance) / (right - left);
    m.mat[1] = m.mat[2] = m.mat[3] = Type(0);
    m.mat[4] = Type(0);
    m.mat[5] = (Type(2) * nearDistance) / (top - bottom);
    m.mat[6] = m.mat[7] = Type(0);
    m.mat[8] = (right + left) / (right - left);
    m.mat[9] = (top + bottom) / (top - bottom);
    m.mat[10] = -(farDistance + nearDistance) / (farDistance - nearDistance);
    m.mat[11] = Type(-1);
    m.mat[12] = m.mat[13] = Type(0);
    m.mat[14] = -(Type(2) * farDistance * nearDistance) / (farDistance - nearDistance);
    m.mat[15] = Type(0);
    return m;
  }

  /** The same, from a vertical field of view and an aspect ratio -- what a
   *  caller with a camera actually has. */
  static Matrix3D fromPerspective(Type fieldOfViewYRadians, Type aspect, Type nearDistance,
                                  Type farDistance) {
    const Type top = nearDistance * (Type) std::tan(fieldOfViewYRadians / Type(2));
    const Type right = top * aspect;
    return fromFrustum(-right, right, -top, top, nearDistance, farDistance);
  }

  /** An orthographic projection, as glOrtho makes it. A 2D overlay drawn in
   *  pixels wants this, with the top and bottom swapped for a y-down space. */
  static Matrix3D fromOrthographic(Type left, Type right, Type bottom, Type top,
                                   Type nearDistance, Type farDistance) {
    Matrix3D m;
    m.mat[0] = Type(2) / (right - left);
    m.mat[5] = Type(2) / (top - bottom);
    m.mat[10] = Type(-2) / (farDistance - nearDistance);
    m.mat[12] = -(right + left) / (right - left);
    m.mat[13] = -(top + bottom) / (top - bottom);
    m.mat[14] = -(farDistance + nearDistance) / (farDistance - nearDistance);
    return m;
  }

  /** A rotation of `radians` about `axis` (normalised here), right-handed. */
  static Matrix3D rotationAroundAxis(const Vector3D<Type>& axis, Type radians) {
    const Vector3D<Type> a = axis.normalised();
    const Type c = (Type) std::cos(radians), s = (Type) std::sin(radians);
    const Type t = Type(1) - c;
    Matrix3D m;
    m.at(0, 0) = c + a.x * a.x * t;
    m.at(0, 1) = a.x * a.y * t - a.z * s;
    m.at(0, 2) = a.x * a.z * t + a.y * s;
    m.at(1, 0) = a.y * a.x * t + a.z * s;
    m.at(1, 1) = c + a.y * a.y * t;
    m.at(1, 2) = a.y * a.z * t - a.x * s;
    m.at(2, 0) = a.z * a.x * t - a.y * s;
    m.at(2, 1) = a.z * a.y * t + a.x * s;
    m.at(2, 2) = c + a.z * a.z * t;
    return m;
  }

  /**
   * A rotation from three Euler angles, in radians: about X by `angles.x`
   * FIRST, then Y, then Z -- that is, Rz * Ry * Rx.
   *
   * The order is stated because it is the whole of the ambiguity: Euler angles
   * applied in another order are a different rotation, and a caller who
   * assumed the other one sees an object that turns correctly about each axis
   * on its own and wrongly about any two together.
   */
  static Matrix3D rotation(const Vector3D<Type>& angles) {
    return rotationAroundAxis(Vector3D<Type>::zAxis(), angles.z) *
           rotationAroundAxis(Vector3D<Type>::yAxis(), angles.y) *
           rotationAroundAxis(Vector3D<Type>::xAxis(), angles.x);
  }

  /** A view matrix: the camera at `eye`, looking at `target`, with `up` as near
   *  to up as the two allow. */
  static Matrix3D lookAt(const Vector3D<Type>& eye, const Vector3D<Type>& target,
                         const Vector3D<Type>& up) {
    const Vector3D<Type> f = (target - eye).normalised();
    const Vector3D<Type> s = f.cross(up).normalised();
    const Vector3D<Type> u = s.cross(f);
    Matrix3D m;
    m.at(0, 0) = s.x; m.at(0, 1) = s.y; m.at(0, 2) = s.z;
    m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z;
    m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z;
    m.at(0, 3) = -s.dot(eye);
    m.at(1, 3) = -u.dot(eye);
    m.at(2, 3) = f.dot(eye);
    return m;
  }

  /** this * other: `other` is applied FIRST. */
  Matrix3D operator*(const Matrix3D& other) const {
    Matrix3D r;
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 4; ++row) {
        Type sum = Type(0);
        for (int k = 0; k < 4; ++k) sum += mat[k * 4 + row] * other.mat[column * 4 + k];
        r.mat[column * 4 + row] = sum;
      }
    return r;
  }

  /** `*this = *this * other`: `other` is applied before what this held. */
  Matrix3D& operator*=(const Matrix3D& other) {
    *this = *this * other;
    return *this;
  }

  /** A point (w = 1), with the perspective divide when the matrix has one. */
  Vector3D<Type> transformPoint(const Vector3D<Type>& p) const {
    const Type x = mat[0] * p.x + mat[4] * p.y + mat[8] * p.z + mat[12];
    const Type y = mat[1] * p.x + mat[5] * p.y + mat[9] * p.z + mat[13];
    const Type z = mat[2] * p.x + mat[6] * p.y + mat[10] * p.z + mat[14];
    const Type w = mat[3] * p.x + mat[7] * p.y + mat[11] * p.z + mat[15];
    if (w != Type(0) && w != Type(1)) return {x / w, y / w, z / w};
    return {x, y, z};
  }

  /** A direction (w = 0): rotated and scaled, never translated. A normal
   *  pushed through transformPoint picks up the translation and points
   *  somewhere else entirely. */
  Vector3D<Type> transformDirection(const Vector3D<Type>& d) const {
    return {mat[0] * d.x + mat[4] * d.y + mat[8] * d.z,
            mat[1] * d.x + mat[5] * d.y + mat[9] * d.z,
            mat[2] * d.x + mat[6] * d.y + mat[10] * d.z};
  }

  /** Rows and columns swapped. For a pure rotation this is also its inverse. */
  Matrix3D transposed() const {
    Matrix3D r;
    for (int row = 0; row < 4; ++row)
      for (int column = 0; column < 4; ++column) r.at(row, column) = at(column, row);
    return r;
  }

  /** The determinant of all sixteen elements; zero for a matrix with no
   *  inverse. */
  Type determinant() const {
    Type s[6], c[6];
    minors(s, c);
    return s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
  }

  /**
   * The inverse, and whether there is one.
   *
   * By the Laplace expansion over complementary 2x2 minors of the top and
   * bottom row pairs: twelve small determinants computed once and reused for
   * all sixteen cofactors, rather than sixteen 3x3 determinants each computed
   * from scratch. A singular matrix -- a scale of zero on some axis, which a
   * collapsed component produces -- returns false and leaves `out` alone,
   * rather than filling it with infinities a shader would then draw as
   * nothing.
   */
  bool invert(Matrix3D* out) const {
    Type s[6], c[6];
    minors(s, c);
    const Type det =
        s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
    if (det == Type(0) || !(det == det)) return false;
    if (!out) return true;
    const Type k = Type(1) / det;
    Matrix3D r;
    r.at(0, 0) = (at(1, 1) * c[5] - at(1, 2) * c[4] + at(1, 3) * c[3]) * k;
    r.at(0, 1) = (-at(0, 1) * c[5] + at(0, 2) * c[4] - at(0, 3) * c[3]) * k;
    r.at(0, 2) = (at(3, 1) * s[5] - at(3, 2) * s[4] + at(3, 3) * s[3]) * k;
    r.at(0, 3) = (-at(2, 1) * s[5] + at(2, 2) * s[4] - at(2, 3) * s[3]) * k;
    r.at(1, 0) = (-at(1, 0) * c[5] + at(1, 2) * c[2] - at(1, 3) * c[1]) * k;
    r.at(1, 1) = (at(0, 0) * c[5] - at(0, 2) * c[2] + at(0, 3) * c[1]) * k;
    r.at(1, 2) = (-at(3, 0) * s[5] + at(3, 2) * s[2] - at(3, 3) * s[1]) * k;
    r.at(1, 3) = (at(2, 0) * s[5] - at(2, 2) * s[2] + at(2, 3) * s[1]) * k;
    r.at(2, 0) = (at(1, 0) * c[4] - at(1, 1) * c[2] + at(1, 3) * c[0]) * k;
    r.at(2, 1) = (-at(0, 0) * c[4] + at(0, 1) * c[2] - at(0, 3) * c[0]) * k;
    r.at(2, 2) = (at(3, 0) * s[4] - at(3, 1) * s[2] + at(3, 3) * s[0]) * k;
    r.at(2, 3) = (-at(2, 0) * s[4] + at(2, 1) * s[2] - at(2, 3) * s[0]) * k;
    r.at(3, 0) = (-at(1, 0) * c[3] + at(1, 1) * c[1] - at(1, 2) * c[0]) * k;
    r.at(3, 1) = (at(0, 0) * c[3] - at(0, 1) * c[1] + at(0, 2) * c[0]) * k;
    r.at(3, 2) = (-at(3, 0) * s[3] + at(3, 1) * s[1] - at(3, 2) * s[0]) * k;
    r.at(3, 3) = (at(2, 0) * s[3] - at(2, 1) * s[1] + at(2, 2) * s[0]) * k;
    *out = r;
    return true;
  }

  /** The inverse, or the identity for a singular matrix. For a caller that has
   *  already ruled singularity out and wants an expression. */
  Matrix3D inverted() const {
    Matrix3D r;
    return invert(&r) ? r : Matrix3D();
  }

  /** True when every element is within `tolerance` (absolute, inclusive)
   *  of the other's: the comparison to use after any arithmetic. */
  bool approximatelyEqual(const Matrix3D& o, Type tolerance) const {
    for (int i = 0; i < 16; ++i) {
      const Type d = mat[i] - o.mat[i];
      if (d > tolerance || d < -tolerance) return false;
    }
    return true;
  }

  /** Exact comparison of all sixteen elements; see approximatelyEqual. */
  bool operator==(const Matrix3D& o) const {
    for (int i = 0; i < 16; ++i)
      if (mat[i] != o.mat[i]) return false;
    return true;
  }
  /** Exact comparison of all sixteen elements; see approximatelyEqual. */
  bool operator!=(const Matrix3D& o) const { return !(*this == o); }

private:
  void setIdentity() {
    for (int i = 0; i < 16; ++i) mat[i] = (i % 5 == 0) ? Type(1) : Type(0);
  }

  /** The twelve 2x2 determinants: s from rows 0-1, c from rows 2-3, over the
   *  column pairs (0,1) (0,2) (0,3) (1,2) (1,3) (2,3) and their complements. */
  void minors(Type* s, Type* c) const {
    s[0] = at(0, 0) * at(1, 1) - at(1, 0) * at(0, 1);
    s[1] = at(0, 0) * at(1, 2) - at(1, 0) * at(0, 2);
    s[2] = at(0, 0) * at(1, 3) - at(1, 0) * at(0, 3);
    s[3] = at(0, 1) * at(1, 2) - at(1, 1) * at(0, 2);
    s[4] = at(0, 1) * at(1, 3) - at(1, 1) * at(0, 3);
    s[5] = at(0, 2) * at(1, 3) - at(1, 2) * at(0, 3);
    c[5] = at(2, 2) * at(3, 3) - at(3, 2) * at(2, 3);
    c[4] = at(2, 1) * at(3, 3) - at(3, 1) * at(2, 3);
    c[3] = at(2, 1) * at(3, 2) - at(3, 1) * at(2, 2);
    c[2] = at(2, 0) * at(3, 3) - at(3, 0) * at(2, 3);
    c[1] = at(2, 0) * at(3, 2) - at(3, 0) * at(2, 2);
    c[0] = at(2, 0) * at(3, 1) - at(3, 0) * at(2, 1);
  }
};

/**
 * A rotation as a unit quaternion: the vector part and the scalar part.
 *
 * Why not just matrices: interpolating two rotation matrices element by
 * element produces something that is not a rotation at all -- the object
 * shrinks through the middle of the move -- and composing thousands of them
 * accumulates skew. A quaternion interpolates on the sphere (slerp) and
 * renormalises with one division.
 */
template <typename Type>
class Quaternion {
public:
  /** The vector part (x, y, z): for a rotation, the axis times the sine of
   *  half the angle. Zero by default. */
  Vector3D<Type> vector;
  /** The scalar part (w): for a rotation, the cosine of half the angle. 1 by
   *  default, which with a zero vector part is no rotation. */
  Type scalar = Type(1);

  /** The identity: no rotation. */
  Quaternion() {}
  /** The parts as given, not normalised. */
  Quaternion(const Vector3D<Type>& vectorPart, Type scalarPart)
      : vector(vectorPart), scalar(scalarPart) {}
  /** x, y, z of the vector part and w, the scalar, as given, not normalised. */
  Quaternion(Type x, Type y, Type z, Type w) : vector(x, y, z), scalar(w) {}

  /** A rotation of `radians` about `axis` (normalised here), right-handed. */
  static Quaternion fromAngle(Type radians, const Vector3D<Type>& axis) {
    const Type half = radians / Type(2);
    return Quaternion(axis.normalised() * (Type) std::sin(half), (Type) std::cos(half));
  }

  /** The Hamilton product: `other` is applied FIRST, as with matrices. */
  Quaternion operator*(const Quaternion& o) const {
    return Quaternion(o.vector * scalar + vector * o.scalar + vector.cross(o.vector),
                      scalar * o.scalar - vector.dot(o.vector));
  }

  /** `*this = *this * o`: `o` is applied before what this held. Not
   *  renormalised; see the header's note on drift. */
  Quaternion& operator*=(const Quaternion& o) {
    *this = *this * o;
    return *this;
  }

  /** The four parts added: for blending, as slerp does; the sum of two
   *  rotations is not a rotation until normalised. */
  Quaternion operator+(const Quaternion& o) const {
    return Quaternion(vector + o.vector, scalar + o.scalar);
  }
  /** The four parts added, in place. */
  Quaternion& operator+=(const Quaternion& o) {
    vector += o.vector;
    scalar += o.scalar;
    return *this;
  }
  /** All four parts times `s`: a weight in a blend, not a larger rotation. */
  Quaternion operator*(Type s) const { return Quaternion(vector * s, scalar * s); }
  /** All four parts negated: the same rotation, from the other hemisphere. */
  Quaternion operator-() const { return Quaternion(-vector, -scalar); }

  /** The four-dimensional dot product. For two unit quaternions, the cosine
   *  of half the angle between the rotations; negative when they lie in
   *  opposite hemispheres. */
  Type dot(const Quaternion& o) const { return vector.dot(o.vector) + scalar * o.scalar; }
  /** The four-dimensional length: 1 for a rotation that has not drifted. */
  Type length() const { return (Type) std::sqrt(dot(*this)); }

  /** Scaled to length 1, or the identity for a zero quaternion. */
  Quaternion normalised() const {
    const Type len = length();
    return len > Type(0) ? Quaternion(vector / len, scalar / len) : Quaternion();
  }

  /** The vector part negated. For a unit quaternion, the reverse rotation;
   *  see inverted() for one that has drifted. */
  Quaternion conjugate() const { return Quaternion(-vector, scalar); }

  /** The reverse rotation. For a unit quaternion this is the conjugate; the
   *  division is there so a caller who has let one drift still gets an
   *  inverse rather than something near it. */
  Quaternion inverted() const {
    const Type n = dot(*this);
    if (n <= Type(0)) return Quaternion();
    const Quaternion c = conjugate();
    return Quaternion(c.vector / n, c.scalar / n);
  }

  /** `v` rotated: q v q*, written out so it costs two cross products rather
   *  than two full quaternion products. */
  Vector3D<Type> rotate(const Vector3D<Type>& v) const {
    const Vector3D<Type> t = vector.cross(v) * Type(2);
    return v + t * scalar + vector.cross(t);
  }

  /** The same rotation as a matrix, for a shader. The quaternion is
   *  normalised first: a drifted one would otherwise scale as it rotated. */
  Matrix3D<Type> rotationMatrix() const {
    const Quaternion q = normalised();
    const Type x = q.vector.x, y = q.vector.y, z = q.vector.z, w = q.scalar;
    Matrix3D<Type> m;
    m.at(0, 0) = Type(1) - Type(2) * (y * y + z * z);
    m.at(0, 1) = Type(2) * (x * y - z * w);
    m.at(0, 2) = Type(2) * (x * z + y * w);
    m.at(1, 0) = Type(2) * (x * y + z * w);
    m.at(1, 1) = Type(1) - Type(2) * (x * x + z * z);
    m.at(1, 2) = Type(2) * (y * z - x * w);
    m.at(2, 0) = Type(2) * (x * z - y * w);
    m.at(2, 1) = Type(2) * (y * z + x * w);
    m.at(2, 2) = Type(1) - Type(2) * (x * x + y * y);
    return m;
  }

  /** The angle and axis this rotates by. The axis of the identity is
   *  arbitrary; zAxis is returned so it is at least a unit vector. */
  void toAngleAxis(Type* radians, Vector3D<Type>* axis) const {
    const Quaternion q = normalised();
    const Type w = q.scalar > Type(1) ? Type(1) : (q.scalar < Type(-1) ? Type(-1) : q.scalar);
    const Type s = (Type) std::sqrt(Type(1) - w * w);
    if (radians) *radians = Type(2) * (Type) std::acos(w);
    if (axis) *axis = s > Type(1.0e-6) ? q.vector / s : Vector3D<Type>::zAxis();
  }

  /**
   * Spherical interpolation from `a` (t = 0) to `b` (t = 1), the SHORT way.
   *
   * q and -q are the same rotation, and interpolating towards the wrong one of
   * the pair turns the object the long way round -- 350 degrees to get 10 --
   * which is why b is negated when the two point into opposite hemispheres.
   * Near-identical inputs fall back to a normalised linear blend: there the
   * sine in the denominator is close to zero, and the division amplifies
   * rounding into a visible jitter.
   */
  static Quaternion slerp(const Quaternion& a, const Quaternion& b, Type t) {
    Quaternion target = b;
    Type cosTheta = a.dot(b);
    if (cosTheta < Type(0)) {
      target = -b;
      cosTheta = -cosTheta;
    }
    if (cosTheta > Type(0.9995)) return (a * (Type(1) - t) + target * t).normalised();
    const Type theta = (Type) std::acos(cosTheta);
    const Type sinTheta = (Type) std::sin(theta);
    const Type wa = (Type) std::sin((Type(1) - t) * theta) / sinTheta;
    const Type wb = (Type) std::sin(t * theta) / sinTheta;
    return a * wa + target * wb;
  }
};

/**
 * An arcball: mouse drags inside a rectangle turned into a rotation.
 *
 * The rectangle is treated as the silhouette of a sphere. A point pressed on
 * it is lifted onto the sphere's front surface, and dragging rotates the
 * sphere so the point under the pointer STAYS under the pointer -- which is
 * what makes it feel like grabbing an object rather than steering it. Outside
 * the sphere's outline the point is held on the rim, so a drag around the
 * edge spins about the view axis, the other thing a user expects to be able
 * to do.
 *
 * Coordinates are this SDK's: logical, y DOWN. The flip to the maths' y-up
 * happens once, in the projection.
 */
class Draggable3DOrientation {
public:
  /** The vector type the arcball works in: float. */
  using VectorType = Vector3D<float>;
  /** The orientation's type: a float quaternion. */
  using QuaternionType = Quaternion<float>;

  /** No rotation, and an empty viewport: set one before the first drag. */
  Draggable3DOrientation() {}
  /** Starting at `start`, taken as given (setOrientation normalises; this
   *  does not). */
  explicit Draggable3DOrientation(const QuaternionType& start) : quaternion_(start) {}

  /** The rectangle the sphere fills, in the same coordinates the mouse events
   *  will arrive in -- normally the component's local bounds. */
  void setViewport(const Rect& area) { area_ = area; }

  /** Replace the orientation with `q`, normalised. */
  void setOrientation(const QuaternionType& q) { quaternion_ = q.normalised(); }
  /** Back to no rotation. The viewport is kept. */
  void reset() { quaternion_ = QuaternionType(); }

  /** A press at `p` (viewport coordinates, y down): where the next drag
   *  starts from. Changes no orientation. */
  void mouseDown(Point p) { last_ = projectOntoSphere(p); }

  /** The pointer moved to `p` with the button held: rotate by the arc from
   *  the last point to this one, about the axis perpendicular to both, and
   *  renormalise. A drag with no mouseDown before it starts from the
   *  sphere's centre. */
  void mouseDrag(Point p) {
    const VectorType now = projectOntoSphere(p);
    const VectorType axis = last_.cross(now);
    // atan2 rather than acos of the dot product: acos loses all precision near
    // 0 and 1, which is exactly where a slow drag lives, and a slow drag that
    // rotates in visible steps feels broken.
    const float angle = std::atan2(axis.length(), last_.dot(now));
    if (!axis.lengthIsBelowEpsilon() && angle != 0.0f)
      quaternion_ = (QuaternionType::fromAngle(angle, axis) * quaternion_).normalised();
    last_ = now;
  }

  /** The orientation the drags have built up. */
  const QuaternionType& quaternion() const { return quaternion_; }
  /** The orientation as a column-major matrix, ready for a shader's model
   *  matrix. */
  Matrix3D<float> rotationMatrix() const { return quaternion_.rotationMatrix(); }

private:
  VectorType projectOntoSphere(Point p) const {
    const float radius = 0.5f * (area_.w < area_.h ? area_.w : area_.h);
    if (!(radius > 0.0f)) return VectorType::zAxis();
    const Point centre = area_.centre();
    const float x = (p.x - centre.x) / radius;
    const float y = (centre.y - p.y) / radius;
    const float d2 = x * x + y * y;
    if (d2 >= 1.0f) {
      const float d = std::sqrt(d2);
      return {x / d, y / d, 0.0f};
    }
    return {x, y, std::sqrt(1.0f - d2)};
  }

  Rect area_;
  QuaternionType quaternion_;
  VectorType last_ = VectorType::zAxis();
};

} // namespace gfx
} // namespace sonore
