// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a small dense matrix, and the linear solve that is usually
// why somebody wanted one.
//
// The places audio actually needs one are few and specific: an ambisonic
// decoder or a matrix mixer (a product per block), linear prediction (a
// Toeplitz system from an autocorrelation), a least-squares fit at prepare()
// -- an EQ curve matched to a measurement, a waveshaper fitted to a table --
// and the state-space form of a circuit. All of them are small, dense and
// square or nearly so, which is what this is built for. It is not a BLAS.
//
// Two decisions shape it:
//
//   ALLOCATION. The storage is a std::vector sized at construction, and every
//   operation that RETURNS a matrix allocates its result. That is the
//   convenient form for prepare() and for tests. For the audio thread there
//   is a second form of each operation that matters -- multiply() into an
//   existing result, solveInPlace(), the compound assignments -- and none of
//   those allocates. A matrix sized up front can be used per block; one built
//   per block is the mistake a general matrix type invites, and the test
//   counts allocations through the in-place forms to hold that line.
//
//   PIVOTING. The solve is Gaussian elimination with PARTIAL pivoting: at each
//   column the row with the largest magnitude is swapped up before it is
//   divided by. Without it, [[0 1][1 1]] cannot be solved at all (a zero on
//   the diagonal) and a matrix that is merely close to that loses every
//   digit. With it, the error is bounded by the matrix's conditioning rather
//   than by the order its rows happened to arrive in.
//
// Storage is row-major: element (r, c) is data[r * columns + c], so a row is
// contiguous and getRow() is a pointer, not a copy.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sonore {

/**
 * A small dense row-major matrix of float, double or long double, with
 * Gaussian elimination (partial pivoting) behind solve(), inverse() and
 * determinant(). It mirrors the reference's dsp::Matrix and keeps its
 * accessor names.
 *
 * Owns its storage, a `std::vector` sized at construction (see the file
 * comment). Anything that returns a Matrix allocates, so build matrices in
 * prepare() or on a message thread; on the audio thread use only the forms
 * that write into existing storage -- multiply(a, b, out), solveInPlace(),
 * the compound operators and element access. A shape mismatch is never a
 * read past the end: it gives a 0 x 0 result, a false, or a target left as
 * it was. Not synchronised: one thread at a time.
 */
template <typename T>
class Matrix {
  static_assert(std::is_floating_point<T>::value, "Matrix holds float, double or long double");

public:
  /** 0 x 0: somewhere for inverse() or solve() to write into. */
  Matrix() = default;
  /** rows x columns, every element zero. */
  Matrix(size_t rows, size_t columns) : rows_(rows), cols_(columns), data_(rows * columns, T(0)) {}
  /** rows x columns from row-major data: rows * columns values. */
  Matrix(size_t rows, size_t columns, const T* rowMajor) : Matrix(rows, columns) {
    if (rowMajor)
      for (size_t i = 0; i < data_.size(); ++i) data_[i] = rowMajor[i];
  }

  /** The n x n identity. */
  static Matrix identity(size_t size) {
    Matrix m(size, size);
    for (size_t i = 0; i < size; ++i) m(i, i) = T(1);
    return m;
  }

  /**
   * The symmetric Toeplitz matrix of a vector: element (i, j) is v[|i - j|].
   * An autocorrelation r[0..n-1] becomes the matrix of the normal equations
   * for linear prediction, which is the shape audio meets most. `vector` can
   * be a row or a column; entries past its end read as zero.
   */
  static Matrix toeplitz(const Matrix& vector, size_t size) {
    Matrix m(size, size);
    for (size_t i = 0; i < size; ++i)
      for (size_t j = 0; j < size; ++j) m(i, j) = vector.element(i > j ? i - j : j - i);
    return m;
  }

  /** The Hankel matrix of a vector: element (i, j) is v[i + j + offset] --
   *  constant along the anti-diagonals. Entries past the end read as zero. */
  static Matrix hankel(const Matrix& vector, size_t size, size_t offset = 0) {
    Matrix m(size, size);
    for (size_t i = 0; i < size; ++i)
      for (size_t j = 0; j < size; ++j) m(i, j) = vector.element(i + j + offset);
    return m;
  }

  /** The number of rows; 0 for a null matrix. */
  size_t getNumRows() const { return rows_; }
  /** The number of columns; 0 for a null matrix. */
  size_t getNumColumns() const { return cols_; }
  /** The rows * columns elements, row-major: (r, c) is at r * columns + c.
   *  Valid until the matrix is assigned to or destroyed; may be null for a
   *  0 x 0 matrix. */
  T* getRawDataPointer() { return data_.data(); }
  /** As above, read-only. */
  const T* getRawDataPointer() const { return data_.data(); }
  /** The first element, row-major: begin() to end() visits every element,
   *  row by row. */
  T* begin() { return data_.data(); }
  /** One past the last element. */
  T* end() { return data_.data() + data_.size(); }
  /** As begin(), read-only. */
  const T* begin() const { return data_.data(); }
  /** As end(), read-only. */
  const T* end() const { return data_.data() + data_.size(); }

  /** Element (row, column), 0-based. Not bounds-checked: both must be in
   *  range. No allocation, safe on the audio thread. */
  T operator()(size_t row, size_t column) const { return data_[row * cols_ + column]; }
  /** Element (row, column) to write, 0-based. Not bounds-checked. */
  T& operator()(size_t row, size_t column) { return data_[row * cols_ + column]; }

  /** A row, in place: getNumColumns() contiguous values. `row` is 0-based
   *  and not bounds-checked. */
  T* getRow(size_t row) { return data_.data() + row * cols_; }
  /** As above, read-only. */
  const T* getRow(size_t row) const { return data_.data() + row * cols_; }
  /** A column, as a rows x 1 matrix. A copy, because a column is strided. */
  Matrix getColumn(size_t column) const {
    Matrix out(rows_, 1);
    for (size_t r = 0; r < rows_; ++r) out.data_[r] = (*this)(r, column);
    return out;
  }

  /** Whether rows == columns; true for 0 x 0. */
  bool isSquare() const { return rows_ == cols_; }
  /** Whether it is one row or one column (a 1 x 1 is both). */
  bool isVector() const { return rows_ == 1 || cols_ == 1; }
  /** Whether it has exactly one column, any number of rows. */
  bool isOneColumnVector() const { return cols_ == 1; }
  /** Whether it has exactly one row, any number of columns. */
  bool isOneRowVector() const { return rows_ == 1; }
  /** Whether it has no elements: what a shape mismatch returns. */
  bool isNullMatrix() const { return rows_ == 0 || cols_ == 0; }
  /** Whether `other` has the same number of rows and of columns. */
  bool sameShapeAs(const Matrix& other) const { return rows_ == other.rows_ && cols_ == other.cols_; }

  /** Every element to zero, shape kept. No allocation. */
  Matrix& clear() {
    for (T& v : data_) v = T(0);
    return *this;
  }
  /** Exchange rows `a` and `b` (0-based) in place; nothing when they are
   *  equal. Neither index is bounds-checked. No allocation. */
  Matrix& swapRows(size_t a, size_t b) {
    if (a != b)
      for (size_t c = 0; c < cols_; ++c) std::swap((*this)(a, c), (*this)(b, c));
    return *this;
  }
  /** Exchange columns `a` and `b` (0-based) in place; nothing when they are
   *  equal. Neither index is bounds-checked. No allocation. */
  Matrix& swapColumns(size_t a, size_t b) {
    if (a != b)
      for (size_t r = 0; r < rows_; ++r) std::swap((*this)(r, a), (*this)(r, b));
    return *this;
  }

  // ── Arithmetic ─────────────────────────────────────────────────────────────
  //
  // Mismatched shapes are a programming error, and the policy is the same
  // everywhere: the compound forms leave the target untouched, the forms that
  // return a matrix return a 0 x 0 one (isNullMatrix()), multiply() returns
  // false. Never a read past the end.

  /** Add `other` element by element, in place; left untouched when the
   *  shapes differ. No allocation. */
  Matrix& operator+=(const Matrix& other) {
    if (sameShapeAs(other))
      for (size_t i = 0; i < data_.size(); ++i) data_[i] += other.data_[i];
    return *this;
  }
  /** Subtract `other` element by element, in place; left untouched when the
   *  shapes differ. No allocation. */
  Matrix& operator-=(const Matrix& other) {
    if (sameShapeAs(other))
      for (size_t i = 0; i < data_.size(); ++i) data_[i] -= other.data_[i];
    return *this;
  }
  /** Scale every element by `scalar`, in place. No allocation. */
  Matrix& operator*=(T scalar) {
    for (T& v : data_) v *= scalar;
    return *this;
  }
  /** Element by element (the Hadamard product), in place. */
  Matrix& hadamard(const Matrix& other) {
    if (sameShapeAs(other))
      for (size_t i = 0; i < data_.size(); ++i) data_[i] *= other.data_[i];
    return *this;
  }

  /** The element-by-element sum, as a new matrix (allocates); 0 x 0 when
   *  the shapes differ. */
  Matrix operator+(const Matrix& other) const {
    if (!sameShapeAs(other)) return Matrix(0, 0);
    Matrix out(*this);
    out += other;
    return out;
  }
  /** The element-by-element difference, as a new matrix (allocates); 0 x 0
   *  when the shapes differ. */
  Matrix operator-(const Matrix& other) const {
    if (!sameShapeAs(other)) return Matrix(0, 0);
    Matrix out(*this);
    out -= other;
    return out;
  }
  /** A copy with every element scaled by `scalar` (allocates). */
  Matrix operator*(T scalar) const {
    Matrix out(*this);
    out *= scalar;
    return out;
  }
  /** The matrix product this * other, getNumRows() x other.getNumColumns(),
   *  as a new matrix (allocates); 0 x 0 when this one's columns are not
   *  `other`'s rows. multiply() is the form that does not allocate. */
  Matrix operator*(const Matrix& other) const {
    Matrix out(rows_, other.cols_);
    if (!multiply(*this, other, out)) return Matrix(0, 0);
    return out;
  }

  /**
   * out = a * b, into a matrix already the right shape (a.rows x b.columns).
   * The audio-thread product: no allocation. `out` must not be `a` or `b`.
   */
  static bool multiply(const Matrix& a, const Matrix& b, Matrix& out) {
    if (a.cols_ != b.rows_ || out.rows_ != a.rows_ || out.cols_ != b.cols_) return false;
    if (&out == &a || &out == &b) return false;
    out.clear();
    // i-k-j order: the inner loop walks a row of b and a row of out, both
    // contiguous, instead of striding down a column of b.
    for (size_t i = 0; i < a.rows_; ++i)
      for (size_t k = 0; k < a.cols_; ++k) {
        const T aik = a(i, k);
        const T* bk = b.getRow(k);
        T* oi = out.getRow(i);
        for (size_t j = 0; j < b.cols_; ++j) oi[j] += aik * bk[j];
      }
    return true;
  }

  /** A columns x rows copy with (r, c) moved to (c, r). Allocates. */
  Matrix transposed() const {
    Matrix out(cols_, rows_);
    for (size_t r = 0; r < rows_; ++r)
      for (size_t c = 0; c < cols_; ++c) out(c, r) = (*this)(r, c);
    return out;
  }

  /** Same shape and every element within `tolerance`. */
  static bool compare(const Matrix& a, const Matrix& b, T tolerance = T(0)) {
    if (!a.sameShapeAs(b)) return false;
    for (size_t i = 0; i < a.data_.size(); ++i)
      if (!(std::fabs(a.data_[i] - b.data_[i]) <= tolerance)) return false;
    return true;
  }

  // ── Solving ────────────────────────────────────────────────────────────────

  /**
   * Solve A X = B for X, A being this (square) and B the n x m `b`, which is
   * replaced by X. Several right-hand sides at once are just more columns.
   *
   * Returns false -- and leaves `b` as it was -- when A is not square, the
   * shapes disagree, or A is singular to working precision: a pivot no
   * bigger than n * epsilon * the largest element of A. That threshold is
   * relative on purpose; an absolute one calls every matrix of small numbers
   * singular and every matrix of large ones fine.
   *
   * Copies A and B to work on, so it allocates. solveInPlace() does not.
   */
  bool solve(Matrix& b) const {
    Matrix a(*this);
    Matrix x(b);
    if (!a.solveInPlace(x)) return false;
    b = std::move(x);
    return true;
  }

  /**
   * The same, destroying this matrix (it is left holding the eliminated
   * upper triangle) and writing X over `b`. No allocation: the audio-thread
   * form, for a system whose matrices are sized at prepare(). On failure
   * both are left part-way through the elimination.
   */
  bool solveInPlace(Matrix& b) {
    const size_t n = rows_;
    if (!isSquare() || n == 0 || b.rows_ != n) return false;
    const T limit = singularLimit();
    if (!(limit > T(0))) return false;
    const size_t m = b.cols_;

    for (size_t col = 0; col < n; ++col) {
      // Partial pivoting: the largest magnitude at or below the diagonal.
      size_t pivot = col;
      T best = std::fabs((*this)(col, col));
      for (size_t r = col + 1; r < n; ++r) {
        const T v = std::fabs((*this)(r, col));
        if (v > best) {
          best = v;
          pivot = r;
        }
      }
      if (!(best > limit)) return false;
      swapRows(col, pivot);
      b.swapRows(col, pivot);

      const T inv = T(1) / (*this)(col, col);
      for (size_t r = col + 1; r < n; ++r) {
        const T f = (*this)(r, col) * inv;
        if (f == T(0)) continue;
        (*this)(r, col) = T(0);
        for (size_t c = col + 1; c < n; ++c) (*this)(r, c) -= f * (*this)(col, c);
        for (size_t c = 0; c < m; ++c) b(r, c) -= f * b(col, c);
      }
    }
    // Back substitution, bottom row up.
    for (size_t r = n; r-- > 0;) {
      const T inv = T(1) / (*this)(r, r);
      for (size_t c = 0; c < m; ++c) {
        T sum = b(r, c);
        for (size_t k = r + 1; k < n; ++k) sum -= (*this)(r, k) * b(k, c);
        b(r, c) = sum * inv;
      }
    }
    return true;
  }

  /**
   * The determinant, by the same elimination: the product of the pivots,
   * negated once per row swap. 0 for a non-square matrix, and exactly 0 when
   * a column has nothing left to pivot on. Allocates a working copy.
   */
  T determinant() const {
    if (!isSquare() || rows_ == 0) return T(0);
    Matrix a(*this);
    const size_t n = rows_;
    T det = T(1);
    for (size_t col = 0; col < n; ++col) {
      size_t pivot = col;
      T best = std::fabs(a(col, col));
      for (size_t r = col + 1; r < n; ++r) {
        const T v = std::fabs(a(r, col));
        if (v > best) {
          best = v;
          pivot = r;
        }
      }
      if (best == T(0)) return T(0);
      if (pivot != col) {
        a.swapRows(col, pivot);
        det = -det;
      }
      const T p = a(col, col);
      det *= p;
      for (size_t r = col + 1; r < n; ++r) {
        const T f = a(r, col) / p;
        for (size_t c = col + 1; c < n; ++c) a(r, c) -= f * a(col, c);
      }
    }
    return det;
  }

  /** The inverse, into `out` (resized as needed). False when singular, with
   *  `out` untouched. Solves A X = I, so it inherits solve()'s pivoting and
   *  its singularity rule. Allocates. */
  bool inverse(Matrix& out) const {
    if (!isSquare()) return false;
    Matrix x = identity(rows_);
    if (!solve(x)) return false;
    out = std::move(x);
    return true;
  }

  /** "[[1, 2], [3, 4]]", for a log line or a failing test. */
  std::string toString() const {
    std::string s = "[";
    char buf[48];
    for (size_t r = 0; r < rows_; ++r) {
      s += r ? ", [" : "[";
      for (size_t c = 0; c < cols_; ++c) {
        std::snprintf(buf, sizeof buf, c ? ", %g" : "%g", (double) (*this)(r, c));
        s += buf;
      }
      s += "]";
    }
    return s + "]";
  }

private:
  /** Element k of the data in storage order, zero past the end: how a row or
   *  column vector is indexed by toeplitz() and hankel(). */
  T element(size_t k) const { return k < data_.size() ? data_[k] : T(0); }

  T singularLimit() const {
    T largest = T(0);
    for (const T v : data_) {
      const T a = std::fabs(v);
      if (a > largest) largest = a;
    }
    return largest * (T) rows_ * std::numeric_limits<T>::epsilon();
  }

  size_t rows_ = 0, cols_ = 0;
  std::vector<T> data_;
};

} // namespace sonore
